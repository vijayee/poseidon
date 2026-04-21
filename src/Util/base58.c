//
// Created by victor on 4/20/26.
//

#include "base58.h"
#include "allocator.h"
#include <string.h>

// Bitcoin alphabet: no 0, O, I, l to avoid human transcription errors
static const char BASE58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Lookup table for decoding (built once)
static const int8_t BASE58_MAP[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8,-1,-1,-1,-1,-1,-1,
    -1, 9,10,11,12,13,14,15,16,-1,17,18,19,20,21,-1,
    22,23,24,25,26,27,28,29,30,31,32,-1,-1,-1,-1,-1,
    -1,33,34,35,36,37,38,39,40,41,42,43,-1,44,45,46,
    47,48,49,50,51,52,53,54,55,56,57,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

// Sufficient for 32-byte inputs (BLAKE3 hashes)
#define BIGINT_SIZE 200

// ============================================================================
// BIG INTEGER HELPERS
// ============================================================================

static void bigint_reverse(uint8_t* data, size_t len) {
    for (size_t i = 0, j = len - 1; i < j; i++, j--) {
        uint8_t tmp = data[i];
        data[i] = data[j];
        data[j] = tmp;
    }
}

static void bigint_multiply(uint8_t* result, size_t* len, uint32_t factor) {
    uint32_t carry = 0;
    for (size_t i = 0; i < *len; i++) {
        uint32_t val = (uint32_t)result[i] * factor + carry;
        result[i] = (uint8_t)(val & 0xFF);
        carry = val >> 8;
    }
    while (carry > 0) {
        result[*len] = (uint8_t)(carry & 0xFF);
        (*len)++;
        carry >>= 8;
    }
}

static void bigint_add(uint8_t* result, size_t* len, uint32_t value) {
    uint32_t carry = value;
    for (size_t i = 0; i < *len && carry > 0; i++) {
        uint32_t val = (uint32_t)result[i] + carry;
        result[i] = (uint8_t)(val & 0xFF);
        carry = val >> 8;
    }
    while (carry > 0) {
        result[*len] = (uint8_t)(carry & 0xFF);
        (*len)++;
        carry >>= 8;
    }
}

static uint8_t bigint_divmod(uint8_t* result, size_t* len, uint32_t divisor) {
    uint32_t remainder = 0;
    for (size_t i = *len; i > 0; i--) {
        uint32_t val = (remainder << 8) | result[i - 1];
        result[i - 1] = (uint8_t)(val / divisor);
        remainder = val % divisor;
    }
    // Trim leading zeros
    while (*len > 0 && result[*len - 1] == 0) {
        (*len)--;
    }
    return (uint8_t)remainder;
}

// ============================================================================
// PUBLIC API
// ============================================================================

size_t base58_encoded_length(size_t input_len) {
    return input_len * 138 / 100 + 1;
}

size_t base58_decoded_length(size_t str_len) {
    return str_len * 733 / 1000 + 1;
}

int base58_encode(const uint8_t* input, size_t input_len, char* output, size_t output_size) {
    if (output == NULL) return -1;
    if (input == NULL && input_len > 0) return -1;
    if (input_len == 0) {
        if (output_size < 1) return -1;
        return 0;
    }
    if (input_len > BIGINT_SIZE / 2) return -1;

    // Count leading zero bytes
    size_t leading_zeros = 0;
    while (leading_zeros < input_len && input[leading_zeros] == 0) {
        leading_zeros++;
    }

    // Convert input bytes to big integer (little-endian)
    uint8_t* bigint = get_clear_memory(BIGINT_SIZE);
    size_t bigint_len = 0;

    for (size_t i = 0; i < input_len; i++) {
        bigint_multiply(bigint, &bigint_len, 256);
        bigint_add(bigint, &bigint_len, input[i]);
    }

    // Extract Base58 digits (LSB first via divmod)
    uint8_t* digits = get_clear_memory(BIGINT_SIZE);
    size_t digits_len = 0;

    while (bigint_len > 0) {
        digits[digits_len++] = bigint_divmod(bigint, &bigint_len, 58);
    }

    // Check output has room: leading '1' chars + digits
    size_t total_len = leading_zeros + digits_len;
    if (output_size < total_len) {
        free(bigint);
        free(digits);
        return -1;
    }

    // Write leading '1' characters for each leading zero byte
    size_t pos = 0;
    for (size_t i = 0; i < leading_zeros; i++) {
        output[pos++] = '1';
    }

    // Write Base58 digits in reverse (most significant first)
    for (size_t i = digits_len; i > 0; i--) {
        output[pos++] = BASE58_ALPHABET[digits[i - 1]];
    }

    free(bigint);
    free(digits);
    return (int)pos;
}

int base58_decode(const char* input, uint8_t* output, size_t output_size, size_t* bytes_written) {
    if (input == NULL || output == NULL) return -1;
    if (bytes_written != NULL) *bytes_written = 0;

    size_t input_len = strlen(input);
    if (input_len == 0) return 0;
    if (input_len > BIGINT_SIZE) return -1;

    // Count leading '1' characters
    size_t leading_ones = 0;
    while (leading_ones < input_len && input[leading_ones] == '1') {
        leading_ones++;
    }

    // Convert Base58 digits to big integer (little-endian)
    uint8_t* bigint = get_clear_memory(BIGINT_SIZE);
    size_t bigint_len = 0;

    for (size_t i = leading_ones; i < input_len; i++) {
        int8_t val = BASE58_MAP[(unsigned char)input[i]];
        if (val < 0) {
            free(bigint);
            return -1;
        }
        bigint_multiply(bigint, &bigint_len, 58);
        bigint_add(bigint, &bigint_len, (uint32_t)val);
    }

    // Extract bytes (little-endian via divmod by 256)
    uint8_t* bytes = get_clear_memory(BIGINT_SIZE);
    size_t bytes_len = 0;

    while (bigint_len > 0) {
        bytes[bytes_len++] = bigint_divmod(bigint, &bigint_len, 256);
    }

    // Reverse to get big-endian
    if (bytes_len > 0) {
        bigint_reverse(bytes, bytes_len);
    }

    // Check output has room: leading zeros + decoded bytes
    size_t total_len = leading_ones + bytes_len;
    if (output_size < total_len) {
        free(bigint);
        free(bytes);
        return -1;
    }

    // Write leading zero bytes
    size_t pos = 0;
    for (size_t i = 0; i < leading_ones; i++) {
        output[pos++] = 0;
    }

    // Write decoded bytes
    memcpy(output + pos, bytes, bytes_len);
    pos += bytes_len;

    if (bytes_written != NULL) *bytes_written = pos;

    free(bigint);
    free(bytes);
    return 0;
}