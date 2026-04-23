//
// Created by victor on 4/20/26.
//

#include "key_pair.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/pem.h>
#include <openssl/err.h>

// ============================================================================
// KEY GENERATION HELPERS
// ============================================================================

static EVP_PKEY* generate_ed25519(void) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(NULL, "ED25519", NULL);
    if (ctx == NULL) return NULL;

    EVP_PKEY* pkey = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static EVP_PKEY* generate_rsa(void) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (ctx == NULL) return NULL;

    EVP_PKEY* pkey = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static EVP_PKEY* generate_ec(void) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) return NULL;

    EVP_PKEY* pkey = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return pkey;
}

static char* strdup_key_type(const char* key_type) {
    if (key_type == NULL) key_type = "ED25519";
    size_t len = strlen(key_type);
    char* copy = get_memory(len + 1);
    memcpy(copy, key_type, len + 1);
    return copy;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_key_pair_t* poseidon_key_pair_create(const char* key_type) {
    if (key_type == NULL) key_type = "ED25519";

    EVP_PKEY* pkey = NULL;
    if (strcmp(key_type, "ED25519") == 0) {
        pkey = generate_ed25519();
    } else if (strcmp(key_type, "RSA") == 0) {
        pkey = generate_rsa();
    } else if (strcmp(key_type, "EC") == 0) {
        pkey = generate_ec();
    } else {
        return NULL;
    }

    if (pkey == NULL) return NULL;

    poseidon_key_pair_t* kp = get_clear_memory(sizeof(poseidon_key_pair_t));
    kp->pkey = pkey;
    kp->key_type = strdup_key_type(key_type);
    refcounter_init((refcounter_t*)kp);
    return kp;
}

void poseidon_key_pair_destroy(poseidon_key_pair_t* kp) {
    if (kp == NULL) return;

    refcounter_dereference((refcounter_t*)kp);
    if (refcounter_count((refcounter_t*)kp) == 0) {
        if (kp->pkey != NULL) EVP_PKEY_free(kp->pkey);
        if (kp->key_type != NULL) free(kp->key_type);
        free(kp);
    }
}

// ============================================================================
// PUBLIC KEY EXTRACTION
// ============================================================================

int poseidon_key_pair_get_public_key(poseidon_key_pair_t* kp, uint8_t** output, size_t* out_len) {
    if (kp == NULL || output == NULL || out_len == NULL) return -1;

    // DER-encoded SubjectPublicKeyInfo — works for any algorithm
    int len = i2d_PUBKEY(kp->pkey, NULL);
    if (len <= 0) return -1;

    uint8_t* buf = get_clear_memory((size_t)len);
    uint8_t* p = buf;
    if (i2d_PUBKEY(kp->pkey, &p) <= 0) {
        free(buf);
        return -1;
    }

    *output = buf;
    *out_len = (size_t)len;
    return 0;
}

const char* poseidon_key_pair_get_key_type(const poseidon_key_pair_t* kp) {
    if (kp == NULL) return NULL;
    return kp->key_type;
}

// ============================================================================
// PEM I/O
// ============================================================================

int poseidon_key_pair_save_private_key(poseidon_key_pair_t* kp, const char* filepath) {
    if (kp == NULL || filepath == NULL) return -1;

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    FILE* fp = fdopen(fd, "w");
    if (fp == NULL) {
        close(fd);
        return -1;
    }

    int ret = PEM_write_PrivateKey(fp, kp->pkey, NULL, NULL, 0, NULL, NULL);
    fclose(fp);
    return (ret == 1) ? 0 : -1;
}

int poseidon_key_pair_save_public_key(poseidon_key_pair_t* kp, const char* filepath) {
    if (kp == NULL || filepath == NULL) return -1;

    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) return -1;

    int ret = PEM_write_PUBKEY(fp, kp->pkey);
    fclose(fp);
    return (ret == 1) ? 0 : -1;
}

poseidon_key_pair_t* poseidon_key_pair_load_from_pem(const char* filepath) {
    if (filepath == NULL) return NULL;

    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) return NULL;

    EVP_PKEY* pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);

    if (pkey == NULL) return NULL;

    // Determine key type from the EVP_PKEY
    int id = EVP_PKEY_base_id(pkey);
    const char* type_name;
    switch (id) {
        case EVP_PKEY_ED25519: type_name = "ED25519"; break;
        case EVP_PKEY_RSA:     type_name = "RSA"; break;
        case EVP_PKEY_EC:      type_name = "EC"; break;
        default:
            EVP_PKEY_free(pkey);
            return NULL;
    }

    poseidon_key_pair_t* kp = get_clear_memory(sizeof(poseidon_key_pair_t));
    kp->pkey = pkey;
    kp->key_type = strdup_key_type(type_name);
    refcounter_init((refcounter_t*)kp);
    return kp;
}

// ============================================================================
// CERTIFICATE GENERATION
// ============================================================================

