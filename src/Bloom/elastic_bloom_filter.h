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

/**
 * Iteration callback for serializing bucket entries.
 * Called once per entry in a non-empty bucket during elastic_bloom_filter_iterate.
 *
 * @param ctx         User context
 * @param bucket_idx  Bucket position (0..bucket_count-1)
 * @param fingerprint Fingerprint value stored in this entry
 */
typedef void (*ebf_entry_cb_t)(void* ctx, size_t bucket_idx, uint32_t fingerprint);

/**
 * Iterates over all non-empty buckets and their entries.
 * Calls the callback for each fingerprint in each non-empty bucket.
 * Used for serializing the EBF state for network transmission.
 *
 * @param ebf      Elastic bloom filter to iterate
 * @param callback Called once per entry in each non-empty bucket
 * @param ctx      User context passed to callback
 * @return         0 on success, -1 on failure
 */
int elastic_bloom_filter_iterate(elastic_bloom_filter_t* ebf, ebf_entry_cb_t callback, void* ctx);

/**
 * Inserts a fingerprint directly into a bucket.
 * Used for deserializing an EBF received over the network.
 * Also sets the corresponding bit in the bitset.
 *
 * @param ebf         Elastic bloom filter
 * @param bucket_idx  Bucket position (must be < bucket_count)
 * @param fingerprint Fingerprint value to insert
 * @return            0 on success, -1 on failure
 */
int elastic_bloom_filter_bucket_insert(elastic_bloom_filter_t* ebf, size_t bucket_idx, uint32_t fingerprint);

/**
 * Gets the bitset data pointer and size for serialization.
 * The returned pointer is valid as long as the EBF is not modified.
 *
 * @param ebf       Elastic bloom filter
 * @param out_data  Output: pointer to bitset byte array
 * @param out_size  Output: number of bytes in the bitset
 * @return          0 on success, -1 on failure
 */
int elastic_bloom_filter_get_bitset(elastic_bloom_filter_t* ebf, const uint8_t** out_data, size_t* out_size);

/**
 * Sets the bitset data from a serialized form.
 * Overwrites the current bitset with the provided data.
 * The data length must match the current bitset size.
 *
 * @param ebf      Elastic bloom filter
 * @param data     Source byte array
 * @param size     Number of bytes to copy
 * @return         0 on success, -1 on failure
 */
int elastic_bloom_filter_set_bitset(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_ELASTIC_BLOOM_FILTER_H