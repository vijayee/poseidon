//
// Created by victor on 4/20/26.
//

#include "channel_manager.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include "../Crypto/key_pair.h"
#include <string.h>

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

    // Subscribe to the topic on the dial channel to discover peers
    poseidon_channel_subscribe(mgr->dial_channel,
                                (const uint8_t*)topic_str, strlen(topic_str), 300);

    // Create a new channel for the topic
    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    return poseidon_channel_manager_create_channel(mgr, "ED25519", topic_str, &config);
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