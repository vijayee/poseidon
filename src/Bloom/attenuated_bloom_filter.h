//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_ATTENUATED_BLOOM_FILTER_H
#define POSEIDON_ATTENUATED_BLOOM_FILTER_H

#include "elastic_bloom_filter.h"
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct attenuated_bloom_filter_t {
    refcounter_t refcounter;
    uint32_t level_count;
    elastic_bloom_filter_t** levels;
    PLATFORMLOCKTYPE(lock);
} attenuated_bloom_filter_t;

attenuated_bloom_filter_t* attenuated_bloom_filter_create(uint32_t levels, size_t size, uint32_t hash_count, float omega, uint32_t fp_bits);
void attenuated_bloom_filter_destroy(attenuated_bloom_filter_t* abf);
int attenuated_bloom_filter_subscribe(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len);
int attenuated_bloom_filter_unsubscribe(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len);
bool attenuated_bloom_filter_check(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len, uint32_t* out_hops);
int attenuated_bloom_filter_merge(attenuated_bloom_filter_t* dest, const attenuated_bloom_filter_t* src);
elastic_bloom_filter_t* attenuated_bloom_filter_get_level(attenuated_bloom_filter_t* abf, uint32_t level);
uint32_t attenuated_bloom_filter_level_count(attenuated_bloom_filter_t* abf);

#include <cbor.h>

/** Encode an attenuated bloom filter to a CBOR item.
 *
 * Produces a definite array: [level_count, [level_0_ebf, level_1_ebf, ...]]
 * Each level is encoded via elastic_bloom_filter_encode.
 * Thread-safe: acquires the filter lock for the duration.
 *
 * @param abf The attenuated bloom filter to encode
 * @return CBOR item representing the filter, or NULL on error
 */
cbor_item_t* attenuated_bloom_filter_encode(const attenuated_bloom_filter_t* abf);

/** Decode an attenuated bloom filter from a CBOR item.
 *
 * Reconstructs the filter from the encoding produced by
 * attenuated_bloom_filter_encode.
 *
 * @param item CBOR item to decode from
 * @return Decoded attenuated bloom filter, or NULL on error
 */
attenuated_bloom_filter_t* attenuated_bloom_filter_decode(cbor_item_t* item);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_ATTENUATED_BLOOM_FILTER_H