//
// Created by victor on 4/20/26.
//

#include "channel.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include "../Crypto/key_pair.h"
#include "channel_message.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <cbor.h>

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
    if (config == NULL || pool == NULL || wheel == NULL) return NULL;

    bool created_key_pair = false;
    if (key_pair == NULL) {
        key_pair = poseidon_key_pair_create("ED25519");
        if (key_pair == NULL) return NULL;
        created_key_pair = true;
    }

    poseidon_channel_t* channel = get_clear_memory(sizeof(poseidon_channel_t));
    if (channel == NULL) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        return NULL;
    }

    // Derive node_id from public key
    uint8_t* pub_key = NULL;
    size_t pub_key_len = 0;
    if (poseidon_key_pair_get_public_key(key_pair, &pub_key, &pub_key_len) != 0) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        free(channel);
        return NULL;
    }
    if (poseidon_node_id_from_public_key(pub_key, pub_key_len, &channel->node_id) != 0) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
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
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
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
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        unlink(key_path);
        unlink(cert_path);
        free(channel);
        return NULL;
    }

    // Create quasar overlay
    channel->quasar = quasar_create(channel->protocol,
                                     config->quasar_max_hops,
                                     config->quasar_alpha,
                                     4096, 3);
    if (channel->quasar == NULL) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        meridian_protocol_destroy(channel->protocol);
        unlink(key_path);
        unlink(cert_path);
        free(channel);
        return NULL;
    }

    channel->subtopic_subs = subtopic_table_create(64);
    if (channel->subtopic_subs == NULL) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        quasar_destroy(channel->quasar);
        meridian_protocol_destroy(channel->protocol);
        unlink(key_path);
        unlink(cert_path);
        free(channel);
        return NULL;
    }

    channel->aliases = topic_alias_registry_create(32);
    if (channel->aliases == NULL) {
        if (created_key_pair) poseidon_key_pair_destroy(key_pair);
        subtopic_table_destroy(channel->subtopic_subs);
        quasar_destroy(channel->quasar);
        meridian_protocol_destroy(channel->protocol);
        unlink(key_path);
        unlink(cert_path);
        free(channel);
        return NULL;
    }

    // Take a reference on the key pair (skip REFERENCE when we own it to avoid count=2 leak)
    if (created_key_pair) {
        channel->key_pair = key_pair;
    } else {
        channel->key_pair = REFERENCE(key_pair, poseidon_key_pair_t);
    }

    channel->state = POSEIDON_CHANNEL_STATE_INIT;
    channel->listen_port = listen_port;
    channel->is_dial = false;
    channel->owns_key_pair = created_key_pair;
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
        if (channel->subtopic_subs != NULL) subtopic_table_destroy(channel->subtopic_subs);
        if (channel->aliases != NULL) topic_alias_registry_destroy(channel->aliases);

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
// INTERNAL DELIVERY WRAPPER
// ============================================================================

static void channel_quasar_delivery_handler(void* ctx, const uint8_t* topic, size_t topic_len,
                                              const uint8_t* data, size_t data_len) {
    poseidon_channel_t* channel = (poseidon_channel_t*)ctx;

    // Intercept callback handles protocol-level packets (e.g. bootstrap)
    if (channel->intercept_cb != NULL) {
        if (channel->intercept_cb(channel->intercept_ctx, data, data_len)) {
            return;
        }
    }

    if (channel->delivery_cb == NULL) return;

    // Decode the channel message envelope to extract subtopic
    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, data_len, &result);
    if (item == NULL) {
        // Not a channel message envelope — deliver raw
        channel->delivery_cb(channel->delivery_cb_ctx, topic, topic_len, "", data, data_len);
        return;
    }

    char subtopic[256] = {0};
    uint8_t payload[4096] = {0};
    size_t payload_len = 0;
    if (channel_message_decode(item, subtopic, sizeof(subtopic),
                                payload, sizeof(payload), &payload_len) == 0) {
        // Check subtopic filter
        if (subtopic_table_should_deliver(channel->subtopic_subs, subtopic)) {
            channel->delivery_cb(channel->delivery_cb_ctx, topic, topic_len,
                                     subtopic, payload, payload_len);
        }
    } else {
        // Failed to decode — deliver raw
        channel->delivery_cb(channel->delivery_cb_ctx, topic, topic_len, "", data, data_len);
    }

    cbor_decref(&item);
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

int poseidon_channel_publish_subtopic(poseidon_channel_t* channel,
                                       const uint8_t* topic, size_t topic_len,
                                       const char* subtopic,
                                       const uint8_t* data, size_t data_len) {
    if (channel == NULL || channel->quasar == NULL || subtopic == NULL) return -1;

    // Wrap data in channel message envelope
    cbor_item_t* msg = channel_message_encode(
        (const uint8_t*)subtopic, strlen(subtopic), data, data_len);
    if (msg == NULL) return -1;

    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(msg, &buf, &buf_len);
    cbor_decref(&msg);

    if (written == 0 || buf == NULL) return -1;

    int rc = quasar_publish(channel->quasar, topic, topic_len, buf, written);
    free(buf);
    return rc;
}

int poseidon_channel_set_delivery_callback(poseidon_channel_t* channel,
                                            poseidon_channel_delivery_cb_t cb, void* ctx) {
    if (channel == NULL || channel->quasar == NULL) return -1;
    channel->delivery_cb = cb;
    channel->delivery_cb_ctx = ctx;
    quasar_set_delivery_callback(channel->quasar, channel_quasar_delivery_handler, channel);
    return 0;
}

int poseidon_channel_enable_quasar_delivery(poseidon_channel_t* channel) {
    if (channel == NULL || channel->quasar == NULL) return -1;
    quasar_set_delivery_callback(channel->quasar, channel_quasar_delivery_handler, channel);
    return 0;
}

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_tick(poseidon_channel_t* channel) {
    if (channel == NULL) return -1;
    int rc = quasar_tick(channel->quasar);
    subtopic_table_tick(channel->subtopic_subs);
    return rc;
}

int poseidon_channel_gossip(poseidon_channel_t* channel) {
    if (channel == NULL) return -1;
    int rc = meridian_protocol_gossip(channel->protocol);
    quasar_propagate(channel->quasar);
    return rc;
}

// ============================================================================
// SUBTOPIC OPERATIONS
// ============================================================================

int poseidon_channel_subscribe_subtopic(poseidon_channel_t* channel,
                                         const char* subtopic, uint32_t ttl) {
    if (channel == NULL || subtopic == NULL) return -1;
    return subtopic_table_subscribe(channel->subtopic_subs, subtopic, ttl);
}

int poseidon_channel_unsubscribe_subtopic(poseidon_channel_t* channel,
                                            const char* subtopic) {
    if (channel == NULL || subtopic == NULL) return -1;
    return subtopic_table_unsubscribe(channel->subtopic_subs, subtopic);
}

// ============================================================================
// TOPIC ALIASES
// ============================================================================

int poseidon_channel_register_alias(poseidon_channel_t* channel,
                                     const char* name, const char* topic) {
    if (channel == NULL) return -1;
    return topic_alias_register(channel->aliases, name, topic);
}

int poseidon_channel_unregister_alias(poseidon_channel_t* channel,
                                       const char* name) {
    if (channel == NULL) return -1;
    return topic_alias_unregister(channel->aliases, name);
}

const char* poseidon_channel_resolve_alias(const poseidon_channel_t* channel,
                                            const char* name) {
    if (channel == NULL) return NULL;
    return topic_alias_resolve(channel->aliases, name);
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