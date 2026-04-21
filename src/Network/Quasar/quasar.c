//
// Created by victor on 4/19/26.
//

#include "quasar.h"
#include "../Meridian/meridian_protocol.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

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

// ============================================================================
// LIFECYCLE
// ============================================================================

quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha) {
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
    quasar->max_hops = max_hops;
    quasar->alpha = alpha;
    platform_lock_init(&quasar->lock);
    refcounter_init((refcounter_t*)quasar);
    return quasar;
}

void quasar_destroy(quasar_t* quasar) {
    if (quasar == NULL) return;
    refcounter_dereference((refcounter_t*)quasar);
    if (refcounter_count((refcounter_t*)quasar) == 0) {
        attenuated_bloom_filter_destroy(quasar->routing);

        // Free all local subscription topic buffers
        for (int i = 0; i < quasar->local_subs.length; i++) {
            if (quasar->local_subs.data[i].topic != NULL) {
                buffer_destroy(quasar->local_subs.data[i].topic);
            }
        }
        vec_deinit(&quasar->local_subs);
        platform_lock_destroy(&quasar->lock);
        free(quasar);
    }
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

int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len, const uint8_t* data, size_t data_len) {
    if (quasar == NULL || topic == NULL) return -1;
    platform_lock(&quasar->lock);

    // Check the attenuated filter to determine routing direction
    uint32_t hops = 0;
    bool found = attenuated_bloom_filter_check(quasar->routing, topic, topic_len, &hops);

    if (found && hops == 0) {
        // Local delivery: this node is subscribed to the topic
    }
    if (found && hops > 0) {
        // Directed routing: a subscriber exists hops steps away through a neighbor.
        // Forward toward the neighbor that contributed the matching filter level.
    }
    if (!found) {
        // Random walk: no known route to a subscriber.
        // Forward to alpha random neighbors to probabilistically discover a subscriber.
    }

    platform_unlock(&quasar->lock);
    return 0;
}

// ============================================================================
// GOSSIP AND PROPAGATION
// ============================================================================

int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from) {
    if (quasar == NULL || data == NULL) return -1;

    struct cbor_load_result result;
    cbor_item_t* root = cbor_load(data, len, &result);
    if (root == NULL || result.error.code != CBOR_ERR_NONE) {
        if (root != NULL) cbor_decref(&root);
        return -1;
    }

    // Validate: must be a definite array with at least 3 elements
    if (!cbor_isa_array(root) || !cbor_array_is_definite(root) ||
        cbor_array_size(root) < 3) {
        cbor_decref(&root);
        return -1;
    }

    cbor_item_t** items = cbor_array_handle(root);

    // items[0]: packet type (must be QUASAR_GOSSIP)
    if (!cbor_isa_uint(items[0])) {
        cbor_decref(&root);
        return -1;
    }
    uint8_t type = cbor_get_uint8(items[0]);
    if (type != MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP) {
        cbor_decref(&root);
        return -1;
    }

    // items[1]: level_count (validated for structure)
    if (!cbor_isa_uint(items[1])) {
        cbor_decref(&root);
        return -1;
    }
    uint32_t level_count = cbor_get_uint32(items[1]);

    // items[2]: encoded attenuated bloom filter
    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(items[2]);
    if (decoded == NULL || attenuated_bloom_filter_level_count(decoded) != level_count) {
        if (decoded != NULL) attenuated_bloom_filter_destroy(decoded);
        cbor_decref(&root);
        return -1;
    }

    // Merge under lock — merge shifts src levels by +1 automatically
    platform_lock(&quasar->lock);
    attenuated_bloom_filter_merge(quasar->routing, decoded);
    platform_unlock(&quasar->lock);

    attenuated_bloom_filter_destroy(decoded);
    cbor_decref(&root);
    return 0;
}

int quasar_propagate(quasar_t* quasar) {
    if (quasar == NULL) return -1;

    platform_lock(&quasar->lock);

    // Encode the attenuated bloom filter to CBOR
    cbor_item_t* encoded_filter = attenuated_bloom_filter_encode(quasar->routing);
    if (encoded_filter == NULL) {
        platform_unlock(&quasar->lock);
        return -1;
    }

    // Read level_count under lock since it reads quasar->routing
    uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);

    platform_unlock(&quasar->lock);

    // Build the QUASAR_GOSSIP packet: [type, level_count, encoded_filter]
    cbor_item_t* packet = cbor_new_definite_array(3);
    if (packet == NULL) {
        cbor_decref(&encoded_filter);
        return -1;
    }

    cbor_item_t* type_item = cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP);
    cbor_item_t* level_item = cbor_build_uint32(level_count);
    if (type_item == NULL || level_item == NULL) {
        if (type_item != NULL) cbor_decref(&type_item);
        if (level_item != NULL) cbor_decref(&level_item);
        cbor_decref(&packet);
        cbor_decref(&encoded_filter);
        return -1;
    }

    bool ok = cbor_array_push(packet, type_item) &&
              cbor_array_push(packet, level_item) &&
              cbor_array_push(packet, encoded_filter);
    if (!ok) {
        cbor_decref(&type_item);
        cbor_decref(&level_item);
        cbor_decref(&packet);
        cbor_decref(&encoded_filter);
        return -1;
    }

    // cbor_array_push increfs each item, so we can decref our local refs now
    cbor_decref(&type_item);
    cbor_decref(&level_item);
    cbor_decref(&encoded_filter);

    // Serialize to bytes
    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);
    if (buf == NULL || buf_size == 0) {
        cbor_decref(&packet);
        return -1;
    }

    // Broadcast to all connected peers
    int rc = meridian_protocol_broadcast(quasar->protocol, buf, buf_size);

    free(buf);
    cbor_decref(&packet);
    return rc == 0 ? 0 : -1;
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