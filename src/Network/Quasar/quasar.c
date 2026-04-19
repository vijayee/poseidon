//
// Created by victor on 4/19/26.
//

#include "quasar.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

#define QUASAR_DEFAULT_LEVELS 5
#define QUASAR_DEFAULT_SIZE 1024
#define QUASAR_DEFAULT_HASH_COUNT 3
#define QUASAR_DEFAULT_OMEGA 0.75f
#define QUASAR_DEFAULT_FP_BITS EBF_DEFAULT_FP_BITS

quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha) {
    quasar_t* quasar = get_clear_memory(sizeof(quasar_t));
    quasar->protocol = protocol;
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

int quasar_subscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl) {
    if (quasar == NULL || topic == NULL) return -1;
    platform_lock(&quasar->lock);
    int result = attenuated_bloom_filter_subscribe(quasar->routing, topic, topic_len);
    if (result == 0) {
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
    int result = attenuated_bloom_filter_unsubscribe(quasar->routing, topic, topic_len);
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

int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len, const uint8_t* data, size_t data_len) {
    if (quasar == NULL || topic == NULL) return -1;
    platform_lock(&quasar->lock);
    uint32_t hops = 0;
    bool found = attenuated_bloom_filter_check(quasar->routing, topic, topic_len, &hops);
    if (found && hops == 0) {
        // Local delivery
    }
    if (found && hops > 0) {
        // Directed routing toward subscriber via Meridian
    }
    if (!found) {
        // Random walk: forward to alpha random neighbors
    }
    platform_unlock(&quasar->lock);
    return 0;
}

int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from) {
    if (quasar == NULL || data == NULL) return -1;
    return 0;
}

int quasar_propagate(quasar_t* quasar) {
    if (quasar == NULL) return -1;
    return 0;
}

int quasar_tick(quasar_t* quasar) {
    if (quasar == NULL) return -1;
    platform_lock(&quasar->lock);
    int i = 0;
    while (i < quasar->local_subs.length) {
        if (quasar->local_subs.data[i].ttl > 0) {
            quasar->local_subs.data[i].ttl--;
            if (quasar->local_subs.data[i].ttl == 0) {
                if (quasar->local_subs.data[i].topic != NULL) {
                    attenuated_bloom_filter_unsubscribe(
                        quasar->routing,
                        quasar->local_subs.data[i].topic->data,
                        quasar->local_subs.data[i].topic->size
                    );
                    buffer_destroy(quasar->local_subs.data[i].topic);
                }
                vec_splice(&quasar->local_subs, i, 1);
                continue;
            }
        }
        i++;
    }
    platform_unlock(&quasar->lock);
    return 0;
}
