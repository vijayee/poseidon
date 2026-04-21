//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_ELASTIC_BLOOM_FILTER_H
#define POSEIDON_ELASTIC_BLOOM_FILTER_H

#include "bitset.h"
#include "bloom_filter.h"
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EBF_DEFAULT_FP_BITS 8

typedef struct ebf_bucket_entry_t {
    uint32_t fingerprint;
    struct ebf_bucket_entry_t* next;
} ebf_bucket_entry_t;

typedef struct elastic_bloom_filter_t {
    refcounter_t refcounter;
    bitset_t* bits;
    size_t size;                        // Number of bits
    size_t bucket_count;                // Number of cooperative buckets (always == size)
    ebf_bucket_entry_t** buckets;
    uint32_t hash_count;
    uint64_t seed_a;
    uint64_t seed_b;
    float omega;                         // Expansion threshold
    uint32_t fp_bits;                    // Fingerprint bit width
    size_t count;
    PLATFORMLOCKTYPE(lock);
} elastic_bloom_filter_t;

elastic_bloom_filter_t* elastic_bloom_filter_create(size_t size, uint32_t hash_count, float omega, uint32_t fp_bits);
void elastic_bloom_filter_destroy(elastic_bloom_filter_t* ebf);
int elastic_bloom_filter_add(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);
bool elastic_bloom_filter_contains(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);
int elastic_bloom_filter_remove(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len);
int elastic_bloom_filter_expand(elastic_bloom_filter_t* ebf);
int elastic_bloom_filter_compress(elastic_bloom_filter_t* ebf);
int elastic_bloom_filter_merge(elastic_bloom_filter_t* dest, const elastic_bloom_filter_t* src);
size_t elastic_bloom_filter_count(elastic_bloom_filter_t* ebf);
size_t elastic_bloom_filter_size(elastic_bloom_filter_t* ebf);
float elastic_bloom_filter_ratio(elastic_bloom_filter_t* ebf);

#include <cbor.h>

/** Encode an elastic bloom filter to a CBOR item.
 *
 * Produces a definite array with sparse bucket encoding.
 * Thread-safe: acquires the filter lock for the duration.
 *
 * @param ebf The elastic bloom filter to encode
 * @return CBOR item representing the filter, or NULL on error
 */
cbor_item_t* elastic_bloom_filter_encode(const elastic_bloom_filter_t* ebf);

/** Decode an elastic bloom filter from a CBOR item.
 *
 * Reconstructs the filter from the sparse bucket encoding produced
 * by elastic_bloom_filter_encode.
 *
 * @param item CBOR item to decode from
 * @return Decoded elastic bloom filter, or NULL on error
 */
elastic_bloom_filter_t* elastic_bloom_filter_decode(cbor_item_t* item);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_ELASTIC_BLOOM_FILTER_H