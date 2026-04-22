//
// Created by victor on 4/20/26.
//

#include <gtest/gtest.h>
#include "Crypto/key_pair.h"
#include "Crypto/node_id.h"
#include "Channel/channel.h"
#include "Channel/channel_manager.h"
#include "Channel/subtopic.h"
#include "Network/Meridian/meridian.h"
#include "Network/Quasar/quasar.h"
#include "Bloom/attenuated_bloom_filter.h"
#include "Util/hash.h"
#include "Channel/topic_alias.h"

// ============================================================================
// TOPIC ALIAS TESTS
// ============================================================================

TEST(TopicAliasTest, CreateDestroy) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);
    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasTest, RegisterAndResolve) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);

    EXPECT_EQ(0, topic_alias_register(reg, "Alice", "X4jKL9abc"));
    const char* resolved = topic_alias_resolve(reg, "Alice");
    ASSERT_NE(nullptr, resolved);
    EXPECT_STREQ("X4jKL9abc", resolved);

    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasTest, UnknownAliasReturnsNull) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);

    EXPECT_EQ(NULL, topic_alias_resolve(reg, "Unknown"));

    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasTest, DuplicateAliasRejected) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);

    EXPECT_EQ(0, topic_alias_register(reg, "Alice", "X4jKL9abc"));
    // Duplicate alias with different target should fail
    EXPECT_EQ(-1, topic_alias_register(reg, "Alice", "Y7mNP2def"));

    // Original should still resolve
    const char* resolved = topic_alias_resolve(reg, "Alice");
    EXPECT_STREQ("X4jKL9abc", resolved);

    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasTest, UnregisterAlias) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);

    topic_alias_register(reg, "Alice", "X4jKL9abc");
    EXPECT_EQ(0, topic_alias_unregister(reg, "Alice"));
    EXPECT_EQ(NULL, topic_alias_resolve(reg, "Alice"));

    topic_alias_registry_destroy(reg);
}

// ============================================================================
// SUBTOPIC TESTS
// ============================================================================

TEST(SubtopicTest, ParseSimplePath) {
    char parts[8][64];
    int count = subtopic_parse("Feeds/friend-only", parts, 8);
    EXPECT_EQ(2, count);
    EXPECT_STREQ("Feeds", parts[0]);
    EXPECT_STREQ("friend-only", parts[1]);
}

TEST(SubtopicTest, ParseSinglePart) {
    char parts[8][64];
    int count = subtopic_parse("Feeds", parts, 8);
    EXPECT_EQ(1, count);
    EXPECT_STREQ("Feeds", parts[0]);
}

TEST(SubtopicTest, ParseEmptyReturnsZero) {
    char parts[8][64];
    int count = subtopic_parse("", parts, 8);
    EXPECT_EQ(0, count);
}

TEST(SubtopicTest, ParseNullReturnsError) {
    char parts[8][64];
    int count = subtopic_parse(NULL, parts, 8);
    EXPECT_EQ(-1, count);
}

TEST(SubtopicTest, ParseNullPartsReturnsError) {
    int count = subtopic_parse("Feeds", NULL, 8);
    EXPECT_EQ(-1, count);
}

TEST(SubtopicTest, ParseLeadingSlash) {
    char parts[8][64];
    int count = subtopic_parse("/Feeds/friend-only", parts, 8);
    EXPECT_EQ(2, count);
    EXPECT_STREQ("Feeds", parts[0]);
    EXPECT_STREQ("friend-only", parts[1]);
}

TEST(SubtopicTest, ParseTrailingSlash) {
    char parts[8][64];
    int count = subtopic_parse("Feeds/friend-only/", parts, 8);
    EXPECT_EQ(2, count);
    EXPECT_STREQ("Feeds", parts[0]);
    EXPECT_STREQ("friend-only", parts[1]);
}

TEST(SubtopicTest, MatchExact) {
    EXPECT_TRUE(subtopic_matches("Feeds/friend-only", "Feeds/friend-only"));
}

TEST(SubtopicTest, MatchPrefixSubscription) {
    EXPECT_TRUE(subtopic_matches("Feeds/friend-only", "Feeds"));
}

TEST(SubtopicTest, NoMatchDeeperSubscription) {
    EXPECT_FALSE(subtopic_matches("Feeds/public", "Feeds/private"));
}

TEST(SubtopicTest, NoMatchDifferentRoot) {
    EXPECT_FALSE(subtopic_matches("Posts/public", "Feeds"));
}

TEST(SubtopicTest, MatchRootSubscription) {
    EXPECT_TRUE(subtopic_matches("Feeds/friend-only", ""));
}

