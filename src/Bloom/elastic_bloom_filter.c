//
// Created by victor on 4/19/26.
//

#include "elastic_bloom_filter.h"
#include "../Util/allocator.h"
#include <cbor.h>
#include <xxh3.h>
#include <stdlib.h>
#include <string.h>

static void compute_hash_pair(const elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len, uint32_t i, uint32_t* fp, size_t* index) {
    uint64_t seed = ebf->seed_a + (uint64_t)i * ebf->seed_b;
    XXH64_hash_t h = XXH3_64bits_withSeed(data, len, (XXH64_hash_t)seed);
    *index = (size_t)(h % ebf->size);
    *fp = (uint32_t)((h / ebf->size) % (1ULL << ebf->fp_bits));
}

static void bucket_insert(ebf_bucket_entry_t** head, uint32_t fp) {
    ebf_bucket_entry_t* entry = get_clear_memory(sizeof(ebf_bucket_entry_t));
    entry->fingerprint = fp;
    entry->next = *head;
    *head = entry;
}

static bool bucket_remove(ebf_bucket_entry_t** head, uint32_t fp) {
    ebf_bucket_entry_t** curr = head;
    while (*curr != NULL) {
        if ((*curr)->fingerprint == fp) {
            ebf_bucket_entry_t* to_free = *curr;
            *curr = to_free->next;
            free(to_free);
            return true;
        }
        curr = &(*curr)->next;
    }
    return false;
}

static bool bucket_contains(ebf_bucket_entry_t* head, uint32_t fp) {
    ebf_bucket_entry_t* curr = head;
    while (curr != NULL) {
        if (curr->fingerprint == fp) return true;
        curr = curr->next;
    }
    return false;
}

static bool bucket_is_empty(ebf_bucket_entry_t* head) {
    return head == NULL;
}

static void bucket_destroy(ebf_bucket_entry_t* head) {
    ebf_bucket_entry_t* curr = head;
    while (curr != NULL) {
        ebf_bucket_entry_t* next = curr->next;
        free(curr);
        curr = next;
    }
}

// rebuild_bitset sets bit i iff bucket i is non-empty (1:1 mapping per EBF paper)
static void rebuild_bitset(elastic_bloom_filter_t* ebf) {
    size_t byte_size = ebf->size / 8;
    if (ebf->size % 8 > 0) byte_size++;
    bitset_destroy(ebf->bits);
    ebf->bits = bitset_create(byte_size);
    for (size_t i = 0; i < ebf->size; i++) {
        if (!bucket_is_empty(ebf->buckets[i])) {
            bitset_set(ebf->bits, i, true);
        }
    }
}

// bucket_count always equals size (per EBF paper: 1 bucket per bit position)
elastic_bloom_filter_t* elastic_bloom_filter_create(size_t size, uint32_t hash_count, float omega, uint32_t fp_bits) {
    elastic_bloom_filter_t* ebf = get_clear_memory(sizeof(elastic_bloom_filter_t));
    size_t byte_size = size / 8;
    if (size % 8 > 0) byte_size++;
    ebf->bits = bitset_create(byte_size);
    ebf->size = size;
    ebf->bucket_count = size;
    ebf->buckets = get_clear_memory(sizeof(ebf_bucket_entry_t*) * size);
    ebf->hash_count = hash_count;
    ebf->seed_a = 1;
    ebf->seed_b = 2;
    ebf->omega = omega;
    ebf->fp_bits = fp_bits;
    ebf->count = 0;
    platform_lock_init(&ebf->lock);
    refcounter_init((refcounter_t*)ebf);
    return ebf;
}

void elastic_bloom_filter_destroy(elastic_bloom_filter_t* ebf) {
    if (ebf == NULL) return;
    refcounter_dereference((refcounter_t*)ebf);
    if (refcounter_count((refcounter_t*)ebf) == 0) {
        bitset_destroy(ebf->bits);
        for (size_t i = 0; i < ebf->bucket_count; i++) {
            bucket_destroy(ebf->buckets[i]);
        }
        free(ebf->buckets);
        platform_lock_destroy(&ebf->lock);
        free(ebf);
    }
}

