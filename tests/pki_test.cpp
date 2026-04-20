//
// Created by victor on 4/20/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

extern "C" {
#include "Util/base58.h"
#include "Crypto/blake3_wrapper.h"
#include "Crypto/node_id.h"
#include "Crypto/key_pair.h"
}

// ============================================================================
// BASE58 TESTS
// ============================================================================

class Base58Test : public ::testing::Test {};

TEST_F(Base58Test, EncodeEmptyInput) {
    char output[64];
    int len = base58_encode((const uint8_t*)"", 0, output, sizeof(output));
    EXPECT_EQ(len, 0);
}

TEST_F(Base58Test, EncodeSingleByte) {
    uint8_t input[] = {0x00};
    char output[64];
    int len = base58_encode(input, 1, output, sizeof(output));
    EXPECT_GT(len, 0);
    EXPECT_EQ(output[0], '1');  // Leading zero byte → '1'
}

TEST_F(Base58Test, EncodeLeadingZeros) {
    uint8_t input[] = {0x00, 0x00, 0x01};
    char output[64];
    int len = base58_encode(input, 3, output, sizeof(output));
    EXPECT_GT(len, 0);
    // Two leading zeros → two '1' characters
    EXPECT_EQ(output[0], '1');
    EXPECT_EQ(output[1], '1');
}

TEST_F(Base58Test, EncodeKnownBitcoinVector) {
    // "Hello World!" → known Base58: "2NEpo7TZRRrLZSi2U"
    const char* input_str = "Hello World!";
    char output[64];
    int len = base58_encode((const uint8_t*)input_str, strlen(input_str), output, sizeof(output));
    EXPECT_GT(len, 0);
    output[len] = '\0';
    EXPECT_STREQ(output, "2NEpo7TZRRrLZSi2U");
}

TEST_F(Base58Test, EncodeDecodeRoundTrip) {
    uint8_t input[32];
    for (int i = 0; i < 32; i++) input[i] = (uint8_t)(i * 7 + 3);

    char encoded[64];
    int encoded_len = base58_encode(input, 32, encoded, sizeof(encoded));
    EXPECT_GT(encoded_len, 0);
    encoded[encoded_len] = '\0';

    uint8_t decoded[64];
    size_t decoded_len = 0;
    int rc = base58_decode(encoded, decoded, sizeof(decoded), &decoded_len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded_len, 32);
    EXPECT_EQ(memcmp(input, decoded, 32), 0);
}

TEST_F(Base58Test, DecodeInvalidChars) {
    const char* invalid = "0OIl";  // Characters not in Base58 alphabet
    uint8_t output[64];
    size_t bytes_written = 0;
    int rc = base58_decode(invalid, output, sizeof(output), &bytes_written);
    EXPECT_EQ(rc, -1);
}

TEST_F(Base58Test, DecodeBufferOverflow) {
    // Encode a long string
    uint8_t input[32];
    memset(input, 0xFF, 32);
    char encoded[64];
    int encoded_len = base58_encode(input, 32, encoded, sizeof(encoded));
    EXPECT_GT(encoded_len, 0);

    // Decode into a buffer that's too small
    uint8_t small_buf[1];
    size_t bytes_written = 0;
    int rc = base58_decode(encoded, small_buf, sizeof(small_buf), &bytes_written);
    EXPECT_EQ(rc, -1);
}

TEST_F(Base58Test, EncodedLengthAccuracy) {
    for (size_t len = 1; len <= 64; len++) {
        size_t est = base58_encoded_length(len);
        uint8_t data[64];
        memset(data, 0xAB, len);
        char buf[256];
        int actual = base58_encode(data, len, buf, sizeof(buf));
        EXPECT_GE((size_t)est, (size_t)actual);
    }
}

TEST_F(Base58Test, DecodeEmptyString) {
    uint8_t output[64];
    size_t bytes_written = 42;
    int rc = base58_decode("", output, sizeof(output), &bytes_written);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(bytes_written, (size_t)0);
}

