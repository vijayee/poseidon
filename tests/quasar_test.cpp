//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include <cbor.h>
#include "Bloom/elastic_bloom_filter.h"
#include "Bloom/attenuated_bloom_filter.h"
#include "Network/Quasar/quasar.h"
#include "Network/Meridian/meridian_packet.h"

class QuasarTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QuasarTest, CreateDestroy) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);
    quasar_destroy(q);
}

TEST_F(QuasarTest, Subscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    EXPECT_EQ(0, quasar_subscribe(q, topic, 6, 100));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Unsubscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_unsubscribe(q, topic, 6));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Publish) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    quasar_destroy(q);
}

TEST_F(QuasarTest, TickExpiresSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 2);
    quasar_tick(q);
    quasar_tick(q);
    attenuated_bloom_filter_t* routing = q->routing;
    EXPECT_FALSE(attenuated_bloom_filter_check(routing, topic, 6, NULL));
    quasar_destroy(q);
}

TEST_F(QuasarTest, MultipleSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* t1 = (const uint8_t*)"sports";
    const uint8_t* t2 = (const uint8_t*)"news";
    const uint8_t* t3 = (const uint8_t*)"tech";
    quasar_subscribe(q, t1, 6, 100);
    quasar_subscribe(q, t2, 4, 100);
    quasar_subscribe(q, t3, 4, 100);
    quasar_unsubscribe(q, t2, 4);
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, t1, 6, NULL));
    EXPECT_FALSE(attenuated_bloom_filter_check(q->routing, t2, 4, NULL));
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, t3, 4, NULL));
    quasar_destroy(q);
}

// ============================================================================
// EBF SERIALIZATION TESTS
// ============================================================================

class EBFSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(EBFSerializationTest, RoundTripPreservesData) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, ebf);

    const uint8_t* hello = (const uint8_t*)"hello";
    const uint8_t* world = (const uint8_t*)"world";
    ASSERT_EQ(0, elastic_bloom_filter_add(ebf, hello, 5));
    ASSERT_EQ(0, elastic_bloom_filter_add(ebf, world, 5));

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    // Serialize to bytes
    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    // Decode from bytes back to CBOR
    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);
    ASSERT_EQ(CBOR_ERR_NONE, load_result.error.code);

    // Decode CBOR back to EBF
    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // Verify both strings are still present
    EXPECT_TRUE(elastic_bloom_filter_contains(decoded, hello, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(decoded, world, 5));

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(EBFSerializationTest, EmptyFilterRoundTrip) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, ebf);

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // No items added, so count should be 0
    EXPECT_EQ(0u, elastic_bloom_filter_count(decoded));

    // Strings that were never added should not be found
    const uint8_t* absent = (const uint8_t*)"absent";
    EXPECT_FALSE(elastic_bloom_filter_contains(decoded, absent, 6));

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(EBFSerializationTest, DecodeWrongSizeReturnsNull) {
    // Manually construct a CBOR array with mismatched bitset_byte_len vs actual bytestring.
    // The EBF encode format is: [size, hash_count, fp_bits, seed_a, seed_b, bitset_byte_len, bitset_bytes, num_occupied, ...]
    cbor_item_t* arr = cbor_new_definite_array(8);
    ASSERT_NE(nullptr, arr);

    // bitset_byte_len says 32 bytes but the bytestring is only 4 — mismatch should fail
    (void)cbor_array_push(arr, cbor_build_uint64(64));           // size
    (void)cbor_array_push(arr, cbor_build_uint32(3));            // hash_count
    (void)cbor_array_push(arr, cbor_build_uint32(8));            // fp_bits
    (void)cbor_array_push(arr, cbor_build_uint64(1));            // seed_a
    (void)cbor_array_push(arr, cbor_build_uint64(2));            // seed_b
    (void)cbor_array_push(arr, cbor_build_uint64(32));           // bitset_byte_len (claims 32)
    uint8_t small[4] = {0};
    (void)cbor_array_push(arr, cbor_build_bytestring(small, 4)); // bitset_bytes (only 4)
    (void)cbor_array_push(arr, cbor_build_uint64(0));            // num_occupied

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(arr);
    EXPECT_EQ(nullptr, decoded);

    cbor_decref(&arr);
}