int elastic_bloom_filter_add(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
    if (ebf == NULL || data == NULL) return -1;
    platform_lock(&ebf->lock);
    for (uint32_t i = 0; i < ebf->hash_count; i++) {
        uint32_t fp;
        size_t index;
        compute_hash_pair(ebf, data, len, i, &fp, &index);
        bitset_set(ebf->bits, index, true);
        // index IS the bucket index (1:1 mapping per EBF paper)
        if (!bucket_contains(ebf->buckets[index], fp)) {
            bucket_insert(&ebf->buckets[index], fp);
        }
    }
    ebf->count++;
    float ratio = elastic_bloom_filter_ratio(ebf);
    platform_unlock(&ebf->lock);
    if (ratio > ebf->omega) {
        elastic_bloom_filter_expand(ebf);
    }
    return 0;
}

bool elastic_bloom_filter_contains(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
    if (ebf == NULL || data == NULL) return false;
    platform_lock(&ebf->lock);
    for (uint32_t i = 0; i < ebf->hash_count; i++) {
        uint32_t fp;
        size_t index;
        compute_hash_pair(ebf, data, len, i, &fp, &index);
        if (!bitset_get(ebf->bits, index)) {
            platform_unlock(&ebf->lock);
            return false;
        }
    }
    platform_unlock(&ebf->lock);
    return true;
}

int elastic_bloom_filter_remove(elastic_bloom_filter_t* ebf, const uint8_t* data, size_t len) {
    if (ebf == NULL || data == NULL) return -1;
    platform_lock(&ebf->lock);
    for (uint32_t i = 0; i < ebf->hash_count; i++) {
        uint32_t fp;
        size_t index;
        compute_hash_pair(ebf, data, len, i, &fp, &index);
        bucket_remove(&ebf->buckets[index], fp);
        if (bucket_is_empty(ebf->buckets[index])) {
            bitset_set(ebf->bits, index, false);
        }
    }
    if (ebf->count > 0) ebf->count--;
    float ratio = elastic_bloom_filter_ratio(ebf);
    platform_unlock(&ebf->lock);
    if (ratio < ebf->omega / 4.0f) {
        elastic_bloom_filter_compress(ebf);
    }
    return 0;
}

int elastic_bloom_filter_expand(elastic_bloom_filter_t* ebf) {
    if (ebf == NULL) return -1;
    platform_lock(&ebf->lock);
    size_t old_bucket_count = ebf->bucket_count;
    size_t new_bucket_count = old_bucket_count * 2;
    size_t new_size = ebf->size * 2;
    ebf_bucket_entry_t** new_buckets = get_clear_memory(sizeof(ebf_bucket_entry_t*) * new_bucket_count);
    for (size_t i = 0; i < old_bucket_count; i++) {
        ebf_bucket_entry_t* curr = ebf->buckets[i];
        ebf_bucket_entry_t* stay_head = NULL;
        ebf_bucket_entry_t* move_head = NULL;
        while (curr != NULL) {
            ebf_bucket_entry_t* next = curr->next;
            if ((curr->fingerprint & 1) == 0) {
                curr->fingerprint >>= 1;
                curr->next = stay_head;
                stay_head = curr;
            } else {
                curr->fingerprint >>= 1;
                curr->next = move_head;
                move_head = curr;
            }
            curr = next;
        }
        new_buckets[i] = stay_head;
        new_buckets[i + old_bucket_count] = move_head;
    }
    free(ebf->buckets);
    ebf->buckets = new_buckets;
    ebf->bucket_count = new_bucket_count;
    ebf->size = new_size;
    rebuild_bitset(ebf);
    platform_unlock(&ebf->lock);
    return 0;
}

