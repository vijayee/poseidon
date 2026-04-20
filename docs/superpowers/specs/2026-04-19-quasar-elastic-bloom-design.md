# Quasar Overlay Network with Elastic Bloom Filters

## Context

Poseidon is a P2P pub/sub network built on Meridian. The Quasar overlay provides probabilistic publish-subscribe routing using attenuated bloom filters. Standard bloom filters cannot support subscription removal, which is critical for a dynamic P2P network where nodes join, leave, and change interests. We replace the basic bloom filter in Quasar's attenuated bloom filter with an **elastic bloom filter** (EBF) that supports adding, removing, and dynamic resizing. The EBF paper (Wu et al., IEEE TC 2021) provides the algorithm. Pony reference implementations of a basic Bitset and Bloom Filter exist in `references/`.

## Architecture

Five layers, each depending only on the layer below:

```
Layer 0: bitset_t         (bit array, auto-grow, bitwise ops)
Layer 1: bloom_filter_t   (standard bloom filter using bitset_t + xxHash double-hashing)
Layer 2: elastic_bloom_filter_t  (EBF: bloom_filter + cooperative bucket array)
Layer 3: attenuated_bloom_filter_t  (k levels of EBF for hop-based routing)
Layer 4: quasar_t          (pub/sub overlay on Meridian using attenuated bloom filter)
```

## File Organization

```
src/Bloom/
  bitset.h / bitset.c
  bloom_filter.h / bloom_filter.c
  elastic_bloom_filter.h / elastic_bloom_filter.c
  attenuated_bloom_filter.h / attenuated_bloom_filter.c

src/Network/Quasar/
  quasar.h / quasar.c
```

## Layer 0: bitset_t

### Struct

```c
typedef struct bitset_t {
    refcounter_t refcounter;
    uint8_t* data;
    size_t size;          // bytes (not bits)
    PLATFORMLOCKTYPE(lock);
} bitset_t;
```

### API

| Function | Description |
|----------|-------------|
| `bitset_create(byte_size)` | Allocate zeroed bit array |
| `bitset_destroy(set)` | Free data and struct |
| `bitset_get(set, bit_index)` | Return bool at bit index |
| `bitset_set(set, bit_index, value)` | Set/clear bit, auto-grow if needed |
| `bitset_update(set, bit_index, value)` | Set/clear bit, return old value |
| `bitset_compare(a, b)` | Lexicographic comparison (-1, 0, 1) |
| `bitset_eq(a, b)` | Equality check |
| `bitset_xor(a, b)` | Bitwise XOR, returns new bitset |
| `bitset_and(a, b)` | Bitwise AND, returns new bitset |
| `bitset_or(a, b)` | Bitwise OR, returns new bitset |
| `bitset_not(a)` | Bitwise NOT, returns new bitset |
| `bitset_size(set)` | Return byte count |
| `bitset_bit_count(set)` | Return bit count (size * 8) |
| `bitset_compact(set)` | Trim trailing zero bytes |

### Implementation Notes

- Bit index n: byte = n / 8, offset = n % 8
- Auto-grow: if `byte_index >= size`, realloc to `byte_index + 1` bytes zeroed
- Bitwise ops follow the Pony reference: operate on min-length, pad with zeros
- Thread-safe: all mutations acquire lock, reads acquire lock

## Layer 1: bloom_filter_t

### Struct

```c
typedef struct bloom_filter_t {
    refcounter_t refcounter;
    bitset_t* bits;
    size_t size;           // number of bits
    uint32_t hash_count;   // k hash functions
    uint64_t seed_a;       // xxHash seed for first hash
    uint64_t seed_b;       // xxHash seed for second hash
    size_t count;           // approximate element count
} bloom_filter_t;
```

### API

| Function | Description |
|----------|-------------|
| `bloom_filter_create(size, hash_count)` | Create with bit count and k |
| `bloom_filter_destroy(filter)` | Destroy bitset and struct |
| `bloom_filter_add(filter, data, len)` | Insert element |
| `bloom_filter_contains(filter, data, len)` | Check membership |
| `bloom_filter_count(filter)` | Return approximate element count |
| `bloom_filter_size(filter)` | Return bit count |
| `bloom_filter_reset(filter)` | Clear all bits, reset count |
| `bloom_filter_optimal_size(n, p, &size, &k)` | Calculate optimal parameters |

### Double Hashing

Uses the Kirschner-Mitzenmacher optimization (same as Pony reference, but with xxHash):

```c
uint64_t a = XXH3_64bits_withSeed(data, len, seed_a);
uint64_t b = XXH3_64bits_withSeed(data, len, seed_b);
for (uint32_t i = 0; i < hash_count; i++) {
    size_t index = (size_t)((a + (uint64_t)i * b + (uint64_t)i * i) % size);
    bitset_set(bits, index, true);
}
```

All arithmetic uses uint64_t to prevent overflow. The `(uint64_t)i * i` replaces Pony's `i.f64().pow(2).u32()` with equivalent integer squaring.