TEST_F(EBFSerializationTest, ExpandPreservesDataRoundTrip) {
    // Use a very small initial size with low omega to trigger expand
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(16, 3, 0.5f, 8);
    ASSERT_NE(nullptr, ebf);

    // Add enough items to trigger expansion (omega=0.5 with size=16 means
    // expansion after ~8 occupied buckets)
    const uint8_t* items[] = {
        (const uint8_t*)"alpha", (const uint8_t*)"bravo",
        (const uint8_t*)"charlie", (const uint8_t*)"delta",
        (const uint8_t*)"echo", (const uint8_t*)"foxtrot",
        (const uint8_t*)"golf", (const uint8_t*)"hotel",
        (const uint8_t*)"india", (const uint8_t*)"juliet"
    };
    size_t lens[] = {5, 5, 7, 5, 4, 7, 4, 5, 5, 6};
    int num_items = 10;

    for (int i = 0; i < num_items; i++) {
        ASSERT_EQ(0, elastic_bloom_filter_add(ebf, items[i], lens[i]));
    }

    // Size should have expanded from 16
    EXPECT_GT(elastic_bloom_filter_size(ebf), 16u);

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // All items should still be present after expand + round trip
    for (int i = 0; i < num_items; i++) {
        EXPECT_TRUE(elastic_bloom_filter_contains(decoded, items[i], lens[i]));
    }

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

// ============================================================================
// ABF SERIALIZATION TESTS
// ============================================================================

class ABFSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ABFSerializationTest, RoundTripPreservesAllLevels) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, abf);

    const uint8_t* topic = (const uint8_t*)"topic1";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(abf, topic, 6));

    cbor_item_t* encoded = attenuated_bloom_filter_encode(abf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // Level count preserved
    EXPECT_EQ(3u, attenuated_bloom_filter_level_count(decoded));

    // Topic should be found at level 0
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(decoded, topic, 6, &hops));
    EXPECT_EQ(0u, hops);

    attenuated_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(ABFSerializationTest, SingleLevelRoundTrip) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(1, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, abf);

    const uint8_t* topic = (const uint8_t*)"single_level_topic";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(abf, topic, 19));

    cbor_item_t* encoded = attenuated_bloom_filter_encode(abf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    EXPECT_EQ(1u, attenuated_bloom_filter_level_count(decoded));
    EXPECT_TRUE(attenuated_bloom_filter_check(decoded, topic, 19, nullptr));

    attenuated_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(ABFSerializationTest, MergeShiftsLevels) {
    // Create two ABFs with the same parameters
    attenuated_bloom_filter_t* dest = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    attenuated_bloom_filter_t* src = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, dest);
    ASSERT_NE(nullptr, src);

    // Subscribe to "topic1" in src at level 0
    const uint8_t* topic = (const uint8_t*)"topic1";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(src, topic, 6));

    // Encode src, decode it, and merge into dest
    cbor_item_t* src_encoded = attenuated_bloom_filter_encode(src);
    ASSERT_NE(nullptr, src_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(src_encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded_src = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded_src);

    // Merge: src level 0 should shift to dest level 1
    EXPECT_EQ(0, attenuated_bloom_filter_merge(dest, decoded_src));

    // The topic should appear at level 1 in the merged dest
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(dest, topic, 6, &hops));
    EXPECT_EQ(1u, hops);

    attenuated_bloom_filter_destroy(decoded_src);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&src_encoded);
    attenuated_bloom_filter_destroy(src);
    attenuated_bloom_filter_destroy(dest);
}

// ============================================================================
// QUASAR GOSSIP TESTS
// ============================================================================

class QuasarGossipTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QuasarGossipTest, PropagateEncodesFilter) {
    // Create a quasar with NULL protocol — propagate will fail at broadcast
    // because meridian_protocol_broadcast(NULL, ...) will fail, but we can
    // verify the encode path works by checking that propagate returns -1
    // (from the broadcast failure) rather than crashing.
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* topic = (const uint8_t*)"test";
    ASSERT_EQ(0, quasar_subscribe(q, topic, 4, 100));

    // propagate will encode the filter and then call meridian_protocol_broadcast(NULL, ...)
    // which should return -1, so propagate returns -1 (not crash)
    int rc = quasar_propagate(q);
    // Expected: -1 because protocol is NULL and broadcast fails
    EXPECT_EQ(-1, rc);

    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipDecodesAndMerges) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);

    // Subscribe "topic1" locally so we can verify local sub is still at level 0 after merge
    const uint8_t* local_topic = (const uint8_t*)"local_topic";
    ASSERT_EQ(0, quasar_subscribe(q, local_topic, 11, 100));

    // Build a QUASAR_GOSSIP packet manually:
    // [uint8(50), uint32(level_count), encoded_abf]
    attenuated_bloom_filter_t* remote_abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    ASSERT_NE(nullptr, remote_abf);

    const uint8_t* remote_topic = (const uint8_t*)"remote_topic";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(remote_abf, remote_topic, 12));

    cbor_item_t* remote_encoded = attenuated_bloom_filter_encode(remote_abf);
    ASSERT_NE(nullptr, remote_encoded);

    cbor_item_t* packet = cbor_new_definite_array(3);
    ASSERT_NE(nullptr, packet);
    (void)cbor_array_push(packet, cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP));
    (void)cbor_array_push(packet, cbor_build_uint32(attenuated_bloom_filter_level_count(remote_abf)));
    (void)cbor_array_push(packet, remote_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    // Create a dummy sender node
    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);
    ASSERT_NE(nullptr, from);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(0, rc);

    // The remote topic should appear at level 1 in the merged routing filter
    // (merge shifts src levels by +1)
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, remote_topic, 12, &hops));
    EXPECT_EQ(1u, hops);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(remote_abf);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, MergePreservesLocalSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* local_topic = (const uint8_t*)"local_topic";
    ASSERT_EQ(0, quasar_subscribe(q, local_topic, 12, 100));

    // Build a minimal valid gossip packet with an empty ABF
    attenuated_bloom_filter_t* remote_abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    ASSERT_NE(nullptr, remote_abf);

    cbor_item_t* remote_encoded = attenuated_bloom_filter_encode(remote_abf);
    ASSERT_NE(nullptr, remote_encoded);

    cbor_item_t* packet = cbor_new_definite_array(3);
    (void)cbor_array_push(packet, cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP));
    (void)cbor_array_push(packet, cbor_build_uint32(attenuated_bloom_filter_level_count(remote_abf)));
    (void)cbor_array_push(packet, remote_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);

    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);
    ASSERT_NE(nullptr, from);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(0, rc);

    // local_topic should still be at level 0 after merge
    uint32_t hops = 99;
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, local_topic, 12, &hops));
    EXPECT_EQ(0u, hops);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(remote_abf);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipInvalidCBOR) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);

    int rc = quasar_on_gossip(q, garbage, sizeof(garbage), from);
    EXPECT_EQ(-1, rc);

    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipWrongType) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);

    // Build a CBOR array with a wrong type byte (not QUASAR_GOSSIP=50)
    cbor_item_t* packet = cbor_new_definite_array(3);
    (void)cbor_array_push(packet, cbor_build_uint8(99));  // wrong type
    (void)cbor_array_push(packet, cbor_build_uint32(5));

    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    cbor_item_t* abf_encoded = attenuated_bloom_filter_encode(abf);
    (void)cbor_array_push(packet, abf_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);

    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(-1, rc);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(abf);
    quasar_destroy(q);
}
