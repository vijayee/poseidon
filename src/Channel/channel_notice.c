//
// Created by victor on 4/22/26.
//

#include "channel_notice.h"
#include "../Crypto/key_pair.h"
#include "../Crypto/node_id.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <cbor.h>

// ============================================================================
// HELPER: BUILD SIGNABLE PAYLOAD
// ============================================================================

static cbor_item_t* build_delete_signable(const char* topic_id, const char* node_id,
                                            const char* key_type,
                                            const uint8_t* public_key, size_t key_len,
                                            uint64_t timestamp_us) {
    cbor_item_t* array = cbor_new_definite_array(6);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(key_type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(public_key, key_len))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(timestamp_us)))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

static cbor_item_t* build_modify_signable(const char* topic_id, const char* node_id,
                                            const char* key_type,
                                            const uint8_t* public_key, size_t key_len,
                                            uint64_t timestamp_us,
                                            const poseidon_channel_config_t* config) {
    cbor_item_t* array = cbor_new_definite_array(7);
    if (array == NULL) return NULL;

    if (!cbor_array_push(array, cbor_move(cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(topic_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(node_id))) ||
        !cbor_array_push(array, cbor_move(cbor_build_string(key_type))) ||
        !cbor_array_push(array, cbor_move(cbor_build_bytestring(public_key, key_len))) ||
        !cbor_array_push(array, cbor_move(cbor_build_uint64(timestamp_us)))) {
        cbor_decref(&array);
        return NULL;
    }

    // Encode config as a map (same format as modify notice encode)
    cbor_item_t* config_map = cbor_new_definite_map(8 + MERIDIAN_MAX_RINGS);
    if (config_map == NULL) { cbor_decref(&array); return NULL; }

    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        char key[4];
        snprintf(key, sizeof(key), "r%d", i);
        if (!cbor_map_add(config_map, (struct cbor_pair) {
            .key = cbor_move(cbor_build_string(key)),
            .value = cbor_move(cbor_build_uint32(config->ring_sizes[i]))
        })) { cbor_decref(&config_map); cbor_decref(&array); return NULL; }
    }
    if (!cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gis")),
        .value = cbor_move(cbor_build_uint32(config->gossip_init_interval_s))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gss")),
        .value = cbor_move(cbor_build_uint32(config->gossip_steady_interval_s))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("gni")),
        .value = cbor_move(cbor_build_uint32(config->gossip_num_init_intervals))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qmh")),
        .value = cbor_move(cbor_build_uint32(config->quasar_max_hops))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qa")),
        .value = cbor_move(cbor_build_uint32(config->quasar_alpha))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qss")),
        .value = cbor_move(cbor_build_uint32(config->quasar_seen_size))
    }) || !cbor_map_add(config_map, (struct cbor_pair) {
        .key = cbor_move(cbor_build_string("qsh")),
        .value = cbor_move(cbor_build_uint32(config->quasar_seen_hashes))
    })) { cbor_decref(&config_map); cbor_decref(&array); return NULL; }

    if (!cbor_array_push(array, cbor_move(config_map))) {
        cbor_decref(&array);
        return NULL;
    }

    return array;
}

// ============================================================================
// DELETE NOTICE CREATION
// ============================================================================

