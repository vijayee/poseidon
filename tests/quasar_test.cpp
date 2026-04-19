//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Quasar/quasar.h"

class QuasarTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QuasarTest, CreateDestroy) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    ASSERT_NE(nullptr, q);
    quasar_destroy(q);
}

TEST_F(QuasarTest, Subscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    EXPECT_EQ(0, quasar_subscribe(q, topic, 6, 100));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Unsubscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_unsubscribe(q, topic, 6));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Publish) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    quasar_destroy(q);
}

TEST_F(QuasarTest, TickExpiresSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 2);
    quasar_tick(q);
    quasar_tick(q);
    attenuated_bloom_filter_t* routing = q->routing;
    EXPECT_FALSE(attenuated_bloom_filter_check(routing, topic, 6, NULL));
    quasar_destroy(q);
}

TEST_F(QuasarTest, MultipleSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* t1 = (const uint8_t*)"sports";
    const uint8_t* t2 = (const uint8_t*)"news";
    const uint8_t* t3 = (const uint8_t*)"tech";
    quasar_subscribe(q, t1, 6, 100);
    quasar_subscribe(q, t2, 4, 100);
    quasar_subscribe(q, t3, 4, 100);
    quasar_unsubscribe(q, t2, 4);
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, t1, 6, NULL));
    EXPECT_FALSE(attenuated_bloom_filter_check(q->routing, t2, 4, NULL));
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, t3, 4, NULL));
    quasar_destroy(q);
}
