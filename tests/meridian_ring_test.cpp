//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Meridian/meridian_ring.h"

class MeridianRingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ring_set = meridian_ring_set_create(8, 4, 2);
    }

    void TearDown() override {
        if (ring_set) {
            meridian_ring_set_destroy(ring_set);
            ring_set = NULL;
        }
    }

    meridian_ring_set_t* ring_set = NULL;
};

TEST_F(MeridianRingTest, CreateRingSet) {
    ASSERT_NE(nullptr, ring_set);
}

TEST_F(MeridianRingTest, InsertNodes) {
    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_node_t* node2 = meridian_node_create(0xC0A80002, 8080, NULL);
    meridian_node_t* node3 = meridian_node_create(0xC0A80003, 8080, NULL);

    ASSERT_NE(nullptr, node1);
    ASSERT_NE(nullptr, node2);
    ASSERT_NE(nullptr, node3);

    // Insert with various latencies
    EXPECT_EQ(0, meridian_ring_set_insert(ring_set, node1, 1000, NULL));
    EXPECT_EQ(0, meridian_ring_set_insert(ring_set, node2, 5000, NULL));
    EXPECT_EQ(0, meridian_ring_set_insert(ring_set, node3, 10000, NULL));

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_node_destroy(node3);
}

TEST_F(MeridianRingTest, FindClosest) {
    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_node_t* node2 = meridian_node_create(0xC0A80002, 8080, NULL);

    ASSERT_NE(nullptr, node1);
    ASSERT_NE(nullptr, node2);

    // node1 at latency 1 maps to ring 0 (lowest), node2 at latency 1000 maps to higher ring
    meridian_ring_set_insert(ring_set, node1, 1, NULL);
    meridian_ring_set_insert(ring_set, node2, 1000, NULL);

    meridian_node_t* closest = meridian_ring_set_find_closest(ring_set, 0xC0A80003, 8080);
    // Should return the lowest-latency node (ring 0)
    ASSERT_NE(nullptr, closest);
    EXPECT_EQ(closest->addr, node1->addr);

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
}

TEST_F(MeridianRingTest, FindClosestReturnsLowestRing) {
    meridian_node_t* node_fast = meridian_node_create(0xC0A80001, 8080, NULL);
    meridian_node_t* node_mid = meridian_node_create(0xC0A80002, 8080, NULL);
    meridian_node_t* node_slow = meridian_node_create(0xC0A80003, 8080, NULL);

    ASSERT_NE(nullptr, node_fast);
    ASSERT_NE(nullptr, node_mid);
    ASSERT_NE(nullptr, node_slow);

    // Insert at different latency tiers: fast (ring 0), medium (ring ~9), slow (ring ~13)
    meridian_ring_set_insert(ring_set, node_fast, 1, NULL);
    meridian_ring_set_insert(ring_set, node_mid, 1000, NULL);
    meridian_ring_set_insert(ring_set, node_slow, 10000, NULL);

    meridian_node_t* closest = meridian_ring_set_find_closest(ring_set, 0, 0);
    ASSERT_NE(nullptr, closest);
    EXPECT_EQ(closest->addr, node_fast->addr);

    meridian_node_destroy(node_fast);
    meridian_node_destroy(node_mid);
    meridian_node_destroy(node_slow);
}

TEST_F(MeridianRingTest, FindClosestSkipsEmptyRings) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);

    ASSERT_NE(nullptr, node);

    // Insert at latency 1000 (higher ring), leaving ring 0 empty
    meridian_ring_set_insert(ring_set, node, 1000, NULL);

    meridian_node_t* closest = meridian_ring_set_find_closest(ring_set, 0, 0);
    ASSERT_NE(nullptr, closest);
    // Should still return the node from the first non-empty ring
    EXPECT_EQ(closest->addr, node->addr);

    meridian_node_destroy(node);
}

TEST_F(MeridianRingTest, FindClosestEmptySet) {
    meridian_node_t* closest = meridian_ring_set_find_closest(ring_set, 0xC0A80001, 8080);
    EXPECT_EQ(nullptr, closest);
}

TEST_F(MeridianRingTest, EraseNode) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, meridian_ring_set_insert(ring_set, node, 1000, NULL));
    EXPECT_EQ(0, meridian_ring_set_erase(ring_set, node));

    meridian_node_destroy(node);
}

TEST_F(MeridianRingTest, FreezeRing) {
    meridian_ring_set_freeze(ring_set, 0);
    EXPECT_TRUE(ring_set->rings[0].frozen);

    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    // Latency 1 maps to ring 0, which is frozen - insert should fail
    EXPECT_NE(0, meridian_ring_set_insert(ring_set, node, 1, NULL));

    meridian_ring_set_unfreeze(ring_set, 0);
    EXPECT_FALSE(ring_set->rings[0].frozen);

    // Insert should succeed on unfrozen ring
    EXPECT_EQ(0, meridian_ring_set_insert(ring_set, node, 1, NULL));

    meridian_node_destroy(node);
}

TEST_F(MeridianRingTest, RingSelection) {
    // Ring exponent base is 2, so:
    // latency 1000 -> ring 9-10
    // latency 100 -> ring ~6-7
    // latency 10000 -> ring 13-14

    EXPECT_GE(meridian_ring_set_get_ring(ring_set, 100), 0);
    EXPECT_GE(meridian_ring_set_get_ring(ring_set, 1000), 0);
    EXPECT_GE(meridian_ring_set_get_ring(ring_set, 10000), 0);
    EXPECT_GE(meridian_ring_set_get_ring(ring_set, 1), 0);
    EXPECT_LT(meridian_ring_set_get_ring(ring_set, UINT32_MAX), MERIDIAN_MAX_RINGS);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}