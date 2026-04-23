//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "Channel/channel_notice.h"
#include "Channel/channel_manager.h"
#include "Channel/channel.h"
#include "Channel/channel_config.h"
#include "Network/Meridian/meridian_packet.h"
#include "Crypto/key_pair.h"
#include "Crypto/node_id.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
#include <cbor.h>
}

// ============================================================================
// NOTICE ENCODE/DECODE TESTS
// ============================================================================

TEST(DeleteNoticePacketTest, EncodeDecodeRoundTrip) {
    meridian_channel_delete_notice_t notice;
    memset(&notice, 0, sizeof(notice));
    notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
    strncpy(notice.topic_id, "X4jKL2mNpQrStUvWxYz", sizeof(notice.topic_id) - 1);
    strncpy(notice.node_id, "AbCdEfGhIjKlMnOpQrStUvWxYz1234", sizeof(notice.node_id) - 1);
    strncpy(notice.key_type, "ED25519", sizeof(notice.key_type) - 1);
    uint8_t fake_pub_key[32];
    memset(fake_pub_key, 0xAB, 32);
    memcpy(notice.public_key, fake_pub_key, 32);
    notice.public_key_len = 32;
    notice.timestamp_us = 1745300000000ULL;
    uint8_t fake_sig[64];
    memset(fake_sig, 0xCD, 64);
    memcpy(notice.signature, fake_sig, 64);
    notice.signature_len = 64;

    cbor_item_t* encoded = meridian_channel_delete_notice_encode(&notice);
    ASSERT_NE(encoded, nullptr);

    meridian_channel_delete_notice_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    int rc = meridian_channel_delete_notice_decode(encoded, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.type, MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE);
    EXPECT_STREQ(decoded.topic_id, notice.topic_id);
    EXPECT_STREQ(decoded.node_id, notice.node_id);
    EXPECT_STREQ(decoded.key_type, notice.key_type);
    EXPECT_EQ(decoded.public_key_len, notice.public_key_len);
    EXPECT_EQ(memcmp(decoded.public_key, notice.public_key, notice.public_key_len), 0);
    EXPECT_EQ(decoded.timestamp_us, notice.timestamp_us);
    EXPECT_EQ(decoded.signature_len, notice.signature_len);
    EXPECT_EQ(memcmp(decoded.signature, notice.signature, notice.signature_len), 0);

    cbor_decref(&encoded);
}

TEST(DeleteNoticePacketTest, DecodeNullReturnsError) {
    meridian_channel_delete_notice_t notice;
    EXPECT_NE(0, meridian_channel_delete_notice_decode(NULL, &notice));
}

TEST(ModifyNoticePacketTest, EncodeDecodeRoundTrip) {
    meridian_channel_modify_notice_t notice;
    memset(&notice, 0, sizeof(notice));
    notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE;
    strncpy(notice.topic_id, "ModifyTopic123", sizeof(notice.topic_id) - 1);
    strncpy(notice.node_id, "ModifyNode456", sizeof(notice.node_id) - 1);
    strncpy(notice.key_type, "ED25519", sizeof(notice.key_type) - 1);
    uint8_t fake_pub_key[32];
    memset(fake_pub_key, 0xEF, 32);
    memcpy(notice.public_key, fake_pub_key, 32);
    notice.public_key_len = 32;
    notice.timestamp_us = 1745400000000ULL;
    notice.config = poseidon_channel_config_defaults();
    notice.config.quasar_max_hops = 10;
    notice.config.quasar_alpha = 5;
    uint8_t fake_sig[64];
    memset(fake_sig, 0xFE, 64);
    memcpy(notice.signature, fake_sig, 64);
    notice.signature_len = 64;

    cbor_item_t* encoded = meridian_channel_modify_notice_encode(&notice);
    ASSERT_NE(encoded, nullptr);

    meridian_channel_modify_notice_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    int rc = meridian_channel_modify_notice_decode(encoded, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.type, MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE);
    EXPECT_STREQ(decoded.topic_id, notice.topic_id);
    EXPECT_STREQ(decoded.node_id, notice.node_id);
    EXPECT_STREQ(decoded.key_type, notice.key_type);
    EXPECT_EQ(decoded.public_key_len, notice.public_key_len);
    EXPECT_EQ(memcmp(decoded.public_key, notice.public_key, notice.public_key_len), 0);
    EXPECT_EQ(decoded.timestamp_us, notice.timestamp_us);
    EXPECT_EQ(decoded.config.quasar_max_hops, notice.config.quasar_max_hops);
    EXPECT_EQ(decoded.config.quasar_alpha, notice.config.quasar_alpha);
    EXPECT_EQ(decoded.signature_len, notice.signature_len);
    EXPECT_EQ(memcmp(decoded.signature, notice.signature, notice.signature_len), 0);

    cbor_decref(&encoded);
}

