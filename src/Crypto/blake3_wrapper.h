//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_BLAKE3_WRAPPER_H
#define POSEIDON_BLAKE3_WRAPPER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLAKE3 output length in bytes */
#define POSEIDON_BLAKE3_HASH_SIZE 32

/**
 * Computes a BLAKE3 hash of the input data.
 * One-shot convenience wrapper over the BLAKE3 C API.
 *
 * @param input     Data to hash
 * @param input_len Length of input data
 * @param output    Output buffer (must have space for POSEIDON_BLAKE3_HASH_SIZE bytes)
 * @return          0 on success, -1 on failure (NULL input with non-zero length)
 */
int poseidon_blake3_hash(const uint8_t* input, size_t input_len, uint8_t* output);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_BLAKE3_WRAPPER_H