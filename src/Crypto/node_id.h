//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_NODE_ID_H
#define POSEIDON_NODE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLAKE3 hash output length (32 bytes) */
#define POSEIDON_NODE_ID_HASH_SIZE   32

/** Base58-encoded hash length (~44 chars + null + margin) */
#define POSEIDON_NODE_ID_STRING_SIZE 48

/**
 * Cryptographic node identity derived from a public key.
 * Hash is BLAKE3 of the DER-encoded SubjectPublicKeyInfo.
 * String is the Base58 encoding of that hash (Bitcoin alphabet).
 *
 * Both fields are populated at derivation time and immutable thereafter.
 */
typedef struct poseidon_node_id_t {
    uint8_t hash[POSEIDON_NODE_ID_HASH_SIZE];   /**< BLAKE3 of public key */
    char str[POSEIDON_NODE_ID_STRING_SIZE];      /**< Base58(hash), null-terminated */
} poseidon_node_id_t;

/**
 * Derives a node ID from raw public key bytes.
 * Computes BLAKE3(pub_key) then Base58-encodes the result.
 *
 * @param pub_key  Raw public key bytes (DER SubjectPublicKeyInfo)
 * @param len      Length of public key bytes
 * @param id       Output: populated node ID
 * @return         0 on success, -1 on failure
 */
int poseidon_node_id_from_public_key(const uint8_t* pub_key, size_t len, poseidon_node_id_t* id);

/**
 * Parses a Base58 node ID string into the struct.
 * Decodes Base58 → hash bytes.
 *
 * @param str  Base58-encoded node ID string (null-terminated)
 * @param id   Output: populated node ID
 * @return     0 on success, -1 on failure
 */
int poseidon_node_id_from_string(const char* str, poseidon_node_id_t* id);

/**
 * Compares two node IDs lexicographically by hash bytes.
 *
 * @param a  First node ID
 * @param b  Second node ID
 * @return   -1 if a < b, 0 if equal, 1 if a > b
 */
int poseidon_node_id_compare(const poseidon_node_id_t* a, const poseidon_node_id_t* b);

/**
 * Tests whether two node IDs are equal.
 */
bool poseidon_node_id_equals(const poseidon_node_id_t* a, const poseidon_node_id_t* b);

/**
 * Tests whether a node ID is null (all hash bytes zero, e.g. uninitialized).
 */
bool poseidon_node_id_is_null(const poseidon_node_id_t* id);

/**
 * Zeroes both hash and string fields.
 */
void poseidon_node_id_clear(poseidon_node_id_t* id);

/**
 * Returns a hash value suitable for hashmap use.
 * Reinterprets first 8 bytes of the hash as a uint64.
 */
uint64_t poseidon_node_id_hash(const poseidon_node_id_t* id);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_NODE_ID_H