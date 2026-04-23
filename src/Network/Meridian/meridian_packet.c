//
// Created by victor on 4/19/26.
//

#include "meridian_packet.h"
#include <stdlib.h>
#include <string.h>
#include <cbor.h>

// ============================================================================
// GOSSIP PACKET
// ============================================================================

/**
 * Creates an empty gossip packet with initialized targets vector.
 *
 * @return  New gossip packet, or NULL on allocation failure
 */
meridian_gossip_packet_t* meridian_gossip_packet_create(void) {
    meridian_gossip_packet_t* pkt = (meridian_gossip_packet_t*)
        get_clear_memory(sizeof(meridian_gossip_packet_t));
    vec_init(&pkt->targets);
    pkt->base.type = MERIDIAN_PACKET_TYPE_GOSSIP;
    pkt->base.magic = MERIDIAN_MAGIC_NUMBER;
    return pkt;
}

/**
 * Destroys a gossip packet and its targets.
 *
 * @param pkt  Packet to destroy
 */
void meridian_gossip_packet_destroy(meridian_gossip_packet_t* pkt) {
    if (pkt == NULL) return;
    vec_deinit(&pkt->targets);
    free(pkt);
}

/**
 * Adds a target node to the gossip packet.
 *
 * @param pkt   Packet to add to
 * @param node  Target node to add
 * @return      0 on success, -1 on failure
 */
int meridian_gossip_packet_add_target(meridian_gossip_packet_t* pkt,
                                       meridian_node_t* node) {
    if (pkt == NULL || node == NULL) return -1;
    return vec_push(&pkt->targets, node);
}

/**
 * Encodes a gossip packet into CBOR format for wire transmission.
 *
 * CBOR structure:
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_targets, ...targets]
 * Where each target is: [addr, port, rendv_addr, rendv_port]
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item (caller must cbor_decref), or NULL on failure
 */
cbor_item_t* meridian_gossip_encode(const meridian_gossip_packet_t* pkt) {
    if (pkt == NULL) return NULL;

    // Allocate array: 7 fixed fields + variable targets
    cbor_item_t* array = cbor_new_definite_array(7 + pkt->targets.length);
    if (array == NULL) return NULL;

    // Encode fixed header fields
    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->base.type)))) {
        cbor_decref(&array);
        return NULL;
    }

    // Split 64-bit query_id into two 32-bit values for CBOR encoding
    uint64_t qid_1 = pkt->base.query_id >> 32;
    uint64_t qid_2 = pkt->base.query_id & 0xFFFFFFFF;
    if (!cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->base.magic))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->base.rendv_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->base.rendv_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    // Encode number of targets
    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->targets.length)))) {
        cbor_decref(&array);
        return NULL;
    }

    // Encode each target as [addr, port, rendv_addr, rendv_port]
    for (int i = 0; i < pkt->targets.length; i++) {
        cbor_item_t* target_array = cbor_new_definite_array(4);
        if (target_array == NULL ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint32(pkt->targets.data[i]->addr))) ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint16(pkt->targets.data[i]->port))) ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint32(pkt->targets.data[i]->rendv_addr))) ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint16(pkt->targets.data[i]->rendv_port))) ||
            !cbor_array_push(array, target_array)) {
            cbor_decref(&target_array);
            cbor_decref(&array);
            return NULL;
        }
        cbor_decref(&target_array);
    }

    return array;
}

/**
 * Decodes a CBOR array into a gossip packet.
 * Validates magic number and target array structure.
 *
 * @param item  CBOR array to decode
 * @return      New gossip packet, or NULL on failure (invalid format, bad magic)
 */
meridian_gossip_packet_t* meridian_gossip_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 7) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_gossip_packet_t* pkt = meridian_gossip_packet_create();
    if (pkt == NULL) return NULL;

    // Decode fixed header
    pkt->base.type = cbor_get_uint8(items[idx++]);

    // Reconstruct 64-bit query_id from two 32-bit values
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->base.query_id = (qid_1 << 32) | qid_2;

    pkt->base.magic = cbor_get_uint32(items[idx++]);
    pkt->base.rendv_addr = cbor_get_uint32(items[idx++]);
    pkt->base.rendv_port = cbor_get_uint16(items[idx++]);

    // Validate magic number
    if (pkt->base.magic != MERIDIAN_MAGIC_NUMBER) {
        meridian_gossip_packet_destroy(pkt);
        return NULL;
    }

    // Decode target count and targets
    uint64_t num_targets = cbor_get_uint64(items[idx++]);
    if (num_targets > MERIDIAN_MAX_NODES || num_targets > arr_size - idx) {
        meridian_gossip_packet_destroy(pkt);
        return NULL;
    }

    for (size_t i = 0; i < num_targets; i++) {
        if (idx >= arr_size) {
            meridian_gossip_packet_destroy(pkt);
            return NULL;
        }
        cbor_item_t* target_arr = items[idx++];
        if (!cbor_array_is_definite(target_arr) || cbor_array_size(target_arr) != 4) {
            meridian_gossip_packet_destroy(pkt);
            return NULL;
        }
        cbor_item_t** target_items = cbor_array_handle(target_arr);
        // Create rendezvous node from encoded values
        meridian_node_t* node = meridian_node_create_rendv(
            cbor_get_uint32(target_items[0]),
            cbor_get_uint16(target_items[1]),
            cbor_get_uint32(target_items[2]),
            cbor_get_uint16(target_items[3]),
            NULL
        );
        if (node == NULL || vec_push(&pkt->targets, node) != 0) {
            if (node) meridian_node_destroy(node);
            meridian_gossip_packet_destroy(pkt);
            return NULL;
        }
    }

    return pkt;
}

