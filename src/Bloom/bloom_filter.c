//
// Created by victor on 4/19/26.
//

#include "bloom_filter.h"
#include "../Util/allocator.h"
#include <xxh3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

bloom_filter_t* bloom_filter_create(size_t size, uint32_t hash_count) {
    bloom_filter_t* filter = get_clear_memory(sizeof(bloom_filter_t));
    size_t byte_size = size / 8;
    if (size % 8 > 0) byte_size++;
    filter->bits = bitset_create(byte_size);
    filter->size = size;
    filter->hash_count = hash_count;
    filter->seed_a = 1;
    filter->seed_b = 2;
    filter->count = 0;
    refcounter_init((refcounter_t*)filter);
    return filter;
}

void bloom_filter_destroy(bloom_filter_t* filter) {
    if (filter == NULL) return;
    refcounter_dereference((refcounter_t*)filter);
    if (refcounter_count((refcounter_t*)filter) == 0) {
        bitset_destroy(filter->bits);
        free(filter);
    }
}

int bloom_filter_add(bloom_filter_t* filter, const uint8_t* data, size_t len) {
    if (filter == NULL || data == NULL) return -1;
    XXH64_hash_t a = XXH3_64bits_withSeed(data, len, filter->seed_a);
    XXH64_hash_t b = XXH3_64bits_withSeed(data, len, filter->seed_b);
    bool is_new = false;
    for (uint32_t i = 0; i < filter->hash_count; i++) {
        size_t index = (size_t)((a + (uint64_t)i * b + (uint64_t)i * i) % filter->size);
        if (!bitset_get(filter->bits, index)) {
            is_new = true;
        }
        bitset_set(filter->bits, index, true);
    }
    if (is_new) {
        filter->count++;
    }
    return 0;
}

bool bloom_filter_contains(bloom_filter_t* filter, const uint8_t* data, size_t len) {
    if (filter == NULL || data == NULL) return false;
    XXH64_hash_t a = XXH3_64bits_withSeed(data, len, filter->seed_a);
    XXH64_hash_t b = XXH3_64bits_withSeed(data, len, filter->seed_b);
    for (uint32_t i = 0; i < filter->hash_count; i++) {
        size_t index = (size_t)((a + (uint64_t)i * b + (uint64_t)i * i) % filter->size);
        if (!bitset_get(filter->bits, index)) {
            return false;
        }
    }
    return true;
}

size_t bloom_filter_count(bloom_filter_t* filter) {
    return filter->count;
}

size_t bloom_filter_size(bloom_filter_t* filter) {
    return filter->size;
}

void bloom_filter_reset(bloom_filter_t* filter) {
    if (filter == NULL) return;
    size_t byte_size = filter->size / 8;
    if (filter->size % 8 > 0) byte_size++;
    bitset_destroy(filter->bits);
    filter->bits = bitset_create(byte_size);
    filter->count = 0;
}

void bloom_filter_optimal_size(size_t n, double false_positive_rate, size_t* out_size, uint32_t* out_hash_count) {
    double m = ((double)n * log(false_positive_rate)) / log(1.0 / pow(2.0, log(2.0)));
    m = ceil(m);
    double r = m / (double)n;
    double k = round(log(2.0) * r);
    *out_size = (size_t)m;
    *out_hash_count = (uint32_t)k;
}