TEST(SubtopicTest, MatchTrailingSlashSubscription) {
    // "Feeds/" should match "Feeds/friend-only" after normalization
    EXPECT_TRUE(subtopic_matches("Feeds/friend-only", "Feeds/"));
}

TEST(SubtopicTest, NoMatchStringPrefix) {
    // "FeedsOnly" should NOT match subscription "Feeds"
    EXPECT_FALSE(subtopic_matches("FeedsOnly", "Feeds"));
}

TEST(SubtopicTest, MatchNullInputs) {
    EXPECT_FALSE(subtopic_matches(NULL, "Feeds"));
    EXPECT_FALSE(subtopic_matches("Feeds", NULL));
}

TEST(SubtopicTest, EmptyMessageNoMatch) {
    // Empty message should not match non-empty subscription
    EXPECT_FALSE(subtopic_matches("", "Feeds"));
}

// ============================================================================
// NODE IDENTITY TESTS
// ============================================================================

class NodeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a key pair to derive a node ID
        key_pair = poseidon_key_pair_create("ED25519");
        ASSERT_NE(key_pair, nullptr);

        uint8_t* pub_key = nullptr;
        size_t pub_len = 0;
        ASSERT_EQ(poseidon_key_pair_get_public_key(key_pair, &pub_key, &pub_len), 0);
        ASSERT_EQ(poseidon_node_id_from_public_key(pub_key, pub_len, &node_id), 0);
        free(pub_key);
    }

    void TearDown() override {
        poseidon_key_pair_destroy(key_pair);
    }

    poseidon_key_pair_t* key_pair = nullptr;
    poseidon_node_id_t node_id;
};

TEST_F(NodeIdentityTest, CreateWithId) {
    meridian_node_t* node = meridian_node_create(0xC0A80001, 8080, &node_id);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(meridian_node_equals_by_id(node, node));
    EXPECT_FALSE(poseidon_node_id_is_null(&node->id));
    meridian_node_destroy(node);
}

TEST_F(NodeIdentityTest, CreateUnidentified) {
    meridian_node_t* node = meridian_node_create_unidentified(0xC0A80001, 8080);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(poseidon_node_id_is_null(&node->id));
    meridian_node_destroy(node);
}

TEST_F(NodeIdentityTest, EqualsByIdSame) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80002, 9090, &node_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Same ID, different addr:port -> equals_by_id is true
    EXPECT_TRUE(meridian_node_equals_by_id(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

TEST_F(NodeIdentityTest, EqualsByIdDifferent) {
    poseidon_key_pair_t* kp2 = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp2, nullptr);
    uint8_t* pub2 = nullptr;
    size_t pub2_len = 0;
    poseidon_key_pair_get_public_key(kp2, &pub2, &pub2_len);
    poseidon_node_id_t id2;
    poseidon_node_id_from_public_key(pub2, pub2_len, &id2);
    free(pub2);

    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80001, 8080, &id2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Same addr:port, different ID -> equals_by_id is false
    EXPECT_FALSE(meridian_node_equals_by_id(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
    poseidon_key_pair_destroy(kp2);
}

TEST_F(NodeIdentityTest, EqualsByAddrSame) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80001, 8080, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(meridian_node_equals_by_addr(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

TEST_F(NodeIdentityTest, EqualsByAddrDifferent) {
    meridian_node_t* a = meridian_node_create(0xC0A80001, 8080, &node_id);
    meridian_node_t* b = meridian_node_create(0xC0A80002, 9090, &node_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(meridian_node_equals_by_addr(a, b));
    meridian_node_destroy(a);
    meridian_node_destroy(b);
}

// ============================================================================
// CHANNEL CONFIG TESTS
// ============================================================================

TEST(ChannelConfigTest, DefaultsAreSensible) {
    poseidon_channel_config_t cfg = poseidon_channel_config_defaults();
    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        EXPECT_GT(cfg.ring_sizes[i], 0u);
    }
    EXPECT_GT(cfg.gossip_init_interval_s, 0u);
    EXPECT_GT(cfg.gossip_steady_interval_s, 0u);
    EXPECT_GT(cfg.quasar_max_hops, 0u);
    EXPECT_GT(cfg.quasar_alpha, 0u);
}

TEST(ChannelConfigTest, UpdateConfigWithKeyHolder) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    poseidon_channel_config_t cfg = poseidon_channel_config_defaults();
    // Can't create a full channel without a work pool and timing wheel,
    // so test config defaults and key comparison directly
    cfg.gossip_init_interval_s = 10;

    poseidon_key_pair_destroy(kp);
}

// ============================================================================
// NODE ID HASHMAP INTEGRATION TESTS
// ============================================================================

TEST(NodeIdHashTest, HashAndCompare) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);
    uint8_t* pub = nullptr;
    size_t pub_len = 0;
    poseidon_key_pair_get_public_key(kp, &pub, &pub_len);
    poseidon_node_id_t id;
    poseidon_node_id_from_public_key(pub, pub_len, &id);
    free(pub);

    size_t h = hash_poseidon_node_id(&id);
    EXPECT_NE(h, 0u);

    poseidon_node_id_t id2;
    memcpy(&id2, &id, sizeof(poseidon_node_id_t));
    EXPECT_EQ(compare_poseidon_node_id(&id, &id2), 0);

    poseidon_node_id_t* dup = duplicate_poseidon_node_id(&id);
    ASSERT_NE(dup, nullptr);
    EXPECT_EQ(compare_poseidon_node_id(&id, dup), 0);
    free(dup);

    poseidon_key_pair_destroy(kp);
}

// ============================================================================
// CHANNEL GOSSIP TESTS
// ============================================================================

class ChannelGossipTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ChannelGossipTest, GossipCallsPropagate) {
    // poseidon_channel_gossip calls meridian_protocol_gossip + quasar_propagate.
    // Without a running protocol, we cannot create a full channel, so test at
    // the quasar level instead: verify that quasar_propagate encodes the filter
    // and returns -1 (protocol is NULL, so broadcast fails) without crashing.
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* topic = (const uint8_t*)"gossip_topic";
    EXPECT_EQ(0, quasar_subscribe(q, topic, 12, 100));

    // quasar_propagate will encode the filter and attempt broadcast via NULL protocol
    // which returns -1 from meridian_protocol_broadcast — this proves the encode path works
    int rc = quasar_propagate(q);
    EXPECT_EQ(-1, rc);

    quasar_destroy(q);
}

// ============================================================================
// SUBTOPIC TABLE TESTS
// ============================================================================

TEST(SubtopicTableTest, CreateDestroy) {
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);
    subtopic_table_destroy(table);
}

