//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_QUASAR_H
#define POSEIDON_QUASAR_H

#include "../Meridian/meridian.h"
#include "../../Bloom/attenuated_bloom_filter.h"
#include "../../Bloom/bloom_filter.h"
#include "quasar_message_id.h"
#include "../../Buffer/buffer.h"
#include "../../Util/vec.h"
#include "../../RefCounter/refcounter.h"
#include "../../Util/threadding.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// SUBSCRIPTION
// ============================================================================

/**
 * A single local subscription to a topic.
 * Tracks the topic identifier and a time-to-live for automatic expiry.
 *
 * When TTL reaches 0, quasar_tick() removes the subscription and
 * updates the routing filter (level 0) accordingly.
 */
typedef struct quasar_subscription_t {
    buffer_t* topic;    /**< Topic name as a byte buffer (arbitrary identifier) */
    uint32_t ttl;       /**< Time-to-live in ticks; 0 triggers auto-expiry */
} quasar_subscription_t;

// ============================================================================
// ROUTE MESSAGE (carries negative bloom filter for random walk dedup)
// ============================================================================

/**
 * A message being routed through the Quasar overlay.
 * Carries a negative bloom filter that records which nodes have already
 * been visited during a random walk, preventing routing loops and
 * redundant delivery per the Quasar paper.
 *
 * Lifecycle:
 * 1. Created by quasar_publish() or received via quasar_on_route_message()
 * 2. Each hop adds itself to the visited filter via quasar_route_message_add_visited()
 * 3. When forwarding via random walk, nodes in the filter are skipped
 * 4. Destroyed when delivery completes or hops_remaining reaches 0
 */
typedef struct quasar_route_message_t {
    refcounter_t refcounter;           /**< Reference counting for lifetime */
    quasar_message_id_t id;           /**< Unique message identifier */
    buffer_t* topic;                   /**< Topic identifier */
    buffer_t* data;                    /**< Message payload */
    bloom_filter_t* visited;           /**< Negative filter: nodes already visited in this walk */
    uint32_t hops_remaining;           /**< TTL for this random walk (decremented each hop) */
    PLATFORMLOCKTYPE(lock);            /**< Thread-safe access */
} quasar_route_message_t;

/**
 * Creates a new route message for network routing.
 *
 * @param topic      Topic identifier bytes
 * @param topic_len  Length of topic identifier
 * @param data       Message payload bytes
 * @param data_len   Length of message payload
 * @param max_hops   Maximum hops this message may take (random walk TTL)
 * @param neg_size   Size in bits for the negative (visited) bloom filter
 * @param neg_hashes Number of hash functions for the negative filter
 * @return           New route message with refcount=1, or NULL on failure
 */
quasar_route_message_t* quasar_route_message_create(const uint8_t* topic, size_t topic_len,
                                                      const uint8_t* data, size_t data_len,
                                                      uint32_t max_hops,
                                                      size_t neg_size, uint32_t neg_hashes);

/**
 * Destroys a route message and frees the topic, payload, and visited filter.
 *
 * @param msg  Route message to destroy
 */
void quasar_route_message_destroy(quasar_route_message_t* msg);

/**
 * Records a node as visited in the negative bloom filter.
 * Subsequent calls to quasar_route_message_has_visited() for this node
 * will return true.
 *
 * @param msg   Route message
 * @param node  Node that has been visited (addr+port used as key)
 * @return      0 on success, -1 on failure
 */
int quasar_route_message_add_visited(quasar_route_message_t* msg, const meridian_node_t* node);

/**
 * Checks whether a node has been visited during this random walk.
 * Uses the negative bloom filter, so false positives are possible
 * (a node may appear visited even if it wasn't). This is by design:
 * it errson the side of skipping nodes rather than revisiting them.
 *
 * @param msg   Route message
 * @param node  Node to check
 * @return      true if the node is likely visited, false if definitely not visited
 */
bool quasar_route_message_has_visited(quasar_route_message_t* msg, const meridian_node_t* node);

// ============================================================================
// LOCAL DELIVERY CALLBACK
// ============================================================================

/**
 * Callback invoked when a published message is delivered to a local subscriber.
 *
 * @param ctx       User context
 * @param topic     Topic identifier bytes
 * @param topic_len Length of topic
 * @param data      Message payload bytes
 * @param data_len  Length of payload
 */
typedef void (*quasar_delivery_cb_t)(void* ctx, const uint8_t* topic, size_t topic_len,
                                      const uint8_t* data, size_t data_len);

// ============================================================================
// QUASAR INSTANCE
// ============================================================================

/**
 * Main Quasar pub/sub overlay instance.
 * Layers content-based routing on top of a Meridian P2P network using
 * attenuated Bloom filters for probabilistic topic-based routing.
 *
 * Routing strategy:
 * - Level 0 of the routing filter holds topics subscribed to locally
 * - Higher levels hold topics reachable through neighbors at increasing hop distances
 * - Publishing checks the filter: local delivery (level 0), directed routing (level N),
 *   or random walk (not found) with alpha fan-out
 *
 * Lifecycle:
 * 1. Create via quasar_create() with a running Meridian protocol
 * 2. Subscribe to topics via quasar_subscribe()
 * 3. Publish messages via quasar_publish()
 * 4. Periodically call quasar_propagate() and quasar_tick()
 * 5. Destroy via quasar_destroy()
 */
