//
// Created by victor on 4/19/26.
//

#include "quasar.h"
#include "quasar_route.h"
#include "../../Bloom/elastic_bloom_filter.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ============================================================================
// DEFAULT PARAMETERS
// ============================================================================

/** Number of levels in the attenuated Bloom filter (max routing hops) */
#define QUASAR_DEFAULT_LEVELS 5

/** Number of bits per elastic Bloom filter level */
#define QUASAR_DEFAULT_SIZE 1024

/** Number of hash functions per Bloom filter level */
#define QUASAR_DEFAULT_HASH_COUNT 3

/** Fill ratio threshold that triggers elastic Bloom filter expansion */
#define QUASAR_DEFAULT_OMEGA 0.75f

/** Fingerprint bit width for elastic Bloom filter bucket entries */
#define QUASAR_DEFAULT_FP_BITS EBF_DEFAULT_FP_BITS

/** Default size in bits for the negative (visited) bloom filter */
#define QUASAR_DEFAULT_NEGATIVE_SIZE 2048

/** Default number of hash functions for the negative bloom filter */
#define QUASAR_DEFAULT_NEGATIVE_HASHES 3

/** Default size in bits for the dedup (seen) bloom filter */
#define QUASAR_DEFAULT_SEEN_SIZE 4096

/** Default number of hash functions for the dedup bloom filter */
#define QUASAR_DEFAULT_SEEN_HASHES 3

// ============================================================================
// ROUTE MESSAGE LIFECYCLE
// ============================================================================

quasar_route_message_t* quasar_route_message_create(const uint8_t* topic, size_t topic_len,
                                                      const uint8_t* data, size_t data_len,
                                                      uint32_t max_hops,
                                                      size_t neg_size, uint32_t neg_hashes) {
    if (topic == NULL || data == NULL) return NULL;
    quasar_message_id_init();
    quasar_route_message_t* msg = get_clear_memory(sizeof(quasar_route_message_t));
    msg->id = quasar_message_id_get_next();
    msg->topic = buffer_create_from_pointer_copy((uint8_t*)topic, topic_len);
    msg->data = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    msg->visited = bloom_filter_create(neg_size, neg_hashes);
    msg->hops_remaining = max_hops;
    msg->pub_addrs = NULL;
    msg->pub_ports = NULL;
    msg->pub_count = 0;
    msg->pub_capacity = 0;
    platform_lock_init(&msg->lock);
    refcounter_init((refcounter_t*)msg);
    return msg;
}

void quasar_route_message_destroy(quasar_route_message_t* msg) {
    if (msg == NULL) return;
    refcounter_dereference((refcounter_t*)msg);
    if (refcounter_count((refcounter_t*)msg) == 0) {
        buffer_destroy(msg->topic);
        buffer_destroy(msg->data);
        bloom_filter_destroy(msg->visited);
        free(msg->pub_addrs);
        free(msg->pub_ports);
        platform_lock_destroy(&msg->lock);
        free(msg);
    }
}

int quasar_route_message_add_visited(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return -1;
    // Hash the node's address and port as the bloom filter key
    // This identifies a node uniquely within the network
    uint8_t node_key[6];
    memcpy(node_key, &node->addr, sizeof(node->addr));
    memcpy(node_key + sizeof(node->addr), &node->port, sizeof(node->port));
    platform_lock(&msg->lock);
    int result = bloom_filter_add(msg->visited, node_key, sizeof(node_key));
    platform_unlock(&msg->lock);
    return result;
}

bool quasar_route_message_has_visited(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return false;
    uint8_t node_key[6];
    memcpy(node_key, &node->addr, sizeof(node->addr));
    memcpy(node_key + sizeof(node->addr), &node->port, sizeof(node->port));
    platform_lock(&msg->lock);
    bool result = bloom_filter_contains(msg->visited, node_key, sizeof(node_key));
    platform_unlock(&msg->lock);
    return result;
}

int quasar_route_message_add_publisher(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return -1;
    platform_lock(&msg->lock);
    if (msg->pub_count >= msg->pub_capacity) {
        uint32_t new_cap = msg->pub_capacity == 0 ? 4 : msg->pub_capacity * 2;
        uint32_t* new_addrs = realloc(msg->pub_addrs, sizeof(uint32_t) * new_cap);
        uint16_t* new_ports = realloc(msg->pub_ports, sizeof(uint16_t) * new_cap);
        if (new_addrs == NULL || new_ports == NULL) {
            free(new_addrs);
            free(new_ports);
            platform_unlock(&msg->lock);
            return -1;
        }
        msg->pub_addrs = new_addrs;
        msg->pub_ports = new_ports;
        msg->pub_capacity = new_cap;
    }
    msg->pub_addrs[msg->pub_count] = node->addr;
    msg->pub_ports[msg->pub_count] = node->port;
    msg->pub_count++;
    platform_unlock(&msg->lock);
    return 0;
}

