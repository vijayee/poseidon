//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include <string.h>
#include "ClientAPIs/client_protocol.h"

class ClientProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ClientProtocolTest, EncodeDecodeRequest) {
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    cbor_item_t* encoded = client_protocol_encode_request(
        42, CLIENT_METHOD_CHANNEL_CREATE, "test-topic", payload, sizeof(payload));
    ASSERT_NE(nullptr, encoded);

    uint8_t* buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(0, client_protocol_serialize(encoded, &buf, &len));
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(len, 0u);

    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, len, &result);
    ASSERT_NE(nullptr, loaded);

    client_frame_t frame = {};
    EXPECT_EQ(0, client_protocol_decode(loaded, &frame));
    EXPECT_EQ(CLIENT_FRAME_REQUEST, frame.frame_type);
    EXPECT_EQ(42u, frame.request_id);
    EXPECT_EQ(CLIENT_METHOD_CHANNEL_CREATE, frame.method);
    EXPECT_STREQ("test-topic", frame.topic_path);
    EXPECT_EQ(sizeof(payload), frame.payload_len);
    EXPECT_EQ(0, memcmp(payload, frame.payload, sizeof(payload)));

    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
}

TEST_F(ClientProtocolTest, EncodeDecodeResponse) {
    cbor_item_t* encoded = client_protocol_encode_response(
        99, CLIENT_ERROR_OK, "success");
    ASSERT_NE(nullptr, encoded);

    uint8_t* buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(0, client_protocol_serialize(encoded, &buf, &len));
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(len, 0u);

    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, len, &result);
    ASSERT_NE(nullptr, loaded);

    client_frame_t frame = {};
    EXPECT_EQ(0, client_protocol_decode(loaded, &frame));
    EXPECT_EQ(CLIENT_FRAME_RESPONSE, frame.frame_type);
    EXPECT_EQ(99u, frame.request_id);
    EXPECT_EQ(CLIENT_ERROR_OK, frame.error_code);
    EXPECT_STREQ("success", frame.result_data);

    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
}

TEST_F(ClientProtocolTest, EncodeDecodeEvent) {
    const uint8_t data[] = {0x01, 0x02, 0x03};
    cbor_item_t* encoded = client_protocol_encode_event(
        CLIENT_EVENT_MESSAGE, "topic-1", "sub/feed", data, sizeof(data));
    ASSERT_NE(nullptr, encoded);

    uint8_t* buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(0, client_protocol_serialize(encoded, &buf, &len));
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(len, 0u);

    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, len, &result);
    ASSERT_NE(nullptr, loaded);

    client_frame_t frame = {};
    EXPECT_EQ(0, client_protocol_decode(loaded, &frame));
    EXPECT_EQ(CLIENT_FRAME_EVENT, frame.frame_type);
    EXPECT_EQ(CLIENT_EVENT_MESSAGE, frame.event_type);
    EXPECT_STREQ("topic-1", frame.topic_path);
    EXPECT_STREQ("sub/feed", frame.subtopic);
    EXPECT_EQ(sizeof(data), frame.payload_len);
    EXPECT_EQ(0, memcmp(data, frame.payload, sizeof(data)));

    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
}

TEST_F(ClientProtocolTest, EncodeAdminRequest) {
    const uint8_t sig[] = {0xAB, 0xCD};
    const uint8_t config[] = {0x11, 0x22};
    cbor_item_t* encoded = client_protocol_encode_admin_request(
        7, CLIENT_METHOD_CHANNEL_MODIFY, "admin-topic", sig, sizeof(sig), config, sizeof(config));
    ASSERT_NE(nullptr, encoded);

    uint8_t* buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(0, client_protocol_serialize(encoded, &buf, &len));
    ASSERT_NE(nullptr, buf);
    ASSERT_GT(len, 0u);

    struct cbor_load_result result;
    cbor_item_t* loaded = cbor_load(buf, len, &result);
    ASSERT_NE(nullptr, loaded);

    client_frame_t frame = {};
    EXPECT_EQ(0, client_protocol_decode(loaded, &frame));
    EXPECT_EQ(CLIENT_FRAME_REQUEST, frame.frame_type);
    EXPECT_EQ(7u, frame.request_id);
    EXPECT_EQ(CLIENT_METHOD_CHANNEL_MODIFY, frame.method);
    EXPECT_STREQ("admin-topic", frame.topic_path);
    EXPECT_EQ(sizeof(sig), frame.signature_len);
    EXPECT_EQ(0, memcmp(sig, frame.signature, sizeof(sig)));
    EXPECT_EQ(sizeof(config), frame.payload_len);
    EXPECT_EQ(0, memcmp(config, frame.payload, sizeof(config)));

    cbor_decref(&loaded);
    free(buf);
    cbor_decref(&encoded);
}

TEST_F(ClientProtocolTest, DecodeInvalidReturnsError) {
    client_frame_t frame = {};
    EXPECT_EQ(-1, client_protocol_decode(nullptr, &frame));

    // Empty array
    cbor_item_t* empty = cbor_new_definite_array(0);
    ASSERT_NE(nullptr, empty);
    EXPECT_EQ(-1, client_protocol_decode(empty, &frame));
    cbor_decref(&empty);

    // Unknown frame type
    cbor_item_t* bad = cbor_new_definite_array(1);
    ASSERT_NE(nullptr, bad);
    cbor_array_push(bad, cbor_move(cbor_build_uint8(0xFF)));
    EXPECT_EQ(-1, client_protocol_decode(bad, &frame));
    cbor_decref(&bad);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
