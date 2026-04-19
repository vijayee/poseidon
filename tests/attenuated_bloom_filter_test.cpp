//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Bloom/attenuated_bloom_filter.h"

class AttenuatedBloomFilterTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(AttenuatedBloomFilterTest, CreateDestroy) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    ASSERT_NE(nullptr, abf);
    EXPECT_EQ(3u, attenuated_bloom_filter_level_count(abf));
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(AttenuatedBloomFilterTest, SubscribeAtLevel0) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* topic = (const uint8_t*)"sports";
    EXPECT_EQ(0, attenuated_bloom_filter_subscribe(abf, topic, 6));
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(abf, topic, 6, &hops));
    EXPECT_EQ(0u, hops);
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(AttenuatedBloomFilterTest, Unsubscribe) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* topic = (const uint8_t*)"sports";
    attenuated_bloom_filter_subscribe(abf, topic, 6);
    EXPECT_EQ(0, attenuated_bloom_filter_unsubscribe(abf, topic, 6));
    uint32_t hops = 0;
    EXPECT_FALSE(attenuated_bloom_filter_check(abf, topic, 6, &hops));
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(AttenuatedBloomFilterTest, MergePropagation) {
    attenuated_bloom_filter_t* a = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    attenuated_bloom_filter_t* b = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* topic = (const uint8_t*)"news";
    attenuated_bloom_filter_subscribe(b, topic, 4);
    EXPECT_EQ(0, attenuated_bloom_filter_merge(a, b));
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(a, topic, 4, &hops));
    EXPECT_EQ(1u, hops);
    attenuated_bloom_filter_destroy(a);
    attenuated_bloom_filter_destroy(b);
}

TEST_F(AttenuatedBloomFilterTest, GetLevel) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    EXPECT_NE(nullptr, attenuated_bloom_filter_get_level(abf, 0));
    EXPECT_NE(nullptr, attenuated_bloom_filter_get_level(abf, 1));
    EXPECT_NE(nullptr, attenuated_bloom_filter_get_level(abf, 2));
    EXPECT_EQ(nullptr, attenuated_bloom_filter_get_level(abf, 3));
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(AttenuatedBloomFilterTest, NoMatchReturnsFalse) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* topic = (const uint8_t*)"nonexistent";
    uint32_t hops = 0;
    EXPECT_FALSE(attenuated_bloom_filter_check(abf, topic, 11, &hops));
    attenuated_bloom_filter_destroy(abf);
}