// ============================================================================
// PING PACKET
// ============================================================================

/**
 * Creates an empty ping packet with initialized nodes and latencies vectors.
 *
 * @return  New ping packet, or NULL on allocation failure
 */
meridian_ping_packet_t* meridian_ping_packet_create(void) {
    meridian_ping_packet_t* pkt = (meridian_ping_packet_t*)
        get_clear_memory(sizeof(meridian_ping_packet_t));
    vec_init(&pkt->nodes);
    vec_init(&pkt->latencies);
    pkt->base.type = MERIDIAN_PACKET_TYPE_PING;
    pkt->base.magic = MERIDIAN_MAGIC_NUMBER;
    return pkt;
}

/**
 * Destroys a ping packet and its vectors.
 *
 * @param pkt  Packet to destroy
 */
void meridian_ping_packet_destroy(meridian_ping_packet_t* pkt) {
    if (pkt == NULL) return;
    vec_deinit(&pkt->nodes);
    vec_deinit(&pkt->latencies);
    free(pkt);
}

/**
 * Adds a node and its latency to the ping packet.
 *
 * @param pkt      Packet to add to
 * @param node     Node to add
 * @param latency  Measured latency in microseconds
 * @return         0 on success, -1 on failure
 */
int meridian_ping_packet_add_node(meridian_ping_packet_t* pkt,
                                   meridian_node_t* node, uint32_t latency) {
    if (pkt == NULL || node == NULL) return -1;
    if (vec_push(&pkt->nodes, node) != 0) return -1;
    if (vec_push(&pkt->latencies, latency) != 0) return -1;
    return 0;
}

/**
 * Encodes a ping packet into CBOR format.
 *
 * CBOR structure:
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, num_nodes, ...nodes]
 * Where each node is: [addr, port, latency]
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_ping_encode(const meridian_ping_packet_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(6 + pkt->nodes.length);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->base.type)))) {
        cbor_decref(&array);
        return NULL;
    }

    uint64_t qid_1 = pkt->base.query_id >> 32;
    uint64_t qid_2 = pkt->base.query_id & 0xFFFFFFFF;
    if (!cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->base.magic))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->base.rendv_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->base.rendv_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->nodes.length)))) {
        cbor_decref(&array);
        return NULL;
    }

    for (int i = 0; i < pkt->nodes.length; i++) {
        cbor_item_t* node_array = cbor_new_definite_array(3);
        if (node_array == NULL ||
            !cbor_array_push(node_array, cbor_move(cbor_build_uint32(pkt->nodes.data[i]->addr))) ||
            !cbor_array_push(node_array, cbor_move(cbor_build_uint16(pkt->nodes.data[i]->port))) ||
            !cbor_array_push(node_array, cbor_move(cbor_build_uint32(pkt->latencies.data[i]))) ||
            !cbor_array_push(array, node_array)) {
            cbor_decref(&node_array);
            cbor_decref(&array);
            return NULL;
        }
        cbor_decref(&node_array);
    }

    return array;
}

/**
 * Decodes a CBOR ping packet.
 *
 * @param item  CBOR array to decode
 * @return      New ping packet, or NULL on failure
 */
meridian_ping_packet_t* meridian_ping_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 6) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_ping_packet_t* pkt = meridian_ping_packet_create();
    if (pkt == NULL) return NULL;

    pkt->base.type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->base.query_id = (qid_1 << 32) | qid_2;
    pkt->base.magic = cbor_get_uint32(items[idx++]);
    pkt->base.rendv_addr = cbor_get_uint32(items[idx++]);
    pkt->base.rendv_port = cbor_get_uint16(items[idx++]);

    if (pkt->base.magic != MERIDIAN_MAGIC_NUMBER) {
        meridian_ping_packet_destroy(pkt);
        return NULL;
    }

    uint64_t num_nodes = cbor_get_uint64(items[idx++]);
    if (num_nodes > MERIDIAN_MAX_NODES || num_nodes > arr_size - idx) {
        meridian_ping_packet_destroy(pkt);
        return NULL;
    }

    for (size_t i = 0; i < num_nodes; i++) {
        if (idx >= arr_size) {
            meridian_ping_packet_destroy(pkt);
            return NULL;
        }
        cbor_item_t* node_arr = items[idx++];
        if (!cbor_array_is_definite(node_arr) || cbor_array_size(node_arr) != 3) {
            meridian_ping_packet_destroy(pkt);
            return NULL;
        }
        cbor_item_t* node_items = cbor_array_handle(node_arr);
        uint32_t addr = cbor_get_uint32(&node_items[0]);
        uint16_t port = cbor_get_uint16(&node_items[1]);
        uint32_t latency = cbor_get_uint32(&node_items[2]);
        meridian_node_t* node = meridian_node_create(addr, port, NULL);
        if (node == NULL || vec_push(&pkt->nodes, node) != 0 ||
            vec_push(&pkt->latencies, latency) != 0) {
            if (node) meridian_node_destroy(node);
            meridian_ping_packet_destroy(pkt);
            return NULL;
        }
    }

    return pkt;
}

