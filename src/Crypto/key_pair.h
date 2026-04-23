//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_KEY_PAIR_H
#define POSEIDON_KEY_PAIR_H

#include <stdint.h>
#include <stddef.h>
#include "../RefCounter/refcounter.h"
#include <openssl/evp.h>
#include <openssl/x509.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A cryptographic key pair supporting any OpenSSL EVP algorithm.
 * Reference-counted for shared ownership across threads.
 *
 * Supports Ed25519 (default), RSA, EC, and any algorithm available
 * through OpenSSL's EVP_PKEY API.
 */
typedef struct poseidon_key_pair_t {
    refcounter_t refcounter;   /**< Must be first member */
    EVP_PKEY* pkey;            /**< OpenSSL key pair handle */
    char* key_type;            /**< Algorithm name (e.g., "ED25519", "RSA", "EC") */
} poseidon_key_pair_t;

/**
 * Generates a new key pair.
 *
 * @param key_type  Algorithm name: "ED25519" (default if NULL), "RSA", "EC", etc.
 * @return          New key pair with refcount=1, or NULL on failure
 */
poseidon_key_pair_t* poseidon_key_pair_create(const char* key_type);

/**
 * Destroys a key pair (reference-counted).
 */
void poseidon_key_pair_destroy(poseidon_key_pair_t* kp);

/**
 * Extracts the DER-encoded SubjectPublicKeyInfo from the key pair.
 * Caller must free the returned buffer.
 *
 * @param kp       Key pair
 * @param output   Output: allocated buffer with DER-encoded public key
 * @param out_len  Output: length of the DER buffer
 * @return         0 on success, -1 on failure
 */
int poseidon_key_pair_get_public_key(poseidon_key_pair_t* kp, uint8_t** output, size_t* out_len);

/**
 * Returns the algorithm name string (e.g., "ED25519").
 * Returned pointer is owned by the key pair — do not free.
 */
const char* poseidon_key_pair_get_key_type(const poseidon_key_pair_t* kp);

/**
 * Writes the private key to a PEM file (PKCS#8, unencrypted).
 *
 * @param kp       Key pair
 * @param filepath Path to write
 * @return         0 on success, -1 on failure
 */
int poseidon_key_pair_save_private_key(poseidon_key_pair_t* kp, const char* filepath);

/**
 * Writes the public key to a PEM file (SubjectPublicKeyInfo format).
 *
 * @param kp       Key pair
 * @param filepath Path to write
 * @return         0 on success, -1 on failure
 */
int poseidon_key_pair_save_public_key(poseidon_key_pair_t* kp, const char* filepath);

/**
 * Loads a private key from a PEM file.
 * Auto-detects the algorithm from the PEM data.
 *
 * @param filepath Path to PEM file
 * @return         New key pair with refcount=1, or NULL on failure
 */
poseidon_key_pair_t* poseidon_key_pair_load_from_pem(const char* filepath);

/**
 * Generates a self-signed X509 certificate for this key pair.
 * Sets CN to the node_id_str parameter.
 * Caller must free the returned certificate with X509_free().
 *
 * @param kp           Key pair to sign with
 * @param node_id_str  Node ID string used as CN (Common Name)
 * @return             New X509 certificate, or NULL on failure
 */
X509* poseidon_key_pair_generate_certificate(poseidon_key_pair_t* kp, const char* node_id_str);

/**
 * Writes an X509 certificate to a PEM file.
 *
 * @param cert     Certificate to write
 * @param filepath Path to write
 * @return         0 on success, -1 on failure
 */
int poseidon_certificate_save_to_pem(X509* cert, const char* filepath);

/**
 * Convenience: generates certificate and writes both key and cert PEM files.
 *
 * @param kp           Key pair
 * @param node_id_str  Node ID string for certificate CN
 * @param key_path     Output path for private key PEM
 * @param cert_path    Output path for certificate PEM
 * @return             0 on success, -1 on failure
 */
int poseidon_key_pair_generate_tls_files(poseidon_key_pair_t* kp, const char* node_id_str,
                                          const char* key_path, const char* cert_path);

/**
 * Signs data with an ED25519 private key.
 *
 * @param kp       Key pair containing the private key (must be ED25519)
 * @param data     Data to sign
 * @param data_len Length of data
 * @param sig_out  Output buffer for signature (must be at least 64 bytes)
 * @param sig_len  Output: actual signature length
 * @return         0 on success, -1 on failure
 */
int poseidon_key_pair_sign(poseidon_key_pair_t* kp,
                            const uint8_t* data, size_t data_len,
                            uint8_t* sig_out, size_t* sig_len);

/**
 * Verifies an ED25519 signature against a public key derived from node_id.
 * Legacy stub — callers should use poseidon_verify_signature_with_key instead.
 *
 * @param topic_id_str  Base58 topic ID string (encodes the public key)
 * @param data          Data that was signed
 * @param data_len      Length of data
 * @param signature     Signature bytes
 * @param sig_len       Length of signature
 * @return              0 if valid, -1 if invalid or error
 */
int poseidon_verify_signature(const char* topic_id_str,
                               const uint8_t* data, size_t data_len,
                               const uint8_t* signature, size_t sig_len);

/**
 * Verifies a signature against a DER-encoded SubjectPublicKeyInfo, dispatching by algorithm.
 * Supports ED25519 (pure EdDSA), RSA (PKCS1v15 + SHA-256), and EC (ECDSA + SHA-256).
 *
 * @param key_type   Algorithm name: "ED25519", "RSA", or "EC"
 * @param public_key DER-encoded SubjectPublicKeyInfo bytes (output of i2d_PUBKEY)
 * @param key_len    Length of public_key
 * @param data       Data that was signed
 * @param data_len   Length of data
 * @param signature  Signature bytes
 * @param sig_len    Length of signature
 * @return           0 if valid, -1 if invalid or error
 */
int poseidon_verify_signature_with_key(const char* key_type,
                                        const uint8_t* public_key, size_t key_len,
                                        const uint8_t* data, size_t data_len,
                                        const uint8_t* signature, size_t sig_len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_KEY_PAIR_H