typedef struct quasar_t {
    refcounter_t refcounter;                      /**< Reference counting for lifetime */
    struct meridian_protocol_t* protocol;          /**< Underlying Meridian P2P network */
    attenuated_bloom_filter_t* routing;            /**< Multi-level routing filter for topic-based routing */
    vec_t(quasar_subscription_t) local_subs;       /**< Locally active subscriptions with TTL */
    uint32_t max_hops;                             /**< Maximum routing hops (determines filter depth) */
    uint32_t alpha;                                /**< Fan-out degree for random walk when no route known */
    bloom_filter_t* seen;                    /**< Per-node dedup: message IDs already processed */
    uint32_t seen_size;                      /**< Dedup filter size in bits */
    uint32_t seen_hashes;                    /**< Dedup filter hash count */
    quasar_delivery_cb_t on_delivery;              /**< Called when a message is delivered to a local subscriber */
    void* delivery_ctx;                            /**< User context for delivery callback */
    PLATFORMLOCKTYPE(lock);                        /**< Thread-safe access to subscriptions and filter */
} quasar_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

/**
 * Creates a new Quasar overlay on top of a Meridian protocol.
 * Initializes the routing filter with max_hops levels, each using an elastic Bloom filter.
 *
 * @param protocol   Running Meridian protocol instance
 * @param max_hops   Maximum routing distance (number of filter levels)
 * @param alpha      Fan-out for random walk routing (number of random neighbors)
 * @param seen_size   Size in bits for the dedup bloom filter
 * @param seen_hashes Number of hash functions for the dedup bloom filter
 * @return           New Quasar instance with refcount=1, or NULL on failure
 */
quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha,
                          uint32_t seen_size, uint32_t seen_hashes);

/**
 * Destroys a Quasar instance and frees all subscriptions and the routing filter.
 *
 * @param quasar  Quasar instance to destroy
 */
void quasar_destroy(quasar_t* quasar);

/**
 * Sets the callback invoked when a message is delivered to a local subscriber.
 *
 * @param quasar  Quasar instance
 * @param cb      Delivery callback (may be NULL to unset)
 * @param ctx     User context passed to the callback
 */
void quasar_set_delivery_callback(quasar_t* quasar, quasar_delivery_cb_t cb, void* ctx);

// ============================================================================
// SUBSCRIPTION MANAGEMENT
// ============================================================================

/**
 * Subscribes to a topic. Adds the topic to level 0 of the routing filter
 * and records it in the local subscription list with the given TTL.
 *
 * @param quasar     Quasar instance
 * @param topic      Topic identifier bytes
 * @param topic_len  Length of topic identifier
 * @param ttl        Time-to-live in ticks (0 = never expires until unsubscribed)
 * @return           0 on success, -1 on failure
 */
int quasar_subscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl);

/**
 * Unsubscribes from a topic. Removes it from the routing filter and local list.
 *
 * @param quasar     Quasar instance
 * @param topic      Topic identifier bytes
 * @param topic_len  Length of topic identifier
 * @return           0 on success, -1 on failure
 */
int quasar_unsubscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len);

// ============================================================================
// PUBLISHING
// ============================================================================

/**
 * Publishes a message to a topic. Routes based on the routing filter:
 * - hops == 0: local delivery (this node is subscribed)
 * - hops > 0:  directed routing toward the subscriber via Meridian
 * - not found: random walk to alpha random neighbors
 *
 * @param quasar     Quasar instance
 * @param topic      Topic identifier bytes
 * @param topic_len  Length of topic identifier
 * @param data       Message payload bytes
 * @param data_len   Length of message payload
 * @return           0 on success, -1 on failure
 */
int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len, const uint8_t* data, size_t data_len);

// ============================================================================
// GOSSIP AND PROPAGATION
// ============================================================================

/**
 * Handles incoming gossip data from a peer.
 * Called when the Meridian protocol delivers a gossip message that
 * contains routing filter updates from a neighbor.
 *
 * Deserializes the incoming attenuated bloom filter and merges it into
 * the local routing filter (shifted by one level).
 *
 * @param quasar  Quasar instance
 * @param data    Raw gossip data (serialized attenuated bloom filter)
 * @param len     Length of data
 * @param from    Node that sent the gossip
 * @return        0 on success, -1 on failure
 */
int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from);

/**
 * Handles an incoming routed message from a peer.
 * Adds this node to the message's negative filter, checks the routing
 * table for the topic, and either delivers locally, forwards toward
 * a subscriber, or continues the random walk.
 *
 * @param quasar  Quasar instance
 * @param msg     Route message received from the network
 * @param from    Node that forwarded the message
 * @return        0 on success, -1 on failure
 */
int quasar_on_route_message(quasar_t* quasar, quasar_route_message_t* msg, const struct meridian_node_t* from);

/**
 * Propagates this node's routing filter to neighbors via Meridian gossip.
 * Serializes the local attenuated bloom filter and broadcasts it to
 * all connected peers. Neighbors merge the filter into their own
 * (shifted by one level) to build their distance-gradient view.
 *
 * @param quasar  Quasar instance
 * @return        0 on success, -1 on failure
 */
int quasar_propagate(quasar_t* quasar);

// ============================================================================
// PERIODIC MAINTENANCE
// ============================================================================

/**
 * Advances the subscription TTL clock by one tick.
 * Decrements TTL for all local subscriptions and removes expired ones
 * from both the subscription list and the routing filter (level 0).
 *
 * @param quasar  Quasar instance
 * @return        0 on success, -1 on failure
 */
int quasar_tick(quasar_t* quasar);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_QUASAR_H