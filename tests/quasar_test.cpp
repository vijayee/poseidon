//
// Created by victor on 4/20/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include <cbor.h>
#include "Network/Quasar/quasar.h"
#include "Network/Quasar/quasar_message_id.h"
#include "Util/portable_endian.h"
#include "Network/Quasar/quasar_route.h"
#include "Bloom/elastic_bloom_filter.h"
#include "Bloom/attenuated_bloom_filter.h"
#include "Network/Meridian/meridian_packet.h"

class QuasarTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};

TEST_F(QuasarTest, CreateDestroy) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);
    quasar_destroy(q);
}

TEST_F(QuasarTest, Subscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    EXPECT_EQ(0, quasar_subscribe(q, topic, 6, 100));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Unsubscribe) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_unsubscribe(q, topic, 6));
    quasar_destroy(q);
}

TEST_F(QuasarTest, Publish) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";
    quasar_subscribe(q, topic, 6, 100);
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    quasar_destroy(q);
}

TEST_F(QuasarTest, TickExpiresSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    quasar_subscribe(q, topic, 6, 2);
    quasar_tick(q);
    quasar_tick(q);
    attenuated_bloom_filter_t* routing = q->routing;
    EXPECT_FALSE(attenuated_bloom_filter_check(routing, topic, 6, NULL));
    quasar_destroy(q);
}

TEST_F(QuasarTest, MultipleSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
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

TEST_F(QuasarTest, SetMessageCallback) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    quasar_set_message_callback(q, NULL, NULL);
    quasar_destroy(q);
}

TEST_F(QuasarTest, NeighborFilterCreateAndLookup) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    meridian_node_t* neighbor = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    ASSERT_NE(nullptr, neighbor);

    // Initially no neighbor filters
    EXPECT_EQ(nullptr, quasar_get_neighbor_filter(q, neighbor));

    // Create a neighbor filter
    attenuated_bloom_filter_t* f = quasar_get_or_create_neighbor_filter(q, neighbor);
    ASSERT_NE(nullptr, f);

    // Lookup should now return the same filter
    attenuated_bloom_filter_t* f2 = quasar_get_neighbor_filter(q, neighbor);
    EXPECT_EQ(f, f2);

    // Remove the filter
    EXPECT_EQ(0, quasar_remove_neighbor_filter(q, neighbor));
    EXPECT_EQ(nullptr, quasar_get_neighbor_filter(q, neighbor));

    // Removing again should fail
    EXPECT_EQ(-1, quasar_remove_neighbor_filter(q, neighbor));

    meridian_node_destroy(neighbor);
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

// ============================================================================
// EBF SERIALIZATION TESTS
// ============================================================================

class EBFSerializationTest : public ::testing::Test {
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

    meridian_node_t* node = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
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

    meridian_node_t* node1 = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    meridian_node_t* node2 = meridian_node_create_unidentified(htobe32(0x0A000002), htobe16(8081));
    meridian_node_t* node3 = meridian_node_create_unidentified(htobe32(0x0A000003), htobe16(8082));

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
    meridian_node_t* node = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
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

TEST_F(RouteMessageTest, PublishersList) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);

    // Initially, publishers list is empty
    EXPECT_EQ(0, msg->pub_count);

    // Add a publisher
    meridian_node_t* pub1 = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    EXPECT_EQ(0, quasar_route_message_add_publisher(msg, pub1));
    EXPECT_EQ(1, msg->pub_count);

    // Add another publisher
    meridian_node_t* pub2 = meridian_node_create_unidentified(htobe32(0x0A000002), htobe16(8081));
    EXPECT_EQ(0, quasar_route_message_add_publisher(msg, pub2));
    EXPECT_EQ(2, msg->pub_count);

    // Check contains
    EXPECT_TRUE(quasar_route_message_has_publisher(msg, pub1));
    EXPECT_TRUE(quasar_route_message_has_publisher(msg, pub2));

    // Unknown node should not be in publisher list
    meridian_node_t* unknown = meridian_node_create_unidentified(htobe32(0x0A000003), htobe16(8082));
    EXPECT_FALSE(quasar_route_message_has_publisher(msg, unknown));

    // NULL args should be safe
    EXPECT_EQ(-1, quasar_route_message_add_publisher(NULL, pub1));
    EXPECT_EQ(-1, quasar_route_message_add_publisher(msg, NULL));
    EXPECT_FALSE(quasar_route_message_has_publisher(NULL, pub1));
    EXPECT_FALSE(quasar_route_message_has_publisher(msg, NULL));

    meridian_node_destroy(pub1);
    meridian_node_destroy(pub2);
    meridian_node_destroy(unknown);
    quasar_route_message_destroy(msg);
}