int elastic_bloom_filter_compress(elastic_bloom_filter_t* ebf) {
    if (ebf == NULL || ebf->bucket_count < 2) return -1;
    platform_lock(&ebf->lock);
    size_t old_bucket_count = ebf->bucket_count;
    size_t new_bucket_count = old_bucket_count / 2;
    size_t new_size = ebf->size / 2;
    for (size_t i = 0; i < new_bucket_count; i++) {
        ebf_bucket_entry_t* first = ebf->buckets[i];
        ebf_bucket_entry_t* second = ebf->buckets[i + new_bucket_count];
        ebf_bucket_entry_t* merged = NULL;
        // Second bucket entries: prepend bit 1
        ebf_bucket_entry_t* curr = second;
        while (curr != NULL) {
            curr->fingerprint = (curr->fingerprint << 1) | 1;
            ebf_bucket_entry_t* next = curr->next;
            curr->next = merged;
            merged = curr;
            curr = next;
        }
        // First bucket entries: prepend bit 0
        curr = first;
        while (curr != NULL) {
            curr->fingerprint = (curr->fingerprint << 1) | 0;
            ebf_bucket_entry_t* next = curr->next;
            curr->next = merged;
            merged = curr;
            curr = next;
        }
        ebf->buckets[i] = merged;
    }
    ebf->bucket_count = new_bucket_count;
    ebf->size = new_size;
    ebf_bucket_entry_t** new_arr = realloc(ebf->buckets, sizeof(ebf_bucket_entry_t*) * new_bucket_count);
    if (new_arr) ebf->buckets = new_arr;
    rebuild_bitset(ebf);
    platform_unlock(&ebf->lock);
    return 0;
}

int elastic_bloom_filter_merge(elastic_bloom_filter_t* dest, const elastic_bloom_filter_t* src) {
    if (dest == NULL || src == NULL) return -1;
    if (dest->size != src->size || dest->bucket_count != src->bucket_count) return -2;
    platform_lock(&dest->lock);
    bitset_t* merged_bits = bitset_or(dest->bits, src->bits);
    bitset_destroy(dest->bits);
    dest->bits = merged_bits;
    for (size_t i = 0; i < dest->bucket_count; i++) {
        ebf_bucket_entry_t* curr = src->buckets[i];
        while (curr != NULL) {
            if (!bucket_contains(dest->buckets[i], curr->fingerprint)) {
                bucket_insert(&dest->buckets[i], curr->fingerprint);
            }
            curr = curr->next;
        }
    }
    dest->count += src->count;
    platform_unlock(&dest->lock);
    return 0;
}

size_t elastic_bloom_filter_count(elastic_bloom_filter_t* ebf) {
    return ebf->count;
}

size_t elastic_bloom_filter_size(elastic_bloom_filter_t* ebf) {
    return ebf->size;
}

float elastic_bloom_filter_ratio(elastic_bloom_filter_t* ebf) {
    if (ebf->bucket_count == 0) return 0.0f;
    size_t occupied = 0;
    for (size_t i = 0; i < ebf->bucket_count; i++) {
        if (!bucket_is_empty(ebf->buckets[i])) occupied++;
    }
    return (float)occupied / (float)ebf->bucket_count;
}

cbor_item_t* elastic_bloom_filter_encode(const elastic_bloom_filter_t* ebf) {
    if (ebf == NULL) return NULL;

    platform_lock(&((elastic_bloom_filter_t*)ebf)->lock);

    /* Count occupied buckets for sparse encoding */
    size_t num_occupied = 0;
    for (size_t i = 0; i < ebf->bucket_count; i++) {
        if (!bucket_is_empty(ebf->buckets[i])) num_occupied++;
    }

    /* 8 fixed fields + one sub-array per occupied bucket */
    cbor_item_t* arr = cbor_new_definite_array(8 + num_occupied);
    if (arr == NULL) goto fail;

    /* Fixed metadata fields */
    if (!cbor_array_push(arr, cbor_build_uint64((uint64_t)ebf->size))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint32(ebf->hash_count))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint32(ebf->fp_bits))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint64(ebf->seed_a))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint64(ebf->seed_b))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint64((uint64_t)ebf->bits->size))) goto fail;
    if (!cbor_array_push(arr, cbor_build_bytestring(ebf->bits->data, ebf->bits->size))) goto fail;
    if (!cbor_array_push(arr, cbor_build_uint64((uint64_t)num_occupied))) goto fail;

    /* Sparse bucket encoding: only non-empty buckets */
    for (size_t i = 0; i < ebf->bucket_count; i++) {
        if (bucket_is_empty(ebf->buckets[i])) continue;

        /* Count fingerprints in this bucket */
        size_t fp_count = 0;
        ebf_bucket_entry_t* curr = ebf->buckets[i];
        while (curr != NULL) {
            fp_count++;
            curr = curr->next;
        }

        /* Sub-array: [bucket_index, num_fps, fp1, fp2, ...] */
        cbor_item_t* bucket_arr = cbor_new_definite_array(2 + fp_count);
        if (bucket_arr == NULL) goto fail;

        if (!cbor_array_push(bucket_arr, cbor_build_uint64((uint64_t)i))) goto fail_bucket;
        if (!cbor_array_push(bucket_arr, cbor_build_uint64((uint64_t)fp_count))) goto fail_bucket;

        curr = ebf->buckets[i];
        while (curr != NULL) {
            if (!cbor_array_push(bucket_arr, cbor_build_uint32(curr->fingerprint))) goto fail_bucket;
            curr = curr->next;
        }

        if (!cbor_array_push(arr, bucket_arr)) goto fail_bucket;
        continue;

    fail_bucket:
        cbor_decref(&bucket_arr);
        goto fail;
    }

    platform_unlock(&((elastic_bloom_filter_t*)ebf)->lock);
    return arr;

