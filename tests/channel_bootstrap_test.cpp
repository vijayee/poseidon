//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include <cbor.h>
#include <string.h>
#include "Network/Meridian/meridian_packet.h"

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