// ============================================================================
// DELIVERY CALLBACK TESTS
// ============================================================================

class MessageCallbackTest : public ::testing::Test {
protected:
    static void message_handler(void* ctx, const uint8_t* topic, size_t topic_len,
                                  const uint8_t* data, size_t data_len) {
        int* call_count = (int*)ctx;
        (*call_count)++;
    }

    int call_count = 0;
};

TEST_F(MessageCallbackTest, LocalMessageCallback) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_message_callback(q, message_handler, &call_count);
    quasar_subscribe(q, topic, 6, 100);

    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(1, call_count);

    quasar_destroy(q);
}

TEST_F(MessageCallbackTest, NoCallbackWhenNotSubscribed) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_message_callback(q, message_handler, &call_count);

    // Publish without subscribing — should not trigger callback
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(0, call_count);

    quasar_destroy(q);
}

TEST_F(MessageCallbackTest, NullCallbackDoesNotCrash) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
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
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    uint8_t bad_data[28] = {0};
    // Magic should be 0x51534152, but set it to 0
    EXPECT_EQ(-1, quasar_on_gossip(q, bad_data, sizeof(bad_data), NULL));
    quasar_destroy(q);
}

TEST_F(GossipTest, NullDataRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    EXPECT_EQ(-1, quasar_on_gossip(q, NULL, 100, NULL));
    quasar_destroy(q);
}

TEST_F(GossipTest, TooShortDataRejected) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    uint8_t short_data[10] = {0};
    EXPECT_EQ(-1, quasar_on_gossip(q, short_data, sizeof(short_data), NULL));
    quasar_destroy(q);
}

TEST_F(GossipTest, GossipStoresInNeighborFilter) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    meridian_node_t* neighbor = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    ASSERT_NE(nullptr, neighbor);

    // Build a CBOR-encoded gossip packet with a topic in level 0
    const uint8_t* topic = (const uint8_t*)"sports";
    uint32_t level_size = 1024;

    attenuated_bloom_filter_t* local_filter = attenuated_bloom_filter_create(
        5, level_size, 3, 0.75f, 8);
    ASSERT_NE(nullptr, local_filter);
    attenuated_bloom_filter_subscribe(local_filter, topic, 6);

    // Encode as CBOR: [type, level_count, encoded_filter]
    cbor_item_t* arr = cbor_new_definite_array(3);
    cbor_array_push(arr, cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP));
    cbor_array_push(arr, cbor_build_uint32(5));
    cbor_item_t* filter_item = attenuated_bloom_filter_encode(local_filter);
    ASSERT_NE(nullptr, filter_item);
    cbor_array_push(arr, filter_item);

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(arr, &buf, &buf_len);
    cbor_decref(&arr);
    attenuated_bloom_filter_destroy(local_filter);

    ASSERT_GT(written, 0u);
    ASSERT_NE(nullptr, buf);

    EXPECT_EQ(0, quasar_on_gossip(q, buf, written, neighbor));

    // Verify neighbor filter was created
    attenuated_bloom_filter_t* nf = quasar_get_neighbor_filter(q, neighbor);
    ASSERT_NE(nullptr, nf);

    // The topic should be findable in the neighbor filter
    EXPECT_TRUE(attenuated_bloom_filter_check(nf, topic, 6, NULL));

    // Also verify it's in the aggregated routing filter (shifted to level 1)
    uint32_t hops = 0;
    bool found = attenuated_bloom_filter_check(q->routing, topic, 6, &hops);
    EXPECT_TRUE(found);
    EXPECT_EQ(1u, hops); // shifted by +1

    free(buf);
    meridian_node_destroy(neighbor);
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
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
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
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    EXPECT_EQ(-1, quasar_on_route_message(NULL, msg, from));
    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
}

