//
// Created by victor on 4/21/26.
//

#include "quasar_route.h"
#include "../../Bloom/bloom_filter.h"
#include "../../Util/allocator.h"
#include "../Meridian/meridian_packet.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ============================================================================
// SERIALIZATION
// ============================================================================

int quasar_route_message_serialize(const quasar_route_message_t* msg, uint8_t** buf, size_t* buf_len) {
    if (msg == NULL || buf == NULL || buf_len == NULL) return -1;

    size_t topic_len = msg->topic ? msg->topic->size : 0;
    size_t data_len = msg->data ? msg->data->size : 0;
    size_t visited_bit_bytes = 0;
    if (msg->visited != NULL) {
        visited_bit_bytes = msg->visited->size / 8;
        if (msg->visited->size % 8 > 0) visited_bit_bytes++;
    }

    // Prevent overflow when computing total size
    if (topic_len > SIZE_MAX - data_len) return -1;
    size_t body_len = topic_len + data_len;
    if (body_len > SIZE_MAX - (sizeof(uint32_t) * msg->pub_count)) return -1;
    body_len += sizeof(uint32_t) * msg->pub_count;
    if (body_len > SIZE_MAX - (sizeof(uint16_t) * msg->pub_count)) return -1;
    body_len += sizeof(uint16_t) * msg->pub_count;
    if (body_len > SIZE_MAX - visited_bit_bytes) return -1;
    body_len += visited_bit_bytes;
    if (body_len > SIZE_MAX - QUASAR_MESSAGE_ID_SIZE) return -1;
    body_len += QUASAR_MESSAGE_ID_SIZE;

    size_t total_size = 8 * sizeof(uint32_t) + body_len;
    if (total_size < body_len) return -1;

    uint8_t* out = get_clear_memory(total_size);

    uint32_t* header = (uint32_t*)out;
    header[0] = htonl(QUASAR_ROUTE_MAGIC);
    header[1] = htonl(QUASAR_ROUTE_VERSION);
    header[2] = htonl((uint32_t)topic_len);
    header[3] = htonl((uint32_t)data_len);
    header[4] = htonl(msg->hops_remaining);
    header[5] = htonl(msg->pub_count);
    header[6] = htonl((uint32_t)(msg->visited ? msg->visited->size : 0));
    header[7] = htonl((uint32_t)(msg->visited ? msg->visited->hash_count : 0));

    size_t offset = 8 * sizeof(uint32_t);
    quasar_message_id_serialize(&msg->id, out + offset);
    offset += QUASAR_MESSAGE_ID_SIZE;

    if (topic_len > 0 && msg->topic != NULL) {
        memcpy(out + offset, msg->topic->data, topic_len);
    }
    offset += topic_len;

    if (data_len > 0 && msg->data != NULL) {
        memcpy(out + offset, msg->data->data, data_len);
    }
    offset += data_len;

    if (msg->pub_count > 0) {
        memcpy(out + offset, msg->pub_addrs, sizeof(uint32_t) * msg->pub_count);
        offset += sizeof(uint32_t) * msg->pub_count;
        memcpy(out + offset, msg->pub_ports, sizeof(uint16_t) * msg->pub_count);
        offset += sizeof(uint16_t) * msg->pub_count;
    }

    if (visited_bit_bytes > 0 && msg->visited != NULL && msg->visited->bits != NULL) {
        memcpy(out + offset, msg->visited->bits->data, visited_bit_bytes);
    }
    offset += visited_bit_bytes;

    *buf = out;
    *buf_len = offset;
    return 0;
}

// ============================================================================
// DESERIALIZATION
// ============================================================================

quasar_route_message_t* quasar_route_message_deserialize(const uint8_t* data, size_t len) {
    if (data == NULL || len < 8 * sizeof(uint32_t)) return NULL;

    const uint32_t* header = (const uint32_t*)data;
    uint32_t magic = ntohl(header[0]);
    uint32_t version = ntohl(header[1]);
    if (magic != QUASAR_ROUTE_MAGIC || version != QUASAR_ROUTE_VERSION) return NULL;

    uint32_t topic_len = ntohl(header[2]);
    uint32_t data_len = ntohl(header[3]);
    uint32_t hops_remaining = ntohl(header[4]);
    uint32_t pub_count = ntohl(header[5]);
    uint32_t visited_size = ntohl(header[6]);
    uint32_t visited_hashes = ntohl(header[7]);

    if (topic_len > MERIDIAN_MAX_PAYLOAD_SIZE || data_len > MERIDIAN_MAX_PAYLOAD_SIZE) return NULL;
    if (pub_count > MERIDIAN_MAX_NODES) return NULL;

    size_t visited_bit_bytes = 0;
    if (visited_size > 0) {
        visited_bit_bytes = visited_size / 8;
        if (visited_size % 8 > 0) visited_bit_bytes++;
    }

    size_t expected = 8 * sizeof(uint32_t);
    expected += QUASAR_MESSAGE_ID_SIZE;
    expected += topic_len;
    expected += data_len;

    size_t pub_addrs_size = (size_t)pub_count * sizeof(uint32_t);
    size_t pub_ports_size = (size_t)pub_count * sizeof(uint16_t);
    if (pub_addrs_size / sizeof(uint32_t) != pub_count) return NULL;
    if (pub_ports_size / sizeof(uint16_t) != pub_count) return NULL;

    expected += pub_addrs_size;
    if (expected < pub_addrs_size) return NULL;
    expected += pub_ports_size;
    if (expected < pub_ports_size) return NULL;
    expected += visited_bit_bytes;
    if (expected < visited_bit_bytes) return NULL;

    if (len < expected) return NULL;

    size_t offset = 8 * sizeof(uint32_t);
    quasar_message_id_t id;
    quasar_message_id_deserialize(&id, data + offset);
    offset += QUASAR_MESSAGE_ID_SIZE;

    const uint8_t* topic_ptr = data + offset;
    offset += topic_len;
    const uint8_t* data_ptr = data + offset;
    offset += data_len;

    quasar_route_message_t* msg = quasar_route_message_create(
        topic_ptr, topic_len, data_ptr, data_len,
        hops_remaining, visited_size, visited_hashes
    );
    if (msg == NULL) return NULL;

    msg->id = id;

    if (pub_count > 0) {
        msg->pub_addrs = get_clear_memory(pub_addrs_size);
        msg->pub_ports = get_clear_memory(pub_ports_size);
        memcpy(msg->pub_addrs, data + offset, pub_addrs_size);
        offset += pub_addrs_size;
        memcpy(msg->pub_ports, data + offset, pub_ports_size);
        offset += pub_ports_size;
        msg->pub_count = pub_count;
        msg->pub_capacity = pub_count;
    }

    if (visited_bit_bytes > 0 && msg->visited != NULL && msg->visited->bits != NULL) {
        memcpy(msg->visited->bits->data, data + offset, visited_bit_bytes);
    }
    offset += visited_bit_bytes;

    (void)offset; // Suppress unused warning in release builds where asserts are off
    return msg;
}
