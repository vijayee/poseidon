//
// Created by victor on 4/22/26.
//

#include <gtest/gtest.h>
#include "client_libs/c/poseidon_client.h"
#include "Crypto/key_pair.h"
#include "ClientAPIs/client_protocol.h"

TEST(PoseidonClientTest, ConnectInvalidUrl) {
    poseidon_client_t* client = poseidon_client_connect("invalid://url");
    EXPECT_EQ(client, nullptr);
}

TEST(PoseidonClientTest, ConnectUnixNotFound) {
    poseidon_client_t* client = poseidon_client_connect("unix:///tmp/nonexistent_poseidon_test.sock");
    EXPECT_EQ(client, nullptr);
}

TEST(PoseidonClientTest, ConnectTcpNotFound) {
    poseidon_client_t* client = poseidon_client_connect("tcp://127.0.0.1:59999");
    EXPECT_EQ(client, nullptr);
}

TEST(PoseidonClientTest, NullArgs) {
    EXPECT_EQ(poseidon_client_connect(nullptr), nullptr);
    poseidon_client_disconnect(nullptr);

    EXPECT_EQ(poseidon_client_channel_create(nullptr, "test", nullptr, 0), -1);
    EXPECT_EQ(poseidon_client_channel_join(nullptr, "test", nullptr, 0), -1);
    EXPECT_EQ(poseidon_client_channel_leave(nullptr, "test"), -1);
    EXPECT_EQ(poseidon_client_subscribe(nullptr, "test"), -1);
    EXPECT_EQ(poseidon_client_unsubscribe(nullptr, "test"), -1);
    EXPECT_EQ(poseidon_client_publish(nullptr, "test", nullptr, 0), -1);
}

TEST(PoseidonClientTest, ProtocolEncodeRequest) {
    uint8_t payload[] = {0x01, 0x02, 0x03};
    cbor_item_t* frame = client_protocol_encode_request(
        1, CLIENT_METHOD_CHANNEL_CREATE, "test_channel", payload, 3);
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    EXPECT_EQ(client_protocol_decode(frame, &decoded), 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_REQUEST);
    EXPECT_EQ(decoded.request_id, 1u);
    EXPECT_EQ(decoded.method, CLIENT_METHOD_CHANNEL_CREATE);
    EXPECT_STREQ(decoded.topic_path, "test_channel");
    EXPECT_EQ(decoded.payload_len, 3u);

    cbor_decref(&frame);
}

TEST(PoseidonClientTest, ProtocolEncodeResponse) {
    cbor_item_t* frame = client_protocol_encode_response(42, CLIENT_ERROR_OK, "created");
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    EXPECT_EQ(client_protocol_decode(frame, &decoded), 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_RESPONSE);
    EXPECT_EQ(decoded.request_id, 42u);
    EXPECT_EQ(decoded.error_code, CLIENT_ERROR_OK);
    EXPECT_STREQ(decoded.result_data, "created");

    cbor_decref(&frame);
}

TEST(PoseidonClientTest, ProtocolEncodeEvent) {
    uint8_t data[] = {0xAA, 0xBB};
    cbor_item_t* frame = client_protocol_encode_event(
        CLIENT_EVENT_DELIVERY, "topic123", "sub", data, 2);
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    EXPECT_EQ(client_protocol_decode(frame, &decoded), 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_EVENT);
    EXPECT_EQ(decoded.event_type, CLIENT_EVENT_DELIVERY);
    EXPECT_STREQ(decoded.topic_path, "topic123");
    EXPECT_STREQ(decoded.subtopic, "sub");
    EXPECT_EQ(decoded.payload_len, 2u);

    cbor_decref(&frame);
}

TEST(PoseidonClientTest, AdminRequestSigning) {
    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    uint8_t data[] = {CLIENT_METHOD_CHANNEL_DESTROY};
    uint8_t sig[64];
    size_t sig_len = 0;
    EXPECT_EQ(poseidon_key_pair_sign(kp, data, sizeof(data), sig, &sig_len), 0);
    EXPECT_EQ(sig_len, 64u);

    cbor_item_t* frame = client_protocol_encode_admin_request(
        1, CLIENT_METHOD_CHANNEL_DESTROY, "topic_id",
        sig, sig_len, nullptr, 0);
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    EXPECT_EQ(client_protocol_decode(frame, &decoded), 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_REQUEST);
    EXPECT_EQ(decoded.method, CLIENT_METHOD_CHANNEL_DESTROY);
    EXPECT_EQ(decoded.signature_len, 64u);

    cbor_decref(&frame);
    poseidon_key_pair_destroy(kp);
}