fail:
    platform_unlock(&((elastic_bloom_filter_t*)ebf)->lock);
    if (arr != NULL) cbor_decref(&arr);
    return NULL;
}

elastic_bloom_filter_t* elastic_bloom_filter_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_isa_array(item) || !cbor_array_is_definite(item))
        return NULL;

    cbor_item_t** elems = cbor_array_handle(item);
    size_t arr_size = cbor_array_size(item);
    if (arr_size < 8) return NULL;

    /* Read fixed metadata */
    uint64_t size = cbor_get_uint64(elems[0]);
    uint32_t hash_count = (uint32_t)cbor_get_int(elems[1]);
    uint32_t fp_bits = (uint32_t)cbor_get_int(elems[2]);
    uint64_t seed_a = cbor_get_uint64(elems[3]);
    uint64_t seed_b = cbor_get_uint64(elems[4]);
    uint64_t bitset_byte_len = cbor_get_uint64(elems[5]);

    if (!cbor_isa_bytestring(elems[6])) return NULL;
    if (cbor_bytestring_length(elems[6]) != bitset_byte_len) return NULL;

    uint64_t num_occupied = cbor_get_uint64(elems[7]);

    /* Validate: 8 fixed + num_occupied bucket sub-arrays */
    if (arr_size != 8 + (size_t)num_occupied) return NULL;

    /* Create filter (omega=0.5 is placeholder; not transmitted) */
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create((size_t)size, hash_count, 0.5f, fp_bits);
    if (ebf == NULL) return NULL;

    /* Overwrite seeds that create() defaulted to 1/2 */
    ebf->seed_a = seed_a;
    ebf->seed_b = seed_b;

    /* Replace bitset with decoded data */
    bitset_destroy(ebf->bits);
    ebf->bits = bitset_create((size_t)bitset_byte_len);
    if (ebf->bits == NULL) {
        elastic_bloom_filter_destroy(ebf);
        return NULL;
    }
    memcpy(ebf->bits->data, cbor_bytestring_handle(elems[6]), (size_t)bitset_byte_len);

    /* Reconstruct sparse buckets */
    for (size_t k = 0; k < (size_t)num_occupied; k++) {
        cbor_item_t* bucket_item = elems[8 + k];
        if (!cbor_isa_array(bucket_item) || !cbor_array_is_definite(bucket_item)) {
            elastic_bloom_filter_destroy(ebf);
            return NULL;
        }
        cbor_item_t** bucket_elems = cbor_array_handle(bucket_item);
        size_t bucket_arr_size = cbor_array_size(bucket_item);
        if (bucket_arr_size < 2) {
            elastic_bloom_filter_destroy(ebf);
            return NULL;
        }

        size_t bucket_index = (size_t)cbor_get_uint64(bucket_elems[0]);
        uint64_t fp_count = cbor_get_uint64(bucket_elems[1]);

        if (bucket_arr_size != 2 + (size_t)fp_count || bucket_index >= ebf->bucket_count) {
            elastic_bloom_filter_destroy(ebf);
            return NULL;
        }

        /* Insert fingerprints in reverse order so the linked list matches original */
        for (uint64_t f = fp_count; f > 0; f--) {
            uint32_t fp = (uint32_t)cbor_get_int(bucket_elems[1 + f]);
            bucket_insert(&ebf->buckets[bucket_index], fp);
        }
    }

    /* Rebuild bitset from buckets for consistency */
    rebuild_bitset(ebf);

    return ebf;
}