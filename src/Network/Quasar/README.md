# Quasar Pub/Sub Overlay

Quasar is a content-based publish-subscribe overlay network built on top of the Meridian P2P network. It enables nodes to subscribe to named topics and route messages toward subscribers without centralized brokers.

## Architecture

Quasar uses **attenuated Bloom filters** for probabilistic routing. Each node maintains a multi-level summary of what topics exist at what distance (in hops) through the network. When a node publishes to a topic, it checks its routing filter to determine the direction toward subscribers — either delivering locally, routing toward a known subscriber, or falling back to a random walk.

```
┌──────────────────────────────────────────────────────────┐
│                       quasar_t                            │
│  ┌────────────────────┐  ┌─────────────────────────────┐ │
│  │  protocol           │  │  routing                     │ │
│  │  (meridian_protocol) │  │  (attenuated_bloom_filter)  │ │
│  └────────────────────┘  └─────────────────────────────┘ │
│  ┌────────────────────┐  ┌─────────────────────────────┐ │
│  │  local_subs         │  │  max_hops / alpha            │ │
│  │  (subscription vec) │  │  (routing parameters)       │ │
│  └────────────────────┘  └─────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

## File Overview

| File | Purpose |
|------|---------|
| `quasar.h` | Public API: types, structs, and function declarations for the Quasar overlay |
| `quasar.c` | Implementation: subscription management, publish routing, gossip propagation, TTL expiry |

## Data Structures

### quasar_subscription_t

Represents a single local subscription to a topic.

```c
typedef struct quasar_subscription_t {
    buffer_t* topic;    // Topic name as a byte buffer (supports arbitrary topic identifiers)
    uint32_t ttl;       // Time-to-live in ticks; when it reaches 0, the subscription auto-expires
} quasar_subscription_t;
```

### quasar_t

Main Quasar instance. Ties together the Meridian protocol, the routing filter, and local subscriptions.

```c
typedef struct quasar_t {
    refcounter_t refcounter;                      // Lifetime management
    struct meridian_protocol_t* protocol;          // Underlying Meridian P2P network
    attenuated_bloom_filter_t* routing;            // Multi-level routing filter
    vec_t(quasar_subscription_t) local_subs;       // Locally active subscriptions
    uint32_t max_hops;                             // Maximum routing hops (determines filter depth)
    uint32_t alpha;                                // Fan-out for random walk when no route known
    PLATFORMLOCKTYPE(lock);                        // Thread-safe access
} quasar_t;
```

## Algorithms

### Attenuated Bloom Filter Routing

The routing filter is an **attenuated Bloom filter** — a stack of `max_hops` elastic Bloom filters, where each level represents a different distance from the local node:

- **Level 0** — Topics subscribed to by this node (local subscriptions)
- **Level 1** — Topics subscribed by direct neighbors (1 hop away)
- **Level 2** — Topics subscribed by neighbors-of-neighbors (2 hops away)
- **Level N** — Topics reachable in N hops

When a node subscribes to a topic, it adds the topic to level 0 of its routing filter. When neighbors exchange routing information (via `quasar_propagate`), each node merges its neighbor's filter into its own — shifted down by one level. This means topics from a neighbor's level 0 appear in our level 1, topics from a neighbor's level 1 appear in our level 2, and so on.

This shift-on-merge creates a distance-gradient: the level at which a topic appears tells us how many hops away a subscriber is, and the direction from which the merge arrived tells us which neighbor leads toward that subscriber.

### Publish Routing

When a node publishes a message to a topic, `quasar_publish` checks the routing filter:

1. **Local delivery (hops == 0):** The topic is in level 0 — this node is subscribed. Deliver locally.
2. **Directed routing (hops > 0):** The topic appears at some level N — a subscriber is N hops away. Forward the message toward the neighbor that contributed the matching filter level.
3. **Random walk (not found):** The topic doesn't appear in any level. No route to a subscriber is known. Forward the message to `alpha` random neighbors (fan-out). This random walk ensures messages eventually reach subscribers even when the routing filter hasn't converged yet.

The `alpha` parameter controls the fan-out of random walks. Higher values increase the probability of reaching subscribers quickly but consume more bandwidth.

### Elastic Bloom Filters

Each level of the attenuated filter uses an **elastic Bloom filter** — a variant that supports dynamic resizing. Unlike standard Bloom filters with fixed bit arrays, elastic Bloom filters maintain:

- A **bitset** for fast negative checks (same as standard Bloom filter)
- **Cooperative buckets** — one per bit position — that store short fingerprints of inserted items

This allows the filter to support **deletion** (via fingerprint matching) and **dynamic resizing** (expansion and compression), which standard Bloom filters cannot do.

#### Fingerprint-based Deletion

When removing an item, the filter recomputes its hash positions, looks up the fingerprint in each corresponding bucket, and removes the matching entry. If a bucket becomes empty, the corresponding bit in the bitset is cleared. This prevents false negatives that would occur if we simply cleared bits (which could belong to other items).

#### Dynamic Resizing

- **Expansion:** When the fill ratio exceeds `omega`, the filter doubles in size. Fingerprints are split by their lowest bit — even-bit entries stay in the original bucket, odd-bit entries move to the corresponding bucket in the new upper half. Fingerprints are right-shifted by one bit to preserve the remaining bits.
- **Compression:** When the fill ratio drops below `omega / 4`, the filter halves in size. Fingerprints from paired buckets are merged — the lowest bit is prepended (0 for the lower bucket, 1 for the upper) and fingerprints are left-shifted by one bit.

This is analogous to how a dynamic array grows and shrinks, but applied to a probabilistic data structure.

### Attenuated Merge

When two nodes exchange routing information, the receiver merges the sender's attenuated filter into its own. The merge shifts the sender's levels by one (because the sender is 1 hop away from the receiver's perspective) and unions them into the receiver's levels:

```
receiver.level[i+1] = union(receiver.level[i+1], sender.level[i])
```

Level 0 is never overwritten by a merge — it always reflects the local node's own subscriptions. The merge stops before the last level of the receiver, since sender information shifted beyond the receiver's depth provides no routing value.

### TTL Expiry

Subscriptions have a time-to-live measured in ticks. The `quasar_tick` function decrements each subscription's TTL and removes expired ones from both the local subscription list and the routing filter (level 0). This prevents stale subscriptions from persisting indefinitely and ensures the routing filter converges toward currently active subscriptions.

## Interaction with Meridian

Quasar depends on Meridian for all transport and peer discovery:

| Quasar Operation | Meridian Dependency |
|---|---|
| Send message to subscriber | `meridian_protocol_send_packet` via QUIC |
| Broadcast to neighbors | `meridian_protocol_broadcast` |
| Receive routed messages | `meridian_protocol_on_packet` → `quasar_on_gossip` |
| Exchange routing filters | `meridian_protocol_gossip` → `quasar_propagate` |
| Find random neighbor for walk | `meridian_ring_set` peer selection |
| Periodic maintenance | `meridian_protocol_ring_management` → `quasar_tick` |

## Usage Example

```c
// Create Quasar overlay on top of a running Meridian protocol
quasar_t* quasar = quasar_create(protocol, 5, 3);

// Subscribe to a topic
quasar_subscribe(quasar, (uint8_t*)"sensors/temp", 12, 100);

// Publish a message
quasar_publish(quasar, (uint8_t*)"sensors/temp", 12, payload, payload_len);

// Periodically: propagate routing filters and expire TTLs
quasar_propagate(quasar);
quasar_tick(quasar);

// Unsubscribe
quasar_unsubscribe(quasar, (uint8_t*)"sensors/temp", 12);

// Cleanup
quasar_destroy(quasar);
```

## See Also

- `src/Bloom/attenuated_bloom_filter.h` — Attenuated Bloom filter implementation
- `src/Bloom/elastic_bloom_filter.h` — Elastic Bloom filter with deletion and resizing
- `src/Bloom/bloom_filter.h` — Standard Bloom filter (base layer)
- `src/Network/Meridian/` — Underlying P2P network protocol