//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_BITSET_H
#define POSEIDON_BITSET_H

#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bitset_t {
    refcounter_t refcounter;
    uint8_t* data;
    size_t size;          // bytes (not bits)
    PLATFORMLOCKTYPE(lock);
} bitset_t;

bitset_t* bitset_create(size_t byte_size);
void bitset_destroy(bitset_t* set);
bool bitset_get(bitset_t* set, size_t bit_index);
bool bitset_set(bitset_t* set, size_t bit_index, bool value);
bool bitset_update(bitset_t* set, size_t bit_index, bool value);
int8_t bitset_compare(bitset_t* a, bitset_t* b);
bool bitset_eq(bitset_t* a, bitset_t* b);
bitset_t* bitset_xor(bitset_t* a, bitset_t* b);
bitset_t* bitset_and(bitset_t* a, bitset_t* b);
bitset_t* bitset_or(bitset_t* a, bitset_t* b);
bitset_t* bitset_not(bitset_t* a);
size_t bitset_size(bitset_t* set);
size_t bitset_bit_count(bitset_t* set);
void bitset_compact(bitset_t* set);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_BITSET_H