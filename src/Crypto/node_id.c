//
// Created by victor on 4/20/26.
//

#include "node_id.h"
#include "blake3_wrapper.h"
#include "../Util/base58.h"
#include <string.h>

int poseidon_node_id_from_public_key(const uint8_t* pub_key, size_t len, poseidon_node_id_t* id) {
    if (pub_key == NULL || id == NULL) return -1;

    // BLAKE3 hash of the public key
    if (poseidon_blake3_hash(pub_key, len, id->hash) != 0) return -1;

    // Base58 encode the hash into the string field (reserve 1 byte for null terminator)
    int encoded_len = base58_encode(id->hash, POSEIDON_NODE_ID_HASH_SIZE,
                                    id->str, POSEIDON_NODE_ID_STRING_SIZE - 1);
    if (encoded_len < 0) return -1;

    id->str[encoded_len] = '\0';
    return 0;
}

int poseidon_node_id_from_string(const char* str, poseidon_node_id_t* id) {
    if (str == NULL || id == NULL) return -1;

    // Copy string
    size_t str_len = strlen(str);
    if (str_len >= POSEIDON_NODE_ID_STRING_SIZE) return -1;
    memcpy(id->str, str, str_len + 1);

    // Decode Base58 into hash
    size_t bytes_written = 0;
    if (base58_decode(str, id->hash, POSEIDON_NODE_ID_HASH_SIZE, &bytes_written) != 0) {
        return -1;
    }

    // Zero-fill remaining bytes (data stays at the beginning, trailing zeros)
    if (bytes_written < POSEIDON_NODE_ID_HASH_SIZE) {
        memset(id->hash + bytes_written, 0, POSEIDON_NODE_ID_HASH_SIZE - bytes_written);
    }

    return 0;
}

int poseidon_node_id_compare(const poseidon_node_id_t* a, const poseidon_node_id_t* b) {
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    int rc = memcmp(a->hash, b->hash, POSEIDON_NODE_ID_HASH_SIZE);
    return (rc < 0) ? -1 : (rc > 0) ? 1 : 0;
}

bool poseidon_node_id_equals(const poseidon_node_id_t* a, const poseidon_node_id_t* b) {
    return poseidon_node_id_compare(a, b) == 0;
}

bool poseidon_node_id_is_null(const poseidon_node_id_t* id) {
    if (id == NULL) return true;
    for (size_t i = 0; i < POSEIDON_NODE_ID_HASH_SIZE; i++) {
        if (id->hash[i] != 0) return false;
    }
    return true;
}

void poseidon_node_id_clear(poseidon_node_id_t* id) {
    if (id == NULL) return;
    memset(id->hash, 0, POSEIDON_NODE_ID_HASH_SIZE);
    memset(id->str, 0, POSEIDON_NODE_ID_STRING_SIZE);
}

uint64_t poseidon_node_id_hash(const poseidon_node_id_t* id) {
    if (id == NULL) return 0;
    uint64_t val;
    memcpy(&val, id->hash, sizeof(uint64_t));
    return val;
}