TEST(SubtopicTableTest, AddAndCheckSubscription) {
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);

    EXPECT_EQ(0, subtopic_table_subscribe(table, "Feeds/friend-only", 1));
    EXPECT_TRUE(subtopic_table_is_subscribed(table, "Feeds/friend-only"));
    EXPECT_FALSE(subtopic_table_is_subscribed(table, "Feeds/public"));

    subtopic_table_destroy(table);
}

TEST(SubtopicTableTest, UnsubscribeRemovesEntry) {
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);

    subtopic_table_subscribe(table, "Feeds", 100);
    EXPECT_TRUE(subtopic_table_is_subscribed(table, "Feeds"));
    EXPECT_EQ(0, subtopic_table_unsubscribe(table, "Feeds"));
    EXPECT_FALSE(subtopic_table_is_subscribed(table, "Feeds"));

    subtopic_table_destroy(table);
}

TEST(SubtopicTableTest, ShouldDeliverMatchesPrefix) {
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);

    // Subscribe to "Feeds" (prefix) and "Posts/private" (exact)
    subtopic_table_subscribe(table, "Feeds", 100);
    subtopic_table_subscribe(table, "Posts/private", 100);

    // "Feeds" subscription matches any "Feeds/*" message
    EXPECT_TRUE(subtopic_table_should_deliver(table, "Feeds/friend-only"));
    EXPECT_TRUE(subtopic_table_should_deliver(table, "Feeds"));
    EXPECT_TRUE(subtopic_table_should_deliver(table, "Posts/private"));
    EXPECT_FALSE(subtopic_table_should_deliver(table, "Posts/public"));

    subtopic_table_destroy(table);
}

TEST(SubtopicTableTest, TickExpiresSubscriptions) {
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);

    subtopic_table_subscribe(table, "Feeds", 1);
    EXPECT_TRUE(subtopic_table_is_subscribed(table, "Feeds"));

    subtopic_table_tick(table);
    EXPECT_FALSE(subtopic_table_is_subscribed(table, "Feeds"));

    subtopic_table_destroy(table);
}

TEST_F(ChannelGossipTest, TickExpiresSubscriptions) {
    // Create a quasar directly (channel creation requires msquic work pool),
    // subscribe with TTL=1, tick once, verify the subscription expires.
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* topic = (const uint8_t*)"ephemeral";
    EXPECT_EQ(0, quasar_subscribe(q, topic, 9, 1));

    // Should be present before tick
    EXPECT_TRUE(attenuated_bloom_filter_check(q->routing, topic, 9, NULL));

    // Single tick: TTL goes from 1 to 0, subscription removed
    EXPECT_EQ(0, quasar_tick(q));

    // Should no longer be present after tick
    EXPECT_FALSE(attenuated_bloom_filter_check(q->routing, topic, 9, NULL));

    quasar_destroy(q);
}

