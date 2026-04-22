//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include <cbor.h>
#include <string.h>
#include "Network/Meridian/meridian_packet.h"
#include "Channel/channel_manager.h"
#include "Channel/channel.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

TEST(BootstrapPacketTest, EncodeDecodeBootstrap) {
    const char* topic_id = "X4jKL2mNpQrStUvWxYz";
    const char* sender_node_id = "AbCdEfGhIjKlMnOpQrStUvWxYz1234";
    uint64_t timestamp_us = 1745300000000ULL;

    cbor_item_t* encoded = meridian_channel_bootstrap_encode(
        topic_id, sender_node_id, timestamp_us);
    ASSERT_NE(encoded, nullptr);
    ASSERT_TRUE(cbor_array_is_definite(encoded));
    EXPECT_EQ(cbor_array_size(encoded), 4u);

    char out_topic[64] = {0};
    char out_node_id[64] = {0};
    uint64_t out_ts = 0;
    int rc = meridian_channel_bootstrap_decode(encoded, out_topic, sizeof(out_topic),
                                                 out_node_id, sizeof(out_node_id), &out_ts);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(out_topic, topic_id);
    EXPECT_STREQ(out_node_id, sender_node_id);
    EXPECT_EQ(out_ts, timestamp_us);

    cbor_decref(&encoded);
}

TEST(BootstrapPacketTest, EncodeDecodeBootstrapReply) {
    const char* topic_id = "X4jKL2mNpQrStUvWxYz";
    const char* responder_node_id = "ReSpOnDeR1234567890AbCdEf";
    uint32_t addr = 0x7F000001;
    uint16_t port = 9000;
    uint64_t timestamp_us = 1745300000000ULL;
    uint32_t seed_addrs[] = {0xC0A80001, 0xC0A80002};
    uint16_t seed_ports[] = {8080, 8081};
    size_t num_seeds = 2;

    cbor_item_t* encoded = meridian_channel_bootstrap_reply_encode(
        topic_id, responder_node_id, addr, port, timestamp_us,
        seed_addrs, seed_ports, num_seeds);
    ASSERT_NE(encoded, nullptr);

    char out_topic[64] = {0};
    char out_node_id[64] = {0};
    uint32_t out_addr = 0;
    uint16_t out_port = 0;
    uint64_t out_ts = 0;
    uint32_t out_seed_addrs[16] = {0};
    uint16_t out_seed_ports[16] = {0};
    size_t out_num_seeds = 0;

    int rc = meridian_channel_bootstrap_reply_decode(encoded, out_topic, sizeof(out_topic),
                                                      out_node_id, sizeof(out_node_id),
                                                      &out_addr, &out_port, &out_ts,
                                                      out_seed_addrs, out_seed_ports,
                                                      &out_num_seeds, 16);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(out_topic, topic_id);
    EXPECT_STREQ(out_node_id, responder_node_id);
    EXPECT_EQ(out_addr, addr);
    EXPECT_EQ(out_port, port);
    EXPECT_EQ(out_ts, timestamp_us);
    EXPECT_EQ(out_num_seeds, 2u);
    EXPECT_EQ(out_seed_addrs[0], seed_addrs[0]);
    EXPECT_EQ(out_seed_ports[0], seed_ports[0]);
    EXPECT_EQ(out_seed_addrs[1], seed_addrs[1]);
    EXPECT_EQ(out_seed_ports[1], seed_ports[1]);

    cbor_decref(&encoded);
}

TEST(BootstrapPacketTest, DecodeInvalidReturnsNull) {
    char buf[64];
    uint64_t ts = 0;
    EXPECT_NE(0, meridian_channel_bootstrap_decode(NULL, buf, sizeof(buf), buf, sizeof(buf), &ts));
}