TEST(ModifyNoticePacketTest, DecodeNullReturnsError) {
    meridian_channel_modify_notice_t notice;
    EXPECT_NE(0, meridian_channel_modify_notice_decode(NULL, &notice));
}

// ============================================================================
// NOTICE CREATION AND VERIFICATION TESTS
// ============================================================================

class ChannelNoticeTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = work_pool_create(2);
        ASSERT_NE(pool, nullptr);
        work_pool_launch(pool);

        wheel = hierarchical_timing_wheel_create(8, pool);
        ASSERT_NE(wheel, nullptr);
        hierarchical_timing_wheel_run(wheel);

        key_pair = poseidon_key_pair_create("ED25519");
        ASSERT_NE(key_pair, nullptr);

        poseidon_channel_config_t config = poseidon_channel_config_defaults();
        channel = poseidon_channel_create(key_pair, "test-channel", 17000,
                                           &config, pool, wheel);
        ASSERT_NE(channel, nullptr);
    }

    void TearDown() override {
        if (channel != nullptr) {
            poseidon_channel_destroy(channel);
            channel = nullptr;
        }
        if (key_pair != nullptr) {
            poseidon_key_pair_destroy(key_pair);
            key_pair = nullptr;
        }
        if (wheel != nullptr) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool != nullptr) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
            pool = nullptr;
        }
    }

    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    poseidon_key_pair_t* key_pair = nullptr;
    poseidon_channel_t* channel = nullptr;
};

TEST_F(ChannelNoticeTest, CreateDeleteNotice) {
    meridian_channel_delete_notice_t* notice =
        poseidon_channel_create_delete_notice(channel);
    ASSERT_NE(notice, nullptr);

    EXPECT_EQ(notice->type, MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE);
    EXPECT_STREQ(notice->topic_id, poseidon_channel_get_topic(channel));
    EXPECT_STREQ(notice->key_type, "ED25519");
    EXPECT_GT(notice->public_key_len, (size_t)0);
    EXPECT_GT(notice->timestamp_us, 0ULL);
    EXPECT_GT(notice->signature_len, (size_t)0);

    free(notice);
}

TEST_F(ChannelNoticeTest, VerifyDeleteNotice) {
    meridian_channel_delete_notice_t* notice =
        poseidon_channel_create_delete_notice(channel);
    ASSERT_NE(notice, nullptr);

    int rc = poseidon_channel_verify_delete_notice(notice);
    EXPECT_EQ(rc, 0);

    free(notice);
}

TEST_F(ChannelNoticeTest, VerifyDeleteNoticeTamperedFails) {
    meridian_channel_delete_notice_t* notice =
        poseidon_channel_create_delete_notice(channel);
    ASSERT_NE(notice, nullptr);

    // Tamper with the topic_id
    notice->topic_id[0] = 'Z';

    int rc = poseidon_channel_verify_delete_notice(notice);
    EXPECT_NE(rc, 0);

    free(notice);
}

TEST_F(ChannelNoticeTest, CreateModifyNotice) {
    poseidon_channel_config_t new_config = channel->config;
    new_config.quasar_max_hops = 10;

    meridian_channel_modify_notice_t* notice =
        poseidon_channel_create_modify_notice(channel, &new_config);
    ASSERT_NE(notice, nullptr);

    EXPECT_EQ(notice->type, MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE);
    EXPECT_STREQ(notice->topic_id, poseidon_channel_get_topic(channel));
    EXPECT_STREQ(notice->key_type, "ED25519");
    EXPECT_GT(notice->public_key_len, (size_t)0);
    EXPECT_GT(notice->timestamp_us, 0ULL);
    EXPECT_GT(notice->signature_len, (size_t)0);
    EXPECT_EQ(notice->config.quasar_max_hops, 10u);

    free(notice);
}