// ============================================================================
// BLAKE3 TESTS
// ============================================================================

class Blake3Test : public ::testing::Test {};

TEST_F(Blake3Test, KnownVector) {
    // BLAKE3("abc") verified against reference implementation
    const uint8_t input[] = {'a', 'b', 'c'};
    uint8_t output[POSEIDON_BLAKE3_HASH_SIZE];
    int rc = poseidon_blake3_hash(input, 3, output);
    EXPECT_EQ(rc, 0);

    uint8_t expected[] = {
        0x64, 0x37, 0xb3, 0xac, 0x38, 0x46, 0x51, 0x33,
        0xff, 0xb6, 0x3b, 0x75, 0x27, 0x3a, 0x8d, 0xb5,
        0x48, 0xc5, 0x58, 0x46, 0x5d, 0x79, 0xdb, 0x03,
        0xfd, 0x35, 0x9c, 0x6c, 0xd5, 0xbd, 0x9d, 0x85
    };
    EXPECT_EQ(memcmp(output, expected, 32), 0);
}

TEST_F(Blake3Test, EmptyInput) {
    uint8_t output[POSEIDON_BLAKE3_HASH_SIZE];
    int rc = poseidon_blake3_hash(NULL, 0, output);
    EXPECT_EQ(rc, 0);

    // BLAKE3("") = af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262
    uint8_t expected[] = {
        0xaf, 0x13, 0x49, 0xb9, 0xf5, 0xf9, 0xa1, 0xa6,
        0xa0, 0x40, 0x4d, 0xea, 0x36, 0xdc, 0xc9, 0x49,
        0x9b, 0xcb, 0x25, 0xc9, 0xad, 0xc1, 0x12, 0xb7,
        0xcc, 0x9a, 0x93, 0xca, 0xe4, 0x1f, 0x32, 0x62
    };
    EXPECT_EQ(memcmp(output, expected, 32), 0);
}

TEST_F(Blake3Test, NullInputWithLengthFails) {
    uint8_t output[32];
    int rc = poseidon_blake3_hash(NULL, 10, output);
    EXPECT_EQ(rc, -1);
}

// ============================================================================
// NODE ID TESTS
// ============================================================================

class NodeIdTest : public ::testing::Test {};

TEST_F(NodeIdTest, FromPublicKeyDerivation) {
    uint8_t pub_key[] = {0x01, 0x02, 0x03, 0x04};
    poseidon_node_id_t id;
    int rc = poseidon_node_id_from_public_key(pub_key, sizeof(pub_key), &id);
    EXPECT_EQ(rc, 0);
    EXPECT_FALSE(poseidon_node_id_is_null(&id));
    EXPECT_GT(strlen(id.str), (size_t)0);
}

