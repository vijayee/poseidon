//
// Created by victor on 4/22/26.
//

#include "client_protocol.h"
#include "../Util/allocator.h"
#include <string.h>

// ============================================================================
// REQUEST ENCODING
// ============================================================================

cbor_item_t* client_protocol_encode_request(uint32_t request_id, uint8_t method,
                                             const char* topic_path,
                                             const uint8_t* payload, size_t payload_len) {
    if (topic_path == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(5);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(CLIENT_FRAME_REQUEST))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(request_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint8(method))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_path))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(payload, payload_len)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

// ============================================================================
// ADMIN REQUEST ENCODING
// ============================================================================

cbor_item_t* client_protocol_encode_admin_request(uint32_t request_id, uint8_t method,
                                                   const char* topic_path,
                                                   const uint8_t* signature, size_t sig_len,
                                                   const uint8_t* config_data, size_t config_len) {
    if (topic_path == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(6);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(CLIENT_FRAME_REQUEST))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(request_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint8(method))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_path))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(signature, sig_len))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(config_data, config_len)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

// ============================================================================
// RESPONSE ENCODING
// ============================================================================

cbor_item_t* client_protocol_encode_response(uint32_t request_id, uint8_t error_code,
                                              const char* result_data) {
    if (result_data == NULL) result_data = "";

    cbor_item_t* array = cbor_new_definite_array(4);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(CLIENT_FRAME_RESPONSE))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(request_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint8(error_code))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(result_data)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

// ============================================================================
// EVENT ENCODING
// ============================================================================

cbor_item_t* client_protocol_encode_event(uint8_t event_type,
                                           const char* topic_id,
                                           const char* subtopic,
                                           const uint8_t* data, size_t data_len) {
    if (topic_id == NULL || subtopic == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(5);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(CLIENT_FRAME_EVENT))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint8(event_type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(subtopic))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(data, data_len)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

// ============================================================================
// FRAME DECODING
// ============================================================================

int client_protocol_decode(const cbor_item_t* item, client_frame_t* out) {
    if (item == NULL || out == NULL || !cbor_array_is_definite(item)) return -1;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 1) return -1;

    cbor_item_t** items = cbor_array_handle(item);

    if (!cbor_isa_uint(items[0])) return -1;
    uint8_t frame_type = cbor_get_uint8(items[0]);

    memset(out, 0, sizeof(client_frame_t));
    out->frame_type = frame_type;

    if (frame_type == CLIENT_FRAME_REQUEST) {
        if (arr_size == 5) {
            // Regular request: [type, request_id, method, topic_path, payload]
            out->request_id = cbor_get_uint32(items[1]);
            out->method = cbor_get_uint8(items[2]);

            if (!cbor_isa_string(items[3])) return -1;
            size_t len = cbor_string_length(items[3]);
            if (len >= CLIENT_MAX_TOPIC_PATH) return -1;
            memcpy(out->topic_path, cbor_string_handle(items[3]), len);
            out->topic_path[len] = '\0';

            if (!cbor_isa_bytestring(items[4])) return -1;
            len = cbor_bytestring_length(items[4]);
            if (len > CLIENT_MAX_PAYLOAD) return -1;
            memcpy(out->payload, cbor_bytestring_handle(items[4]), len);
            out->payload_len = len;

            return 0;
        } else if (arr_size == 6) {
            // Admin request: [type, request_id, method, topic_path, signature, config]
            out->request_id = cbor_get_uint32(items[1]);
            out->method = cbor_get_uint8(items[2]);

            if (!cbor_isa_string(items[3])) return -1;
            size_t len = cbor_string_length(items[3]);
            if (len >= CLIENT_MAX_TOPIC_PATH) return -1;
            memcpy(out->topic_path, cbor_string_handle(items[3]), len);
            out->topic_path[len] = '\0';

            if (!cbor_isa_bytestring(items[4])) return -1;
            len = cbor_bytestring_length(items[4]);
            if (len > CLIENT_MAX_SIGNATURE) return -1;
            memcpy(out->signature, cbor_bytestring_handle(items[4]), len);
            out->signature_len = len;

            if (!cbor_isa_bytestring(items[5])) return -1;
            len = cbor_bytestring_length(items[5]);
            if (len > CLIENT_MAX_PAYLOAD) return -1;
            memcpy(out->payload, cbor_bytestring_handle(items[5]), len);
            out->payload_len = len;

            return 0;
        }
        return -1;
    } else if (frame_type == CLIENT_FRAME_RESPONSE) {
        if (arr_size < 4) return -1;
        out->request_id = cbor_get_uint32(items[1]);
        out->error_code = cbor_get_uint8(items[2]);

        if (!cbor_isa_string(items[3])) return -1;
        size_t len = cbor_string_length(items[3]);
        if (len >= CLIENT_MAX_RESULT_DATA) return -1;
        memcpy(out->result_data, cbor_string_handle(items[3]), len);
        out->result_data[len] = '\0';

        return 0;
    } else if (frame_type == CLIENT_FRAME_EVENT) {
        if (arr_size < 5) return -1;
        out->event_type = cbor_get_uint8(items[1]);

        if (!cbor_isa_string(items[2])) return -1;
        size_t len = cbor_string_length(items[2]);
        if (len >= CLIENT_MAX_TOPIC_PATH) return -1;
        memcpy(out->topic_path, cbor_string_handle(items[2]), len);
        out->topic_path[len] = '\0';

        if (!cbor_isa_string(items[3])) return -1;
        len = cbor_string_length(items[3]);
        if (len >= CLIENT_MAX_SUBTOPIC) return -1;
        memcpy(out->subtopic, cbor_string_handle(items[3]), len);
        out->subtopic[len] = '\0';

        if (!cbor_isa_bytestring(items[4])) return -1;
        len = cbor_bytestring_length(items[4]);
        if (len > CLIENT_MAX_PAYLOAD) return -1;
        memcpy(out->payload, cbor_bytestring_handle(items[4]), len);
        out->payload_len = len;

        return 0;
    }

    return -1;
}

// ============================================================================
// SERIALIZATION
// ============================================================================

int client_protocol_serialize(const cbor_item_t* frame, uint8_t** buf, size_t* len) {
    if (frame == NULL || buf == NULL || len == NULL) return -1;

    unsigned char* tmp = NULL;
    size_t tmp_len = 0;
    size_t written = cbor_serialize_alloc(frame, &tmp, &tmp_len);
    if (written == 0 || tmp == NULL) return -1;

    *buf = tmp;
    *len = written;
    return 0;
}
