//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_QUASAR_H
#define POSEIDON_QUASAR_H

#include "../Meridian/meridian.h"
#include "../../Bloom/attenuated_bloom_filter.h"
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

typedef struct quasar_subscription_t {
    buffer_t* topic;
    uint32_t ttl;
} quasar_subscription_t;

typedef struct quasar_t {
    refcounter_t refcounter;
    struct meridian_protocol_t* protocol;
    attenuated_bloom_filter_t* routing;
    vec_t(quasar_subscription_t) local_subs;
    uint32_t max_hops;
    uint32_t alpha;
    PLATFORMLOCKTYPE(lock);
} quasar_t;

quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha);
void quasar_destroy(quasar_t* quasar);
int quasar_subscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl);
int quasar_unsubscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len);
int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len, const uint8_t* data, size_t data_len);
int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from);
int quasar_propagate(quasar_t* quasar);
int quasar_tick(quasar_t* quasar);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_QUASAR_H
