//
// Created by victor on 4/21/26.
//

#include "channel_message.h"
#include <string.h>

cbor_item_t* channel_message_encode(const uint8_t* subtopic, size_t subtopic_len,
                                     const uint8_t* data, size_t data_len) {
    if (subtopic == NULL || data == NULL) return NULL;

    cbor_item_t* arr = cbor_new_definite_array(2);

    cbor_item_t* sub_str = cbor_build_stringn((const char*)subtopic, subtopic_len);
    if (sub_str == NULL) {
        cbor_decref(&arr);
        return NULL;
    }
    if (!cbor_array_push(arr, sub_str)) {
        cbor_decref(&sub_str);
        cbor_decref(&arr);
        return NULL;
    }

    cbor_item_t* data_bytes = cbor_build_bytestring(data, data_len);
    if (data_bytes == NULL) {
        cbor_decref(&arr);
        return NULL;
    }
    if (!cbor_array_push(arr, data_bytes)) {
        cbor_decref(&data_bytes);
        cbor_decref(&arr);
        return NULL;
    }

    // cbor_array_push increfs each item, so we can decref our local refs now
    cbor_decref(&sub_str);
    cbor_decref(&data_bytes);

    return arr;
}

int channel_message_decode(const cbor_item_t* item,
                            char* out_subtopic, size_t subtopic_buf_size,
                            uint8_t* out_data, size_t data_buf_size,
                            size_t* out_data_len) {
    if (item == NULL || out_subtopic == NULL || out_data == NULL || out_data_len == NULL)
        return -1;

    if (!cbor_isa_array(item) || cbor_array_size(item) != 2) return -1;

    cbor_item_t** items = cbor_array_handle((cbor_item_t*)item);

    // items[0]: subtopic string
    if (!cbor_isa_string(items[0])) return -1;
    size_t st_len = cbor_string_length(items[0]);
    if (st_len >= subtopic_buf_size) return -1;
    memcpy(out_subtopic, cbor_string_handle(items[0]), st_len);
    out_subtopic[st_len] = '\0';

    // items[1]: data bytestring
    if (!cbor_isa_bytestring(items[1])) return -1;
    size_t d_len = cbor_bytestring_length(items[1]);
    if (d_len > data_buf_size) return -1;
    memcpy(out_data, cbor_bytestring_handle(items[1]), d_len);
    *out_data_len = d_len;

    return 0;
}
