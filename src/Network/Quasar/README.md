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

### quasar_route_message_t

A message being routed through the Quasar overlay. Carries a **negative bloom filter** that records which nodes have already been visited during a random walk, preventing routing loops and redundant delivery per the Quasar paper.

```c
typedef struct quasar_route_message_t {
    refcounter_t refcounter;           // Lifetime management
    buffer_t* topic;                   // Topic identifier
    buffer_t* data;                    // Message payload
    bloom_filter_t* visited;           // Negative filter: nodes already visited in this walk
    uint32_t hops_remaining;           // TTL for this random walk (decremented each hop)
    PLATFORMLOCKTYPE(lock);            // Thread-safe access
} quasar_route_message_t;
```

### quasar_t

Main Quasar instance. Ties together the Meridian protocol, the routing filter, local subscriptions, and the message callback.

```c
typedef struct quasar_t {
    refcounter_t refcounter;                      // Lifetime management
    struct meridian_protocol_t* protocol;          // Underlying Meridian P2P network
    attenuated_bloom_filter_t* routing;            // Multi-level routing filter
    vec_t(quasar_subscription_t) local_subs;       // Locally active subscriptions
    uint32_t max_hops;                             // Maximum routing hops (determines filter depth)
    uint32_t alpha;                                // Fan-out for random walk when no route known
    quasar_message_cb_t on_message;              // Called when a message is delivered locally
    void* message_ctx;                            // User context for message callback
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

1. **Local message (hops == 0):** The topic is in level 0 — this node is subscribed. Deliver locally via the `on_message` callback.
2. **Directed routing (hops > 0):** The topic appears at some level N — a subscriber is N hops away. Forward the message toward the neighbor that contributed the matching filter level.
3. **Random walk (not found):** The topic doesn't appear in any level. No route to a subscriber is known. Create a `quasar_route_message_t` with a **negative bloom filter** initialized with the local node, then forward to `alpha` random neighbors that are not in the negative filter.

The `alpha` parameter controls the fan-out of random walks. Higher values increase the probability of reaching subscribers quickly but consume more bandwidth.

### Negative Bloom Filter (Random Walk Dedup)

Each routed message carries a **negative bloom filter** (`visited` field in `quasar_route_message_t`). When a node receives a message via random walk:

1. The node adds itself to the negative filter (`quasar_route_message_add_visited`)
2. When selecting random neighbors for forwarding, nodes already in the negative filter are skipped
3. This prevents the message from being routed to the same peer repeatedly, avoiding routing loops

The negative filter is probabilistic: it may produce false positives (a node may appear visited even if it wasn't), which causes some neighbors to be unnecessarily skipped. This is by design — it errs on the side of skipping rather than revisiting, reducing bandwidth waste.

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

// Set a callback for local message
quasar_set_message_callback(quasar, my_message_handler, my_ctx);

// Subscribe to a topic
quasar_subscribe(quasar, (uint8_t*)"sensors/temp", 12, 100);

// Publish a message
quasar_publish(quasar, (uint8_t*)"sensors/temp", 12, payload, payload_len);

// Periodically: propagate routing filters and expire TTLs
quasar_propagate(quasar);
quasar_tick(quasar);

// Handle an incoming routed message (from Meridian on_packet callback)
quasar_route_message_t* msg = quasar_route_message_create(
    topic, topic_len, data, data_len, max_hops, 2048, 3
);
quasar_on_route_message(quasar, msg, from_node);
quasar_route_message_destroy(msg);

// Handle incoming gossip (from Meridian gossip callback)
quasar_on_gossip(quasar, gossip_data, gossip_len, from_node);

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