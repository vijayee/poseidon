//
// Created by victor on 4/20/26.
//

#include "channel_manager.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include "../Crypto/key_pair.h"
#include "../Network/Meridian/meridian_packet.h"
#include "../Network/Meridian/meridian_protocol.h"
#include <string.h>
#include <cbor.h>
#include <time.h>

// ============================================================================
// BOOTSTRAP INTERCEPT
// ============================================================================

static bool channel_manager_bootstrap_intercept(void* ctx, const uint8_t* data, size_t data_len) {
    poseidon_channel_manager_t* mgr = (poseidon_channel_manager_t*)ctx;

    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, data_len, &result);
    if (item == NULL || !cbor_array_is_definite(item) || cbor_array_size(item) < 1) {
        if (item != NULL) cbor_decref(&item);
        return false;
    }

    cbor_item_t** items = cbor_array_handle(item);
    if (!cbor_isa_uint(items[0])) {
        cbor_decref(&item);
        return false;
    }

    uint8_t pkt_type = cbor_get_uint8(items[0]);
    bool handled = false;

    if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP) {
        char topic_id[64], sender_node_id[64];
        uint64_t ts;
        if (meridian_channel_bootstrap_decode(item, topic_id, sizeof(topic_id),
                                               sender_node_id, sizeof(sender_node_id), &ts) == 0) {
            poseidon_channel_manager_handle_bootstrap_request(mgr, topic_id, sender_node_id);
            handled = true;
        }
    } else if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY) {
        char topic_id[64], responder_node_id[64];
        uint32_t addr; uint16_t port; uint64_t ts;
        uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds;
        if (meridian_channel_bootstrap_reply_decode(item, topic_id, sizeof(topic_id),
                                                      responder_node_id, sizeof(responder_node_id),
                                                      &addr, &port, &ts,
                                                      seed_addrs, seed_ports, &num_seeds, 16) == 0) {
            poseidon_channel_manager_handle_bootstrap_reply(
                mgr, topic_id, addr, port, ts, seed_addrs, seed_ports, num_seeds);
            handled = true;
        }
    }

    cbor_decref(&item);
    return handled;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_channel_manager_t* poseidon_channel_manager_create(
    poseidon_key_pair_t* dial_key_pair,
    uint16_t dial_port,
    uint16_t port_range_start,
    uint16_t port_range_end,
    work_pool_t* pool,
    hierarchical_timing_wheel_t* wheel) {
    if (dial_key_pair == NULL || pool == NULL || wheel == NULL) return NULL;
    if (port_range_end <= port_range_start) return NULL;

    poseidon_channel_manager_t* mgr = get_clear_memory(sizeof(poseidon_channel_manager_t));
    if (mgr == NULL) return NULL;

    mgr->port_range_start = port_range_start;
    mgr->port_range_end = port_range_end;
    mgr->next_port = port_range_start;
    mgr->pool = pool;
    mgr->wheel = wheel;
    mgr->num_channels = 0;

    // Create the dial channel
    poseidon_channel_config_t dial_config = poseidon_channel_config_defaults();
    mgr->dial_channel = poseidon_channel_create(dial_key_pair, "dial", dial_port,
                                                  &dial_config, pool, wheel);
    if (mgr->dial_channel == NULL) {
        free(mgr);
        return NULL;
    }
    mgr->dial_channel->is_dial = true;

    // Wire intercept + delivery so bootstrap packets delivered via Quasar are dispatched
    mgr->dial_channel->intercept_cb = channel_manager_bootstrap_intercept;
    mgr->dial_channel->intercept_ctx = mgr;
    poseidon_channel_enable_quasar_delivery(mgr->dial_channel);

    platform_lock_init(&mgr->lock);
    refcounter_init((refcounter_t*)mgr);
    return mgr;
}

void poseidon_channel_manager_destroy(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return;
    refcounter_dereference((refcounter_t*)mgr);
    if (refcounter_count((refcounter_t*)mgr) == 0) {
        if (mgr->dial_channel != NULL) {
            poseidon_channel_destroy(mgr->dial_channel);
        }
        for (size_t i = 0; i < mgr->num_channels; i++) {
            if (mgr->channels[i] != NULL) {
                poseidon_channel_destroy(mgr->channels[i]);
            }
        }
        platform_lock_destroy(&mgr->lock);
        free(mgr);
    }
}

// ============================================================================
// CHANNEL MANAGEMENT
// ============================================================================

static uint16_t allocate_port(poseidon_channel_manager_t* mgr) {
    if (mgr->next_port > mgr->port_range_end) return 0;
    uint16_t port = mgr->next_port++;
    return port;
}