TEST_F(OnRouteMessageTest, LocalMessageViaRouteMessage) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_message_callback(q, [](void* ctx, const uint8_t* t, size_t tl,
                                        const uint8_t* d, size_t dl) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));

    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count);

    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(OnRouteMessageTest, ZeroHopsRemainingStopsForwarding) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    // Create message with 0 hops remaining
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 0, 256, 3
    );
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));

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

// ============================================================================
// DEDUP FILTER TESTS
// ============================================================================

class DedupFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};

TEST_F(DedupFilterTest, DuplicateRouteMessageDiscarded) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_message_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Create a route message and deliver it — should trigger callback
    quasar_route_message_t* msg = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count);

    // Deliver the SAME message again — dedup filter should discard it
    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count);

    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(DedupFilterTest, DifferentMessagesNotDiscarded) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_message_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Two different route messages — different IDs — should both be delivered
    quasar_route_message_t* msg1 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    quasar_route_message_t* msg2 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));

    EXPECT_EQ(0, quasar_on_route_message(q, msg1, from));
    EXPECT_EQ(1, call_count);

    EXPECT_EQ(0, quasar_on_route_message(q, msg2, from));
    EXPECT_EQ(2, call_count);

    quasar_route_message_destroy(msg1);
    quasar_route_message_destroy(msg2);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(DedupFilterTest, RouteMessageCarriesID) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    ASSERT_NE(nullptr, msg);

    // ID should have been auto-generated (time > 0)
    EXPECT_GT(msg->id.time, 0u);
    EXPECT_GT(msg->id.count + 1, 0u);

    quasar_route_message_destroy(msg);
}

TEST_F(QuasarTest, Algorithm2DirectedWalk) {
    // Node A subscribes to "sports"
    quasar_t* node_a = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    int delivered = 0;
    quasar_set_message_callback(node_a, [](void* ctx, const uint8_t*, size_t,
                                            const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &delivered);
    quasar_subscribe(node_a, topic, 6, 100);

    // Node B publishes to "sports" — has node A as a neighbor with "sports" in its filter
    quasar_t* node_b = quasar_create(NULL, 5, 3, 4096, 3);
    meridian_node_t* node_a_endpoint = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));

    // Set up node B's neighbor filter for node A
    attenuated_bloom_filter_t* nf = quasar_get_or_create_neighbor_filter(node_b, node_a_endpoint);
    ASSERT_NE(nullptr, nf);
    // Node A subscribes to "sports", so level 0 of A's filter has it
    attenuated_bloom_filter_subscribe(nf, topic, 6);
    // Also merge into routing for local check
    attenuated_bloom_filter_merge(node_b->routing, nf);

    // Check: node B's routing filter should show "sports" at hops > 0
    uint32_t hops = 0;
    bool found = attenuated_bloom_filter_check(node_b->routing, topic, 6, &hops);
    EXPECT_TRUE(found);
    EXPECT_GT(hops, 0u);

    // Publish from node B — should find directed route to node A
    const uint8_t* data = (const uint8_t*)"goal!";
    EXPECT_EQ(0, quasar_publish(node_b, topic, 6, data, 5));

    meridian_node_destroy(node_a_endpoint);
    quasar_destroy(node_b);
    quasar_destroy(node_a);
}