TEST_F(ChannelNoticeTest, VerifyModifyNotice) {
    poseidon_channel_config_t new_config = channel->config;

    meridian_channel_modify_notice_t* notice =
        poseidon_channel_create_modify_notice(channel, &new_config);
    ASSERT_NE(notice, nullptr);

    int rc = poseidon_channel_verify_modify_notice(notice);
    EXPECT_EQ(rc, 0);

    free(notice);
}

TEST_F(ChannelNoticeTest, VerifyModifyNoticeTamperedFails) {
    poseidon_channel_config_t new_config = channel->config;

    meridian_channel_modify_notice_t* notice =
        poseidon_channel_create_modify_notice(channel, &new_config);
    ASSERT_NE(notice, nullptr);

    // Tamper with the config
    notice->config.quasar_max_hops = 99;

    int rc = poseidon_channel_verify_modify_notice(notice);
    EXPECT_NE(rc, 0);

    free(notice);
}

TEST_F(ChannelNoticeTest, VerifyNullNoticeFails) {
    EXPECT_NE(0, poseidon_channel_verify_delete_notice(NULL));
    EXPECT_NE(0, poseidon_channel_verify_modify_notice(NULL));
}

// ============================================================================
// TOMBSTONE TESTS
// ============================================================================

class TombstoneTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = work_pool_create(2);
        ASSERT_NE(pool, nullptr);
        work_pool_launch(pool);

        wheel = hierarchical_timing_wheel_create(8, pool);
        ASSERT_NE(wheel, nullptr);
        hierarchical_timing_wheel_run(wheel);

        dial_key_pair = poseidon_key_pair_create("ED25519");
        ASSERT_NE(dial_key_pair, nullptr);

        mgr = poseidon_channel_manager_create(dial_key_pair, 18000, 18001, 18010,
                                               pool, wheel);
        ASSERT_NE(mgr, nullptr);
    }

    void TearDown() override {
        if (mgr != nullptr) {
            poseidon_channel_manager_destroy(mgr);
            mgr = nullptr;
        }
        if (dial_key_pair != nullptr) {
            poseidon_key_pair_destroy(dial_key_pair);
            dial_key_pair = nullptr;
        }
        if (wheel != nullptr) {
            hierarchical_timing_wheel_wait_for_idle_signal(wheel);
            hierarchical_timing_wheel_stop(wheel);
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool != nullptr) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
            work_pool_destroy(pool);
            pool = nullptr;
        }
    }

    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    poseidon_key_pair_t* dial_key_pair = nullptr;
    poseidon_channel_manager_t* mgr = nullptr;
};

TEST_F(TombstoneTest, AddAndFindDeleteTombstone) {
    meridian_channel_delete_notice_t notice;
    memset(&notice, 0, sizeof(notice));
    notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
    strncpy(notice.topic_id, "DeletedTopic123", sizeof(notice.topic_id) - 1);
    strncpy(notice.node_id, "OwnerNodeId456", sizeof(notice.node_id) - 1);
    strncpy(notice.key_type, "ED25519", sizeof(notice.key_type) - 1);
    memset(notice.public_key, 0xAA, 32);
    notice.public_key_len = 32;
    notice.timestamp_us = 1745300000000ULL;
    memset(notice.signature, 0xBB, 32);
    notice.signature_len = 32;

    poseidon_tombstone_t tombstone;
    ASSERT_EQ(0, poseidon_tombstone_from_delete_notice(&notice, &tombstone));
    EXPECT_EQ(tombstone.type, POSEIDON_TOMBSTONE_DELETE);
    EXPECT_STREQ(tombstone.topic_id, "DeletedTopic123");
    EXPECT_FALSE(tombstone.has_config);

    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &tombstone));

    const poseidon_tombstone_t* found =
        poseidon_channel_manager_find_tombstone(mgr, "DeletedTopic123");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->type, POSEIDON_TOMBSTONE_DELETE);
    EXPECT_STREQ(found->topic_id, "DeletedTopic123");
}