X509* poseidon_key_pair_generate_certificate(poseidon_key_pair_t* kp, const char* node_id_str) {
    if (kp == NULL || node_id_str == NULL) return NULL;

    X509* cert = X509_new();
    if (cert == NULL) return NULL;

    // Version 3
    X509_set_version(cert, 2);

    // Serial number
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    // Validity: now + 3650 days (10 years)
    X509_gmtime_adj(X509_get0_notBefore(cert), 0);
    X509_gmtime_adj(X509_get0_notAfter(cert), 3650L * 24 * 60 * 60);

    // Set subject CN to node ID string
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (const unsigned char*)node_id_str, -1, -1, 0);
    X509_set_issuer_name(cert, name);

    // Set public key
    X509_set_pubkey(cert, kp->pkey);

    // Ed25519 requires NULL digest (RFC 8410), others use SHA-256
    const EVP_MD* md = NULL;
    if (EVP_PKEY_base_id(kp->pkey) == EVP_PKEY_ED25519) {
        md = NULL;
    } else {
        md = EVP_sha256();
    }

    if (X509_sign(cert, kp->pkey, md) <= 0) {
        X509_free(cert);
        return NULL;
    }

    return cert;
}

int poseidon_certificate_save_to_pem(X509* cert, const char* filepath) {
    if (cert == NULL || filepath == NULL) return -1;

    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) return -1;

    int ret = PEM_write_X509(fp, cert);
    fclose(fp);
    return (ret == 1) ? 0 : -1;
}

int poseidon_key_pair_generate_tls_files(poseidon_key_pair_t* kp, const char* node_id_str,
                                          const char* key_path, const char* cert_path) {
    if (kp == NULL || node_id_str == NULL || key_path == NULL || cert_path == NULL) return -1;

    X509* cert = poseidon_key_pair_generate_certificate(kp, node_id_str);
    if (cert == NULL) return -1;

    if (poseidon_key_pair_save_private_key(kp, key_path) != 0) {
        X509_free(cert);
        return -1;
    }

    if (poseidon_certificate_save_to_pem(cert, cert_path) != 0) {
        X509_free(cert);
        return -1;
    }

    X509_free(cert);
    return 0;
}

int poseidon_key_pair_sign(poseidon_key_pair_t* kp,
                            const uint8_t* data, size_t data_len,
                            uint8_t* sig_out, size_t* sig_len) {
    if (kp == NULL || data == NULL || sig_out == NULL || sig_len == NULL)
        return -1;

    if (EVP_PKEY_base_id(kp->pkey) != EVP_PKEY_ED25519) return -1;

    size_t sig_sz = EVP_PKEY_size(kp->pkey);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return -1;

    int rc = -1;
    if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, kp->pkey) != 1) goto cleanup;
    if (EVP_DigestSign(ctx, sig_out, &sig_sz, data, data_len) != 1) goto cleanup;

    *sig_len = sig_sz;
    rc = 0;

cleanup:
    EVP_MD_CTX_free(ctx);
    return rc;
}

int poseidon_verify_signature(const char* topic_id_str,
                               const uint8_t* data, size_t data_len,
                               const uint8_t* signature, size_t sig_len) {
    (void)topic_id_str;
    (void)data;
    (void)data_len;
    (void)signature;
    (void)sig_len;
    return -1;
}

int poseidon_verify_signature_with_key(const char* key_type,
                                        const uint8_t* public_key, size_t key_len,
                                        const uint8_t* data, size_t data_len,
                                        const uint8_t* signature, size_t sig_len) {
    if (key_type == NULL || public_key == NULL || data == NULL || signature == NULL)
        return -1;

    EVP_PKEY* pkey = NULL;
    {
        // All key types are DER-encoded SubjectPublicKeyInfo from poseidon_key_pair_get_public_key
        const unsigned char* p = public_key;
        pkey = d2i_PUBKEY(NULL, &p, (long)key_len);
    }
    if (pkey == NULL) return -1;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == NULL) { EVP_PKEY_free(pkey); return -1; }

    int result = -1;
    if (strcmp(key_type, "ED25519") == 0) {
        if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) == 1) {
            result = (EVP_DigestVerify(ctx, signature, sig_len,
                                       data, data_len) == 1) ? 0 : -1;
        }
    } else if (strcmp(key_type, "RSA") == 0) {
        EVP_PKEY_CTX* pctx = NULL;
        if (EVP_DigestVerifyInit(ctx, EVP_sha256(), &pctx, NULL, pkey) == 1) {
            result = (EVP_DigestVerify(ctx, signature, sig_len,
                                       data, data_len) == 1) ? 0 : -1;
        }
    } else if (strcmp(key_type, "EC") == 0) {
        EVP_PKEY_CTX* pctx = NULL;
        if (EVP_DigestVerifyInit(ctx, EVP_sha256(), &pctx, NULL, pkey) == 1) {
            result = (EVP_DigestVerify(ctx, signature, sig_len,
                                       data, data_len) == 1) ? 0 : -1;
        }
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return result;
}