//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Bloom/bitset.h"

class BitsetTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(BitsetTest, CreateDestroy) {
    bitset_t* set = bitset_create(16);
    ASSERT_NE(nullptr, set);
    EXPECT_EQ(16u, bitset_size(set));
    EXPECT_EQ(128u, bitset_bit_count(set));
    bitset_destroy(set);
}

TEST_F(BitsetTest, GetSetBasic) {
    bitset_t* set = bitset_create(4);
    EXPECT_FALSE(bitset_get(set, 0));
    EXPECT_FALSE(bitset_get(set, 7));
    EXPECT_FALSE(bitset_get(set, 31));

    bitset_set(set, 0, true);
    EXPECT_TRUE(bitset_get(set, 0));

    bitset_set(set, 7, true);
    EXPECT_TRUE(bitset_get(set, 7));

    bitset_set(set, 31, true);
    EXPECT_TRUE(bitset_get(set, 31));
    EXPECT_FALSE(bitset_get(set, 30));

    bitset_destroy(set);
}

TEST_F(BitsetTest, SetReturnsOldValue) {
    bitset_t* set = bitset_create(4);
    EXPECT_FALSE(bitset_set(set, 10, true));
    EXPECT_TRUE(bitset_set(set, 10, true));
    EXPECT_TRUE(bitset_set(set, 10, false));
    EXPECT_FALSE(bitset_set(set, 10, false));
    bitset_destroy(set);
}

TEST_F(BitsetTest, AutoGrow) {
    bitset_t* set = bitset_create(1);
    EXPECT_EQ(1u, bitset_size(set));
    bitset_set(set, 20, true);
    EXPECT_TRUE(bitset_get(set, 20));
    EXPECT_GT(bitset_size(set), 1u);
    bitset_destroy(set);
}

TEST_F(BitsetTest, GetBeyondSizeReturnsFalse) {
    bitset_t* set = bitset_create(1);
    EXPECT_FALSE(bitset_get(set, 100));
    bitset_destroy(set);
}

TEST_F(BitsetTest, BitwiseOr) {
    bitset_t* a = bitset_create(2);
    bitset_t* b = bitset_create(2);
    bitset_set(a, 0, true);
    bitset_set(a, 3, true);
    bitset_set(b, 1, true);
    bitset_set(b, 3, true);

    bitset_t* result = bitset_or(a, b);
    EXPECT_TRUE(bitset_get(result, 0));
    EXPECT_TRUE(bitset_get(result, 1));
    EXPECT_TRUE(bitset_get(result, 3));
    EXPECT_FALSE(bitset_get(result, 2));

    bitset_destroy(a);
    bitset_destroy(b);
    bitset_destroy(result);
}

TEST_F(BitsetTest, BitwiseAnd) {
    bitset_t* a = bitset_create(2);
    bitset_t* b = bitset_create(2);
    bitset_set(a, 0, true);
    bitset_set(a, 3, true);
    bitset_set(b, 1, true);
    bitset_set(b, 3, true);

    bitset_t* result = bitset_and(a, b);
    EXPECT_FALSE(bitset_get(result, 0));
    EXPECT_TRUE(bitset_get(result, 3));

    bitset_destroy(a);
    bitset_destroy(b);
    bitset_destroy(result);
}

TEST_F(BitsetTest, BitwiseXor) {
    bitset_t* a = bitset_create(2);
    bitset_t* b = bitset_create(2);
    bitset_set(a, 0, true);
    bitset_set(a, 3, true);
    bitset_set(b, 1, true);
    bitset_set(b, 3, true);

    bitset_t* result = bitset_xor(a, b);
    EXPECT_TRUE(bitset_get(result, 0));
    EXPECT_TRUE(bitset_get(result, 1));
    EXPECT_FALSE(bitset_get(result, 3));

    bitset_destroy(a);
    bitset_destroy(b);
    bitset_destroy(result);
}

TEST_F(BitsetTest, BitwiseNot) {
    bitset_t* a = bitset_create(1);
    bitset_set(a, 0, true);

    bitset_t* result = bitset_not(a);
    EXPECT_FALSE(bitset_get(result, 0));
    EXPECT_TRUE(bitset_get(result, 1));
    EXPECT_TRUE(bitset_get(result, 7));

    bitset_destroy(a);
    bitset_destroy(result);
}

TEST_F(BitsetTest, Compare) {
    bitset_t* a = bitset_create(2);
    bitset_t* b = bitset_create(2);
    EXPECT_EQ(0, bitset_compare(a, b));
    EXPECT_TRUE(bitset_eq(a, b));

    bitset_set(a, 0, true);
    EXPECT_EQ(1, bitset_compare(a, b));

    bitset_destroy(a);
    bitset_destroy(b);
}

TEST_F(BitsetTest, Compact) {
    bitset_t* set = bitset_create(16);
    bitset_set(set, 0, true);
    bitset_compact(set);
    EXPECT_EQ(1u, bitset_size(set));
    bitset_destroy(set);
}