bool quasar_route_message_has_publisher(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return false;
    platform_lock(&msg->lock);
    for (uint32_t i = 0; i < msg->pub_count; i++) {
        if (msg->pub_addrs[i] == node->addr && msg->pub_ports[i] == node->port) {
            platform_unlock(&msg->lock);
            return true;
        }
    }
    platform_unlock(&msg->lock);
    return false;
}

// ============================================================================
// QUASAR LIFECYCLE
// ============================================================================

quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha,
                          uint32_t seen_size, uint32_t seen_hashes) {
    quasar_t* quasar = get_clear_memory(sizeof(quasar_t));
    quasar->protocol = protocol;

    // Build the attenuated Bloom filter: max_hops levels, each an elastic filter
    // Level 0 = local subscriptions, level N = topics N hops away
    quasar->routing = attenuated_bloom_filter_create(
        max_hops,
        QUASAR_DEFAULT_SIZE,
        QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA,
        QUASAR_DEFAULT_FP_BITS
    );
    vec_init(&quasar->local_subs);
    vec_init(&quasar->neighbor_filters);
    quasar->max_hops = max_hops;
    quasar->alpha = alpha;
    quasar->seen = bloom_filter_create(seen_size, seen_hashes);
    quasar->seen_size = seen_size;
    quasar->seen_hashes = seen_hashes;
    quasar->on_delivery = NULL;
    quasar->delivery_ctx = NULL;
    platform_lock_init(&quasar->lock);
    refcounter_init((refcounter_t*)quasar);
    return quasar;
}

void quasar_destroy(quasar_t* quasar) {
    if (quasar == NULL) return;
    refcounter_dereference((refcounter_t*)quasar);
    if (refcounter_count((refcounter_t*)quasar) == 0) {
        attenuated_bloom_filter_destroy(quasar->routing);
        bloom_filter_destroy(quasar->seen);

        // Free all local subscription topic buffers
        for (int i = 0; i < quasar->local_subs.length; i++) {
            if (quasar->local_subs.data[i].topic != NULL) {
                buffer_destroy(quasar->local_subs.data[i].topic);
            }
        }
        vec_deinit(&quasar->local_subs);
        for (int i = 0; i < quasar->neighbor_filters.length; i++) {
            attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
        }
        vec_deinit(&quasar->neighbor_filters);
        platform_lock_destroy(&quasar->lock);
        free(quasar);
    }
}

void quasar_set_delivery_callback(quasar_t* quasar, quasar_delivery_cb_t cb, void* ctx) {
    if (quasar == NULL) return;
    platform_lock(&quasar->lock);
    quasar->on_delivery = cb;
    quasar->delivery_ctx = ctx;
    platform_unlock(&quasar->lock);
}

attenuated_bloom_filter_t* quasar_get_neighbor_filter(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;
    for (int i = 0; i < quasar->neighbor_filters.length; i++) {
        if (quasar->neighbor_filters.data[i].addr == node->addr &&
            quasar->neighbor_filters.data[i].port == node->port) {
            return quasar->neighbor_filters.data[i].filter;
        }
    }
    return NULL;
}

attenuated_bloom_filter_t* quasar_get_or_create_neighbor_filter(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;
    platform_lock(&quasar->lock);
    attenuated_bloom_filter_t* existing = quasar_get_neighbor_filter(quasar, node);
    if (existing != NULL) {
        platform_unlock(&quasar->lock);
        return existing;
    }
    quasar_neighbor_filter_t nf;
    nf.addr = node->addr;
    nf.port = node->port;
    nf.filter = attenuated_bloom_filter_create(
        quasar->max_hops,
        QUASAR_DEFAULT_SIZE,
        QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA,
        QUASAR_DEFAULT_FP_BITS
    );
    if (nf.filter == NULL) {
        platform_unlock(&quasar->lock);
        return NULL;
    }
    vec_push(&quasar->neighbor_filters, nf);
    platform_unlock(&quasar->lock);
    return nf.filter;
}

