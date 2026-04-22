//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Meridian/meridian_measure.h"
#include "Util/threadding.h"

class MeridianMeasureTest : public ::testing::Test {
protected:
    void SetUp() override {
        cache = meridian_latency_cache_create(16);
    }

    void TearDown() override {
        if (cache) {
            meridian_latency_cache_destroy(cache);
            cache = NULL;
        }
    }

    meridian_latency_cache_t* cache = NULL;
};

TEST_F(MeridianMeasureTest, CreateCache) {
    ASSERT_NE(nullptr, cache);
}

TEST_F(MeridianMeasureTest, InsertAndRetrieve) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, meridian_latency_cache_insert(cache, node, 1500));

    uint32_t latency = 0;
    EXPECT_EQ(0, meridian_latency_cache_get(cache, node, &latency));
    EXPECT_EQ(1500, latency);

    meridian_node_destroy(node);
}

TEST_F(MeridianMeasureTest, UpdateExisting) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, meridian_latency_cache_insert(cache, node, 1000));
    EXPECT_EQ(0, meridian_latency_cache_insert(cache, node, 2000));

    uint32_t latency = 0;
    EXPECT_EQ(0, meridian_latency_cache_get(cache, node, &latency));
    EXPECT_EQ(2000, latency);

    meridian_node_destroy(node);
}

TEST_F(MeridianMeasureTest, MissOnEmpty) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    uint32_t latency = 999;
    EXPECT_NE(0, meridian_latency_cache_get(cache, node, &latency));
    EXPECT_EQ(999, latency); // unchanged

    meridian_node_destroy(node);
}

TEST_F(MeridianMeasureTest, CreateMeasureRequest) {
    meridian_node_t* target = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, target);

    meridian_measure_request_t* req = meridian_measure_request_create(
        12345, target, MERIDIAN_MEASURE_TYPE_TCP, 5000, NULL, NULL);

    ASSERT_NE(nullptr, req);
    EXPECT_EQ(12345, req->query_id);
    EXPECT_EQ(MERIDIAN_MEASURE_TYPE_TCP, req->type);
    EXPECT_FALSE(req->completed);

    meridian_measure_request_destroy(req);
    meridian_node_destroy(target);
}

TEST_F(MeridianMeasureTest, MeasureRequestExpiry) {
    meridian_node_t* target = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, target);

    meridian_measure_request_t* req = meridian_measure_request_create(
        12345, target, MERIDIAN_MEASURE_TYPE_TCP, 10, NULL, NULL);

    ASSERT_NE(nullptr, req);
    EXPECT_FALSE(meridian_measure_request_is_expired(req));

    // Simulate time passing - small timeout should expire quickly
    platform_usleep(20000); // 20ms

    EXPECT_TRUE(meridian_measure_request_is_expired(req));

    meridian_measure_request_destroy(req);
    meridian_node_destroy(target);
}

TEST_F(MeridianMeasureTest, NullCacheInsert) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, NULL);
    ASSERT_NE(nullptr, node);

    EXPECT_NE(0, meridian_latency_cache_insert(NULL, node, 1000));
    EXPECT_NE(0, meridian_latency_cache_insert(cache, NULL, 1000));

    meridian_node_destroy(node);
}

TEST_F(MeridianMeasureTest, NullCacheGet) {
    uint32_t latency = 0;
    EXPECT_NE(0, meridian_latency_cache_get(NULL, NULL, &latency));
    EXPECT_NE(0, meridian_latency_cache_get(cache, NULL, &latency));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}