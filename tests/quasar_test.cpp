//
// Created by victor on 4/20/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include <arpa/inet.h>
#include "Network/Quasar/quasar.h"
#include "Network/Quasar/quasar_message_id.h"
#include "Bloom/attenuated_bloom_filter.h"

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

TEST_F(QuasarTest, SetDeliveryCallback) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    quasar_set_delivery_callback(q, NULL, NULL);
    quasar_destroy(q);
}

// ============================================================================
// ROUTE MESSAGE TESTS
// ============================================================================

class RouteMessageTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(RouteMessageTest, CreateDestroy) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);
    quasar_route_message_destroy(msg);
}

TEST_F(RouteMessageTest, AddVisitedAndCheck) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);

    meridian_node_t* node = meridian_node_create(htonl(0x0A000001), htons(8080));
    ASSERT_NE(nullptr, node);

    // Node should not be visited initially
    EXPECT_FALSE(quasar_route_message_has_visited(msg, node));

    // Add node to visited filter
    EXPECT_EQ(0, quasar_route_message_add_visited(msg, node));

    // Node should now be visited
    EXPECT_TRUE(quasar_route_message_has_visited(msg, node));

    meridian_node_destroy(node);
    quasar_route_message_destroy(msg);
}

TEST_F(RouteMessageTest, MultipleVisitedNodes) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);

    meridian_node_t* node1 = meridian_node_create(htonl(0x0A000001), htons(8080));
    meridian_node_t* node2 = meridian_node_create(htonl(0x0A000002), htons(8081));
    meridian_node_t* node3 = meridian_node_create(htonl(0x0A000003), htons(8082));

    EXPECT_EQ(0, quasar_route_message_add_visited(msg, node1));
    EXPECT_EQ(0, quasar_route_message_add_visited(msg, node2));

    EXPECT_TRUE(quasar_route_message_has_visited(msg, node1));
    EXPECT_TRUE(quasar_route_message_has_visited(msg, node2));
    EXPECT_FALSE(quasar_route_message_has_visited(msg, node3));

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_node_destroy(node3);
    quasar_route_message_destroy(msg);
}

TEST_F(RouteMessageTest, NullNodeHandling) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);

    EXPECT_EQ(-1, quasar_route_message_add_visited(msg, NULL));
    EXPECT_FALSE(quasar_route_message_has_visited(msg, NULL));

    quasar_route_message_destroy(msg);
}

TEST_F(RouteMessageTest, NullMessageHandling) {
    meridian_node_t* node = meridian_node_create(htonl(0x0A000001), htons(8080));
    EXPECT_EQ(-1, quasar_route_message_add_visited(NULL, node));
    EXPECT_FALSE(quasar_route_message_has_visited(NULL, node));
    meridian_node_destroy(node);
}

TEST_F(RouteMessageTest, NullTopicOrDataFails) {
    quasar_route_message_t* msg = quasar_route_message_create(
        NULL, 6, (const uint8_t*)"data", 4, 10, 256, 3
    );
    EXPECT_EQ(nullptr, msg);

    msg = quasar_route_message_create(
        (const uint8_t*)"topic", 5, NULL, 0, 10, 256, 3
    );
    EXPECT_EQ(nullptr, msg);
}

// ============================================================================
// DELIVERY CALLBACK TESTS
// ============================================================================

class DeliveryCallbackTest : public ::testing::Test {
protected:
    static void delivery_handler(void* ctx, const uint8_t* topic, size_t topic_len,
                                  const uint8_t* data, size_t data_len) {
        int* call_count = (int*)ctx;
        (*call_count)++;
    }

    int call_count = 0;
};

TEST_F(DeliveryCallbackTest, LocalDeliveryCallback) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_delivery_callback(q, delivery_handler, &call_count);
    quasar_subscribe(q, topic, 6, 100);

    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(1, call_count);

    quasar_destroy(q);
}

TEST_F(DeliveryCallbackTest, NoCallbackWhenNotSubscribed) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_delivery_callback(q, delivery_handler, &call_count);

    // Publish without subscribing — should not trigger callback
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(0, call_count);

    quasar_destroy(q);
}

