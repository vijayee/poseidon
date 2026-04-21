//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_BASE58_H
#define POSEIDON_BASE58_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encodes raw bytes to a Base58 string using the Bitcoin alphabet.
 * Leading zero bytes in input become leading '1' characters in output.
 *
 * @param input       Raw bytes to encode
 * @param input_len   Number of input bytes
 * @param output      Output buffer for Base58 string (not null-terminated)
 * @param output_size Size of output buffer
 * @return            Number of characters written, or -1 on overflow/error
 */
int base58_encode(const uint8_t* input, size_t input_len, char* output, size_t output_size);

/**
 * Decodes a Base58 string to raw bytes using the Bitcoin alphabet.
 * Leading '1' characters become leading zero bytes.
 *
 * @param input         Base58 string to decode (null-terminated)
 * @param output        Output buffer for raw bytes
 * @param output_size   Size of output buffer
 * @param bytes_written Output: number of bytes written
 * @return              0 on success, -1 on error (invalid chars, overflow)
 */
int base58_decode(const char* input, uint8_t* output, size_t output_size, size_t* bytes_written);

/**
 * Returns the maximum encoded length (excluding null terminator) for input_len bytes.
 */
size_t base58_encoded_length(size_t input_len);

/**
 * Returns the maximum decoded length for a Base58 string of str_len characters.
 */
size_t base58_decoded_length(size_t str_len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_BASE58_H