TEST_F(QuasarTest, NegativeInformationPreventsSelfLoop) {
    // Node subscribes to "sports" and has itself as a neighbor
    // Publishing should deliver locally but NOT forward to self
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";

    int delivered = 0;
    quasar_set_message_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &delivered);

    // Subscribe locally
    quasar_subscribe(q, topic, 6, 100);

    // Set up a neighbor filter for our own address (self-loop scenario)
    meridian_node_t* self_node = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    attenuated_bloom_filter_t* nf = quasar_get_or_create_neighbor_filter(q, self_node);
    ASSERT_NE(nullptr, nf);
    attenuated_bloom_filter_subscribe(nf, topic, 6);
    // Add self node ID to level 0 (this is what negative info checks against)
    uint8_t self_key[6];
    memcpy(self_key, &self_node->addr, sizeof(uint32_t));
    memcpy(self_key + sizeof(uint32_t), &self_node->port, sizeof(uint16_t));
    elastic_bloom_filter_add(attenuated_bloom_filter_get_level(nf, 0), self_key, sizeof(self_key));
    attenuated_bloom_filter_merge(q->routing, nf);

    // Publish — should deliver locally (we're subscribed) but NOT forward to self
    const uint8_t* data = (const uint8_t*)"goal!";
    EXPECT_EQ(0, quasar_publish(q, topic, 6, data, 5));
    EXPECT_EQ(1, delivered);

    meridian_node_destroy(self_node);
    quasar_destroy(q);
}

// ============================================================================
// ROUTE MESSAGE SERIALIZATION TESTS
// ============================================================================

class RouteSerializeTest : public ::testing::Test {
protected:
    void SetUp() override { quasar_message_id_init(); }
};

TEST_F(RouteSerializeTest, RoundTrip) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    quasar_route_message_t* msg = quasar_route_message_create(
        topic, 6, data, 5, 10, 256, 3
    );
    ASSERT_NE(nullptr, msg);

    meridian_node_t* pub1 = meridian_node_create_unidentified(htobe32(0x0A000001), htobe16(8080));
    quasar_route_message_add_publisher(msg, pub1);

    meridian_node_t* visited1 = meridian_node_create_unidentified(htobe32(0x0A000002), htobe16(8081));
    quasar_route_message_add_visited(msg, visited1);

    // Serialize
    uint8_t* buf = NULL;
    size_t buf_len = 0;
    EXPECT_EQ(0, quasar_route_message_serialize(msg, &buf, &buf_len));
    ASSERT_NE(nullptr, buf);
    EXPECT_GT(buf_len, 0u);

    // Deserialize
    quasar_route_message_t* msg2 = quasar_route_message_deserialize(buf, buf_len);
    ASSERT_NE(nullptr, msg2);

    // Verify fields match
    EXPECT_EQ(msg->hops_remaining, msg2->hops_remaining);
    EXPECT_EQ(msg->topic->size, msg2->topic->size);
    EXPECT_EQ(0, memcmp(msg->topic->data, msg2->topic->data, msg->topic->size));
    EXPECT_EQ(msg->data->size, msg2->data->size);
    EXPECT_EQ(0, memcmp(msg->data->data, msg2->data->data, msg->data->size));
    EXPECT_EQ(msg->pub_count, msg2->pub_count);
    if (msg2->pub_count > 0) {
        EXPECT_EQ(msg->pub_addrs[0], msg2->pub_addrs[0]);
        EXPECT_EQ(msg->pub_ports[0], msg2->pub_ports[0]);
    }

    // Verify visited filter
    EXPECT_TRUE(quasar_route_message_has_visited(msg2, visited1));

    free(buf);
    meridian_node_destroy(pub1);
    meridian_node_destroy(visited1);
    quasar_route_message_destroy(msg);
    quasar_route_message_destroy(msg2);
}

TEST_F(RouteSerializeTest, NullMessageRejected) {
    uint8_t* buf = NULL;
    size_t buf_len = 0;
    EXPECT_EQ(-1, quasar_route_message_serialize(NULL, &buf, &buf_len));
}