// ============================================================================
// BASE PACKET
// ============================================================================

/**
 * Encodes a base packet header (type, query_id, magic, rendezvous).
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_packet_encode(const meridian_packet_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(5);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->type)))) {
        cbor_decref(&array);
        return NULL;
    }

    uint64_t qid_1 = pkt->query_id >> 32;
    uint64_t qid_2 = pkt->query_id & 0xFFFFFFFF;
    if (!cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->magic))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->rendv_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->rendv_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

/**
 * Decodes into an existing packet struct.
 *
 * @param item  CBOR array to decode
 * @param pkt   Output structure to fill
 * @return      0 on success, -1 on failure
 */
int meridian_packet_decode(cbor_item_t* item, meridian_packet_t* pkt) {
    if (item == NULL || pkt == NULL || !cbor_array_is_definite(item)) return -1;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 5) return -1;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    pkt->type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->query_id = (qid_1 << 32) | qid_2;
    pkt->magic = cbor_get_uint32(items[idx++]);
    pkt->rendv_addr = cbor_get_uint32(items[idx++]);
    pkt->rendv_port = cbor_get_uint16(items[idx++]);

    if (pkt->magic != MERIDIAN_MAGIC_NUMBER) return -1;
    return 0;
}

// ============================================================================
// RESPONSE PACKET
// ============================================================================

/**
 * Creates an empty response packet with initialized targets vector.
 *
 * @return  New response packet, or NULL on failure
 */
meridian_ret_response_t* meridian_ret_response_create(void) {
    meridian_ret_response_t* pkt = (meridian_ret_response_t*)
        get_clear_memory(sizeof(meridian_ret_response_t));
    vec_init(&pkt->targets);
    pkt->type = MERIDIAN_PACKET_TYPE_RET_RESPONSE;
    pkt->magic = MERIDIAN_MAGIC_NUMBER;
    return pkt;
}

/**
 * Destroys a response packet and its targets.
 *
 * @param pkt  Packet to destroy
 */
void meridian_ret_response_destroy(meridian_ret_response_t* pkt) {
    if (pkt == NULL) return;
    vec_deinit(&pkt->targets);
    free(pkt);
}

/**
 * Adds a target with latency to the response packet.
 *
 * @param pkt      Packet to add to
 * @param addr     Node address
 * @param port     Node port
 * @param latency  Latency in microseconds
 * @return         0 on success, -1 on failure
 */
int meridian_ret_response_add_target(meridian_ret_response_t* pkt,
                                      uint32_t addr, uint16_t port, uint32_t latency) {
    if (pkt == NULL) return -1;
    meridian_node_latency_t nl = {.addr = addr, .port = port, .latency_us = latency};
    return vec_push(&pkt->targets, nl);
}

/**
 * Encodes a response packet into CBOR.
 *
 * CBOR structure:
 * [type, query_id_hi, query_id_lo, magic, rendv_addr, rendv_port, closest_addr, closest_port, num_targets, ...targets]
 *
 * @param pkt  Packet to encode
 * @return     CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_ret_response_encode(const meridian_ret_response_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(9 + pkt->targets.length);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->type)))) {
        cbor_decref(&array);
        return NULL;
    }

    uint64_t qid_1 = pkt->query_id >> 32;
    uint64_t qid_2 = pkt->query_id & 0xFFFFFFFF;
    if (!cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->magic))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->rendv_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->rendv_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    if (!cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->closest_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->closest_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->targets.length)))) {
        cbor_decref(&array);
        return NULL;
    }

    for (int i = 0; i < pkt->targets.length; i++) {
        cbor_item_t* target_array = cbor_new_definite_array(3);
        if (target_array == NULL ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint32(pkt->targets.data[i].addr))) ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint16(pkt->targets.data[i].port))) ||
            !cbor_array_push(target_array, cbor_move(cbor_build_uint32(pkt->targets.data[i].latency_us))) ||
            !cbor_array_push(array, target_array)) {
            cbor_decref(&target_array);
            cbor_decref(&array);
            return NULL;
        }
        cbor_decref(&target_array);
    }

    return array;
}

/**
 * Decodes a CBOR response packet.
 *
 * @param item  CBOR array to decode
 * @return      New response packet, or NULL on failure
 */
