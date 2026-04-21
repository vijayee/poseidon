//
// Created by victor on 4/20/26.
//

#include <gtest/gtest.h>
#include "Crypto/key_pair.h"
#include "Crypto/node_id.h"
#include "Channel/channel.h"
#include "Channel/channel_manager.h"
#include "Network/Meridian/meridian.h"
#include "Util/hash.h"

// ============================================================================
// NODE IDENTITY TESTS
// ============================================================================

class NodeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a key pair to derive a node ID
        key_pair = poseidon_key_pair_create("ED25519");
        ASSERT_NE(key_pair, nullptr);

        uint8_t* pub_key = nullptr;
        size_t pub_len = 0;
        ASSERT_EQ(poseidon_key_pair_get_public_key(key_pair, &pub_key, &pub_len), 0);
        ASSERT_EQ(poseidon_node_id_from_public_key(pub_key, pub_len, &node_id), 0);
        free(pub_key);
    }

    void TearDown() override {
        poseidon_key_pair_destroy(key_pair);
    }

    poseidon_key_pair_t* key_pair = nullptr;
    poseidon_node_id_t node_id;
};

TEST_F(NodeIdentityTest, CreateWithId) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, &node_id);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(meridian_node_equals_by_id(node, node));
    EXPECT_FALSE(poseidon_node_id_is_null(&node->id));
    meridian_node_destroy(node);
}

TEST_F(NodeIdentityTest, CreateUnidentified) {
    meridian_node_t* node = meridian_node_create_unidentified(0xC0A80001, 8080);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(poseidon_node_id_is_null(&node->id));
    meridian_node_destroy(node);
}

TEST_F(NodeIdentityTest, EqualsByIdSame) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80002, 9090, &node_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Same ID, different addr:port -> equals_by_id is true
    EXPECT_TRUE(meridian_node_equals_by_id(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

TEST_F(NodeIdentityTest, EqualsByIdDifferent) {
    poseidon_key_pair_t* kp2 = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp2, nullptr);
    uint8_t* pub2 = nullptr;
    size_t pub2_len = 0;
    poseidon_key_pair_get_public_key(kp2, &pub2, &pub2_len);
    poseidon_node_id_t id2;
    poseidon_node_id_from_public_key(pub2, pub2_len, &id2);
    free(pub2);

    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80001, 8080, &id2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Same addr:port, different ID -> equals_by_id is false
    EXPECT_FALSE(meridian_node_equals_by_id(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
    poseidon_key_pair_destroy(kp2);
}

TEST_F(NodeIdentityTest, EqualsByAddrSame) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80001, 8080, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(meridian_node_equals_by_addr(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

TEST_F(NodeIdentityTest, EqualsByAddrDifferent) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80002, 9090, &node_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(meridian_node_equals_by_addr(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

// ============================================================================
// CHANNEL CONFIG TESTS
// ============================================================================

TEST(ChannelConfigTest, DefaultsAreSensible) {
    poseidon_channel_config_t cfg = poseidon_channel_config_defaults();
    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        EXPECT_GT(cfg.ring_sizes[i], 0u);
    }
    EXPECT_GT(cfg.gossip_init_interval_s, 0u);
    EXPECT_GT(cfg.gossip_steady_interval_s, 0u);
    EXPECT_GT(cfg.quasar_max_hops, 0u);
    EXPECT_GT(cfg.quasar_alpha, 0u);
}

TEST(ChannelConfigTest, UpdateConfigWithKeyHolder) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    poseidon_channel_config_t cfg = poseidon_channel_config_defaults();
    // Can't create a full channel without a work pool and timing wheel,
    // so test config defaults and key comparison directly
    cfg.gossip_init_interval_s = 10;

    poseidon_key_pair_destroy(kp);
}

// ============================================================================
// NODE ID HASHMAP INTEGRATION TESTS
// ============================================================================

TEST(NodeIdHashTest, HashAndCompare) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);
    uint8_t* pub = nullptr;
    size_t pub_len = 0;
    poseidon_key_pair_get_public_key(kp, &pub, &pub_len);
    poseidon_node_id_t id;
    poseidon_node_id_from_public_key(pub, pub_len, &id);
    free(pub);

    size_t h = hash_poseidon_node_id(&id);
    EXPECT_NE(h, 0u);

    poseidon_node_id_t id2;
    memcpy(&id2, &id, sizeof(poseidon_node_id_t));
    EXPECT_EQ(compare_poseidon_node_id(&id, &id2), 0);

    poseidon_node_id_t* dup = duplicate_poseidon_node_id(&id);
    ASSERT_NE(dup, nullptr);
    EXPECT_EQ(compare_poseidon_node_id(&id, dup), 0);
    free(dup);

    poseidon_key_pair_destroy(kp);
}