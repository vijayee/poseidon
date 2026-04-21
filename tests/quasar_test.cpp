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

TEST_F(QuasarTest, SetDeliveryCallback) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    quasar_set_delivery_callback(q, NULL, NULL);
    quasar_destroy(q);
}

TEST_F(QuasarTest, NeighborFilterCreateAndLookup) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    meridian_node_t* neighbor = meridian_node_create(htonl(0x0A000001), htons(8080));
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
    meridian_node_t* pub1 = meridian_node_create(htonl(0x0A000001), htons(8080));
    EXPECT_EQ(0, quasar_route_message_add_publisher(msg, pub1));
    EXPECT_EQ(1, msg->pub_count);

    // Add another publisher
    meridian_node_t* pub2 = meridian_node_create(htonl(0x0A000002), htons(8081));
    EXPECT_EQ(0, quasar_route_message_add_publisher(msg, pub2));
    EXPECT_EQ(2, msg->pub_count);

    // Check contains
    EXPECT_TRUE(quasar_route_message_has_publisher(msg, pub1));
    EXPECT_TRUE(quasar_route_message_has_publisher(msg, pub2));

    // Unknown node should not be in publisher list
    meridian_node_t* unknown = meridian_node_create(htonl(0x0A000003), htons(8082));
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
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_delivery_callback(q, delivery_handler, &call_count);
    quasar_subscribe(q, topic, 6, 100);

    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(1, call_count);

    quasar_destroy(q);
}

TEST_F(DeliveryCallbackTest, NoCallbackWhenNotSubscribed) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* msg = (const uint8_t*)"goal!";

    quasar_set_delivery_callback(q, delivery_handler, &call_count);

    // Publish without subscribing — should not trigger callback
    EXPECT_EQ(0, quasar_publish(q, topic, 6, msg, 5));
    EXPECT_EQ(0, call_count);

    quasar_destroy(q);
}

TEST_F(DeliveryCallbackTest, NullCallbackDoesNotCrash) {
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

    meridian_node_t* neighbor = meridian_node_create(htonl(0x0A000001), htons(8080));
    ASSERT_NE(nullptr, neighbor);

    // Build a minimal gossip buffer with a topic in level 0
    // Header: magic, version, level_count, level_size, hash_count, omega, fp_bits
    uint32_t level_size = 1024;
    size_t bitset_bytes = level_size / 8;

    // First, add the topic to a local filter to get its bitset data
    const uint8_t* topic = (const uint8_t*)"sports";
    attenuated_bloom_filter_t* local_filter = attenuated_bloom_filter_create(
        5, level_size, 3, 0.75f, 8);
    ASSERT_NE(nullptr, local_filter);
    attenuated_bloom_filter_subscribe(local_filter, topic, 6);

    // Now serialize it using the same format as quasar_propagate
    // We'll manually build the buffer
    size_t header_size = 7 * sizeof(uint32_t);
    size_t level_data_size = sizeof(uint32_t) + bitset_bytes + sizeof(uint32_t); // count + bitset + num_buckets (0)
    size_t total = header_size + 5 * level_data_size; // 5 levels
    uint8_t* data = (uint8_t*)calloc(total, 1);

    // Header
    uint32_t* header = (uint32_t*)data;
    header[0] = htonl(0x51534152u); // magic
    header[1] = htonl(1u);          // version
    header[2] = htonl(5u);          // level_count
    header[3] = htonl(level_size);   // level_size
    header[4] = htonl(3u);          // hash_count
    header[5] = htonl(750u);        // omega * 1000
    header[6] = htonl(8u);          // fp_bits

    size_t offset = header_size;
    for (uint32_t level = 0; level < 5; level++) {
        elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(local_filter, level);
        // count
        *(uint32_t*)(data + offset) = htonl((uint32_t)elastic_bloom_filter_count(ebf));
        offset += sizeof(uint32_t);
        // bitset
        const uint8_t* bitset_data = NULL;
        size_t bitset_size = 0;
        elastic_bloom_filter_get_bitset(ebf, &bitset_data, &bitset_size);
        size_t copy_bytes = bitset_bytes < bitset_size ? bitset_bytes : bitset_size;
        if (bitset_data != NULL && copy_bytes > 0) {
            memcpy(data + offset, bitset_data, copy_bytes);
        }
        offset += bitset_bytes;
        // num_buckets (0 for simplicity — we're just testing bitset propagation)
        *(uint32_t*)(data + offset) = htonl(0u);
        offset += sizeof(uint32_t);
    }

    EXPECT_EQ(0, quasar_on_gossip(q, data, total, neighbor));

    // Verify neighbor filter was created
    attenuated_bloom_filter_t* nf = quasar_get_neighbor_filter(q, neighbor);
    ASSERT_NE(nullptr, nf);

    // The topic should be findable in the neighbor filter
    // (level 0 of incoming becomes level 0 of stored filter)
    EXPECT_TRUE(attenuated_bloom_filter_check(nf, topic, 6, NULL));

    // Also verify it's in the aggregated routing filter (shifted to level 1)
    uint32_t hops = 0;
    bool found = attenuated_bloom_filter_check(q->routing, topic, 6, &hops);
    EXPECT_TRUE(found);
    EXPECT_EQ(1u, hops); // shifted by +1

    free(data);
    attenuated_bloom_filter_destroy(local_filter);
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
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
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
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
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
    quasar_set_delivery_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Create a route message and deliver it — should trigger callback
    quasar_route_message_t* msg = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));
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
    quasar_set_delivery_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Two different route messages — different IDs — should both be delivered
    quasar_route_message_t* msg1 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    quasar_route_message_t* msg2 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));

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