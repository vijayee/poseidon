//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_PROTOCOL_H
#define POSEIDON_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

// Frame types
#define CLIENT_FRAME_REQUEST  0x01
#define CLIENT_FRAME_RESPONSE 0x02
#define CLIENT_FRAME_EVENT    0x03

// Method codes
#define CLIENT_METHOD_CHANNEL_CREATE    1
#define CLIENT_METHOD_CHANNEL_JOIN      2
#define CLIENT_METHOD_CHANNEL_LEAVE     3
#define CLIENT_METHOD_CHANNEL_DESTROY   4
#define CLIENT_METHOD_CHANNEL_MODIFY    5
#define CLIENT_METHOD_SUBSCRIBE         6
#define CLIENT_METHOD_UNSUBSCRIBE       7
#define CLIENT_METHOD_PUBLISH           8
#define CLIENT_METHOD_ALIAS_REGISTER    9
#define CLIENT_METHOD_ALIAS_UNREGISTER  10
#define CLIENT_METHOD_ALIAS_RESOLVE     11

// Error codes
#define CLIENT_ERROR_OK                0
#define CLIENT_ERROR_UNKNOWN_METHOD    1
#define CLIENT_ERROR_INVALID_PARAMS    2
#define CLIENT_ERROR_CHANNEL_NOT_FOUND 3
#define CLIENT_ERROR_ALIAS_AMBIGUOUS  4
#define CLIENT_ERROR_NOT_AUTHORIZED    5
#define CLIENT_ERROR_CHANNEL_EXISTS    6
#define CLIENT_ERROR_TOO_MANY_CHANNELS 7
#define CLIENT_ERROR_TRANSPORT         8

// Event types
#define CLIENT_EVENT_DELIVERY       1
#define CLIENT_EVENT_CHANNEL_JOINED 2
#define CLIENT_EVENT_CHANNEL_LEFT    3
#define CLIENT_EVENT_PEER_EVENT      4

// String/buffer limits
#define CLIENT_MAX_TOPIC_PATH 256
#define CLIENT_MAX_RESULT_DATA 1024
#define CLIENT_MAX_SUBTOPIC  256
#define CLIENT_MAX_PAYLOAD   65536
#define CLIENT_MAX_SIGNATURE 64

// Decoded frame
typedef struct client_frame_t {
    uint8_t frame_type;
    uint32_t request_id;
    uint8_t method;
    uint8_t error_code;
    uint8_t event_type;
    char topic_path[CLIENT_MAX_TOPIC_PATH];
    char subtopic[CLIENT_MAX_SUBTOPIC];
    char result_data[CLIENT_MAX_RESULT_DATA];
    uint8_t payload[CLIENT_MAX_PAYLOAD];
    size_t payload_len;
    uint8_t signature[CLIENT_MAX_SIGNATURE];
    size_t signature_len;
    char name[64];
    char* candidates[8];
    size_t num_candidates;
} client_frame_t;

// Encode functions
cbor_item_t* client_protocol_encode_request(uint32_t request_id, uint8_t method,
                                             const char* topic_path,
                                             const uint8_t* payload, size_t payload_len);

cbor_item_t* client_protocol_encode_admin_request(uint32_t request_id, uint8_t method,
                                                   const char* topic_path,
                                                   const uint8_t* signature, size_t sig_len,
                                                   const uint8_t* config_data, size_t config_len);

cbor_item_t* client_protocol_encode_response(uint32_t request_id, uint8_t error_code,
                                              const char* result_data);

cbor_item_t* client_protocol_encode_event(uint8_t event_type,
                                           const char* topic_id,
                                           const char* subtopic,
                                           const uint8_t* data, size_t data_len);

// Decode function
int client_protocol_decode(const cbor_item_t* item, client_frame_t* out);

// Serialize to buffer
int client_protocol_serialize(const cbor_item_t* frame, uint8_t** buf, size_t* len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_PROTOCOL_H