int quasar_remove_neighbor_filter(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return -1;
    platform_lock(&quasar->lock);
    for (int i = 0; i < quasar->neighbor_filters.length; i++) {
        if (quasar->neighbor_filters.data[i].addr == node->addr &&
            quasar->neighbor_filters.data[i].port == node->port) {
            attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
            vec_splice(&quasar->neighbor_filters, i, 1);
            platform_unlock(&quasar->lock);
            return 0;
        }
    }
    platform_unlock(&quasar->lock);
    return -1;
}

// ============================================================================
// SUBSCRIPTION MANAGEMENT
// ============================================================================

int quasar_subscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl) {
    if (quasar == NULL || topic == NULL) return -1;
    platform_lock(&quasar->lock);

    // Add topic to level 0 of the routing filter (local subscription)
    int result = attenuated_bloom_filter_subscribe(quasar->routing, topic, topic_len);
    if (result == 0) {
        // Record the subscription locally with its TTL
        quasar_subscription_t sub;
        sub.topic = buffer_create_from_pointer_copy((uint8_t*)topic, topic_len);
        sub.ttl = ttl;
        vec_push(&quasar->local_subs, sub);

        // Algorithm 1, line 4: Insert own node ID at level 0 for negative information
        if (quasar->protocol != NULL) {
            uint32_t addr = 0;
            uint16_t port = 0;
            meridian_protocol_get_local_node(quasar->protocol, &addr, &port);
            if (addr != 0 || port != 0) {
                uint8_t node_key[6];
                memcpy(node_key, &addr, sizeof(addr));
                memcpy(node_key + sizeof(addr), &port, sizeof(port));
                elastic_bloom_filter_t* level0 = attenuated_bloom_filter_get_level(quasar->routing, 0);
                if (level0 != NULL) {
                    elastic_bloom_filter_add(level0, node_key, sizeof(node_key));
                }
            }
        }
    }

    platform_unlock(&quasar->lock);
    return result;
}

int quasar_unsubscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len) {
    if (quasar == NULL || topic == NULL) return -1;
    platform_lock(&quasar->lock);

    // Remove topic from level 0 of the routing filter
    int result = attenuated_bloom_filter_unsubscribe(quasar->routing, topic, topic_len);

    // Find and remove the matching local subscription entry
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == topic_len &&
            memcmp(quasar->local_subs.data[i].topic->data, topic, topic_len) == 0) {
            buffer_destroy(quasar->local_subs.data[i].topic);
            quasar->local_subs.data[i].topic = NULL;
            vec_splice(&quasar->local_subs, i, 1);
            break;
        }
    }

    platform_unlock(&quasar->lock);
    return result;
}

// ============================================================================
// PUBLISHING
// ============================================================================

