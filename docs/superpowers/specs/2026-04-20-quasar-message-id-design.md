# Quasar Message ID and Dedup Filter Design

## Context

Quasar route messages have no unique identifier, making it impossible to detect duplicate messages arriving via different paths (e.g., one via directed routing, one via random walk). This design adds a `quasar_message_id_t` type following WaveDB's `transaction_id_t` pattern, and a per-node dedup bloom filter.

## Message ID Type

A 24-byte compound identifier providing total ordering and uniqueness:

```c
typedef struct quasar_message_id_t {
    uint64_t time;    // Seconds from CLOCK_MONOTONIC
    uint64_t nanos;   // Nanoseconds within second
    uint64_t count;   // Atomic sequence counter for same-timestamp uniqueness
} quasar_message_id_t;
```

### Generation

`quasar_message_id_get_next()` reads `clock_gettime(CLOCK_MONOTONIC)` and increments a file-scope `_Atomic uint64_t` counter. Initialized once via `pthread_once` / `InitOnceExecuteOnce` in `quasar_message_id_init()`. Thread-safe, no lock needed.

### Comparison

`quasar_message_id_compare(a, b)` — lexicographic ordering on (time, nanos, count). Returns -1, 0, or 1. Enables total ordering across all messages from all nodes.

### Serialization

`quasar_message_id_serialize(id, buf)` — writes 24 bytes in network byte order (big-endian) for wire transmission.

`quasar_message_id_deserialize(id, buf)` — reads 24 bytes from network byte order, converts to host byte order.

### Endianness and the Dedup Bloom Filter

The dedup bloom filter is **purely local** — it is never transmitted between nodes. Each node only queries its own filter. After deserialization, both little-endian and big-endian machines hold the same `uint64_t` field values. When hashing into the local bloom filter, the raw struct bytes `(uint8_t*)&id, sizeof(quasar_message_id_t)` are used directly. Since a node only ever checks its own filter, endianness is irrelevant — the filter is always self-consistent.

Serialization to network byte order is only needed for the wire format of route messages, not for bloom filter keys.

## Integration with quasar_route_message_t

The route message struct gains an ID field:

```c
typedef struct quasar_route_message_t {
    refcounter_t refcounter;
    quasar_message_id_t id;           // Unique message identifier
    buffer_t* topic;
    buffer_t* data;
    bloom_filter_t* visited;          // Negative filter for random walk dedup
    uint32_t hops_remaining;
    PLATFORMLOCKTYPE(lock);
} quasar_route_message_t;
```

## Per-Node Dedup Filter

`quasar_t` gains a dedup bloom filter:

```c
typedef struct quasar_t {
    // ... existing fields ...
    bloom_filter_t* seen;             // Per-node dedup: message IDs already processed
    PLATFORMLOCKTYPE(lock);
} quasar_t;
```

### Dedup Flow

In `quasar_on_route_message()`:
1. Check `bloom_filter_contains(quasar->seen, &msg->id, sizeof(msg->id))` — if true, discard as duplicate
2. If not seen, `bloom_filter_add(quasar->seen, &msg->id, sizeof(msg->id))` — mark as seen
3. Process normally (local delivery or forwarding)

In `quasar_publish()` for local delivery:
1. Add the message ID to `seen` to prevent re-delivery if the same message arrives from another path

### Filter Saturation

The dedup bloom filter is a standard bloom filter with configurable size (default: 4096 bits, 3 hashes). As it saturates, false positives increase, causing some non-duplicate messages to be discarded. This is acceptable — it errs on the side of skipping duplicates. The filter can be periodically reset in `quasar_tick()` if a saturation threshold is exceeded.

## New Files

- `src/Network/Quasar/quasar_message_id.h` — type definition, API declarations
- `src/Network/Quasar/quasar_message_id.c` — implementation

## API

### quasar_message_id.h

| Function | Purpose |
|----------|---------|
| `quasar_message_id_init()` | One-time global generator init (call at startup) |
| `quasar_message_id_get_next()` | Generate unique ID (thread-safe, no lock) |
| `quasar_message_id_compare(a, b)` | Lexicographic comparison (-1, 0, 1) |
| `quasar_message_id_serialize(id, buf)` | 24-byte network byte order |
| `quasar_message_id_deserialize(id, buf)` | From network byte order |

### Changes to quasar.h

- Add `#include "quasar_message_id.h"`
- Add `quasar_message_id_t id` to `quasar_route_message_t`
- Add `bloom_filter_t* seen` to `quasar_t`
- Add `bloom_filter_t* seen` size/hash params to `quasar_create()` signature: `quasar_create(protocol, max_hops, alpha, seen_size, seen_hashes)`
- `quasar_route_message_create()` always auto-generates the ID via `quasar_message_id_get_next()`. The ID is not a parameter — callers don't supply it. When a route message is received from the network, `quasar_on_route_message()` receives the deserialized ID as part of the message.

### Changes to quasar.c

- `quasar_create()`: initialize dedup bloom filter
- `quasar_destroy()`: free dedup bloom filter
- `quasar_publish()`: generate ID via `quasar_message_id_get_next()`, add to `seen` for local delivery
- `quasar_on_route_message()`: check dedup filter, add ID if not seen, process or discard

### Changes to quasar_test.cpp

- Message ID generation: IDs are unique across calls
- Message ID comparison: ordering is correct
- Serialization round-trip: serialize then deserialize preserves values
- Dedup filter: duplicate messages are discarded, non-duplicates are processed
- Route message with ID: create/destroy carries the ID correctly

## Wire Format (Route Message)

The route message wire format includes the message ID as the first 24 bytes:

```
message_id (24 bytes, network byte order)
topic_len (uint32_t)
topic (topic_len bytes)
data_len (uint32_t)
data (data_len bytes)
visited_filter_size (uint32_t)
visited_filter_hash_count (uint32_t)
visited_filter_bits (visited_filter_size / 8 bytes, rounded up)
hops_remaining (uint32_t)
```