meridian_ret_response_t* meridian_ret_response_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 9) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_ret_response_t* pkt = meridian_ret_response_create();
    if (pkt == NULL) return NULL;

    pkt->type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->query_id = (qid_1 << 32) | qid_2;
    pkt->magic = cbor_get_uint32(items[idx++]);
    pkt->rendv_addr = cbor_get_uint32(items[idx++]);
    pkt->rendv_port = cbor_get_uint16(items[idx++]);
    pkt->closest_addr = cbor_get_uint32(items[idx++]);
    pkt->closest_port = cbor_get_uint16(items[idx++]);

    if (pkt->magic != MERIDIAN_MAGIC_NUMBER) {
        meridian_ret_response_destroy(pkt);
        return NULL;
    }

    uint64_t num_targets = cbor_get_uint64(items[idx++]);
    if (num_targets > MERIDIAN_MAX_NODES || num_targets > arr_size - idx) {
        meridian_ret_response_destroy(pkt);
        return NULL;
    }

    for (size_t i = 0; i < num_targets; i++) {
        if (idx >= arr_size) {
            meridian_ret_response_destroy(pkt);
            return NULL;
        }
        cbor_item_t* target_arr = items[idx++];
        if (!cbor_array_is_definite(target_arr) || cbor_array_size(target_arr) != 3) {
            meridian_ret_response_destroy(pkt);
            return NULL;
        }
        cbor_item_t* target_items = cbor_array_handle(target_arr);
        meridian_node_latency_t nl = {
            .addr = cbor_get_uint32(&target_items[0]),
            .port = cbor_get_uint16(&target_items[1]),
            .latency_us = cbor_get_uint32(&target_items[2])
        };
        if (vec_push(&pkt->targets, nl) != 0) {
            meridian_ret_response_destroy(pkt);
            return NULL;
        }
    }

    return pkt;
}

/**
 * Creates a node from a latency entry.
 *
 * @param nl  Latency entry with addr/port
 * @return    New node, or NULL on failure
 */
meridian_node_t* meridian_node_from_latency(const meridian_node_latency_t* nl) {
    if (nl == NULL) return NULL;
    return meridian_node_create(nl->addr, nl->port, NULL);
}

// ============================================================================
// ADDR RESPONSE PACKET
// ============================================================================

meridian_addr_response_t* meridian_addr_response_create(uint64_t query_id,
                                                         uint32_t reflexive_addr,
                                                         uint16_t reflexive_port,
                                                         uint32_t endpoint_id) {
    meridian_addr_response_t* pkt = (meridian_addr_response_t*)
        get_clear_memory(sizeof(meridian_addr_response_t));
    if (pkt == NULL) return NULL;
    pkt->type = MERIDIAN_PACKET_TYPE_ADDR_RESPONSE;
    pkt->query_id = query_id;
    pkt->reflexive_addr = reflexive_addr;
    pkt->reflexive_port = reflexive_port;
    pkt->endpoint_id = endpoint_id;
    return pkt;
}

void meridian_addr_response_destroy(meridian_addr_response_t* pkt) {
    if (pkt == NULL) return;
    free(pkt);
}

cbor_item_t* meridian_addr_response_encode(const meridian_addr_response_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(6);
    if (array == NULL) return NULL;

    uint64_t qid_1 = pkt->query_id >> 32;
    uint64_t qid_2 = pkt->query_id & 0xFFFFFFFF;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->reflexive_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->reflexive_port))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->endpoint_id)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

meridian_addr_response_t* meridian_addr_response_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;
    if (cbor_array_size(item) < 6) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_addr_response_t* pkt = (meridian_addr_response_t*)
        get_clear_memory(sizeof(meridian_addr_response_t));
    if (pkt == NULL) return NULL;

    pkt->type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->query_id = (qid_1 << 32) | qid_2;
    pkt->reflexive_addr = cbor_get_uint32(items[idx++]);
    pkt->reflexive_port = cbor_get_uint16(items[idx++]);
    pkt->endpoint_id = cbor_get_uint32(items[idx++]);

    if (pkt->type != MERIDIAN_PACKET_TYPE_ADDR_RESPONSE) {
        free(pkt);
        return NULL;
    }

    return pkt;
}

// ============================================================================
// PUNCH REQUEST PACKET
// ============================================================================

meridian_punch_request_t* meridian_punch_request_create(uint64_t query_id,
                                                         uint32_t from_endpoint_id,
                                                         uint32_t target_addr,
                                                         uint16_t target_port) {
    meridian_punch_request_t* pkt = (meridian_punch_request_t*)
        get_clear_memory(sizeof(meridian_punch_request_t));
    if (pkt == NULL) return NULL;
    pkt->type = MERIDIAN_PACKET_TYPE_PUNCH_REQUEST;
    pkt->query_id = query_id;
    pkt->from_endpoint_id = from_endpoint_id;
    pkt->target_addr = target_addr;
    pkt->target_port = target_port;
    return pkt;
}

void meridian_punch_request_destroy(meridian_punch_request_t* pkt) {
    if (pkt == NULL) return;
    free(pkt);
}