int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len,
                   const uint8_t* data, size_t data_len) {
    if (quasar == NULL || topic == NULL) return -1;

    // Check if locally subscribed
    bool locally_subscribed = false;
    platform_lock(&quasar->lock);
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == topic_len &&
            memcmp(quasar->local_subs.data[i].topic->data, topic, topic_len) == 0) {
            locally_subscribed = true;
            break;
        }
    }

    if (locally_subscribed) {
        // Algorithm 2, lines 4-8: deliver locally, add self to PUB, re-publish to all neighbors

        if (quasar->on_delivery != NULL) {
            quasar_delivery_cb_t cb = quasar->on_delivery;
            void* ctx = quasar->delivery_ctx;
            platform_unlock(&quasar->lock);
            cb(ctx, topic, topic_len, data, data_len);
            platform_lock(&quasar->lock);
        }

        // Re-publish to all overlay neighbors
        if (quasar->protocol != NULL) {
            uint32_t local_addr = 0;
            uint16_t local_port = 0;
            meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);

            size_t num_peers = 0;
            meridian_node_t** peers = meridian_protocol_get_connected_peers(quasar->protocol, &num_peers);
            for (size_t i = 0; i < num_peers; i++) {
                quasar_route_message_t* republish_msg = quasar_route_message_create(
                    topic, topic_len, data, data_len,
                    quasar->max_hops, QUASAR_DEFAULT_NEGATIVE_SIZE, QUASAR_DEFAULT_NEGATIVE_HASHES
                );
                if (republish_msg != NULL) {
                    bloom_filter_add(quasar->seen, (const uint8_t*)&republish_msg->id, sizeof(republish_msg->id));
                    meridian_node_t* local = meridian_node_create(local_addr, local_port);
                    if (local != NULL) {
                        quasar_route_message_add_publisher(republish_msg, local);
                        quasar_route_message_add_visited(republish_msg, local);
                        meridian_node_destroy(local);
                    }
                    uint8_t* route_buf = NULL;
                    size_t route_buf_len = 0;
                    if (quasar_route_message_serialize(republish_msg, &route_buf, &route_buf_len) == 0) {
                        meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, peers[i]);
                        free(route_buf);
                    }
                    quasar_route_message_destroy(republish_msg);
                }
            }
        }
        platform_unlock(&quasar->lock);
        return 0;
    }
    platform_unlock(&quasar->lock);

    // Algorithm 2, lines 10-19: Directed walk
    if (quasar->protocol != NULL) {
        platform_lock(&quasar->lock);

        uint32_t local_addr = 0;
        uint16_t local_port = 0;
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
        uint8_t local_key[6];
        memcpy(local_key, &local_addr, sizeof(uint32_t));
        memcpy(local_key + sizeof(uint32_t), &local_port, sizeof(uint16_t));

        uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);
        for (uint32_t level = 0; level < level_count; level++) {
            for (int ni = 0; ni < quasar->neighbor_filters.length; ni++) {
                quasar_neighbor_filter_t* nf = &quasar->neighbor_filters.data[ni];
                if (nf->filter == NULL) continue;
                elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(nf->filter, level);
                if (ebf == NULL) continue;

                if (!elastic_bloom_filter_contains(ebf, topic, topic_len)) continue;

                // Topic found at level L in neighbor O's filter — check negative info
                bool negated = false;
                if (local_addr != 0 || local_port != 0) {
                    if (elastic_bloom_filter_contains(ebf, local_key, sizeof(local_key))) {
                        negated = true;
                    }
                }

                if (!negated) {
                    // Directed walk: forward to this neighbor
                    meridian_node_t* target = meridian_node_create(nf->addr, nf->port);
                    quasar_route_message_t* fwd = quasar_route_message_create(
                        topic, topic_len, data, data_len,
                        quasar->max_hops, QUASAR_DEFAULT_NEGATIVE_SIZE, QUASAR_DEFAULT_NEGATIVE_HASHES
                    );
                    if (fwd != NULL) {
                        bloom_filter_add(quasar->seen, (const uint8_t*)&fwd->id, sizeof(fwd->id));
                        meridian_node_t* local = meridian_node_create(local_addr, local_port);
                        if (local != NULL) {
                            quasar_route_message_add_publisher(fwd, local);
                            quasar_route_message_add_visited(fwd, local);
                            meridian_node_destroy(local);
                        }
                        uint8_t* route_buf = NULL;
                        size_t route_buf_len = 0;
                        if (quasar_route_message_serialize(fwd, &route_buf, &route_buf_len) == 0) {
                            meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, target);
                            free(route_buf);
                        }
                        quasar_route_message_destroy(fwd);
                    }
                    meridian_node_destroy(target);
                    platform_unlock(&quasar->lock);
                    return 0;
                }
            }
        }
        platform_unlock(&quasar->lock);
    }

    // Algorithm 2, lines 20-21: Random walk
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, topic_len, data, data_len,
        quasar->max_hops,
        QUASAR_DEFAULT_NEGATIVE_SIZE,
        QUASAR_DEFAULT_NEGATIVE_HASHES
    );
    if (msg == NULL) return -1;

    platform_lock(&quasar->lock);
    bloom_filter_add(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id));
    platform_unlock(&quasar->lock);

    // Add local node as publisher and to visited filter
    if (quasar->protocol != NULL) {
        uint32_t local_addr = 0;
        uint16_t local_port = 0;
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
        meridian_node_t* local_node = meridian_node_create(local_addr, local_port);
        if (local_node != NULL) {
            quasar_route_message_add_visited(msg, local_node);
            quasar_route_message_add_publisher(msg, local_node);
            meridian_node_destroy(local_node);
        }
    }

    // Forward to alpha random unvisited neighbors
    int result = 0;
    if (quasar->protocol != NULL) {
        size_t num_peers = 0;
        meridian_node_t** peers = meridian_protocol_get_connected_peers(quasar->protocol, &num_peers);
        uint32_t sent = 0;
        for (size_t i = 0; i < num_peers && sent < quasar->alpha; i++) {
            if (!quasar_route_message_has_visited(msg, peers[i])) {
                uint8_t* route_buf = NULL;
                size_t route_buf_len = 0;
                if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                    meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, peers[i]);
                    free(route_buf);
                }
                quasar_route_message_add_visited(msg, peers[i]);
                sent++;
            }
        }
    }

    quasar_route_message_destroy(msg);
    return result;
}

// ============================================================================
// ROUTED MESSAGE HANDLING
// ============================================================================

