//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_CHANNEL_MANAGER_H
#define POSEIDON_CHANNEL_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "../RefCounter/refcounter.h"
#include "../Crypto/key_pair.h"
#include "../Crypto/node_id.h"
#include "channel.h"
#include "../Network/Meridian/meridian.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS 32

typedef struct poseidon_channel_manager_t {
    refcounter_t refcounter;
    poseidon_channel_t* dial_channel;
    poseidon_channel_t* channels[POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS];
    size_t num_channels;
    uint16_t next_port;
    uint16_t port_range_start;
    uint16_t port_range_end;
    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;
    PLATFORMLOCKTYPE(lock);
} poseidon_channel_manager_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

/**
 * Creates a channel manager with a dial channel.
 * The dial channel is created from the provided key pair and started.
 *
 * @param dial_key_pair   Key pair for the dial channel
 * @param dial_port       Port for the dial channel
 * @param port_range_start Start of the port range for data channels
 * @param port_range_end   End of the port range (inclusive)
 * @param pool            Work pool for async operations
 * @param wheel           Timing wheel for scheduling
 * @return                New manager with refcount=1, or NULL on failure
 */
poseidon_channel_manager_t* poseidon_channel_manager_create(
    poseidon_key_pair_t* dial_key_pair,
    uint16_t dial_port,
    uint16_t port_range_start,
    uint16_t port_range_end,
    work_pool_t* pool,
    hierarchical_timing_wheel_t* wheel);

void poseidon_channel_manager_destroy(poseidon_channel_manager_t* mgr);

// ============================================================================
// CHANNEL MANAGEMENT
// ============================================================================

/**
 * Creates a new channel with a generated key pair.
 * Allocates a port from the manager's port range.
 *
 * @param mgr       Channel manager
 * @param key_type  Key type for the generated key pair ("ED25519", "RSA", "EC")
 * @param name      Human-readable name for the channel
 * @param config    Channel configuration
 * @return          New channel (manager retains a ref), or NULL on failure
 */
poseidon_channel_t* poseidon_channel_manager_create_channel(
    poseidon_channel_manager_t* mgr,
    const char* key_type,
    const char* name,
    const poseidon_channel_config_t* config);

/**
 * Joins an existing channel by publishing a bootstrap request on the dial channel.
 *
 * @param mgr       Channel manager
 * @param topic_str Channel topic string (Base58-encoded node ID)
 * @return          New channel connected to the topic, or NULL on failure
 */
poseidon_channel_t* poseidon_channel_manager_join_channel(
    poseidon_channel_manager_t* mgr,
    const char* topic_str);

int poseidon_channel_manager_destroy_channel(poseidon_channel_manager_t* mgr,
                                               const poseidon_node_id_t* node_id);

poseidon_channel_t* poseidon_channel_manager_find_channel(
    const poseidon_channel_manager_t* mgr,
    const poseidon_node_id_t* node_id);

poseidon_channel_t* poseidon_channel_manager_get_dial(
    const poseidon_channel_manager_t* mgr);

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_manager_tick_all(poseidon_channel_manager_t* mgr);
int poseidon_channel_manager_gossip_all(poseidon_channel_manager_t* mgr);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_MANAGER_H