TEST_F(TombstoneTest, AddAndFindModifyTombstone) {
    meridian_channel_modify_notice_t notice;
    memset(&notice, 0, sizeof(notice));
    notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE;
    strncpy(notice.topic_id, "ModifiedTopic789", sizeof(notice.topic_id) - 1);
    strncpy(notice.node_id, "OwnerNodeId012", sizeof(notice.node_id) - 1);
    strncpy(notice.key_type, "ED25519", sizeof(notice.key_type) - 1);
    memset(notice.public_key, 0xCC, 32);
    notice.public_key_len = 32;
    notice.timestamp_us = 1745400000000ULL;
    notice.config = poseidon_channel_config_defaults();
    notice.config.quasar_max_hops = 7;
    memset(notice.signature, 0xDD, 32);
    notice.signature_len = 32;

    poseidon_tombstone_t tombstone;
    ASSERT_EQ(0, poseidon_tombstone_from_modify_notice(&notice, &tombstone));
    EXPECT_EQ(tombstone.type, POSEIDON_TOMBSTONE_MODIFY);
    EXPECT_TRUE(tombstone.has_config);
    EXPECT_EQ(tombstone.config.quasar_max_hops, 7u);

    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &tombstone));

    const poseidon_tombstone_t* found =
        poseidon_channel_manager_find_tombstone(mgr, "ModifiedTopic789");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->type, POSEIDON_TOMBSTONE_MODIFY);
    EXPECT_EQ(found->config.quasar_max_hops, 7u);
}

TEST_F(TombstoneTest, FindNonexistentReturnsNull) {
    EXPECT_EQ(nullptr, poseidon_channel_manager_find_tombstone(mgr, "NoSuchTopic"));
}

TEST_F(TombstoneTest, DuplicateReplacesIfNewer) {
    meridian_channel_delete_notice_t notice1;
    memset(&notice1, 0, sizeof(notice1));
    notice1.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
    strncpy(notice1.topic_id, "TopicDup", sizeof(notice1.topic_id) - 1);
    strncpy(notice1.node_id, "Node1", sizeof(notice1.node_id) - 1);
    strncpy(notice1.key_type, "ED25519", sizeof(notice1.key_type) - 1);
    notice1.public_key_len = 0;
    notice1.timestamp_us = 1000ULL;
    notice1.signature_len = 0;

    poseidon_tombstone_t ts1;
    ASSERT_EQ(0, poseidon_tombstone_from_delete_notice(&notice1, &ts1));
    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &ts1));

    // Add a newer tombstone for the same topic
    meridian_channel_delete_notice_t notice2;
    memset(&notice2, 0, sizeof(notice2));
    notice2.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
    strncpy(notice2.topic_id, "TopicDup", sizeof(notice2.topic_id) - 1);
    strncpy(notice2.node_id, "Node2", sizeof(notice2.node_id) - 1);
    strncpy(notice2.key_type, "ED25519", sizeof(notice2.key_type) - 1);
    notice2.public_key_len = 0;
    notice2.timestamp_us = 2000ULL;
    notice2.signature_len = 0;

    poseidon_tombstone_t ts2;
    ASSERT_EQ(0, poseidon_tombstone_from_delete_notice(&notice2, &ts2));
    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &ts2));

    // Should have the newer tombstone
    const poseidon_tombstone_t* found =
        poseidon_channel_manager_find_tombstone(mgr, "TopicDup");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->timestamp_us, 2000ULL);
    EXPECT_STREQ(found->node_id, "Node2");

    // Count should still be 1
    EXPECT_EQ(mgr->num_tombstones, 1u);
}

TEST_F(TombstoneTest, AddNullFails) {
    EXPECT_NE(0, poseidon_channel_manager_add_tombstone(mgr, NULL));
}

TEST_F(TombstoneTest, ExpireTombstones) {
    poseidon_tombstone_t tombstone;
    memset(&tombstone, 0, sizeof(tombstone));
    tombstone.type = POSEIDON_TOMBSTONE_DELETE;
    strncpy(tombstone.topic_id, "ExpiringTopic", sizeof(tombstone.topic_id) - 1);
    tombstone.timestamp_us = 1000ULL;
    // Set expires_at to the past so it gets expired
    tombstone.expires_at_us = 1ULL;

    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &tombstone));
    EXPECT_EQ(1u, mgr->num_tombstones);

    poseidon_channel_manager_expire_tombstones(mgr);
    EXPECT_EQ(0u, mgr->num_tombstones);
}