cbor_item_t* meridian_punch_request_encode(const meridian_punch_request_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(6);
    if (array == NULL) return NULL;

    uint64_t qid_1 = pkt->query_id >> 32;
    uint64_t qid_2 = pkt->query_id & 0xFFFFFFFF;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->from_endpoint_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->target_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->target_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

meridian_punch_request_t* meridian_punch_request_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;
    if (cbor_array_size(item) < 6) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_punch_request_t* pkt = (meridian_punch_request_t*)
        get_clear_memory(sizeof(meridian_punch_request_t));
    if (pkt == NULL) return NULL;

    pkt->type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->query_id = (qid_1 << 32) | qid_2;
    pkt->from_endpoint_id = cbor_get_uint32(items[idx++]);
    pkt->target_addr = cbor_get_uint32(items[idx++]);
    pkt->target_port = cbor_get_uint16(items[idx++]);

    if (pkt->type != MERIDIAN_PACKET_TYPE_PUNCH_REQUEST) {
        free(pkt);
        return NULL;
    }

    return pkt;
}

// ============================================================================
// PUNCH SYNC PACKET
// ============================================================================

meridian_punch_sync_t* meridian_punch_sync_create(uint64_t query_id,
                                                    uint32_t from_endpoint_id,
                                                    uint32_t from_addr,
                                                    uint16_t from_port) {
    meridian_punch_sync_t* pkt = (meridian_punch_sync_t*)
        get_clear_memory(sizeof(meridian_punch_sync_t));
    if (pkt == NULL) return NULL;
    pkt->type = MERIDIAN_PACKET_TYPE_PUNCH_SYNC;
    pkt->query_id = query_id;
    pkt->from_endpoint_id = from_endpoint_id;
    pkt->from_addr = from_addr;
    pkt->from_port = from_port;
    return pkt;
}

void meridian_punch_sync_destroy(meridian_punch_sync_t* pkt) {
    if (pkt == NULL) return;
    free(pkt);
}

cbor_item_t* meridian_punch_sync_encode(const meridian_punch_sync_t* pkt) {
    if (pkt == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(6);
    if (array == NULL) return NULL;

    uint64_t qid_1 = pkt->query_id >> 32;
    uint64_t qid_2 = pkt->query_id & 0xFFFFFFFF;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(pkt->type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_1))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(qid_2))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->from_endpoint_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(pkt->from_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(pkt->from_port)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

meridian_punch_sync_t* meridian_punch_sync_decode(cbor_item_t* item) {
    if (item == NULL || !cbor_array_is_definite(item)) return NULL;
    if (cbor_array_size(item) < 6) return NULL;

    cbor_item_t** items = cbor_array_handle(item);
    size_t idx = 0;

    meridian_punch_sync_t* pkt = (meridian_punch_sync_t*)
        get_clear_memory(sizeof(meridian_punch_sync_t));
    if (pkt == NULL) return NULL;

    pkt->type = cbor_get_uint8(items[idx++]);
    uint64_t qid_1 = cbor_get_uint64(items[idx++]);
    uint64_t qid_2 = cbor_get_uint64(items[idx++]);
    pkt->query_id = (qid_1 << 32) | qid_2;
    pkt->from_endpoint_id = cbor_get_uint32(items[idx++]);
    pkt->from_addr = cbor_get_uint32(items[idx++]);
    pkt->from_port = cbor_get_uint16(items[idx++]);

    if (pkt->type != MERIDIAN_PACKET_TYPE_PUNCH_SYNC) {
        free(pkt);
        return NULL;
    }

    return pkt;
}

// ============================================================================
// CHANNEL BOOTSTRAP PACKET OPERATIONS
// ============================================================================

/**
 * Encodes a channel bootstrap request into CBOR.
 *
 * CBOR structure:
 * [type(40), string(topic_id), string(sender_node_id), uint64(timestamp_us)]
 *
 * @param topic_id         Topic ID string
 * @param sender_node_id   Sender node ID string
 * @param timestamp_us     Timestamp in microseconds
 * @return                 CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_channel_bootstrap_encode(const char* topic_id,
                                                const char* sender_node_id,
                                                uint64_t timestamp_us) {
    if (topic_id == NULL || sender_node_id == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(4);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(sender_node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(timestamp_us)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

/**
 * Decodes a CBOR channel bootstrap request.
 *
 * @param item               CBOR array to decode
 * @param topic_id           Output buffer for topic ID
 * @param topic_buf_size     Size of topic_id buffer
 * @param sender_node_id     Output buffer for sender node ID
 * @param node_id_buf_size   Size of sender_node_id buffer
 * @param timestamp_us       Output timestamp
 * @return                   0 on success, -1 on failure
 */
int meridian_channel_bootstrap_decode(const cbor_item_t* item,
                                       char* topic_id, size_t topic_buf_size,
                                       char* sender_node_id, size_t node_id_buf_size,
                                       uint64_t* timestamp_us) {
    if (item == NULL || topic_id == NULL || sender_node_id == NULL || timestamp_us == NULL)
        return -1;
    if (!cbor_array_is_definite(item)) return -1;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 4) return -1;

    cbor_item_t** items = cbor_array_handle(item);

    if (!cbor_isa_uint(items[0]) || cbor_get_uint8(items[0]) != MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP)
        return -1;

    if (!cbor_isa_string(items[1])) return -1;
    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= topic_buf_size) return -1;
    memcpy(topic_id, cbor_string_handle(items[1]), topic_len);
    topic_id[topic_len] = '\0';

    if (!cbor_isa_string(items[2])) return -1;
    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= node_id_buf_size) return -1;
    memcpy(sender_node_id, cbor_string_handle(items[2]), node_id_len);
    sender_node_id[node_id_len] = '\0';

    if (!cbor_isa_uint(items[3])) return -1;
    *timestamp_us = cbor_get_uint64(items[3]);

    return 0;
}