TEST(BootstrapPacketTest, EncodeDecodeReplyWithNoSeeds) {
    const char* topic_id = "TestTopic123";
    const char* node_id = "NodeId456";
    uint64_t ts = 12345;

    cbor_item_t* encoded = meridian_channel_bootstrap_reply_encode(
        topic_id, node_id, 0, 0, ts, NULL, NULL, 0);
    ASSERT_NE(encoded, nullptr);

    char out_topic[64], out_node_id[64];
    uint32_t out_addr; uint16_t out_port; uint64_t out_ts;
    uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds = 0;

    int rc = meridian_channel_bootstrap_reply_decode(encoded, out_topic, sizeof(out_topic),
                                                      out_node_id, sizeof(out_node_id),
                                                      &out_addr, &out_port, &out_ts,
                                                      seed_addrs, seed_ports, &num_seeds, 16);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(num_seeds, 0u);
    EXPECT_STREQ(out_topic, topic_id);
    EXPECT_STREQ(out_node_id, node_id);
    EXPECT_EQ(out_addr, 0u);
    EXPECT_EQ(out_port, 0u);
    EXPECT_EQ(out_ts, ts);

    cbor_decref(&encoded);
}

// ============================================================================
// CHANNEL MANAGER BOOTSTRAP TESTS
// ============================================================================

class ChannelManagerBootstrapTest : public ::testing::Test {
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
        }

        if (pool != nullptr) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }

        if (pool != nullptr) {
            work_pool_destroy(pool);
            pool = nullptr;
        }

        if (wheel != nullptr) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
    }

    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    poseidon_key_pair_t* dial_key_pair = nullptr;
    poseidon_channel_manager_t* mgr = nullptr;
};

TEST_F(ChannelManagerBootstrapTest, JoinChannelCreatesPendingBootstrap) {
    mgr = poseidon_channel_manager_create(dial_key_pair, 16000, 16001, 16010, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    const char* topic = "TestTopic123";
    poseidon_channel_t* channel = poseidon_channel_manager_join_channel(mgr, topic);
    ASSERT_NE(channel, nullptr);

    // Channel should be in BOOTSTRAPPING state
    EXPECT_EQ(POSEIDON_CHANNEL_STATE_BOOTSTRAPPING, channel->state);

    // Manager should have a pending bootstrap entry
    EXPECT_EQ(1u, mgr->num_pending_bootstraps);
    EXPECT_STREQ(topic, mgr->pending_bootstraps[0].topic_id);
    EXPECT_EQ(channel, mgr->pending_bootstraps[0].channel);
    EXPECT_GT(mgr->pending_bootstraps[0].timestamp_us, 0u);
}

TEST_F(ChannelManagerBootstrapTest, HandleBootstrapRequestRejectsUnknownTopic) {
    mgr = poseidon_channel_manager_create(dial_key_pair, 16000, 16001, 16010, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    // No channels joined yet, so request for any topic should fail
    EXPECT_EQ(-1, poseidon_channel_manager_handle_bootstrap_request(mgr, "UnknownTopic", "SomeNode"));
}

TEST_F(ChannelManagerBootstrapTest, HandleBootstrapReplyFindsPending) {
    mgr = poseidon_channel_manager_create(dial_key_pair, 16000, 16001, 16010, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    const char* topic = "TestTopic456";
    poseidon_channel_t* channel = poseidon_channel_manager_join_channel(mgr, topic);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(1u, mgr->num_pending_bootstraps);

    uint64_t timestamp_us = mgr->pending_bootstraps[0].timestamp_us;

    // Simulate a bootstrap reply
    uint32_t seed_addrs[] = {0x7F000001};
    uint16_t seed_ports[] = {9000};
    EXPECT_EQ(0, poseidon_channel_manager_handle_bootstrap_reply(
        mgr, topic, 0x7F000001, 9000, timestamp_us, seed_addrs, seed_ports, 1));

    // Pending bootstrap should be removed and channel transitioned to RUNNING
    EXPECT_EQ(0u, mgr->num_pending_bootstraps);
    EXPECT_EQ(POSEIDON_CHANNEL_STATE_RUNNING, channel->state);
}

TEST_F(ChannelManagerBootstrapTest, HandleBootstrapReplyNotFound) {
    mgr = poseidon_channel_manager_create(dial_key_pair, 16000, 16001, 16010, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    // No pending bootstrap for this topic/timestamp
    EXPECT_EQ(-1, poseidon_channel_manager_handle_bootstrap_reply(
        mgr, "NoSuchTopic", 0x7F000001, 9000, 12345, NULL, NULL, 0));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