int quasar_on_route_message(quasar_t* quasar, quasar_route_message_t* msg, const struct meridian_node_t* from) {
    if (quasar == NULL || msg == NULL) return -1;

    // Algorithm 2, line 1: Dedup check
    platform_lock(&quasar->lock);
    if (bloom_filter_contains(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id))) {
        platform_unlock(&quasar->lock);
        return 0;
    }
    bloom_filter_add(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id));

    // Get local node info
    uint32_t local_addr = 0;
    uint16_t local_port = 0;
    if (quasar->protocol != NULL) {
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
    }
    meridian_node_t* local_node = meridian_node_create(local_addr, local_port);
    if (local_node != NULL) {
        quasar_route_message_add_visited(msg, local_node);
    }

    // Algorithm 2, lines 3-8: Check if locally subscribed
    bool locally_subscribed = false;
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == msg->topic->size &&
            memcmp(quasar->local_subs.data[i].topic->data, msg->topic->data, msg->topic->size) == 0) {
            locally_subscribed = true;
            break;
        }
    }

    if (locally_subscribed) {
        // Algorithm 2, line 8: append own node ID to PUB
        if (local_node != NULL) {
            quasar_route_message_add_publisher(msg, local_node);
        }

        if (quasar->on_delivery != NULL) {
            quasar_delivery_cb_t cb = quasar->on_delivery;
            void* ctx = quasar->delivery_ctx;
            platform_unlock(&quasar->lock);
            cb(ctx, msg->topic->data, msg->topic->size, msg->data->data, msg->data->size);
            platform_lock(&quasar->lock);
        }

        // Re-publish to all overlay neighbors
        if (quasar->protocol != NULL) {
            size_t num_peers = 0;
            meridian_node_t** peers = meridian_protocol_get_connected_peers(quasar->protocol, &num_peers);
            for (size_t i = 0; i < num_peers; i++) {
                uint8_t* route_buf = NULL;
                size_t route_buf_len = 0;
                if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                    meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, peers[i]);
                    free(route_buf);
                }
            }
        }

        if (local_node != NULL) meridian_node_destroy(local_node);
        platform_unlock(&quasar->lock);
        return 0;
    }

    platform_unlock(&quasar->lock);

    // Algorithm 2, line 9: Check TTL
    platform_lock(&msg->lock);
    if (msg->hops_remaining == 0) {
        platform_unlock(&msg->lock);
        if (local_node != NULL) meridian_node_destroy(local_node);
        return 0;
    }
    msg->hops_remaining--;
    platform_unlock(&msg->lock);

    // Algorithm 2, lines 10-19: Directed walk
    if (quasar != NULL) {
        platform_lock(&quasar->lock);
        uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);

        for (uint32_t level = 0; level < level_count; level++) {
            for (int ni = 0; ni < quasar->neighbor_filters.length; ni++) {
                quasar_neighbor_filter_t* nf = &quasar->neighbor_filters.data[ni];
                if (nf->filter == NULL) continue;
                elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(nf->filter, level);
                if (ebf == NULL) continue;

                if (!elastic_bloom_filter_contains(ebf, msg->topic->data, msg->topic->size)) continue;

                // Check negative information: is any publisher at this same level?
                bool negated = false;
                for (uint32_t pi = 0; pi < msg->pub_count; pi++) {
                    uint8_t pub_key[6];
                    memcpy(pub_key, &msg->pub_addrs[pi], sizeof(uint32_t));
                    memcpy(pub_key + sizeof(uint32_t), &msg->pub_ports[pi], sizeof(uint16_t));
                    if (elastic_bloom_filter_contains(ebf, pub_key, sizeof(pub_key))) {
                        negated = true;
                        break;
                    }
                }

                if (!negated) {
                    // Directed walk: forward the existing route message to this neighbor
                    meridian_node_t* target = meridian_node_create(nf->addr, nf->port);
                    uint8_t* route_buf = NULL;
                    size_t route_buf_len = 0;
                    if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                        meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, target);
                        free(route_buf);
                    }
                    meridian_node_destroy(target);
                    if (local_node != NULL) meridian_node_destroy(local_node);
                    platform_unlock(&quasar->lock);
                    return 0;
                }
            }
        }
        platform_unlock(&quasar->lock);
    }

    // Algorithm 2, lines 20-21: Random walk
    if (quasar->protocol == NULL) {
        if (local_node != NULL) meridian_node_destroy(local_node);
        return 0;
    }

    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_get_connected_peers(quasar->protocol, &num_peers);
    uint32_t sent = 0;
    for (size_t i = 0; i < num_peers && sent < quasar->alpha; i++) {
        if (!quasar_route_message_has_visited(msg, peers[i])) {
            uint8_t* route_buf = NULL;
            size_t route_buf_len = 0;
            if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                meridian_protocol_send_packet(quasar->protocol, route_buf, route_buf_len, peers[i]);
                free(route_buf);
            }
            quasar_route_message_add_visited(msg, peers[i]);
            sent++;
        }
    }

    if (local_node != NULL) meridian_node_destroy(local_node);
    return 0;
}

