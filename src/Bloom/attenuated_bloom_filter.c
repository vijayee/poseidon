//
// Created by victor on 4/19/26.
//

#include "attenuated_bloom_filter.h"
#include "../Util/allocator.h"
#include <cbor.h>
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

cbor_item_t* attenuated_bloom_filter_encode(const attenuated_bloom_filter_t* abf) {
    if (abf == NULL) return NULL;

    platform_lock(&((attenuated_bloom_filter_t*)abf)->lock);

    cbor_item_t* levels_arr = cbor_new_definite_array(abf->level_count);
    if (levels_arr == NULL) goto fail;

    for (uint32_t i = 0; i < abf->level_count; i++) {
        cbor_item_t* encoded = elastic_bloom_filter_encode(abf->levels[i]);
        if (encoded == NULL) {
            cbor_decref(&levels_arr);
            goto fail;
        }
        if (!cbor_array_push(levels_arr, encoded)) {
            cbor_decref(&encoded);
            cbor_decref(&levels_arr);
            goto fail;
        }
    }

    cbor_item_t* arr = cbor_new_definite_array(2);
    if (arr == NULL) {
        cbor_decref(&levels_arr);
        goto fail;
    }

    if (!cbor_array_push(arr, cbor_build_uint32(abf->level_count))) {
        cbor_decref(&levels_arr);
        cbor_decref(&arr);
        goto fail;
    }
    if (!cbor_array_push(arr, levels_arr)) {
        cbor_decref(&levels_arr);
        cbor_decref(&arr);
        goto fail;
    }

    platform_unlock(&((attenuated_bloom_filter_t*)abf)->lock);
    return arr;

fail:
    platform_unlock(&((attenuated_bloom_filter_t*)abf)->lock);
    return NULL;
}

attenuated_bloom_filter_t* attenuated_bloom_filter_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_isa_array(item) || !cbor_array_is_definite(item))
        return NULL;

    cbor_item_t** elems = cbor_array_handle(item);
    if (cbor_array_size(item) != 2) return NULL;

    uint32_t level_count = (uint32_t)cbor_get_int(elems[0]);

    cbor_item_t* levels_item = elems[1];
    if (!cbor_isa_array(levels_item) || !cbor_array_is_definite(levels_item))
        return NULL;
    if (cbor_array_size(levels_item) != level_count) return NULL;

    cbor_item_t** level_elems = cbor_array_handle(levels_item);

    /* Manually construct ABF instead of using attenuated_bloom_filter_create
       because create() would allocate fresh EBFs we'd have to destroy */
    attenuated_bloom_filter_t* abf = get_clear_memory(sizeof(attenuated_bloom_filter_t));
    if (abf == NULL) return NULL;

    abf->level_count = level_count;
    abf->levels = get_clear_memory(sizeof(elastic_bloom_filter_t*) * level_count);
    if (abf->levels == NULL) {
        free(abf);
        return NULL;
    }

    for (uint32_t i = 0; i < level_count; i++) {
        abf->levels[i] = elastic_bloom_filter_decode(level_elems[i]);
        if (abf->levels[i] == NULL) {
            /* Clean up already-decoded levels */
            for (uint32_t j = 0; j < i; j++) {
                elastic_bloom_filter_destroy(abf->levels[j]);
            }
            free(abf->levels);
            free(abf);
            return NULL;
        }
    }

    platform_lock_init(&abf->lock);
    refcounter_init((refcounter_t*)abf);

    return abf;
}