/**
 * Encodes a channel bootstrap reply into CBOR.
 *
 * CBOR structure:
 * [type(41), string(topic_id), string(responder_node_id),
 *  uint32(responder_addr), uint16(responder_port), uint64(timestamp_us),
 *  array([uint32, uint16], ...)]
 *
 * @param topic_id            Topic ID string
 * @param responder_node_id   Responder node ID string
 * @param responder_addr      Responder IPv4 address
 * @param responder_port      Responder port
 * @param timestamp_us        Timestamp in microseconds
 * @param seed_addrs          Seed node addresses
 * @param seed_ports          Seed node ports
 * @param num_seeds           Number of seed nodes
 * @return                    CBOR array item, or NULL on failure
 */
cbor_item_t* meridian_channel_bootstrap_reply_encode(const char* topic_id,
                                                      const char* responder_node_id,
                                                      uint32_t responder_addr,
                                                      uint16_t responder_port,
                                                      uint64_t timestamp_us,
                                                      const uint32_t* seed_addrs,
                                                      const uint16_t* seed_ports,
                                                      size_t num_seeds) {
    if (topic_id == NULL || responder_node_id == NULL) return NULL;
    if (num_seeds > 0 && (seed_addrs == NULL || seed_ports == NULL)) return NULL;

    cbor_item_t* array = cbor_new_definite_array(7);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(responder_node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint32(responder_addr))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint16(responder_port))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(timestamp_us)))) {
        cbor_decref(&array);
        return NULL;
    }

    cbor_item_t* seeds = cbor_new_definite_array(num_seeds);
    if (seeds == NULL) {
        cbor_decref(&array);
        return NULL;
    }

    for (size_t i = 0; i < num_seeds; i++) {
        cbor_item_t* seed = cbor_new_definite_array(2);
        if (seed == NULL ||
            !cbor_array_push(seed, cbor_move(cbor_build_uint32(seed_addrs[i]))) ||
            !cbor_array_push(seed, cbor_move(cbor_build_uint16(seed_ports[i]))) ||
            !cbor_array_push(seeds, seed)) {
            cbor_decref(&seed);
            cbor_decref(&seeds);
            cbor_decref(&array);
            return NULL;
        }
        cbor_decref(&seed);
    }

    if (!cbor_array_push(array, seeds)) {
        cbor_decref(&seeds);
        cbor_decref(&array);
        return NULL;
    }
    cbor_decref(&seeds);

    return array;
}

/**
 * Decodes a CBOR channel bootstrap reply.
 *
 * @param item                CBOR array to decode
 * @param topic_id            Output buffer for topic ID
 * @param topic_buf_size      Size of topic_id buffer
 * @param responder_node_id   Output buffer for responder node ID
 * @param node_id_buf_size    Size of responder_node_id buffer
 * @param responder_addr      Output responder address
 * @param responder_port      Output responder port
 * @param timestamp_us        Output timestamp
 * @param seed_addrs          Output seed addresses array
 * @param seed_ports          Output seed ports array
 * @param num_seeds           Output number of seeds read
 * @param max_seeds           Maximum seeds that fit in output arrays
 * @return                    0 on success, -1 on failure
 */