TEST_F(RouteSerializeTest, InvalidMagicRejected) {
    uint8_t bad_data[32] = {0};
    quasar_route_message_t* msg = quasar_route_message_deserialize(bad_data, sizeof(bad_data));
    EXPECT_EQ(nullptr, msg);
}
TEST_F(EBFSerializationTest, RoundTripPreservesData) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, ebf);

    const uint8_t* hello = (const uint8_t*)"hello";
    const uint8_t* world = (const uint8_t*)"world";
    ASSERT_EQ(0, elastic_bloom_filter_add(ebf, hello, 5));
    ASSERT_EQ(0, elastic_bloom_filter_add(ebf, world, 5));

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    // Serialize to bytes
    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    // Decode from bytes back to CBOR
    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);
    ASSERT_EQ(CBOR_ERR_NONE, load_result.error.code);

    // Decode CBOR back to EBF
    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // Verify both strings are still present
    EXPECT_TRUE(elastic_bloom_filter_contains(decoded, hello, 5));
    EXPECT_TRUE(elastic_bloom_filter_contains(decoded, world, 5));

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(EBFSerializationTest, EmptyFilterRoundTrip) {
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, ebf);

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // No items added, so count should be 0
    EXPECT_EQ(0u, elastic_bloom_filter_count(decoded));

    // Strings that were never added should not be found
    const uint8_t* absent = (const uint8_t*)"absent";
    EXPECT_FALSE(elastic_bloom_filter_contains(decoded, absent, 6));

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

TEST_F(EBFSerializationTest, DecodeWrongSizeReturnsNull) {
    // Manually construct a CBOR array with mismatched bitset_byte_len vs actual bytestring.
    // The EBF encode format is: [size, hash_count, fp_bits, seed_a, seed_b, bitset_byte_len, bitset_bytes, num_occupied, ...]
    cbor_item_t* arr = cbor_new_definite_array(8);
    ASSERT_NE(nullptr, arr);

    // bitset_byte_len says 32 bytes but the bytestring is only 4 — mismatch should fail
    (void)cbor_array_push(arr, cbor_build_uint64(64));           // size
    (void)cbor_array_push(arr, cbor_build_uint32(3));            // hash_count
    (void)cbor_array_push(arr, cbor_build_uint32(8));            // fp_bits
    (void)cbor_array_push(arr, cbor_build_uint64(1));            // seed_a
    (void)cbor_array_push(arr, cbor_build_uint64(2));            // seed_b
    (void)cbor_array_push(arr, cbor_build_uint64(32));           // bitset_byte_len (claims 32)
    uint8_t small[4] = {0};
    (void)cbor_array_push(arr, cbor_build_bytestring(small, 4)); // bitset_bytes (only 4)
    (void)cbor_array_push(arr, cbor_build_uint64(0));            // num_occupied

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(arr);
    EXPECT_EQ(nullptr, decoded);

    cbor_decref(&arr);
}

