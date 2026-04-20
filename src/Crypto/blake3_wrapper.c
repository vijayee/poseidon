//
// Created by victor on 4/20/26.
//

#include "blake3_wrapper.h"
#include <blake3.h>

int poseidon_blake3_hash(const uint8_t* input, size_t input_len, uint8_t* output) {
    if (input == NULL && input_len > 0) return -1;
    if (output == NULL) return -1;

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, input, input_len);
    blake3_hasher_finalize(&hasher, output, POSEIDON_BLAKE3_HASH_SIZE);
    return 0;
}