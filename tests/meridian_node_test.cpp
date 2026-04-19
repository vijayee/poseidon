//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Meridian/meridian.h"

class MeridianNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(MeridianNodeTest, CreateBasicNode) {
    uint32_t addr = 0xC0A80001; // 192.168.0.1
    uint16_t port = 8080;

    meridian_node_t* node = meridian_node_create(addr, port);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(addr, node->addr);
    EXPECT_EQ(port, node->port);
    EXPECT_EQ(0u, node->rendv_addr);
    EXPECT_EQ(0u, node->rendv_port);
    EXPECT_EQ(MERIDIAN_NODE_FLAG_NONE, node->flags);

    meridian_node_destroy(node);
}

TEST_F(MeridianNodeTest, CreateRendvNode) {
    uint32_t addr = 0xC0A80001;
    uint16_t port = 8080;
    uint32_t rendv_addr = 0xC0A80002;
    uint16_t rendv_port = 8081;

    meridian_node_t* node = meridian_node_create_rendv(addr, port, rendv_addr, rendv_port);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(addr, node->addr);
    EXPECT_EQ(port, node->port);
    EXPECT_EQ(rendv_addr, node->rendv_addr);
    EXPECT_EQ(rendv_port, node->rendv_port);
    EXPECT_EQ(MERIDIAN_NODE_FLAG_RENDEZVOUS, node->flags);

    meridian_node_destroy(node);
}

TEST_F(MeridianNodeTest, LatencyComparison) {
    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080);
    meridian_node_t* node2 = meridian_node_create(0xC0A80002, 8080);
    meridian_node_t* node3 = meridian_node_create(0xC0A80001, 8080); // same addr and port as node1

    ASSERT_NE(nullptr, node1);
    ASSERT_NE(nullptr, node2);
    ASSERT_NE(nullptr, node3);

    EXPECT_EQ(0, meridian_node_latency_cmp((const void*)&node1, (const void*)&node1));
    EXPECT_GT(0, meridian_node_latency_cmp((const void*)&node1, (const void*)&node2)); // lower addr first
    EXPECT_LT(0, meridian_node_latency_cmp((const void*)&node2, (const void*)&node1));
    EXPECT_EQ(0, meridian_node_latency_cmp((const void*)&node1, (const void*)&node3)); // same addr, same port

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_node_destroy(node3);
}

TEST_F(MeridianNodeTest, NullDestruction) {
    meridian_node_destroy(NULL);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}