Count tracking follows the Pony pattern: `count` only increments when at least one bit was not previously set (element is actually new, not already in the filter).

## Layer 2: elastic_bloom_filter_t

### Struct

```c
typedef struct ebf_bucket_entry_t {
    uint32_t fingerprint;
    struct ebf_bucket_entry_t* next;
} ebf_bucket_entry_t;

typedef struct elastic_bloom_filter_t {
    refcounter_t refcounter;
    bitset_t* bits;                     // Bloom filter bit array
    size_t size;                        // Number of bits
    size_t bucket_count;                // Number of cooperative buckets
    ebf_bucket_entry_t** buckets;      // Array of linked-list bucket heads
    uint32_t hash_count;               // k hash functions
    uint64_t seed_a;
    uint64_t seed_b;
    float omega;                        // Expansion threshold (space-to-bit ratio)
    size_t count;                       // Element count
    PLATFORMLOCKTYPE(lock);
} elastic_bloom_filter_t;
```

### API

| Function | Description |
|----------|-------------|
| `elastic_bloom_filter_create(size, bucket_count, hash_count, omega, fp_bits)` | Create EBF |
| `elastic_bloom_filter_destroy(ebf)` | Destroy all resources |
| `elastic_bloom_filter_add(ebf, data, len)` | Insert element (returns 0 on success) |
| `elastic_bloom_filter_contains(ebf, data, len)` | Query (fast: only checks bits) |
| `elastic_bloom_filter_remove(ebf, data, len)` | Delete element (returns 0 on success) |
| `elastic_bloom_filter_expand(ebf)` | Double size, redistribute fingerprints |
| `elastic_bloom_filter_compress(ebf)` | Halve size, merge bucket pairs |
| `elastic_bloom_filter_merge(dest, src)` | Union two EBFs into dest |
| `elastic_bloom_filter_count(ebf)` | Return element count |
| `elastic_bloom_filter_ratio(ebf)` | Return space-to-bit ratio |

### Fingerprint Derivation

For each of k hash functions, compute a pair (fingerprint, index):

```
h = XXH3_64bits(data, len, seed_i)
index = h % size              // bit position in bloom filter
fp = (h / size) % (1 << EBF_FINGERPRINT_BITS)  // elastic fingerprint
```

Where `seed_i = seed_a + i * seed_b` (derived from the two base seeds).

`EBF_FINGERPRINT_BITS` defaults to 8 (configurable at create time via the `fp_bits` parameter). Fingerprint width determines bucket capacity and false positive rate. Wider fingerprints reduce false deletions but increase memory per bucket entry.

### Insert Algorithm

1. For each i in 0..hash_count-1:
   - Compute (fp, index) pair
   - Set bit at `index` in bloom filter bits
   - Insert `fp` into `buckets[index % bucket_count]`
2. Increment count
3. If `ratio > omega`, trigger expand

### Delete Algorithm

1. For each i in 0..hash_count-1:
   - Compute (fp, index) pair
   - Remove `fp` from `buckets[index % bucket_count]`
   - If bucket is now empty, clear bit at `index` in bloom filter
2. Decrement count
3. If `ratio < omega/4`, trigger compress

### Expand Algorithm

1. Double `size` (bits) and `bucket_count`
2. Reallocate `buckets` array to new bucket_count
3. For each non-empty bucket at old index `i`:
   - For each entry with fingerprint `fp`:
     - If `(fp & 1) == 0`: entry stays in bucket `i`
     - If `(fp & 1) == 1`: entry moves to bucket `i + old_bucket_count`
     - New fingerprint = `fp >> 1`
4. Rebuild bloom filter from non-empty buckets
5. New bitset size = old size * 2

### Compress Algorithm (Inverse of Expand)

1. For each bucket pair (i, i + old_bucket_count):
   - Entries from first bucket: `fp = (fp << 1) | 0`
   - Entries from second bucket: `fp = (fp << 1) | 1`
   - Merge into single bucket
2. Halve bucket_count and size
3. Rebuild bloom filter

### Merge Algorithm (for Attenuated Propagation)

Precondition: both EBFs must have the same `size` and `bucket_count`. If sizes differ, the smaller one must be expanded first.

1. OR the two bloom filter bitsets
2. For each bucket index, merge linked lists (union of fingerprints, dedup)
3. Result: combined set membership (union semantics)

## Layer 3: attenuated_bloom_filter_t

### Struct

```c
typedef struct attenuated_bloom_filter_t {
    refcounter_t refcounter;
    uint32_t level_count;           // k levels (hop radius)
    elastic_bloom_filter_t** levels; // Array of k EBF pointers
    PLATFORMLOCKTYPE(lock);
} attenuated_bloom_filter_t;
```

### API

