//
// Created by victor on 4/20/26.
//

#include "channel.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include "../Crypto/key_pair.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// ============================================================================
// DEFAULT CONFIGURATION
// ============================================================================

poseidon_channel_config_t poseidon_channel_config_defaults(void) {
    poseidon_channel_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        cfg.ring_sizes[i] = 8;
    }
    cfg.gossip_init_interval_s = 5;
    cfg.gossip_steady_interval_s = 30;
    cfg.gossip_num_init_intervals = 6;
    cfg.quasar_max_hops = 5;
    cfg.quasar_alpha = 3;
    cfg.quasar_seen_size = 1024;
    cfg.quasar_seen_hashes = 3;
    return cfg;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_channel_t* poseidon_channel_create(poseidon_key_pair_t* key_pair,
                                             const char* name,
                                             uint16_t listen_port,
                                             const poseidon_channel_config_t* config,
                                             work_pool_t* pool,
                                             hierarchical_timing_wheel_t* wheel) {
    if (key_pair == NULL || config == NULL || pool == NULL || wheel == NULL) return NULL;

    poseidon_channel_t* channel = get_clear_memory(sizeof(poseidon_channel_t));
    if (channel == NULL) return NULL;

    // Derive node_id from public key
    uint8_t* pub_key = NULL;
    size_t pub_key_len = 0;
    if (poseidon_key_pair_get_public_key(key_pair, &pub_key, &pub_key_len) != 0) {
        free(channel);
        return NULL;
    }
    if (poseidon_node_id_from_public_key(pub_key, pub_key_len, &channel->node_id) != 0) {
        free(pub_key);
        free(channel);
        return NULL;
    }
    free(pub_key);

    // Generate TLS files
    char key_path[256];
    char cert_path[256];
    snprintf(key_path, sizeof(key_path), "/tmp/poseidon_%s_key.pem", channel->node_id.str);
    snprintf(cert_path, sizeof(cert_path), "/tmp/poseidon_%s_cert.pem", channel->node_id.str);
    if (poseidon_key_pair_generate_tls_files(key_pair, channel->node_id.str,
                                              key_path, cert_path) != 0) {
        free(channel);
        return NULL;
    }

    // Create protocol config
    meridian_protocol_config_t proto_config = {
        .listen_port = listen_port,
        .info_port = 0,
        .primary_ring_size = config->ring_sizes[0],
        .secondary_ring_size = config->ring_sizes[0] / 2,
        .ring_exponent_base = 2,
        .init_gossip_interval_s = config->gossip_init_interval_s,
        .num_init_gossip_intervals = config->gossip_num_init_intervals,
        .steady_state_gossip_interval_s = config->gossip_steady_interval_s,
        .replace_interval_s = 60,
        .gossip_timeout_ms = 5000,
        .measure_timeout_ms = 5000,
        .tls_key_path = key_path,
        .tls_cert_path = cert_path,
        .local_node_id = channel->node_id,
        .pool = pool,
        .wheel = wheel
    };

    channel->protocol = meridian_protocol_create(&proto_config);
    if (channel->protocol == NULL) {
        free(channel);
        return NULL;
    }

    // Create quasar overlay
    channel->quasar = quasar_create(channel->protocol,
                                     config->quasar_max_hops,
                                     config->quasar_alpha,
                                     4096, 3);
    if (channel->quasar == NULL) {
        meridian_protocol_destroy(channel->protocol);
        free(channel);
        return NULL;
    }

    // Take a reference on the key pair
    channel->key_pair = REFERENCE(key_pair, poseidon_key_pair_t);

    channel->state = POSEIDON_CHANNEL_STATE_INIT;
    channel->listen_port = listen_port;
    channel->is_dial = false;
    channel->config = *config;

    if (name != NULL) {
        strncpy(channel->name, name, POSEIDON_CHANNEL_MAX_NAME - 1);
        channel->name[POSEIDON_CHANNEL_MAX_NAME - 1] = '\0';
    }

    platform_lock_init(&channel->lock);
    refcounter_init((refcounter_t*)channel);
    return channel;
}