// ============================================================================
// GOSSIP AND PROPAGATION
// ============================================================================

/**
 * Wire format for attenuated bloom filter serialization:
 *
 * Header (all uint32_t, network byte order):
 *   magic          - QUASAR_GOSSIP_MAGIC (0x51534152 = "QSAR")
 *   version        - QUASAR_GOSSIP_VERSION (1)
 *   level_count    - number of filter levels
 *   level_size     - bits per level
 *   hash_count      - hash functions per level
 *   omega_bits     - omega threshold * 1000 (fixed-point)
 *   fp_bits        - fingerprint width
 *
 * Per level:
 *   count          - element count in this level
 *   bitset_bytes   - raw bitset data (level_size / 8 bytes, rounded up)
 *   num_buckets    - number of non-empty buckets
 *   Per non-empty bucket:
 *     bucket_index - bucket position (uint32_t)
 *     num_entries  - entries in this bucket (uint32_t)
 *     Per entry:
 *       fingerprint - uint32_t
 */

#define QUASAR_GOSSIP_MAGIC   0x51534152u
#define QUASAR_GOSSIP_VERSION 1u

int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from) {
    if (quasar == NULL || data == NULL || len < 7 * sizeof(uint32_t)) return -1;

    // Read header
    const uint32_t* header = (const uint32_t*)data;
    uint32_t magic = ntohl(header[0]);
    uint32_t version = ntohl(header[1]);
    uint32_t level_count = ntohl(header[2]);
    uint32_t level_size = ntohl(header[3]);
    uint32_t hash_count = ntohl(header[4]);
    uint32_t omega_fixed = ntohl(header[5]);
    uint32_t fp_bits = ntohl(header[6]);

    if (magic != QUASAR_GOSSIP_MAGIC || version != QUASAR_GOSSIP_VERSION) return -1;

    float omega = (float)omega_fixed / 1000.0f;

    // Create a temporary attenuated bloom filter from the received data
    attenuated_bloom_filter_t* incoming = attenuated_bloom_filter_create(
        level_count, level_size, hash_count, omega, fp_bits
    );
    if (incoming == NULL) return -1;

    // Deserialize each level's elastic bloom filter
    size_t offset = 7 * sizeof(uint32_t);
    for (uint32_t level = 0; level < level_count && offset < len; level++) {
        if (offset + sizeof(uint32_t) > len) break;
        // Skip count field (used for verification, not reconstruction)
        offset += sizeof(uint32_t);

        // Read bitset data
        size_t bitset_bytes = level_size / 8;
        if (level_size % 8 > 0) bitset_bytes++;
        if (offset + bitset_bytes > len) break;

        elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(incoming, level);
        if (ebf != NULL) {
            // Set bitset from serialized data
            if (elastic_bloom_filter_set_bitset(ebf, data + offset, bitset_bytes) != 0) {
                offset += bitset_bytes;
                break;
            }
        }
        offset += bitset_bytes;

        // Read bucket entries
        if (offset + sizeof(uint32_t) > len) break;
        uint32_t num_buckets = ntohl(*(const uint32_t*)(data + offset));
        offset += sizeof(uint32_t);

        for (uint32_t b = 0; b < num_buckets && offset + 2 * sizeof(uint32_t) <= len; b++) {
            uint32_t bucket_index = ntohl(*(const uint32_t*)(data + offset));
            offset += sizeof(uint32_t);
            uint32_t num_entries = ntohl(*(const uint32_t*)(data + offset));
            offset += sizeof(uint32_t);

            if (ebf != NULL && bucket_index < elastic_bloom_filter_size(ebf)) {
                for (uint32_t e = 0; e < num_entries && offset + sizeof(uint32_t) <= len; e++) {
                    uint32_t fp = ntohl(*(const uint32_t*)(data + offset));
                    offset += sizeof(uint32_t);
                    elastic_bloom_filter_bucket_insert(ebf, bucket_index, fp);
                }
            } else {
                offset += num_entries * sizeof(uint32_t);
            }
        }
    }

    // Store the incoming filter in the per-neighbor slot for directed routing (Algorithm 2)
    if (from != NULL) {
        platform_lock(&quasar->lock);
        // Remove old filter for this neighbor if it exists
        for (int i = 0; i < quasar->neighbor_filters.length; i++) {
            if (quasar->neighbor_filters.data[i].addr == from->addr &&
                quasar->neighbor_filters.data[i].port == from->port) {
                attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
                vec_splice(&quasar->neighbor_filters, i, 1);
                break;
            }
        }
        // Transfer ownership of incoming to the neighbor filter slot
        quasar_neighbor_filter_t nf;
        nf.addr = from->addr;
        nf.port = from->port;
        nf.filter = incoming;
        vec_push(&quasar->neighbor_filters, nf);
        // Also merge into the aggregated routing filter for local checks
        attenuated_bloom_filter_merge(quasar->routing, incoming);
        platform_unlock(&quasar->lock);
    } else {
        // No sender info — just merge into routing and destroy
        int result = attenuated_bloom_filter_merge(quasar->routing, incoming);
        attenuated_bloom_filter_destroy(incoming);
        return result;
    }
    return 0;
}