poseidon_channel_t* poseidon_channel_manager_create_channel(
    poseidon_channel_manager_t* mgr,
    const char* key_type,
    const char* name,
    const poseidon_channel_config_t* config) {
    if (mgr == NULL || config == NULL) return NULL;

    platform_lock(&mgr->lock);

    if (mgr->num_channels >= POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    uint16_t port = allocate_port(mgr);
    if (port == 0) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    poseidon_key_pair_t* kp = poseidon_key_pair_create(key_type);
    if (kp == NULL) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    poseidon_channel_t* channel = poseidon_channel_create(kp, name, port,
                                                            config, mgr->pool, mgr->wheel);
    poseidon_key_pair_destroy(kp);

    if (channel == NULL) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    mgr->channels[mgr->num_channels++] = channel;
    platform_unlock(&mgr->lock);
    return channel;
}

poseidon_channel_t* poseidon_channel_manager_join_channel(
    poseidon_channel_manager_t* mgr,
    const char* topic_str) {
    if (mgr == NULL || topic_str == NULL) return NULL;

    platform_lock(&mgr->lock);

    if (mgr->num_channels >= POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    if (mgr->num_pending_bootstraps >= POSEIDON_MAX_PENDING_BOOTSTRAPS) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    uint16_t port = allocate_port(mgr);
    if (port == 0) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(NULL, topic_str, port,
                                                          &config, mgr->pool, mgr->wheel);
    if (channel == NULL) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    channel->state = POSEIDON_CHANNEL_STATE_BOOTSTRAPPING;

    // Register pending bootstrap entry
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;

    pending_bootstrap_t* pending = &mgr->pending_bootstraps[mgr->num_pending_bootstraps];
    strncpy(pending->topic_id, topic_str, sizeof(pending->topic_id) - 1);
    pending->topic_id[sizeof(pending->topic_id) - 1] = '\0';
    pending->timestamp_us = timestamp_us;
    pending->channel = channel;
    pending->num_replies = 0;
    mgr->num_pending_bootstraps++;

    mgr->channels[mgr->num_channels++] = channel;
    platform_unlock(&mgr->lock);

    // I/O operations outside lock to avoid deadlock with Quasar delivery callbacks
    poseidon_channel_subscribe(mgr->dial_channel,
                                (const uint8_t*)topic_str, strlen(topic_str), 300);

    const char* sender_node_id = poseidon_channel_get_topic(mgr->dial_channel);
    cbor_item_t* bootstrap = meridian_channel_bootstrap_encode(topic_str, sender_node_id, timestamp_us);
    if (bootstrap != NULL) {
        unsigned char* buf = NULL;
        size_t buf_len = 0;
        size_t written = cbor_serialize_alloc(bootstrap, &buf, &buf_len);
        cbor_decref(&bootstrap);
        if (written > 0 && buf != NULL) {
            poseidon_channel_publish(mgr->dial_channel,
                                      (const uint8_t*)topic_str, strlen(topic_str),
                                      buf, written);
            free(buf);
        }
    }

    return channel;
}

int poseidon_channel_manager_destroy_channel(poseidon_channel_manager_t* mgr,
                                               const poseidon_node_id_t* node_id) {
    if (mgr == NULL || node_id == NULL) return -1;

    platform_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (poseidon_node_id_compare(poseidon_channel_get_node_id(mgr->channels[i]),
                                      node_id) == 0) {
            poseidon_channel_destroy(mgr->channels[i]);
            mgr->channels[i] = mgr->channels[mgr->num_channels - 1];
            mgr->channels[mgr->num_channels - 1] = NULL;
            mgr->num_channels--;
            platform_unlock(&mgr->lock);
            return 0;
        }
    }
    platform_unlock(&mgr->lock);
    return -1;
}

poseidon_channel_t* poseidon_channel_manager_find_channel(
    const poseidon_channel_manager_t* mgr,
    const poseidon_node_id_t* node_id) {
    if (mgr == NULL || node_id == NULL) return NULL;

    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (poseidon_node_id_compare(poseidon_channel_get_node_id(mgr->channels[i]),
                                      node_id) == 0) {
            return mgr->channels[i];
        }
    }
    return NULL;
}

poseidon_channel_t* poseidon_channel_manager_get_dial(
    const poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return NULL;
    return mgr->dial_channel;
}

