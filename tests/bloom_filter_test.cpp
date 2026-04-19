//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Bloom/bloom_filter.h"

class BloomFilterTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(BloomFilterTest, CreateDestroy) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    ASSERT_NE(nullptr, filter);
    EXPECT_EQ(1024u, bloom_filter_size(filter));
    EXPECT_EQ(0u, bloom_filter_count(filter));
    bloom_filter_destroy(filter);
}

TEST_F(BloomFilterTest, AddAndContains) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    const uint8_t* elem = (const uint8_t*)"hello";
    bloom_filter_add(filter, elem, 5);
    EXPECT_TRUE(bloom_filter_contains(filter, elem, 5));
    EXPECT_EQ(1u, bloom_filter_count(filter));
    bloom_filter_destroy(filter);
}

TEST_F(BloomFilterTest, DoesNotContainMissing) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    const uint8_t* elem = (const uint8_t*)"hello";
    const uint8_t* missing = (const uint8_t*)"world";
    bloom_filter_add(filter, elem, 5);
    EXPECT_FALSE(bloom_filter_contains(filter, missing, 5));
    bloom_filter_destroy(filter);
}

TEST_F(BloomFilterTest, MultipleAdds) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    const uint8_t* a = (const uint8_t*)"alpha";
    const uint8_t* b = (const uint8_t*)"beta";
    const uint8_t* c = (const uint8_t*)"gamma";
    bloom_filter_add(filter, a, 5);
    bloom_filter_add(filter, b, 4);
    bloom_filter_add(filter, c, 5);
    EXPECT_TRUE(bloom_filter_contains(filter, a, 5));
    EXPECT_TRUE(bloom_filter_contains(filter, b, 4));
    EXPECT_TRUE(bloom_filter_contains(filter, c, 5));
    EXPECT_EQ(3u, bloom_filter_count(filter));
    bloom_filter_destroy(filter);
}

TEST_F(BloomFilterTest, Reset) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    const uint8_t* elem = (const uint8_t*)"hello";
    bloom_filter_add(filter, elem, 5);
    bloom_filter_reset(filter);
    EXPECT_FALSE(bloom_filter_contains(filter, elem, 5));
    EXPECT_EQ(0u, bloom_filter_count(filter));
    bloom_filter_destroy(filter);
}

TEST_F(BloomFilterTest, OptimalSize) {
    size_t size;
    uint32_t hash_count;
    bloom_filter_optimal_size(1000, 0.01, &size, &hash_count);
    EXPECT_GT(size, 0u);
    EXPECT_GT(hash_count, 0u);
    EXPECT_GT(size, 5000u);
    EXPECT_LT(hash_count, 20u);
}

TEST_F(BloomFilterTest, DuplicateAddDoesNotDoubleCount) {
    bloom_filter_t* filter = bloom_filter_create(1024, 3);
    const uint8_t* elem = (const uint8_t*)"hello";
    bloom_filter_add(filter, elem, 5);
    bloom_filter_add(filter, elem, 5);
    EXPECT_EQ(1u, bloom_filter_count(filter));
    bloom_filter_destroy(filter);
}