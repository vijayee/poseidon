//
// Created by victor on 4/19/26.
//

#include "bitset.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

static void bitset_ensure_capacity(bitset_t* set, size_t byte_index) {
    if (byte_index < set->size) return;
    size_t new_size = byte_index + 1;
    uint8_t* new_data = get_clear_memory(new_size);
    if (set->data && set->size > 0) {
        memcpy(new_data, set->data, set->size);
    }
    free(set->data);
    set->data = new_data;
    set->size = new_size;
}

bitset_t* bitset_create(size_t byte_size) {
    bitset_t* set = get_clear_memory(sizeof(bitset_t));
    set->data = get_clear_memory(byte_size);
    set->size = byte_size;
    platform_lock_init(&set->lock);
    refcounter_init((refcounter_t*)set);
    return set;
}

void bitset_destroy(bitset_t* set) {
    if (set == NULL) return;
    refcounter_dereference((refcounter_t*)set);
    if (refcounter_count((refcounter_t*)set) == 0) {
        free(set->data);
        platform_lock_destroy(&set->lock);
        free(set);
    }
}

bool bitset_get(bitset_t* set, size_t bit_index) {
    size_t byte_index = bit_index / 8;
    size_t bit_offset = bit_index % 8;
    if (byte_index >= set->size) return false;
    return (set->data[byte_index] & (1 << bit_offset)) != 0;
}

bool bitset_set(bitset_t* set, size_t bit_index, bool value) {
    platform_lock(&set->lock);
    size_t byte_index = bit_index / 8;
    size_t bit_offset = bit_index % 8;
    bitset_ensure_capacity(set, byte_index);
    bool old = (set->data[byte_index] & (1 << bit_offset)) != 0;
    if (value) {
        set->data[byte_index] |= (1 << bit_offset);
    } else {
        set->data[byte_index] &= ~(1 << bit_offset);
    }
    platform_unlock(&set->lock);
    return old;
}

bool bitset_update(bitset_t* set, size_t bit_index, bool value) {
    return bitset_set(set, bit_index, value);
}

int8_t bitset_compare(bitset_t* a, bitset_t* b) {
    size_t length = a->size < b->size ? a->size : b->size;
    size_t a_tail = a->size;
    size_t b_tail = b->size;
    for (size_t i = 0; i < length; i++) {
        if (a->data[i] != b->data[i]) {
            a_tail = a->data[i];
            b_tail = b->data[i];
            break;
        }
    }
    if (a_tail > b_tail) return 1;
    if (b_tail > a_tail) return -1;
    return 0;
}

bool bitset_eq(bitset_t* a, bitset_t* b) {
    return bitset_compare(a, b) == 0;
}

static bitset_t* bitset_bitwise_op(bitset_t* a, bitset_t* b, uint8_t(*op)(uint8_t, uint8_t)) {
    size_t size = a->size > b->size ? a->size : b->size;
    bitset_t* result = bitset_create(size);
    size_t min = a->size < b->size ? a->size : b->size;
    for (size_t i = 0; i < min; i++) {
        result->data[i] = op(a->data[i], b->data[i]);
    }
    bitset_t* longer = a->size >= b->size ? a : b;
    for (size_t i = min; i < longer->size; i++) {
        result->data[i] = op(longer->data[i], 0);
    }
    return result;
}

static uint8_t op_xor(uint8_t a, uint8_t b) { return a ^ b; }
static uint8_t op_and(uint8_t a, uint8_t b) { return a & b; }
static uint8_t op_or(uint8_t a, uint8_t b) { return a | b; }

bitset_t* bitset_xor(bitset_t* a, bitset_t* b) {
    return bitset_bitwise_op(a, b, op_xor);
}

bitset_t* bitset_and(bitset_t* a, bitset_t* b) {
    return bitset_bitwise_op(a, b, op_and);
}

bitset_t* bitset_or(bitset_t* a, bitset_t* b) {
    return bitset_bitwise_op(a, b, op_or);
}

bitset_t* bitset_not(bitset_t* a) {
    bitset_t* result = bitset_create(a->size);
    for (size_t i = 0; i < a->size; i++) {
        result->data[i] = ~a->data[i];
    }
    return result;
}

size_t bitset_size(bitset_t* set) {
    return set->size;
}

size_t bitset_bit_count(bitset_t* set) {
    return set->size * 8;
}

void bitset_compact(bitset_t* set) {
    platform_lock(&set->lock);
    while (set->size > 1 && set->data[set->size - 1] == 0) {
        set->size--;
    }
    set->data = realloc(set->data, set->size);
    platform_unlock(&set->lock);
}