TEST_F(TombstoneTest, ExpirePreservesValidTombstones) {
    // Add a tombstone with a far-future expiration
    poseidon_tombstone_t tombstone;
    memset(&tombstone, 0, sizeof(tombstone));
    tombstone.type = POSEIDON_TOMBSTONE_DELETE;
    strncpy(tombstone.topic_id, "ValidTopic", sizeof(tombstone.topic_id) - 1);
    tombstone.timestamp_us = 1000ULL;
    tombstone.expires_at_us = UINT64_MAX;

    ASSERT_EQ(0, poseidon_channel_manager_add_tombstone(mgr, &tombstone));
    EXPECT_EQ(1u, mgr->num_tombstones);

    poseidon_channel_manager_expire_tombstones(mgr);
    EXPECT_EQ(1u, mgr->num_tombstones);

    EXPECT_NE(nullptr, poseidon_channel_manager_find_tombstone(mgr, "ValidTopic"));
}

// ============================================================================
// NODE ID PUBLIC KEY VERIFICATION TESTS
// ============================================================================

class NodeIdVerifyTest : public ::testing::Test {};

TEST_F(NodeIdVerifyTest, VerifyEd25519PublicKey) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t* pub_key = NULL;
    size_t pub_len = 0;
    ASSERT_EQ(0, poseidon_key_pair_get_public_key(kp, &pub_key, &pub_len));

    poseidon_node_id_t node_id;
    ASSERT_EQ(0, poseidon_node_id_from_public_key(pub_key, pub_len, &node_id));

    EXPECT_EQ(0, poseidon_node_id_verify_public_key(&node_id, pub_key, pub_len));

    // Wrong key should fail
    uint8_t wrong_key[32];
    memset(wrong_key, 0xFF, 32);
    EXPECT_NE(0, poseidon_node_id_verify_public_key(&node_id, wrong_key, 32));

    free(pub_key);
    poseidon_key_pair_destroy(kp);
}

TEST_F(NodeIdVerifyTest, NullInputsFail) {
    poseidon_node_id_t id;
    poseidon_node_id_clear(&id);
    uint8_t key[32] = {0};

    EXPECT_NE(0, poseidon_node_id_verify_public_key(NULL, key, 32));
    EXPECT_NE(0, poseidon_node_id_verify_public_key(&id, NULL, 32));
    EXPECT_NE(0, poseidon_node_id_verify_public_key(&id, key, 0));
}

// ============================================================================
// SIGNATURE VERIFICATION TESTS
// ============================================================================

class SignatureVerifyTest : public ::testing::Test {};

TEST_F(SignatureVerifyTest, VerifyEd25519Signature) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    const uint8_t data[] = "test message for signing";
    size_t data_len = sizeof(data);

    uint8_t signature[64];
    size_t sig_len = 0;
    ASSERT_EQ(0, poseidon_key_pair_sign(kp, data, data_len, signature, &sig_len));
    EXPECT_GT(sig_len, (size_t)0);

    uint8_t* pub_key = NULL;
    size_t pub_len = 0;
    ASSERT_EQ(0, poseidon_key_pair_get_public_key(kp, &pub_key, &pub_len));

    EXPECT_EQ(0, poseidon_verify_signature_with_key("ED25519", pub_key, pub_len,
                                                      data, data_len, signature, sig_len));

    // Tampered data should fail
    const uint8_t tampered[] = "tampered message for signing";
    EXPECT_NE(0, poseidon_verify_signature_with_key("ED25519", pub_key, pub_len,
                                                      tampered, sizeof(tampered), signature, sig_len));

    free(pub_key);
    poseidon_key_pair_destroy(kp);
}

TEST_F(SignatureVerifyTest, NullInputsFail) {
    uint8_t key[32] = {0};
    uint8_t data[] = "data";
    uint8_t sig[64] = {0};

    EXPECT_NE(0, poseidon_verify_signature_with_key(NULL, key, 32, data, 4, sig, 64));
    EXPECT_NE(0, poseidon_verify_signature_with_key("ED25519", NULL, 32, data, 4, sig, 64));
    EXPECT_NE(0, poseidon_verify_signature_with_key("ED25519", key, 32, NULL, 4, sig, 64));
    EXPECT_NE(0, poseidon_verify_signature_with_key("ED25519", key, 32, data, 4, NULL, 64));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}