TEST_F(EBFSerializationTest, ExpandPreservesDataRoundTrip) {
    // Use a very small initial size with low omega to trigger expand
    elastic_bloom_filter_t* ebf = elastic_bloom_filter_create(16, 3, 0.5f, 8);
    ASSERT_NE(nullptr, ebf);

    // Add enough items to trigger expansion (omega=0.5 with size=16 means
    // expansion after ~8 occupied buckets)
    const uint8_t* items[] = {
        (const uint8_t*)"alpha", (const uint8_t*)"bravo",
        (const uint8_t*)"charlie", (const uint8_t*)"delta",
        (const uint8_t*)"echo", (const uint8_t*)"foxtrot",
        (const uint8_t*)"golf", (const uint8_t*)"hotel",
        (const uint8_t*)"india", (const uint8_t*)"juliet"
    };
    size_t lens[] = {5, 5, 7, 5, 4, 7, 4, 5, 5, 6};
    int num_items = 10;

    for (int i = 0; i < num_items; i++) {
        ASSERT_EQ(0, elastic_bloom_filter_add(ebf, items[i], lens[i]));
    }

    // Size should have expanded from 16
    EXPECT_GT(elastic_bloom_filter_size(ebf), 16u);

    cbor_item_t* encoded = elastic_bloom_filter_encode(ebf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    elastic_bloom_filter_t* decoded = elastic_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // All items should still be present after expand + round trip
    for (int i = 0; i < num_items; i++) {
        EXPECT_TRUE(elastic_bloom_filter_contains(decoded, items[i], lens[i]));
    }

    elastic_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    elastic_bloom_filter_destroy(ebf);
}

// ============================================================================
// ABF SERIALIZATION TESTS
// ============================================================================

class ABFSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ABFSerializationTest, RoundTripPreservesAllLevels) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, abf);

    const uint8_t* topic = (const uint8_t*)"topic1";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(abf, topic, 6));

    cbor_item_t* encoded = attenuated_bloom_filter_encode(abf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    // Level count preserved
    EXPECT_EQ(3u, attenuated_bloom_filter_level_count(decoded));

    // Topic should be found at level 0
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(decoded, topic, 6, &hops));
    EXPECT_EQ(0u, hops);

    attenuated_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(ABFSerializationTest, SingleLevelRoundTrip) {
    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(1, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, abf);

    const uint8_t* topic = (const uint8_t*)"single_level_topic";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(abf, topic, 19));

    cbor_item_t* encoded = attenuated_bloom_filter_encode(abf);
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded);

    EXPECT_EQ(1u, attenuated_bloom_filter_level_count(decoded));
    EXPECT_TRUE(attenuated_bloom_filter_check(decoded, topic, 19, nullptr));

    attenuated_bloom_filter_destroy(decoded);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
    attenuated_bloom_filter_destroy(abf);
}

TEST_F(ABFSerializationTest, MergeShiftsLevels) {
    // Create two ABFs with the same parameters
    attenuated_bloom_filter_t* dest = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    attenuated_bloom_filter_t* src = attenuated_bloom_filter_create(3, 256, 3, 0.75f, 8);
    ASSERT_NE(nullptr, dest);
    ASSERT_NE(nullptr, src);

    // Subscribe to "topic1" in src at level 0
    const uint8_t* topic = (const uint8_t*)"topic1";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(src, topic, 6));

    // Encode src, decode it, and merge into dest
    cbor_item_t* src_encoded = attenuated_bloom_filter_encode(src);
    ASSERT_NE(nullptr, src_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(src_encoded, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);

    struct cbor_load_result load_result;
    cbor_item_t* loaded = cbor_load(buf, buf_size, &load_result);
    ASSERT_NE(nullptr, loaded);

    attenuated_bloom_filter_t* decoded_src = attenuated_bloom_filter_decode(loaded);
    ASSERT_NE(nullptr, decoded_src);

    // Merge: src level 0 should shift to dest level 1
    EXPECT_EQ(0, attenuated_bloom_filter_merge(dest, decoded_src));

    // The topic should appear at level 1 in the merged dest
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(dest, topic, 6, &hops));
    EXPECT_EQ(1u, hops);

    attenuated_bloom_filter_destroy(decoded_src);
    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&src_encoded);
    attenuated_bloom_filter_destroy(src);
    attenuated_bloom_filter_destroy(dest);
}

// ============================================================================
// QUASAR GOSSIP TESTS
// ============================================================================

class QuasarGossipTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(QuasarGossipTest, PropagateEncodesFilter) {
    // Create a quasar with NULL protocol — propagate will fail at broadcast
    // because meridian_protocol_broadcast(NULL, ...) will fail, but we can
    // verify the encode path works by checking that propagate returns -1
    // (from the broadcast failure) rather than crashing.
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* topic = (const uint8_t*)"test";
    ASSERT_EQ(0, quasar_subscribe(q, topic, 4, 100));