| Function | Description |
|----------|-------------|
| `attenuated_bloom_filter_create(levels, size, bucket_count, hash_count, omega)` | Create k-level filter |
| `attenuated_bloom_filter_destroy(abf)` | Destroy all levels |
| `attenuated_bloom_filter_subscribe(abf, topic, topic_len)` | Add subscription at level 0 |
| `attenuated_bloom_filter_unsubscribe(abf, topic, topic_len)` | Remove subscription at level 0 |
| `attenuated_bloom_filter_check(abf, topic, topic_len, &hops)` | Find subscription, return hop distance |
| `attenuated_bloom_filter_merge(dest, src)` | Merge remote filter (shift levels by 1) |
| `attenuated_bloom_filter_get_level(abf, level)` | Get EBF at specific level |
| `attenuated_bloom_filter_propagate(abf)` | Get filter for sending to neighbors |

### Level Semantics

- Level 0: Local node's subscriptions
- Level n: Subscriptions of nodes n+1 hops away
- When receiving from neighbor: merge their level[i] into our level[i+1]
- Maximum propagation depth = level_count - 1

### Merge for Propagation

When node A receives B's attenuated filter, merge each of B's levels into A's next level (shifted by 1 hop). Bounds check: only merge where `i+1 < A->level_count`.

```
for i in 0..min(B.level_count, A.level_count - 1):
    elastic_bloom_filter_merge(A->levels[i+1], B->levels[i])
```

## Layer 4: quasar_t

### Struct

```c
typedef struct quasar_subscription_t {
    buffer_t* topic;         // Ref-counted topic (owned by subscription)
    uint32_t ttl;
} quasar_subscription_t;

typedef struct quasar_t {
    refcounter_t refcounter;
    meridian_protocol_t* protocol;           // Parent Meridian (not owned)
    attenuated_bloom_filter_t* routing;      // Routing filter
    vec_t(quasar_subscription_t) local_subs; // Local subscriptions with TTL
    uint32_t max_hops;
    uint32_t alpha;                           // Replication factor
    PLATFORMLOCKTYPE(lock);
} quasar_t;
```

### API

| Function | Description |
|----------|-------------|
| `quasar_create(protocol, max_hops, alpha)` | Create overlay on Meridian |
| `quasar_destroy(quasar)` | Cleanup |
| `quasar_subscribe(quasar, topic, ttl)` | Subscribe to topic |
| `quasar_unsubscribe(quasar, topic)` | Unsubscribe from topic |
| `quasar_publish(quasar, topic, data, len)` | Route publication |
| `quasar_on_gossip(quasar, data, len, from)` | Handle incoming filter sync |
| `quasar_propagate(quasar)` | Send local filter to neighbors |
| `quasar_tick(quasar)` | Decrement TTLs, expire stale entries |

### Publication Routing

1. Check if this node is subscribed at level 0 → deliver locally
2. For each level 1..k-1, check if topic matches in that level's EBF
3. If match found at level n, route toward the corresponding neighbor (via Meridian ring set)
4. If no match, forward to `alpha` random neighbors (parallel random walks)

### Integration with Meridian

- Quasar hooks into `meridian_protocol_t` via the existing callbacks mechanism
- Subscription propagation piggybacks on Meridian gossip intervals
- Publication routing uses Meridian's `find_closest` for directed forwarding
- Quasar packet type is encoded alongside Meridian packets using cbor

## Hash Function Strategy

Use xxHash (already in deps/xxhash):

- `XXH3_64bits_withSeed(data, len, seed)` for 64-bit hashes
- `XXH32_withSeed(data, len, seed)` for 32-bit hashes (when fingerprint width is smaller)
- Double-hashing: generate k hash values from two base seeds

## Thread Safety

All structs use `PLATFORMLOCKTYPE(lock)`:
- bitset_t: lock for mutations (set, update, compact)
- elastic_bloom_filter_t: lock for add/remove/expand/compress/merge
- attenuated_bloom_filter_t: lock for subscribe/unsubscribe/merge
- quasar_t: lock for subscribe/unsubscribe/publish/propagate

bloom_filter_t does NOT have its own lock (protected by caller or EBF's lock).

## Testing Strategy

Each layer gets unit tests with Google Test:

1. **bitset_test**: create/destroy, get/set at boundaries, auto-grow, bitwise ops, comparison, concurrent access
2. **bloom_filter_test**: add/contains, false positive rate, optimal size calc, reset, count
3. **elastic_bloom_filter_test**: add/contains/remove, expand trigger, compress trigger, merge correctness, bucket integrity, concurrent modifications
4. **attenuated_bloom_filter_test**: level creation, multi-level propagation, merge shift, subscribe/unsubscribe, check with hop distance
5. **quasar_test**: subscribe/unsubscribe round-trip, publish routing, gossip propagation, TTL expiration, Meridian integration

## Build Integration

Add to CMakeLists.txt:
- Source files in `src/Bloom/` and `src/Network/Quasar/`
- Test executables: `bitset_test`, `bloom_filter_test`, `elastic_bloom_filter_test`, `attenuated_bloom_filter_test`, `quasar_test`