TEST_F(NodeIdTest, FromStringRoundTrip) {
    uint8_t pub_key[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    poseidon_node_id_t original;
    int rc = poseidon_node_id_from_public_key(pub_key, sizeof(pub_key), &original);
    EXPECT_EQ(rc, 0);

    poseidon_node_id_t restored;
    rc = poseidon_node_id_from_string(original.str, &restored);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(poseidon_node_id_equals(&original, &restored));
}

TEST_F(NodeIdTest, CompareSame) {
    uint8_t key[] = {0x42};
    poseidon_node_id_t a, b;
    poseidon_node_id_from_public_key(key, sizeof(key), &a);
    poseidon_node_id_from_public_key(key, sizeof(key), &b);
    EXPECT_EQ(poseidon_node_id_compare(&a, &b), 0);
    EXPECT_TRUE(poseidon_node_id_equals(&a, &b));
}

TEST_F(NodeIdTest, CompareDifferent) {
    uint8_t key1[] = {0x01};
    uint8_t key2[] = {0x02};
    poseidon_node_id_t a, b;
    poseidon_node_id_from_public_key(key1, sizeof(key1), &a);
    poseidon_node_id_from_public_key(key2, sizeof(key2), &b);
    EXPECT_NE(poseidon_node_id_compare(&a, &b), 0);
    EXPECT_FALSE(poseidon_node_id_equals(&a, &b));
}

TEST_F(NodeIdTest, IsNullOnZeroed) {
    poseidon_node_id_t id;
    poseidon_node_id_clear(&id);
    EXPECT_TRUE(poseidon_node_id_is_null(&id));
}

TEST_F(NodeIdTest, ClearZeroes) {
    uint8_t key[] = {0xFF};
    poseidon_node_id_t id;
    poseidon_node_id_from_public_key(key, sizeof(key), &id);
    EXPECT_FALSE(poseidon_node_id_is_null(&id));
    poseidon_node_id_clear(&id);
    EXPECT_TRUE(poseidon_node_id_is_null(&id));
}

TEST_F(NodeIdTest, HashConsistency) {
    uint8_t key[] = {0x99};
    poseidon_node_id_t id;
    poseidon_node_id_from_public_key(key, sizeof(key), &id);
    uint64_t h1 = poseidon_node_id_hash(&id);
    uint64_t h2 = poseidon_node_id_hash(&id);
    EXPECT_EQ(h1, h2);
}

// ============================================================================
// KEY PAIR TESTS
// ============================================================================

class KeyPairTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Clean up temp files
        std::remove("/tmp/test_key.pem");
        std::remove("/tmp/test_cert.pem");
        std::remove("/tmp/test_pub.pem");
    }
};

TEST_F(KeyPairTest, CreateDestroyEd25519) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp), "ED25519");
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, CreateDestroyDefault) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create(NULL);
    ASSERT_NE(kp, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp), "ED25519");
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, CreateRSA) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("RSA");
    ASSERT_NE(kp, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp), "RSA");
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, CreateEC) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("EC");
    ASSERT_NE(kp, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp), "EC");
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, GetPublicKey) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t* pub_der = NULL;
    size_t pub_len = 0;
    int rc = poseidon_key_pair_get_public_key(kp, &pub_der, &pub_len);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(pub_der, nullptr);
    EXPECT_GT(pub_len, (size_t)0);

    free(pub_der);
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, DifferentPairsDifferentKeys) {
    poseidon_key_pair_t* kp1 = poseidon_key_pair_create("ED25519");
    poseidon_key_pair_t* kp2 = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp1, nullptr);
    ASSERT_NE(kp2, nullptr);

    uint8_t* pub1 = NULL;
    size_t pub1_len = 0;
    uint8_t* pub2 = NULL;
    size_t pub2_len = 0;

    poseidon_key_pair_get_public_key(kp1, &pub1, &pub1_len);
    poseidon_key_pair_get_public_key(kp2, &pub2, &pub2_len);

    // Different key pairs should have different public keys
    EXPECT_NE(memcmp(pub1, pub2, pub1_len < pub2_len ? pub1_len : pub2_len), 0)
        << "Two different key pairs should not have the same public key";

    free(pub1);
    free(pub2);
    poseidon_key_pair_destroy(kp1);
    poseidon_key_pair_destroy(kp2);
}

TEST_F(KeyPairTest, KeyTypeReturnsCorrectString) {
    poseidon_key_pair_t* kp_ed = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp_ed, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp_ed), "ED25519");
    poseidon_key_pair_destroy(kp_ed);

    poseidon_key_pair_t* kp_rsa = poseidon_key_pair_create("RSA");
    ASSERT_NE(kp_rsa, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(kp_rsa), "RSA");
    poseidon_key_pair_destroy(kp_rsa);
}

