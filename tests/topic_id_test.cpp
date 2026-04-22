//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "Channel/topic_id.h"
#include "Crypto/node_id.h"
#include "Channel/channel.h"
#include "Channel/topic_alias.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

TEST(TopicIdTest, FromNodeId) {
    poseidon_node_id_t node_id;
    memset(node_id.str, 0, sizeof(node_id.str));
    strncpy(node_id.str, "AbCdEfGhIjKlMnOpQrStUvWxYz1234", sizeof(node_id.str) - 1);

    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_node_id(&node_id, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 256);
    EXPECT_STREQ(tid.str, node_id.str);
}

TEST(TopicIdTest, GenerateRandom) {
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_generate(&tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 128);
    EXPECT_GT(strlen(tid.str), 0u);
}

TEST(TopicIdTest, GenerateUnique) {
    poseidon_topic_id_t tid1, tid2;
    poseidon_topic_id_generate(&tid1);
    poseidon_topic_id_generate(&tid2);
    EXPECT_NE(memcmp(tid1.bytes, tid2.bytes, 16), 0);
}

TEST(TopicIdTest, FromString256Bit) {
    const char* node_id_str = "AbCdEfGhIjKlMnOpQrStUvWxYz1234567890AbCdE";
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_string(node_id_str, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 256);
}

TEST(TopicIdTest, FromString128Bit) {
    const char* uuid_str = "X4jKL2mNpQrStUvWxYz12";
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_string(uuid_str, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 128);
}

TEST(TopicIdTest, FromStringNullFails) {
    poseidon_topic_id_t tid;
    EXPECT_NE(poseidon_topic_id_from_string(NULL, &tid), 0);
    EXPECT_NE(poseidon_topic_id_from_string("abc", NULL), 0);
}

TEST(TopicAliasResolveExTest, SingleMatch) {
    topic_alias_registry_t* reg = topic_alias_registry_create(8);
    ASSERT_NE(reg, nullptr);
    topic_alias_register(reg, "Alice", "TopicA");

    topic_alias_resolve_out_t result;
    int rc = topic_alias_resolve_ex(reg, "Alice", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(result.status, TOPIC_ALIAS_RESOLVE_OK);
    EXPECT_STREQ(result.topic, "TopicA");
    EXPECT_EQ(result.num_candidates, 1u);

    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasResolveExTest, AmbiguousMatch) {
    topic_alias_registry_t* reg = topic_alias_registry_create(8);
    ASSERT_NE(reg, nullptr);
    topic_alias_register(reg, "Alice", "TopicA");
    topic_alias_register(reg, "Alice", "TopicB");

    topic_alias_resolve_out_t result;
    int rc = topic_alias_resolve_ex(reg, "Alice", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(result.status, TOPIC_ALIAS_RESOLVE_AMBIGUOUS);
    EXPECT_EQ(result.num_candidates, 2u);

    topic_alias_registry_destroy(reg);
}

TEST(TopicAliasResolveExTest, NotFound) {
    topic_alias_registry_t* reg = topic_alias_registry_create(8);
    ASSERT_NE(reg, nullptr);

    topic_alias_resolve_out_t result;
    int rc = topic_alias_resolve_ex(reg, "Nobody", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(result.status, TOPIC_ALIAS_RESOLVE_NOT_FOUND);
    EXPECT_EQ(result.num_candidates, 0u);

    topic_alias_registry_destroy(reg);
}

TEST(PathResolveTest, RawTopicId) {
    work_pool_t* pool = work_pool_create(1);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(kp, "test", 14000, &config, pool, wheel);

    poseidon_path_resolve_result_t result;
    int rc = poseidon_resolve_path(channel, "X4jKL2mNpQrStUvWxYz12", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.ambiguous);
    EXPECT_STREQ(result.subtopic, "");

    poseidon_channel_destroy(channel);
    poseidon_key_pair_destroy(kp);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
}

TEST(PathResolveTest, AliasWithSubtopic) {
    work_pool_t* pool = work_pool_create(1);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(kp, "test", 14001, &config, pool, wheel);
    poseidon_channel_register_alias(channel, "Alice", "TopicAlice1234567890123456789012345678");

    poseidon_path_resolve_result_t result;
    int rc = poseidon_resolve_path(channel, "Alice/Feeds/friend-only", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.ambiguous);
    EXPECT_STREQ(result.topic_id.str, "TopicAlice1234567890123456789012345678");
    EXPECT_STREQ(result.subtopic, "Feeds/friend-only");

    poseidon_channel_destroy(channel);
    poseidon_key_pair_destroy(kp);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
}

TEST(PathResolveTest, AmbiguousAlias) {
    work_pool_t* pool = work_pool_create(1);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(kp, "test", 14002, &config, pool, wheel);
    poseidon_channel_register_alias(channel, "Alice", "TopicA1234567890123456789012345678");
    poseidon_channel_register_alias(channel, "Alice", "TopicB1234567890123456789012345678");

    poseidon_path_resolve_result_t result;
    int rc = poseidon_resolve_path(channel, "Alice", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.ambiguous);

    poseidon_channel_destroy(channel);
    poseidon_key_pair_destroy(kp);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