// Iteration callback context for collecting bucket entries during propagate
typedef struct {
    size_t* bucket_indices;
    uint32_t* fingerprints;
    size_t count;
    size_t capacity;
} propagate_collect_ctx_t;

// File-scope callback for elastic_bloom_filter_iterate
static void propagate_collect_entry(void* ctx, size_t bucket_idx, uint32_t fingerprint) {
    propagate_collect_ctx_t* cctx = (propagate_collect_ctx_t*)ctx;
    if (cctx->count >= cctx->capacity) return;
    cctx->bucket_indices[cctx->count] = bucket_idx;
    cctx->fingerprints[cctx->count] = fingerprint;
    cctx->count++;
}

int quasar_propagate(quasar_t* quasar) {
    if (quasar == NULL) return -1;
    if (quasar->protocol == NULL) return -1;

    uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);
    size_t level_size = QUASAR_DEFAULT_SIZE;
    size_t bitset_bytes = level_size / 8;
    if (level_size % 8 > 0) bitset_bytes++;

    // Phase 1: Calculate total size by iterating all levels
    // Extra margin per level for concurrent modifications between sizing and writing
    const size_t margin_per_level = 256;
    size_t total_size = 7 * sizeof(uint32_t); // header

    for (uint32_t level = 0; level < level_count; level++) {
        elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(quasar->routing, level);
        if (ebf == NULL) break;

        total_size += sizeof(uint32_t); // count
        total_size += bitset_bytes;     // bitset data
        total_size += sizeof(uint32_t); // num_buckets

        // Iterate to count entries for sizing
        propagate_collect_ctx_t cctx;
        cctx.capacity = elastic_bloom_filter_count(ebf) * QUASAR_DEFAULT_HASH_COUNT + 16;
        cctx.bucket_indices = get_clear_memory(sizeof(size_t) * cctx.capacity);
        cctx.fingerprints = get_clear_memory(sizeof(uint32_t) * cctx.capacity);
        cctx.count = 0;

        elastic_bloom_filter_iterate(ebf, propagate_collect_entry, &cctx);

        // Count distinct non-empty buckets
        uint32_t num_non_empty = 0;
        size_t last_bucket = SIZE_MAX;
        for (size_t i = 0; i < cctx.count; i++) {
            if (cctx.bucket_indices[i] != last_bucket) {
                num_non_empty++;
                last_bucket = cctx.bucket_indices[i];
            }
        }

        total_size += num_non_empty * 2 * sizeof(uint32_t); // bucket headers
        total_size += cctx.count * sizeof(uint32_t);         // fingerprints
        total_size += margin_per_level;                       // safety margin

        free(cctx.bucket_indices);
        free(cctx.fingerprints);
    }

    // Phase 2: Write serialized data
    uint8_t* buf = get_clear_memory(total_size);
    if (buf == NULL) return -1;

    // Write header
    uint32_t* header = (uint32_t*)buf;
    header[0] = htonl(QUASAR_GOSSIP_MAGIC);
    header[1] = htonl(QUASAR_GOSSIP_VERSION);
    header[2] = htonl(level_count);
    header[3] = htonl((uint32_t)level_size);
    header[4] = htonl(QUASAR_DEFAULT_HASH_COUNT);
    header[5] = htonl((uint32_t)(QUASAR_DEFAULT_OMEGA * 1000.0f));
    header[6] = htonl(QUASAR_DEFAULT_FP_BITS);
    size_t offset = 7 * sizeof(uint32_t);

    // Serialize each level
    for (uint32_t level = 0; level < level_count; level++) {
        elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(quasar->routing, level);
        if (ebf == NULL) break;

        // count
        *(uint32_t*)(buf + offset) = htonl((uint32_t)elastic_bloom_filter_count(ebf));
        offset += sizeof(uint32_t);

        // bitset data
        const uint8_t* bitset_data = NULL;
        size_t bitset_size = 0;
        elastic_bloom_filter_get_bitset(ebf, &bitset_data, &bitset_size);
        size_t copy_bytes = bitset_bytes < bitset_size ? bitset_bytes : bitset_size;
        if (bitset_data != NULL && copy_bytes > 0) {
            memcpy(buf + offset, bitset_data, copy_bytes);
        }
        offset += bitset_bytes;

        // Collect all bucket entries via iterate
        propagate_collect_ctx_t cctx;
        cctx.capacity = elastic_bloom_filter_count(ebf) * QUASAR_DEFAULT_HASH_COUNT + 16;
        cctx.bucket_indices = get_clear_memory(sizeof(size_t) * cctx.capacity);
        cctx.fingerprints = get_clear_memory(sizeof(uint32_t) * cctx.capacity);
        cctx.count = 0;

        elastic_bloom_filter_iterate(ebf, propagate_collect_entry, &cctx);

        // Group entries by bucket and write
        size_t entry_idx = 0;
        uint32_t num_non_empty = 0;

        // First pass: count non-empty buckets
        size_t last_bucket = SIZE_MAX;
        for (size_t i = 0; i < cctx.count; i++) {
            if (cctx.bucket_indices[i] != last_bucket) {
                num_non_empty++;
                last_bucket = cctx.bucket_indices[i];
            }
        }

        // Write num_buckets
        *(uint32_t*)(buf + offset) = htonl(num_non_empty);
        offset += sizeof(uint32_t);

        // Second pass: write bucket entries grouped by bucket
        entry_idx = 0;
        while (entry_idx < cctx.count) {
            size_t current_bucket = cctx.bucket_indices[entry_idx];

            // Count entries in this bucket
            uint32_t entries_in_bucket = 0;
            size_t start = entry_idx;
            while (entry_idx < cctx.count && cctx.bucket_indices[entry_idx] == current_bucket) {
                entries_in_bucket++;
                entry_idx++;
            }

            // Write bucket header
            *(uint32_t*)(buf + offset) = htonl((uint32_t)current_bucket);
            offset += sizeof(uint32_t);
            *(uint32_t*)(buf + offset) = htonl(entries_in_bucket);
            offset += sizeof(uint32_t);

            // Write fingerprints
            for (size_t e = start; e < entry_idx; e++) {
                *(uint32_t*)(buf + offset) = htonl(cctx.fingerprints[e]);
                offset += sizeof(uint32_t);
            }
        }

        free(cctx.bucket_indices);
        free(cctx.fingerprints);
    }

    // Broadcast the serialized filter to all connected peers
    int result = meridian_protocol_broadcast(quasar->protocol, buf, offset);
    free(buf);
    return result;
}

// ============================================================================
// PERIODIC MAINTENANCE
// ============================================================================

int quasar_tick(quasar_t* quasar) {
    if (quasar == NULL) return -1;
    platform_lock(&quasar->lock);

    // Walk through all local subscriptions and decrement their TTL.
    // Expired subscriptions are removed from both the local list and the
    // routing filter (level 0), so neighbors will eventually learn via
    // the next filter propagation cycle.
    int i = 0;
    while (i < quasar->local_subs.length) {
        if (quasar->local_subs.data[i].ttl > 0) {
            quasar->local_subs.data[i].ttl--;

            if (quasar->local_subs.data[i].ttl == 0) {
                // Subscription expired — remove from routing filter and local list
                if (quasar->local_subs.data[i].topic != NULL) {
                    attenuated_bloom_filter_unsubscribe(
                        quasar->routing,
                        quasar->local_subs.data[i].topic->data,
                        quasar->local_subs.data[i].topic->size
                    );
                    buffer_destroy(quasar->local_subs.data[i].topic);
                }
                vec_splice(&quasar->local_subs, i, 1);
                continue; // Don't increment i — next element slid into this position
            }
        }
        i++;
    }

    platform_unlock(&quasar->lock);
    return 0;
}