int poseidon_channel_manager_handle_bootstrap_reply(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds) {
    if (mgr == NULL || topic_id == NULL) return -1;

    meridian_protocol_t* protocol = NULL;
    bool first_reply = false;

    platform_lock(&mgr->lock);

    pending_bootstrap_t* pending = NULL;
    size_t pending_index = 0;
    for (size_t i = 0; i < mgr->num_pending_bootstraps; i++) {
        if (strcmp(mgr->pending_bootstraps[i].topic_id, topic_id) == 0 &&
            mgr->pending_bootstraps[i].timestamp_us == timestamp_us) {
            pending = &mgr->pending_bootstraps[i];
            pending_index = i;
            break;
        }
    }

    if (pending == NULL) {
        platform_unlock(&mgr->lock);
        return -1;
    }

    // Store reply address/port
    if (pending->num_replies < POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
        pending->reply_addrs[pending->num_replies] = responder_addr;
        pending->reply_ports[pending->num_replies] = responder_port;
        pending->num_replies++;
    }

    // On first reply: transition to RUNNING, remove pending, defer I/O
    if (pending->num_replies == 1) {
        protocol = pending->channel->protocol;
        pending->channel->state = POSEIDON_CHANNEL_STATE_RUNNING;
        first_reply = true;

        // Remove pending entry by shifting array
        for (size_t i = pending_index; i + 1 < mgr->num_pending_bootstraps; i++) {
            mgr->pending_bootstraps[i] = mgr->pending_bootstraps[i + 1];
        }
        memset(&mgr->pending_bootstraps[mgr->num_pending_bootstraps - 1], 0, sizeof(pending_bootstrap_t));
        mgr->num_pending_bootstraps--;
    }

    platform_unlock(&mgr->lock);

    // I/O operations outside lock
    if (first_reply && protocol != NULL) {
        meridian_protocol_connect(protocol, responder_addr, responder_port);

        size_t seeds_to_add = num_seeds;
        if (seeds_to_add > 16) seeds_to_add = 16;
        for (size_t i = 0; i < seeds_to_add; i++) {
            meridian_protocol_add_seed_node(protocol, seed_addrs[i], seed_ports[i]);
        }
    }

    return 0;
}

int poseidon_channel_manager_handle_bootstrap_request(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id) {
    if (mgr == NULL || topic_id == NULL || sender_node_id == NULL) return -1;

    platform_lock(&mgr->lock);

    // Check if we're a member of the requested channel
    bool is_member = false;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (mgr->channels[i] != NULL) {
            const char* channel_topic = poseidon_channel_get_topic(mgr->channels[i]);
            if (channel_topic != NULL && strcmp(channel_topic, topic_id) == 0) {
                is_member = true;
                break;
            }
        }
    }

    if (!is_member) {
        platform_unlock(&mgr->lock);
        return -1;
    }

    // Collect info needed for reply while locked
    const char* responder_node_id = poseidon_channel_get_topic(mgr->dial_channel);
    uint32_t responder_addr = 0;
    uint16_t responder_port = 0;
    meridian_protocol_get_local_node(mgr->dial_channel->protocol, &responder_addr, &responder_port);
    poseidon_channel_t* dial_channel = mgr->dial_channel;

    platform_unlock(&mgr->lock);

    // Build and publish a BOOTSTRAP_REPLY outside lock
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;

    cbor_item_t* reply = meridian_channel_bootstrap_reply_encode(
        topic_id, responder_node_id, responder_addr, responder_port, timestamp_us,
        NULL, NULL, 0);

    int rc = 0;
    if (reply != NULL) {
        unsigned char* buf = NULL;
        size_t buf_len = 0;
        size_t written = cbor_serialize_alloc(reply, &buf, &buf_len);
        cbor_decref(&reply);
        if (written > 0 && buf != NULL) {
            rc = poseidon_channel_publish(dial_channel,
                                           (const uint8_t*)topic_id, strlen(topic_id),
                                           buf, written);
            free(buf);
        } else {
            rc = -1;
        }
    } else {
        rc = -1;
    }

    return rc;
}

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_manager_tick_all(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return -1;

    int rc = 0;
    if (mgr->dial_channel != NULL) {
        rc |= poseidon_channel_tick(mgr->dial_channel);
    }
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (mgr->channels[i] != NULL) {
            rc |= poseidon_channel_tick(mgr->channels[i]);
        }
    }
    return rc;
}

int poseidon_channel_manager_gossip_all(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return -1;

    int rc = 0;
    if (mgr->dial_channel != NULL) {
        rc |= poseidon_channel_gossip(mgr->dial_channel);
    }
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (mgr->channels[i] != NULL) {
            rc |= poseidon_channel_gossip(mgr->channels[i]);
        }
    }
    return rc;
}