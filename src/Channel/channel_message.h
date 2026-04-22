#ifndef POSEIDON_CHANNEL_MESSAGE_H
#define POSEIDON_CHANNEL_MESSAGE_H

#include <stdint.h>
#include <stddef.h>
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encodes a channel message as a CBOR array: [subtopic_string, data_bytes].
 * The subtopic allows the receiver to filter delivery by subscription.
 *
 * @param subtopic    Subtopic path (null-terminated string)
 * @param subtopic_len Length of subtopic string
 * @param data        Message payload bytes
 * @param data_len    Length of payload
 * @return            CBOR array item, or NULL on error
 */
cbor_item_t* channel_message_encode(const uint8_t* subtopic, size_t subtopic_len,
                                     const uint8_t* data, size_t data_len);

/**
 * Decodes a channel message from a CBOR array.
 * Extracts subtopic string and data bytes.
 *
 * @param item              CBOR array item to decode
 * @param out_subtopic      Output buffer for subtopic string
 * @param subtopic_buf_size Size of output subtopic buffer
 * @param out_data          Output buffer for data bytes
 * @param data_buf_size     Size of output data buffer
 * @param out_data_len      Output: actual data length
 * @return                  0 on success, -1 on error
 */
int channel_message_decode(const cbor_item_t* item,
                            char* out_subtopic, size_t subtopic_buf_size,
                            uint8_t* out_data, size_t data_buf_size,
                            size_t* out_data_len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_MESSAGE_H
