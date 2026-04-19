//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Bloom/elastic_bloom_filter.h"

class ElasticBloomFilterTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ElasticBloomFilterTest, CreateDestroy) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    ASSERT_NE(nullptr, ebf);
    EXPECT_EQ(0u, elastic_bloom_filter_count(ebf));
    EXPECT_EQ(256u, elastic_bloom_filter_size(ebf));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, AddAndContains) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* elem = (const uint8_t*)"hello";
    EXPECT_EQ(0, elastic_bloom_filter_add(ebf, elem, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, elem, 5));
    EXPECT_EQ(1u, elastic_bloom_filter_count(ebf));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, DoesNotContainMissing) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* a = (const uint8_t*)"hello";
    const uint8_t* b = (const uint8_t*)"world";
    elastic_bloom_filter_add(ebf, a, 5);
    EXPECT_FALSE(elastic_bloom_filter_contains(ebf, b, 5));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, Remove) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* elem = (const uint8_t*)"hello";
    elastic_bloom_filter_add(ebf, elem, 5);
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, elem, 5));
    EXPECT_EQ(0, elastic_bloom_filter_remove(ebf, elem, 5));
    EXPECT_FALSE(elastic_bloom_filter_contains(ebf, elem, 5));
    EXPECT_EQ(0u, elastic_bloom_filter_count(ebf));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, AddRemoveReAdd) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* elem = (const uint8_t*)"hello";
    elastic_bloom_filter_add(ebf, elem, 5);
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, elem, 5));
    elastic_bloom_filter_remove(ebf, elem, 5);
    EXPECT_FALSE(elastic_bloom_filter_contains(ebf, elem, 5));
    elastic_bloom_filter_add(ebf, elem, 5);
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, elem, 5));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, MultipleElements) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(1024, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* a = (const uint8_t*)"alpha";
    const uint8_t* b = (const uint8_t*)"beta";
    const uint8_t* c = (const uint8_t*)"gamma";
    elastic_bloom_filter_add(ebf, a, 5);
    elastic_bloom_filter_add(ebf, b, 4);
    elastic_bloom_filter_add(ebf, c, 5);
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, a, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, b, 4));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, c, 5));
    elastic_bloom_filter_remove(ebf, b, 4);
    EXPECT_FALSE(elastic_bloom_filter_contains(ebf, b, 4));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, a, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, c, 5));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, Expand) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(64, 3, 0.5f, EBF_DEFAULT_FP_BITS);
    const uint8_t* a = (const uint8_t*)"test_expand";
    elastic_bloom_filter_add(ebf, a, 11);
    size_t old_size = elastic_bloom_filter_size(ebf);
    EXPECT_EQ(0, elastic_bloom_filter_expand(ebf));
    EXPECT_EQ(old_size * 2, elastic_bloom_filter_size(ebf));
    EXPECT_TRUE(elastic_bloom_filter_contains(ebf, a, 11));
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(ElasticBloomFilterTest, MergeSameSize) {
    elastic_bloom_filter_t* a = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    elastic_bloom_filter_t* b = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    const uint8_t* elem_a = (const uint8_t*)"alpha";
    const uint8_t* elem_b = (const uint8_t*)"beta";
    elastic_bloom_filter_add(a, elem_a, 5);
    elastic_bloom_filter_add(b, elem_b, 4);
    EXPECT_EQ(0, elastic_bloom_filter_merge(a, b));
    EXPECT_TRUE(elastic_bloom_filter_contains(a, elem_a, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(a, elem_b, 4));
    elastic_bloom_filter_destroy(a);
    elastic_bloom_filter_destroy(b);
}

TEST_F(ElasticBloomFilterTest, MergeFailsOnDifferentSize) {
    elastic_bloom_filter_t* a = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    elastic_bloom_filter_t* b = elastic_bloom_filter_create(512, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    EXPECT_NE(0, elastic_bloom_filter_merge(a, b));
    elastic_bloom_filter_destroy(a);
    elastic_bloom_filter_destroy(b);
}

TEST_F(ElasticBloomFilterTest, Ratio) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, EBF_DEFAULT_FP_BITS);
    float ratio = elastic_bloom_filter_ratio(ebf);
    EXPECT_EQ(0.0f, ratio);
    const uint8_t* elem = (const uint8_t*)"hello";
    elastic_bloom_filter_add(ebf, elem, 5);
    ratio = elastic_bloom_filter_ratio(ebf);
    EXPECT_GT(ratio, 0.0f);
    elastic_bloom_filter_destroy(ebf);
}