TEST_F(DeliveryCallbackTest, NullCallbackDoesNotCrash) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_subscribe(q, topic, 6, 100);

    // No callback set — should not crash
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));

    quasar_destroy(q);
}

// ============================================================================
// GOSSIP DESERIALIZATION TEST
// ============================================================================

class GossipTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(GossipTest, InvalidMagicRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    uint8_t bad_data[28] = {0};
    // Magic should be 0x51534152, but set it to 0
    EXPECT_EQ(-1, quasar_on_gossip(q, bad_data, sizeof(bad_data), NULL));
    quasar_destroy(q);
}

TEST_F(GossipTest, NullDataRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    EXPECT_EQ(-1, quasar_on_gossip(q, NULL, 100, NULL));
    quasar_destroy(q);
}

TEST_F(GossipTest, TooShortDataRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    uint8_t short_data[10] = {0};
    EXPECT_EQ(-1, quasar_on_gossip(q, short_data, sizeof(short_data), NULL));
    quasar_destroy(q);
}

// ============================================================================
// ON ROUTE MESSAGE TEST
// ============================================================================

class OnRouteMessageTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(OnRouteMessageTest, NullMessageRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));
    EXPECT_EQ(-1, quasar_on_route_message(q, NULL, from));
    quasar_destroy(q);
    meridian_node_destroy(from);
}

TEST_F(OnRouteMessageTest, NullQuasarRejected) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));
    EXPECT_EQ(-1, quasar_on_route_message(NULL, msg, from));
    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
}

TEST_F(OnRouteMessageTest, LocalDeliveryViaRouteMessage) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_delivery_callback(q, [](void* ctx, const uint8_t* t, size_t tl,
                                        const uint8_t* d, size_t dl) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));

    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count);

    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(OnRouteMessageTest, ZeroHopsRemainingStopsForwarding) {
    quasar_t* q = quasar_create(NULL, 5, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    // Create message with 0 hops remaining
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 0, 256, 3
    );
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));

    // Should return 0 without crashing (message has expired)
    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));

    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

// ============================================================================
// MESSAGE ID TESTS
// ============================================================================

class MessageIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};

TEST_F(MessageIdTest, GetNextReturnsNonZero) {
    quasar_message_id_t id = quasar_message_id_get_next();
    EXPECT_GT(id.time, 0u);
}

TEST_F(MessageIdTest, GetNextProducesUniqueIDs) {
    quasar_message_id_t id1 = quasar_message_id_get_next();
    quasar_message_id_t id2 = quasar_message_id_get_next();
    EXPECT_NE(0, quasar_message_id_compare(&id1, &id2));
}

TEST_F(MessageIdTest, CompareSameID) {
    quasar_message_id_t id = quasar_message_id_get_next();
    EXPECT_EQ(0, quasar_message_id_compare(&id, &id));
}

TEST_F(MessageIdTest, CompareOrdering) {
    quasar_message_id_t id1 = quasar_message_id_get_next();
    quasar_message_id_t id2 = quasar_message_id_get_next();
    EXPECT_EQ(-1, quasar_message_id_compare(&id1, &id2));
    EXPECT_EQ(1, quasar_message_id_compare(&id2, &id1));
}

TEST_F(MessageIdTest, SerializeDeserializeRoundTrip) {
    quasar_message_id_t id = quasar_message_id_get_next();
    uint8_t buf[QUASAR_MESSAGE_ID_SIZE];
    quasar_message_id_serialize(&id, buf);
    quasar_message_id_t id2;
    quasar_message_id_deserialize(&id2, buf);
    EXPECT_EQ(0, quasar_message_id_compare(&id, &id2));
}

TEST_F(MessageIdTest, SerializedFieldsAreNetworkByteOrder) {
    quasar_message_id_t id = {0x0102030405060708ull, 0x0A0B0C0D0E0F0102ull, 42};
    uint8_t buf[QUASAR_MESSAGE_ID_SIZE];
    quasar_message_id_serialize(&id, buf);
    EXPECT_EQ(0x01, buf[0]);
    quasar_message_id_t id2;
    quasar_message_id_deserialize(&id2, buf);
    EXPECT_EQ(id.time, id2.time);
    EXPECT_EQ(id.nanos, id2.nanos);
    EXPECT_EQ(id.count, id2.count);
}