TEST_F(KeyPairTest, SaveLoadPrivateKeyRoundTrip) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    int rc = poseidon_key_pair_save_private_key(kp, "/tmp/test_key.pem");
    EXPECT_EQ(rc, 0);

    poseidon_key_pair_t* loaded = poseidon_key_pair_load_from_pem("/tmp/test_key.pem");
    ASSERT_NE(loaded, nullptr);
    EXPECT_STREQ(poseidon_key_pair_get_key_type(loaded), "ED25519");

    // Compare public keys
    uint8_t* orig_pub = NULL;
    size_t orig_len = 0;
    uint8_t* loaded_pub = NULL;
    size_t loaded_len = 0;
    poseidon_key_pair_get_public_key(kp, &orig_pub, &orig_len);
    poseidon_key_pair_get_public_key(loaded, &loaded_pub, &loaded_len);

    EXPECT_EQ(orig_len, loaded_len);
    EXPECT_EQ(memcmp(orig_pub, loaded_pub, orig_len), 0);

    free(orig_pub);
    free(loaded_pub);
    poseidon_key_pair_destroy(kp);
    poseidon_key_pair_destroy(loaded);
}

TEST_F(KeyPairTest, SavePublicKey) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    int rc = poseidon_key_pair_save_public_key(kp, "/tmp/test_pub.pem");
    EXPECT_EQ(rc, 0);

    // Verify file exists and is non-empty
    FILE* fp = fopen("/tmp/test_pub.pem", "r");
    ASSERT_NE(fp, nullptr);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    EXPECT_GT(size, 0);

    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, GenerateCertificate) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t* pub_der = NULL;
    size_t pub_len = 0;
    poseidon_key_pair_get_public_key(kp, &pub_der, &pub_len);

    poseidon_node_id_t node_id;
    poseidon_node_id_from_public_key(pub_der, pub_len, &node_id);

    X509* cert = poseidon_key_pair_generate_certificate(kp, node_id.str);
    EXPECT_NE(cert, nullptr);

    if (cert) {
        X509_free(cert);
    }
    free(pub_der);
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, CertificateSaveLoad) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t* pub_der = NULL;
    size_t pub_len = 0;
    poseidon_key_pair_get_public_key(kp, &pub_der, &pub_len);

    poseidon_node_id_t node_id;
    poseidon_node_id_from_public_key(pub_der, pub_len, &node_id);

    X509* cert = poseidon_key_pair_generate_certificate(kp, node_id.str);
    ASSERT_NE(cert, nullptr);

    int rc = poseidon_certificate_save_to_pem(cert, "/tmp/test_cert.pem");
    EXPECT_EQ(rc, 0);

    // Verify file exists and is non-empty
    FILE* fp = fopen("/tmp/test_cert.pem", "r");
    ASSERT_NE(fp, nullptr);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    EXPECT_GT(size, 0);

    X509_free(cert);
    free(pub_der);
    poseidon_key_pair_destroy(kp);
}

TEST_F(KeyPairTest, GenerateTlsFiles) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t* pub_der = NULL;
    size_t pub_len = 0;
    poseidon_key_pair_get_public_key(kp, &pub_der, &pub_len);

    poseidon_node_id_t node_id;
    poseidon_node_id_from_public_key(pub_der, pub_len, &node_id);

    int rc = poseidon_key_pair_generate_tls_files(kp, node_id.str,
                                                   "/tmp/test_key.pem",
                                                   "/tmp/test_cert.pem");
    EXPECT_EQ(rc, 0);

    // Verify both files exist and are non-empty
    FILE* key_fp = fopen("/tmp/test_key.pem", "r");
    FILE* cert_fp = fopen("/tmp/test_cert.pem", "r");
    ASSERT_NE(key_fp, nullptr);
    ASSERT_NE(cert_fp, nullptr);

    fseek(key_fp, 0, SEEK_END);
    fseek(cert_fp, 0, SEEK_END);
    EXPECT_GT(ftell(key_fp), 0);
    EXPECT_GT(ftell(cert_fp), 0);

    fclose(key_fp);
    fclose(cert_fp);
    free(pub_der);
    poseidon_key_pair_destroy(kp);
}