meridian_channel_delete_notice_t* poseidon_channel_create_delete_notice(
    poseidon_channel_t* channel) {
    if (channel == NULL || channel->key_pair == NULL) return NULL;

    meridian_channel_delete_notice_t* notice =
        (meridian_channel_delete_notice_t*)calloc(1, sizeof(*notice));
    if (notice == NULL) return NULL;

    const char* topic = poseidon_channel_get_topic(channel);
    const poseidon_node_id_t* nid = poseidon_channel_get_node_id(channel);
    const char* kt = poseidon_key_pair_get_key_type(channel->key_pair);

    if (topic == NULL || nid == NULL || kt == NULL) { free(notice); return NULL; }

    uint8_t* pub_key = NULL;
    size_t pub_key_len = 0;
    if (poseidon_key_pair_get_public_key(channel->key_pair, &pub_key, &pub_key_len) != 0) {
        free(notice);
        return NULL;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_us = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;

    notice->type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
    strncpy(notice->topic_id, topic, sizeof(notice->topic_id) - 1);
    strncpy(notice->node_id, nid->str, sizeof(notice->node_id) - 1);
    strncpy(notice->key_type, kt, sizeof(notice->key_type) - 1);

    if (pub_key_len > POSEIDON_CHANNEL_NOTICE_MAX_PUBLIC_KEY) {
        free(pub_key);
        free(notice);
        return NULL;
    }
    memcpy(notice->public_key, pub_key, pub_key_len);
    notice->public_key_len = pub_key_len;
    notice->timestamp_us = timestamp_us;

    // Build signable payload and sign it
    cbor_item_t* signable = build_delete_signable(topic, nid->str, kt,
                                                    pub_key, pub_key_len, timestamp_us);
    if (signable == NULL) { free(pub_key); free(notice); return NULL; }

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(signable, &buf, &buf_len);
    cbor_decref(&signable);

    if (written == 0 || buf == NULL) { free(buf); free(pub_key); free(notice); return NULL; }

    size_t sig_len = 0;
    int rc = poseidon_key_pair_sign(channel->key_pair, buf, written,
                                     notice->signature, &sig_len);
    free(buf);
    free(pub_key);

    if (rc != 0 || sig_len > POSEIDON_CHANNEL_NOTICE_MAX_SIGNATURE) {
        free(notice);
        return NULL;
    }
    notice->signature_len = sig_len;

    return notice;
}

// ============================================================================
// MODIFY NOTICE CREATION
// ============================================================================

meridian_channel_modify_notice_t* poseidon_channel_create_modify_notice(
    poseidon_channel_t* channel,
    const poseidon_channel_config_t* new_config) {
    if (channel == NULL || channel->key_pair == NULL || new_config == NULL) return NULL;

    meridian_channel_modify_notice_t* notice =
        (meridian_channel_modify_notice_t*)calloc(1, sizeof(*notice));
    if (notice == NULL) return NULL;

    const char* topic = poseidon_channel_get_topic(channel);
    const poseidon_node_id_t* nid = poseidon_channel_get_node_id(channel);
    const char* kt = poseidon_key_pair_get_key_type(channel->key_pair);

    if (topic == NULL || nid == NULL || kt == NULL) { free(notice); return NULL; }

    uint8_t* pub_key = NULL;
    size_t pub_key_len = 0;
    if (poseidon_key_pair_get_public_key(channel->key_pair, &pub_key, &pub_key_len) != 0) {
        free(notice);
        return NULL;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_us = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;

    notice->type = MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE;
    strncpy(notice->topic_id, topic, sizeof(notice->topic_id) - 1);
    strncpy(notice->node_id, nid->str, sizeof(notice->node_id) - 1);
    strncpy(notice->key_type, kt, sizeof(notice->key_type) - 1);

    if (pub_key_len > POSEIDON_CHANNEL_NOTICE_MAX_PUBLIC_KEY) {
        free(pub_key);
        free(notice);
        return NULL;
    }
    memcpy(notice->public_key, pub_key, pub_key_len);
    notice->public_key_len = pub_key_len;
    notice->timestamp_us = timestamp_us;
    notice->config = *new_config;

    cbor_item_t* signable = build_modify_signable(topic, nid->str, kt,
                                                    pub_key, pub_key_len, timestamp_us,
                                                    new_config);
    if (signable == NULL) { free(pub_key); free(notice); return NULL; }

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(signable, &buf, &buf_len);
    cbor_decref(&signable);

    if (written == 0 || buf == NULL) { free(buf); free(pub_key); free(notice); return NULL; }

    size_t sig_len = 0;
    int rc = poseidon_key_pair_sign(channel->key_pair, buf, written,
                                     notice->signature, &sig_len);
    free(buf);
    free(pub_key);

    if (rc != 0 || sig_len > POSEIDON_CHANNEL_NOTICE_MAX_SIGNATURE) {
        free(notice);
        return NULL;
    }
    notice->signature_len = sig_len;

    return notice;
}

// ============================================================================
// VERIFICATION
// ============================================================================

int poseidon_channel_verify_delete_notice(const meridian_channel_delete_notice_t* notice) {
    if (notice == NULL) return -1;

    // Step 1: Verify BLAKE3(public_key) == node_id
    poseidon_node_id_t expected_id;
    if (poseidon_node_id_from_string(notice->node_id, &expected_id) != 0) return -1;
    if (poseidon_node_id_verify_public_key(&expected_id, notice->public_key,
                                             notice->public_key_len) != 0)
        return -1;

    // Step 2: Reconstruct signable payload and verify signature
    cbor_item_t* signable = build_delete_signable(
        notice->topic_id, notice->node_id, notice->key_type,
        notice->public_key, notice->public_key_len, notice->timestamp_us);
    if (signable == NULL) return -1;

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(signable, &buf, &buf_len);
    cbor_decref(&signable);

    if (written == 0 || buf == NULL) { free(buf); return -1; }

    int rc = poseidon_verify_signature_with_key(notice->key_type,
                                                  notice->public_key, notice->public_key_len,
                                                  buf, written,
                                                  notice->signature, notice->signature_len);
    free(buf);
    return rc;
}

int poseidon_channel_verify_modify_notice(const meridian_channel_modify_notice_t* notice) {
    if (notice == NULL) return -1;

    poseidon_node_id_t expected_id;
    if (poseidon_node_id_from_string(notice->node_id, &expected_id) != 0) return -1;
    if (poseidon_node_id_verify_public_key(&expected_id, notice->public_key,
                                             notice->public_key_len) != 0)
        return -1;

    cbor_item_t* signable = build_modify_signable(
        notice->topic_id, notice->node_id, notice->key_type,
        notice->public_key, notice->public_key_len, notice->timestamp_us,
        &notice->config);
    if (signable == NULL) return -1;

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(signable, &buf, &buf_len);
    cbor_decref(&signable);

    if (written == 0 || buf == NULL) { free(buf); return -1; }

    int rc = poseidon_verify_signature_with_key(notice->key_type,
                                                  notice->public_key, notice->public_key_len,
                                                  buf, written,
                                                  notice->signature, notice->signature_len);
    free(buf);
    return rc;
}