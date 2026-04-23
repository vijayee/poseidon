//
// Cryptographic key pair supporting any OpenSSL EVP algorithm.
//

#ifndef POSEIDON_KEY_PAIR_H
#define POSEIDON_KEY_PAIR_H

#include <stdint.h>
#include <stddef.h>
#include "poseidon_refcounter.h"
#include <openssl/evp.h>
#include <openssl/x509.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct poseidon_key_pair_t {
    refcounter_t refcounter;
    EVP_PKEY* pkey;
    char* key_type;
} poseidon_key_pair_t;

poseidon_key_pair_t* poseidon_key_pair_create(const char* key_type);
void poseidon_key_pair_destroy(poseidon_key_pair_t* kp);

int poseidon_key_pair_get_public_key(poseidon_key_pair_t* kp, uint8_t** output, size_t* out_len);
const char* poseidon_key_pair_get_key_type(const poseidon_key_pair_t* kp);

int poseidon_key_pair_save_private_key(poseidon_key_pair_t* kp, const char* filepath);
int poseidon_key_pair_save_public_key(poseidon_key_pair_t* kp, const char* filepath);
poseidon_key_pair_t* poseidon_key_pair_load_from_pem(const char* filepath);

X509* poseidon_key_pair_generate_certificate(poseidon_key_pair_t* kp, const char* node_id_str);
int poseidon_certificate_save_to_pem(X509* cert, const char* filepath);

int poseidon_key_pair_generate_tls_files(poseidon_key_pair_t* kp, const char* node_id_str,
                                          const char* key_path, const char* cert_path);

int poseidon_key_pair_sign(poseidon_key_pair_t* kp,
                            const uint8_t* data, size_t data_len,
                            uint8_t* sig_out, size_t* sig_len);

int poseidon_verify_signature(const char* topic_id_str,
                               const uint8_t* data, size_t data_len,
                               const uint8_t* signature, size_t sig_len);

int poseidon_verify_signature_with_key(const char* key_type,
                                        const uint8_t* public_key, size_t key_len,
                                        const uint8_t* data, size_t data_len,
                                        const uint8_t* signature, size_t sig_len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_KEY_PAIR_H