    // propagate will encode the filter and then call meridian_protocol_broadcast(NULL, ...)
    // which should return -1, so propagate returns -1 (not crash)
    int rc = quasar_propagate(q);
    // Expected: -1 because protocol is NULL and broadcast fails
    EXPECT_EQ(-1, rc);

    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipDecodesAndMerges) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    // Subscribe "topic1" locally so we can verify local sub is still at level 0 after merge
    const uint8_t* local_topic = (const uint8_t*)"local_topic";
    ASSERT_EQ(0, quasar_subscribe(q, local_topic, 11, 100));

    // Build a QUASAR_GOSSIP packet manually:
    // [uint8(50), uint32(level_count), encoded_abf]
    attenuated_bloom_filter_t* remote_abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    ASSERT_NE(nullptr, remote_abf);

    const uint8_t* remote_topic = (const uint8_t*)"remote_topic";
    ASSERT_EQ(0, attenuated_bloom_filter_subscribe(remote_abf, remote_topic, 12));

    cbor_item_t* remote_encoded = attenuated_bloom_filter_encode(remote_abf);
    ASSERT_NE(nullptr, remote_encoded);

    cbor_item_t* packet = cbor_new_definite_array(3);
    ASSERT_NE(nullptr, packet);
    (void)cbor_array_push(packet, cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP));
    (void)cbor_array_push(packet, cbor_build_uint32(attenuated_bloom_filter_level_count(remote_abf)));
    (void)cbor_array_push(packet, remote_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(buf_size, 0u);

    // Create a dummy sender node
    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);
    ASSERT_NE(nullptr, from);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(0, rc);

    // The remote topic should appear at level 1 in the merged routing filter
    // (merge shifts src levels by +1)
    uint32_t hops = 0;
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, remote_topic, 12, &hops));
    EXPECT_EQ(1u, hops);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(remote_abf);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, MergePreservesLocalSubscriptions) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* local_topic = (const uint8_t*)"local_topic";
    ASSERT_EQ(0, quasar_subscribe(q, local_topic, 12, 100));

    // Build a minimal valid gossip packet with an empty ABF
    attenuated_bloom_filter_t* remote_abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    ASSERT_NE(nullptr, remote_abf);

    cbor_item_t* remote_encoded = attenuated_bloom_filter_encode(remote_abf);
    ASSERT_NE(nullptr, remote_encoded);

    cbor_item_t* packet = cbor_new_definite_array(3);
    (void)cbor_array_push(packet, cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP));
    (void)cbor_array_push(packet, cbor_build_uint32(attenuated_bloom_filter_level_count(remote_abf)));
    (void)cbor_array_push(packet, remote_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);

    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);
    ASSERT_NE(nullptr, from);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(0, rc);

    // local_topic should still be at level 0 after merge
    uint32_t hops = 99;
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, local_topic, 12, &hops));
    EXPECT_EQ(0u, hops);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(remote_abf);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipInvalidCBOR) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);

    int rc = quasar_on_gossip(q, garbage, sizeof(garbage), from);
    EXPECT_EQ(-1, rc);

    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(QuasarGossipTest, OnGossipWrongType) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    // Build a CBOR array with a wrong type byte (not QUASAR_GOSSIP=50)
    cbor_item_t* packet = cbor_new_definite_array(3);
    (void)cbor_array_push(packet, cbor_build_uint8(99));  // wrong type
    (void)cbor_array_push(packet, cbor_build_uint32(5));

    attenuated_bloom_filter_t* abf = attenuated_bloom_filter_create(5, 1024, 3, 0.75f, 8);
    cbor_item_t* abf_encoded = attenuated_bloom_filter_encode(abf);
    (void)cbor_array_push(packet, abf_encoded);

    unsigned char* buf = nullptr;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);

    meridian_node_t* from = meridian_node_create_unidentified(0x7F000001, 8080);

    int rc = quasar_on_gossip(q, buf, buf_size, from);
    EXPECT_EQ(-1, rc);

    meridian_node_destroy(from);
    free(buf);
    cbor_decref(&packet);
    attenuated_bloom_filter_destroy(abf);
    quasar_destroy(q);
}