int meridian_channel_bootstrap_reply_decode(const cbor_item_t* item,
                                             char* topic_id, size_t topic_buf_size,
                                             char* responder_node_id, size_t node_id_buf_size,
                                             uint32_t* responder_addr,
                                             uint16_t* responder_port,
                                             uint64_t* timestamp_us,
                                             uint32_t* seed_addrs,
                                             uint16_t* seed_ports,
                                             size_t* num_seeds,
                                             size_t max_seeds) {
    if (item == NULL || topic_id == NULL || responder_node_id == NULL) return -1;
    if (responder_addr == NULL || responder_port == NULL || timestamp_us == NULL) return -1;
    if (seed_addrs == NULL || seed_ports == NULL || num_seeds == NULL) return -1;
    if (!cbor_array_is_definite(item)) return -1;

    size_t arr_size = cbor_array_size(item);
    if (arr_size < 7) return -1;

    cbor_item_t** items = cbor_array_handle(item);

    if (!cbor_isa_uint(items[0]) || cbor_get_uint8(items[0]) != MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY)
        return -1;

    if (!cbor_isa_string(items[1])) return -1;
    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= topic_buf_size) return -1;
    memcpy(topic_id, cbor_string_handle(items[1]), topic_len);
    topic_id[topic_len] = '\0';

    if (!cbor_isa_string(items[2])) return -1;
    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= node_id_buf_size) return -1;
    memcpy(responder_node_id, cbor_string_handle(items[2]), node_id_len);
    responder_node_id[node_id_len] = '\0';

    if (!cbor_isa_uint(items[3])) return -1;
    *responder_addr = (uint32_t)cbor_get_uint32(items[3]);

    if (!cbor_isa_uint(items[4])) return -1;
    *responder_port = (uint16_t)cbor_get_uint16(items[4]);

    if (!cbor_isa_uint(items[5])) return -1;
    *timestamp_us = cbor_get_uint64(items[5]);

    *num_seeds = 0;
    if (cbor_isa_array(items[6])) {
        size_t seed_count = cbor_array_size(items[6]);
        size_t to_read = seed_count < max_seeds ? seed_count : max_seeds;
        cbor_item_t** seed_items = cbor_array_handle(items[6]);
        for (size_t i = 0; i < to_read; i++) {
            if (!cbor_array_is_definite(seed_items[i])) return -1;
            if (cbor_array_size(seed_items[i]) < 2) return -1;
            cbor_item_t** pair = cbor_array_handle(seed_items[i]);
            if (!cbor_isa_uint(pair[0]) || !cbor_isa_uint(pair[1])) return -1;
            seed_addrs[i] = (uint32_t)cbor_get_uint32(pair[0]);
            seed_ports[i] = (uint16_t)cbor_get_uint16(pair[1]);
            (*num_seeds)++;
        }
    }

    return 0;
}

// ============================================================================
// CHANNEL DELETE NOTICE
// ============================================================================

cbor_item_t* meridian_channel_delete_notice_encode(
    const meridian_channel_delete_notice_t* notice) {
    if (notice == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(7);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->key_type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(notice->public_key, notice->public_key_len))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(notice->timestamp_us))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(notice->signature, notice->signature_len)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

int meridian_channel_delete_notice_decode(const cbor_item_t* item,
                                            meridian_channel_delete_notice_t* notice) {
    if (item == NULL || notice == NULL) return -1;
    if (!cbor_isa_array(item)) return -1;
    if (cbor_array_size(item) != 7) return -1;

    cbor_item_t** items = cbor_array_handle(item);

    if (!cbor_isa_uint(items[0]) || !cbor_isa_string(items[1]) ||
        !cbor_isa_string(items[2]) || !cbor_isa_string(items[3]) ||
        !cbor_isa_bytestring(items[4]) || !cbor_isa_uint(items[5]) ||
        !cbor_isa_bytestring(items[6]))
        return -1;

    memset(notice, 0, sizeof(*notice));
    notice->type = (uint8_t)cbor_get_uint8(items[0]);

    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= sizeof(notice->topic_id)) return -1;
    memcpy(notice->topic_id, cbor_string_handle(items[1]), topic_len);

    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= sizeof(notice->node_id)) return -1;
    memcpy(notice->node_id, cbor_string_handle(items[2]), node_id_len);

    size_t key_type_len = cbor_string_length(items[3]);
    if (key_type_len >= sizeof(notice->key_type)) return -1;
    memcpy(notice->key_type, cbor_string_handle(items[3]), key_type_len);

    notice->public_key_len = cbor_bytestring_length(items[4]);
    if (notice->public_key_len > POSEIDON_CHANNEL_NOTICE_MAX_PUBLIC_KEY) return -1;
    memcpy(notice->public_key, cbor_bytestring_handle(items[4]), notice->public_key_len);

    notice->timestamp_us = cbor_get_uint64(items[5]);

    notice->signature_len = cbor_bytestring_length(items[6]);
    if (notice->signature_len > POSEIDON_CHANNEL_NOTICE_MAX_SIGNATURE) return -1;
    memcpy(notice->signature, cbor_bytestring_handle(items[6]), notice->signature_len);

    return 0;
}

// ============================================================================
// CHANNEL MODIFY NOTICE
// ============================================================================

