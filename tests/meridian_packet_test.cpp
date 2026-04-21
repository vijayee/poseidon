//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Meridian/meridian_packet.h"

class MeridianPacketTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(MeridianPacketTest, CreateGossipPacket) {
    meridian_gossip_packet_t* pkt = meridian_gossip_packet_create();
    ASSERT_NE(nullptr, pkt);
    EXPECT_EQ(MERIDIAN_PACKET_TYPE_GOSSIP, pkt->base.type);
    EXPECT_EQ(MERIDIAN_MAGIC_NUMBER, pkt->base.magic);
    EXPECT_EQ(0, pkt->targets.length);
    meridian_gossip_packet_destroy(pkt);
}

TEST_F(MeridianPacketTest, AddGossipTargets) {
    meridian_gossip_packet_t* pkt = meridian_gossip_packet_create();
    ASSERT_NE(nullptr, pkt);

    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_node_t* node2 = meridian_node_create_rendv(0xC0A80002, 8081, 0xC0A80003, 8082, NULL);

    EXPECT_EQ(0, meridian_gossip_packet_add_target(pkt, node1));
    EXPECT_EQ(0, meridian_gossip_packet_add_target(pkt, node2));
    EXPECT_EQ(2, pkt->targets.length);

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_gossip_packet_destroy(pkt);
}

TEST_F(MeridianPacketTest, CreatePingPacket) {
    meridian_ping_packet_t* pkt = meridian_ping_packet_create();
    ASSERT_NE(nullptr, pkt);
    EXPECT_EQ(MERIDIAN_PACKET_TYPE_PING, pkt->base.type);
    EXPECT_EQ(MERIDIAN_MAGIC_NUMBER, pkt->base.magic);
    EXPECT_EQ(0, pkt->nodes.length);
    EXPECT_EQ(0, pkt->latencies.length);
    meridian_ping_packet_destroy(pkt);
}

TEST_F(MeridianPacketTest, AddPingNodes) {
    meridian_ping_packet_t* pkt = meridian_ping_packet_create();
    ASSERT_NE(nullptr, pkt);

    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, meridian_ping_packet_add_node(pkt, node, 1500));
    EXPECT_EQ(1, pkt->nodes.length);
    EXPECT_EQ(1, pkt->latencies.length);
    EXPECT_EQ(1500, pkt->latencies.data[0]);

    meridian_node_destroy(node);
    meridian_ping_packet_destroy(pkt);
}

TEST_F(MeridianPacketTest, CreateRetResponse) {
    meridian_ret_response_t* pkt = meridian_ret_response_create();
    ASSERT_NE(nullptr, pkt);
    EXPECT_EQ(MERIDIAN_PACKET_TYPE_RET_RESPONSE, pkt->type);
    EXPECT_EQ(MERIDIAN_MAGIC_NUMBER, pkt->magic);
    EXPECT_EQ(0, pkt->targets.length);
    meridian_ret_response_destroy(pkt);
}

TEST_F(MeridianPacketTest, AddRetResponseTargets) {
    meridian_ret_response_t* pkt = meridian_ret_response_create();
    ASSERT_NE(nullptr, pkt);

    EXPECT_EQ(0, meridian_ret_response_add_target(pkt, 0xC0A80001, 8080, 1000));
    EXPECT_EQ(0, meridian_ret_response_add_target(pkt, 0xC0A80002, 8081, 2000));
    EXPECT_EQ(2, pkt->targets.length);
    EXPECT_EQ(1000, pkt->targets.data[0].latency_us);
    EXPECT_EQ(2000, pkt->targets.data[1].latency_us);

    meridian_ret_response_destroy(pkt);
}

TEST_F(MeridianPacketTest, NodeFromLatency) {
    meridian_node_latency_t nl = {
        .addr = 0xC0A80001,
        .port = 8080,
        .latency_us = 5000
    };

    meridian_node_t* node = meridian_node_from_latency(&nl);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(0xC0A80001, node->addr);
    EXPECT_EQ(8080, node->port);

    meridian_node_destroy(node);
}

TEST_F(MeridianPacketTest, NullLatencyNode) {
    meridian_node_t* node = meridian_node_from_latency(NULL);
    EXPECT_EQ(nullptr, node);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}