void poseidon_channel_destroy(poseidon_channel_t* channel) {
    if (channel == NULL) return;
    refcounter_dereference((refcounter_t*)channel);
    if (refcounter_count((refcounter_t*)channel) == 0) {
        if (channel->quasar != NULL) quasar_destroy(channel->quasar);
        if (channel->protocol != NULL) meridian_protocol_destroy(channel->protocol);
        if (channel->key_pair != NULL) poseidon_key_pair_destroy(channel->key_pair);

        // Clean up TLS files
        char key_path[256];
        char cert_path[256];
        snprintf(key_path, sizeof(key_path), "/tmp/poseidon_%s_key.pem", channel->node_id.str);
        snprintf(cert_path, sizeof(cert_path), "/tmp/poseidon_%s_cert.pem", channel->node_id.str);
        unlink(key_path);
        unlink(cert_path);

        platform_lock_destroy(&channel->lock);
        free(channel);
    }
}

int poseidon_channel_start(poseidon_channel_t* channel,
                            meridian_node_t** seed_nodes, size_t num_seeds) {
    if (channel == NULL) return -1;

    channel->state = POSEIDON_CHANNEL_STATE_BOOTSTRAPPING;
    if (meridian_protocol_start(channel->protocol) != 0) return -1;

    for (size_t i = 0; i < num_seeds; i++) {
        meridian_protocol_add_seed_node(channel->protocol,
                                         seed_nodes[i]->addr, seed_nodes[i]->port);
    }

    channel->state = POSEIDON_CHANNEL_STATE_RUNNING;
    return 0;
}

int poseidon_channel_stop(poseidon_channel_t* channel) {
    if (channel == NULL) return -1;
    channel->state = POSEIDON_CHANNEL_STATE_SHUTTING_DOWN;
    int rc = meridian_protocol_stop(channel->protocol);
    channel->state = POSEIDON_CHANNEL_STATE_INIT;
    return rc;
}

// ============================================================================
// ACCESSORS
// ============================================================================

const poseidon_node_id_t* poseidon_channel_get_node_id(const poseidon_channel_t* channel) {
    if (channel == NULL) return NULL;
    return &channel->node_id;
}

const char* poseidon_channel_get_topic(const poseidon_channel_t* channel) {
    if (channel == NULL) return NULL;
    return channel->node_id.str;
}

// ============================================================================
// QUASAR OPERATIONS
// ============================================================================

int poseidon_channel_subscribe(poseidon_channel_t* channel,
                                const uint8_t* topic, size_t topic_len, uint32_t ttl) {
    if (channel == NULL || channel->quasar == NULL) return -1;
    return quasar_subscribe(channel->quasar, topic, topic_len, ttl);
}

int poseidon_channel_unsubscribe(poseidon_channel_t* channel,
                                  const uint8_t* topic, size_t topic_len) {
    if (channel == NULL || channel->quasar == NULL) return -1;
    return quasar_unsubscribe(channel->quasar, topic, topic_len);
}

int poseidon_channel_publish(poseidon_channel_t* channel,
                              const uint8_t* topic, size_t topic_len,
                              const uint8_t* data, size_t data_len) {
    if (channel == NULL || channel->quasar == NULL) return -1;
    return quasar_publish(channel->quasar, topic, topic_len, data, data_len);
}

int poseidon_channel_set_delivery_callback(poseidon_channel_t* channel,
                                            poseidon_channel_delivery_cb_t cb, void* ctx) {
    // Quasar doesn't yet have a delivery callback API — reserved for future wiring
    (void)channel;
    (void)cb;
    (void)ctx;
    return 0;
}

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_tick(poseidon_channel_t* channel) {
    if (channel == NULL) return -1;
    return quasar_tick(channel->quasar);
}

int poseidon_channel_gossip(poseidon_channel_t* channel) {
    if (channel == NULL) return -1;
    int rc = meridian_protocol_gossip(channel->protocol);
    quasar_propagate(channel->quasar);
    return rc;
}

// ============================================================================
// CONFIGURATION
// ============================================================================

int poseidon_channel_update_config(poseidon_channel_t* channel,
                                    const poseidon_channel_config_t* new_config,
                                    poseidon_key_pair_t* key_pair) {
    if (channel == NULL || new_config == NULL || key_pair == NULL) return -1;

    // Verify ownership: the provided key_pair must be the channel's key_pair
    if (key_pair != channel->key_pair) return -1;

    platform_lock(&channel->lock);
    channel->config = *new_config;
    platform_unlock(&channel->lock);
    return 0;
}