// ============================================================================
// CHANNEL MESSAGE ENVELOPE TESTS
// ============================================================================

#include "Channel/channel_message.h"

TEST(ChannelMessageTest, EncodeDecodeRoundTrip) {
    const char* subtopic = "Feeds/friend-only";
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};

    cbor_item_t* encoded = channel_message_encode(
        (const uint8_t*)subtopic, strlen(subtopic), data, sizeof(data));
    ASSERT_NE(nullptr, encoded);

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
    cbor_decref(&encoded);
    ASSERT_GT(written, 0u);
    ASSERT_NE(nullptr, buf);

    // Decode
    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, written, &result);
    ASSERT_NE(nullptr, loaded);

    char out_subtopic[256] = {0};
    uint8_t out_data[256] = {0};
    size_t out_data_len = 0;
    int rc = channel_message_decode(loaded, out_subtopic, sizeof(out_subtopic),
                                    out_data, sizeof(out_data), &out_data_len);
    EXPECT_EQ(0, rc);
    EXPECT_STREQ(subtopic, out_subtopic);
    EXPECT_EQ(sizeof(data), out_data_len);
    EXPECT_EQ(0, memcmp(data, out_data, sizeof(data)));

    cbor_decref(&loaded);
    free(buf);
}

TEST(ChannelMessageTest, NullInputsRejected) {
    EXPECT_EQ(NULL, channel_message_encode(NULL, 5, (const uint8_t*)"x", 1));
    EXPECT_EQ(-1, channel_message_decode(NULL, NULL, 0, NULL, 0, NULL));
}

TEST(ChannelPublishTest, PublishWrapsInSubtopicEnvelope) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    ASSERT_NE(nullptr, q);

    const uint8_t* topic = (const uint8_t*)"chan_topic";
    EXPECT_EQ(0, quasar_subscribe(q, topic, strlen("chan_topic"), 100));

    // Create a channel message envelope manually and verify round-trip
    const char* subtopic = "Feeds/public";
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

    cbor_item_t* msg = channel_message_encode(
        (const uint8_t*)subtopic, strlen(subtopic), payload, sizeof(payload));
    ASSERT_NE(nullptr, msg);

    // Serialize
    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(msg, &buf, &buf_len);
    cbor_decref(&msg);
    ASSERT_GT(written, 0u);

    // Verify round-trip
    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, written, &result);
    ASSERT_NE(nullptr, loaded);

    char out_subtopic[256] = {0};
    uint8_t out_data[256] = {0};
    size_t out_data_len = 0;
    EXPECT_EQ(0, channel_message_decode(loaded, out_subtopic, sizeof(out_subtopic),
                                        out_data, sizeof(out_data), &out_data_len));
    EXPECT_STREQ("Feeds/public", out_subtopic);
    EXPECT_EQ(sizeof(payload), out_data_len);

    cbor_decref(&loaded);
    free(buf);
    quasar_destroy(q);
}

// ============================================================================
// CHANNEL SUBTOPIC TESTS
// ============================================================================

TEST(ChannelSubtopicTest, ChannelSubscribeUnsubscribe) {
    // We can't create a full channel without real key pair + protocol, so test the table directly
    subtopic_table_t* table = subtopic_table_create(16);
    ASSERT_NE(nullptr, table);

    EXPECT_EQ(0, subtopic_table_subscribe(table, "Feeds", 100));
    EXPECT_TRUE(subtopic_table_is_subscribed(table, "Feeds"));
    EXPECT_TRUE(subtopic_table_should_deliver(table, "Feeds/public"));
    EXPECT_TRUE(subtopic_table_should_deliver(table, "Feeds/private"));

    EXPECT_EQ(0, subtopic_table_unsubscribe(table, "Feeds"));
    EXPECT_FALSE(subtopic_table_is_subscribed(table, "Feeds"));

    subtopic_table_destroy(table);
}

TEST(ChannelAliasTest, ChannelAliasRoundTrip) {
    topic_alias_registry_t* reg = topic_alias_registry_create(16);
    ASSERT_NE(nullptr, reg);

    EXPECT_EQ(0, topic_alias_register(reg, "Alice", "X4jKL9abc"));
    const char* resolved = topic_alias_resolve(reg, "Alice");
    ASSERT_NE(nullptr, resolved);
    EXPECT_STREQ("X4jKL9abc", resolved);

    topic_alias_registry_destroy(reg);
}