//
// Created by victor on 4/19/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Network/Meridian/meridian_query.h"

class MeridianQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        table = meridian_query_table_create(16);
    }

    void TearDown() override {
        if (table) {
            meridian_query_table_destroy(table);
            table = NULL;
        }
    }

    meridian_query_table_t* table = NULL;
};

TEST_F(MeridianQueryTest, CreateTable) {
    ASSERT_NE(nullptr, table);
}

TEST_F(MeridianQueryTest, CreateQuery) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_GOSSIP, 5000);

    ASSERT_NE(nullptr, query);
    EXPECT_EQ(12345, query->query_id);
    EXPECT_EQ(MERIDIAN_QUERY_TYPE_GOSSIP, query->type);
    EXPECT_EQ(MERIDIAN_QUERY_STATUS_INIT, query->status);
    EXPECT_EQ(0, query->num_targets);

    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, AddTargetsToQuery) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_CLOSEST, 5000);

    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080);
    meridian_node_t* node2 = meridian_node_create(0xC0A80002, 8081);

    ASSERT_NE(nullptr, node1);
    ASSERT_NE(nullptr, node2);

    EXPECT_EQ(0, meridian_query_add_target(query, node1));
    EXPECT_EQ(0, meridian_query_add_target(query, node2));
    EXPECT_EQ(2, query->num_targets);

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, SetLatency) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_MEASURE, 5000);

    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080);
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, meridian_query_add_target(query, node));
    EXPECT_EQ(0, meridian_query_set_latency(query, 0, 1500));
    EXPECT_EQ(1500, query->latencies[0]);

    meridian_node_destroy(node);
    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, GetClosest) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_CLOSEST, 5000);

    meridian_node_t* node1 = meridian_node_create(0xC0A80001, 8080);
    meridian_node_t* node2 = meridian_node_create(0xC0A80002, 8080);
    meridian_node_t* node3 = meridian_node_create(0xC0A80003, 8080);

    ASSERT_NE(nullptr, node1);
    ASSERT_NE(nullptr, node2);
    ASSERT_NE(nullptr, node3);

    EXPECT_EQ(0, meridian_query_add_target(query, node1));
    EXPECT_EQ(0, meridian_query_add_target(query, node2));
    EXPECT_EQ(0, meridian_query_add_target(query, node3));

    EXPECT_EQ(0, meridian_query_set_latency(query, 0, 5000));
    EXPECT_EQ(0, meridian_query_set_latency(query, 1, 1000)); // closest
    EXPECT_EQ(0, meridian_query_set_latency(query, 2, 3000));

    meridian_node_t* closest = meridian_query_get_closest(query);
    ASSERT_NE(nullptr, closest);
    EXPECT_EQ(0xC0A80002, closest->addr);

    meridian_node_destroy(node1);
    meridian_node_destroy(node2);
    meridian_node_destroy(node3);
    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, QueryExpiry) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_GOSSIP, 50); // 50ms timeout

    ASSERT_NE(nullptr, query);
    EXPECT_FALSE(meridian_query_is_expired(query));

    usleep(60000); // 60ms

    EXPECT_TRUE(meridian_query_is_expired(query));

    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, InsertAndLookup) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_GOSSIP, 5000);

    ASSERT_NE(nullptr, query);
    EXPECT_EQ(0, meridian_query_table_insert(table, query));

    meridian_query_t* found = meridian_query_table_lookup(table, 12345);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(12345, found->query_id);

    meridian_query_destroy(found);
    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, RemoveQuery) {
    meridian_query_t* query = meridian_query_create(
        12345, MERIDIAN_QUERY_TYPE_GOSSIP, 5000);

    ASSERT_NE(nullptr, query);
    EXPECT_EQ(0, meridian_query_table_insert(table, query));
    EXPECT_EQ(0, meridian_query_table_remove(table, 12345));

    meridian_query_t* found = meridian_query_table_lookup(table, 12345);
    EXPECT_EQ(nullptr, found);

    meridian_query_destroy(query);
}

TEST_F(MeridianQueryTest, TickWithExpired) {
    meridian_query_t* query1 = meridian_query_create(
        11111, MERIDIAN_QUERY_TYPE_GOSSIP, 5000);
    meridian_query_t* query2 = meridian_query_create(
        22222, MERIDIAN_QUERY_TYPE_GOSSIP, 5); // will expire quickly

    ASSERT_NE(nullptr, query1);
    ASSERT_NE(nullptr, query2);

    EXPECT_EQ(0, meridian_query_table_insert(table, query1));
    EXPECT_EQ(0, meridian_query_table_insert(table, query2));

    usleep(10000); // 10ms

    meridian_query_t** expired = NULL;
    size_t num_expired = 0;
    EXPECT_EQ(0, meridian_query_table_tick(table, &expired, &num_expired));
    EXPECT_EQ(1, num_expired);
    EXPECT_EQ(22222, expired[0]->query_id);

    for (size_t i = 0; i < num_expired; i++) {
        meridian_query_destroy(expired[i]);
    }
    free(expired);
    // Note: query1 is still in the table, it will be destroyed by TearDown's destroy(table)
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}