cbor_item_t* meridian_channel_modify_notice_encode(
    const meridian_channel_modify_notice_t* notice) {
    if (notice == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(8);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(notice->key_type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(notice->public_key, notice->public_key_len))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(notice->timestamp_us)))) {
        cbor_decref(&array);
        return NULL;
    }

    cbor_item_t* config_map = cbor_new_definite_map(8 + MERIDIAN_MAX_RINGS);
    if (config_map == NULL) { cbor_decref(&array); return NULL; }

    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        char key[4];
        snprintf(key, sizeof(key), "r%d", i);
        if (!cbor_map_add(config_map, (struct cbor_pair) {
            .key = cbor_move(cbor_build_string(key)),
            .value = cbor_move(cbor_build_uint32(notice->config.ring_sizes[i]))
        })) { cbor_decref(&config_map); cbor_decref(&array); return NULL; }
    }
    if (!cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gis")),
        .value = cbor_move(cbor_build_uint32(notice->config.gossip_init_interval_s))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gss")),
        .value = cbor_move(cbor_build_uint32(notice->config.gossip_steady_interval_s))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gni")),
        .value = cbor_move(cbor_build_uint32(notice->config.gossip_num_init_intervals))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qmh")),
        .value = cbor_move(cbor_build_uint32(notice->config.quasar_max_hops))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qa")),
        .value = cbor_move(cbor_build_uint32(notice->config.quasar_alpha))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qss")),
        .value = cbor_move(cbor_build_uint32(notice->config.quasar_seen_size))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qsh")),
        .value = cbor_move(cbor_build_uint32(notice->config.quasar_seen_hashes))
    })) { cbor_decref(&config_map); cbor_decref(&array); return NULL; }

    if (!cbor_array_push(array, cbor_move(config_map))) {
        cbor_decref(&array);
        return NULL;
    }

    if (!cbor_array_push(array, cbor_move(cbor_build_bytestring(notice->signature, notice->signature_len)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

int meridian_channel_modify_notice_decode(const cbor_item_t* item,
                                           meridian_channel_modify_notice_t* notice) {
    if (item == NULL || notice == NULL) return -1;
    if (!cbor_isa_array(item)) return -1;
    if (cbor_array_size(item) != 8) return -1;

    cbor_item_t** items = cbor_array_handle(item);

    if (!cbor_isa_uint(items[0]) || !cbor_isa_string(items[1]) ||
        !cbor_isa_string(items[2]) || !cbor_isa_string(items[3]) ||
        !cbor_isa_bytestring(items[4]) || !cbor_isa_uint(items[5]) ||
        !cbor_isa_map(items[6]) || !cbor_isa_bytestring(items[7]))
        return -1;

    memset(notice, 0, sizeof(*notice));
    notice->type = (uint8_t)cbor_get_uint8(items[0]);

    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= sizeof(notice->topic_id)) return -1;
    memcpy(notice->topic_id, cbor_string_handle(items[1]), topic_len);

    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= sizeof(notice->node_id)) return -1;
    memcpy(notice->node_id, cbor_string_handle(items[2]), node_id_len);

    size_t key_type_len = cbor_string_length(items[3]);
    if (key_type_len >= sizeof(notice->key_type)) return -1;
    memcpy(notice->key_type, cbor_string_handle(items[3]), key_type_len);

    notice->public_key_len = cbor_bytestring_length(items[4]);
    if (notice->public_key_len > POSEIDON_CHANNEL_NOTICE_MAX_PUBLIC_KEY) return -1;
    memcpy(notice->public_key, cbor_bytestring_handle(items[4]), notice->public_key_len);

    notice->timestamp_us = cbor_get_uint64(items[5]);

    notice->config = poseidon_channel_config_defaults();
    size_t map_size = cbor_map_size(items[6]);
    struct cbor_pair* pairs = cbor_map_handle(items[6]);
    for (size_t i = 0; i < map_size; i++) {
        if (!cbor_isa_string(pairs[i].key) || !cbor_isa_uint(pairs[i].value)) continue;
        size_t klen = cbor_string_length(pairs[i].key);
        const char* key = (const char*)cbor_string_handle(pairs[i].key);
        uint32_t val = (uint32_t)cbor_get_uint32(pairs[i].value);
        if (klen == 2 && key[0] == 'r' && key[1] >= '0' && key[1] <= '9') {
            int idx = key[1] - '0';
            if (idx < MERIDIAN_MAX_RINGS) notice->config.ring_sizes[idx] = val;
        } else if (klen == 3 && memcmp(key, "gis", 3) == 0) {
            notice->config.gossip_init_interval_s = val;
        } else if (klen == 3 && memcmp(key, "gss", 3) == 0) {
            notice->config.gossip_steady_interval_s = val;
        } else if (klen == 3 && memcmp(key, "gni", 3) == 0) {
            notice->config.gossip_num_init_intervals = val;
        } else if (klen == 3 && memcmp(key, "qmh", 3) == 0) {
            notice->config.quasar_max_hops = val;
        } else if (klen == 2 && memcmp(key, "qa", 2) == 0) {
            notice->config.quasar_alpha = val;
        } else if (klen == 3 && memcmp(key, "qss", 3) == 0) {
            notice->config.quasar_seen_size = val;
        } else if (klen == 3 && memcmp(key, "qsh", 3) == 0) {
            notice->config.quasar_seen_hashes = val;
        }
    }

    notice->signature_len = cbor_bytestring_length(items[7]);
    if (notice->signature_len > POSEIDON_CHANNEL_NOTICE_MAX_SIGNATURE) return -1;
    memcpy(notice->signature, cbor_bytestring_handle(items[7]), notice->signature_len);

    return 0;
}