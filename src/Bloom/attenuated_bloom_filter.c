//
// Created by victor on 4/19/26.
//

#include "attenuated_bloom_filter.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

attenuated_bloom_filter_t* attenuated_bloom_filter_create(uint32_t levels, size_t size, uint32_t hash_count, float omega, uint32_t fp_bits) {
    attenuated_bloom_filter_t* abf = get_clear_memory(sizeof(attenuated_bloom_filter_t));
    abf->level_count = levels;
    abf->levels = get_clear_memory(sizeof(elastic_bloom_filter_t*) * levels);
    for (uint32_t i = 0; i < levels; i++) {
        abf->levels[i] = elastic_bloom_filter_create(size, hash_count, omega, fp_bits);
    }
    platform_lock_init(&abf->lock);
    refcounter_init((refcounter_t*)abf);
    return abf;
}

void attenuated_bloom_filter_destroy(attenuated_bloom_filter_t* abf) {
    if (abf == NULL) return;
    refcounter_dereference((refcounter_t*)abf);
    if (refcounter_count((refcounter_t*)abf) == 0) {
        for (uint32_t i = 0; i < abf->level_count; i++) {
            elastic_bloom_filter_destroy(abf->levels[i]);
        }
        free(abf->levels);
        platform_lock_destroy(&abf->lock);
        free(abf);
    }
}

int attenuated_bloom_filter_subscribe(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len) {
    if (abf == NULL || topic == NULL) return -1;
    platform_lock(&abf->lock);
    int result = elastic_bloom_filter_add(abf->levels[0], topic, topic_len);
    platform_unlock(&abf->lock);
    return result;
}

int attenuated_bloom_filter_unsubscribe(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len) {
    if (abf == NULL || topic == NULL) return -1;
    platform_lock(&abf->lock);
    int result = elastic_bloom_filter_remove(abf->levels[0], topic, topic_len);
    platform_unlock(&abf->lock);
    return result;
}

bool attenuated_bloom_filter_check(attenuated_bloom_filter_t* abf, const uint8_t* topic, size_t topic_len, uint32_t* out_hops) {
    if (abf == NULL || topic == NULL) return false;
    platform_lock(&abf->lock);
    for (uint32_t level = 0; level < abf->level_count; level++) {
        if (elastic_bloom_filter_contains(abf->levels[level], topic, topic_len)) {
            if (out_hops != NULL) {
                *out_hops = level;
            }
            platform_unlock(&abf->lock);
            return true;
        }
    }
    platform_unlock(&abf->lock);
    return false;
}

int attenuated_bloom_filter_merge(attenuated_bloom_filter_t* dest, const attenuated_bloom_filter_t* src) {
    if (dest == NULL || src == NULL) return -1;
    platform_lock(&dest->lock);
    uint32_t max_levels = src->level_count < (dest->level_count - 1) ? src->level_count : (dest->level_count - 1);
    for (uint32_t i = 0; i < max_levels; i++) {
        elastic_bloom_filter_merge(dest->levels[i + 1], src->levels[i]);
    }
    platform_unlock(&dest->lock);
    return 0;
}

elastic_bloom_filter_t* attenuated_bloom_filter_get_level(attenuated_bloom_filter_t* abf, uint32_t level) {
    if (abf == NULL || level >= abf->level_count) return NULL;
    return abf->levels[level];
}

uint32_t attenuated_bloom_filter_level_count(attenuated_bloom_filter_t* abf) {
    if (abf == NULL) return 0;
    return abf->level_count;
}