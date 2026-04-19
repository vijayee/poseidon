//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_BLOOM_FILTER_H
#define POSEIDON_BLOOM_FILTER_H

#include "bitset.h"
#include "../RefCounter/refcounter.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bloom_filter_t {
    refcounter_t refcounter;
    bitset_t* bits;
    size_t size;           // number of bits
    uint32_t hash_count;   // k hash functions
    uint64_t seed_a;
    uint64_t seed_b;
    size_t count;
} bloom_filter_t;

bloom_filter_t* bloom_filter_create(size_t size, uint32_t hash_count);
void bloom_filter_destroy(bloom_filter_t* filter);
int bloom_filter_add(bloom_filter_t* filter, const uint8_t* data, size_t len);
bool bloom_filter_contains(bloom_filter_t* filter, const uint8_t* data, size_t len);
size_t bloom_filter_count(bloom_filter_t* filter);
size_t bloom_filter_size(bloom_filter_t* filter);
void bloom_filter_reset(bloom_filter_t* filter);
void bloom_filter_optimal_size(size_t n, double false_positive_rate, size_t* out_size, uint32_t* out_hash_count);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_BLOOM_FILTER_H