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
#include "../Network/Meridian/meridian_packet.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS 32
#define POSEIDON_SESSION_REGISTRY_MAX 256
#define POSEIDON_MAX_PENDING_BOOTSTRAPS 16
#define POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX 16
#define POSEIDON_TOMBSTONE_MAX 64
#define POSEIDON_TOMBSTONE_DEFAULT_TTL_SECONDS (90 * 24 * 3600)

typedef enum {
    POSEIDON_TOMBSTONE_DELETE,
    POSEIDON_TOMBSTONE_MODIFY
} poseidon_tombstone_type_t;

typedef struct poseidon_tombstone_t {
    poseidon_tombstone_type_t type;
    char topic_id[64];
    char node_id[64];
    char key_type[16];
    uint8_t public_key[64];
    size_t public_key_len;
    uint64_t timestamp_us;
    uint64_t expires_at_us;
    uint8_t signature[64];
    size_t signature_len;
    poseidon_channel_config_t config;
    bool has_config;
} poseidon_tombstone_t;

typedef struct pending_bootstrap_t {
    char topic_id[64];
    uint64_t timestamp_us;
    poseidon_channel_t* channel;
    uint32_t reply_addrs[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    uint16_t reply_ports[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    size_t num_replies;
} pending_bootstrap_t;

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
    pending_bootstrap_t pending_bootstraps[POSEIDON_MAX_PENDING_BOOTSTRAPS];
    size_t num_pending_bootstraps;
    poseidon_tombstone_t tombstones[POSEIDON_TOMBSTONE_MAX];
    size_t num_tombstones;
    void* sessions[POSEIDON_SESSION_REGISTRY_MAX];
    size_t num_sessions;
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

/**
 * Handles a bootstrap reply from a peer.
 * Finds the matching pending bootstrap, stores the reply info, and on first
 * reply connects to the responder and transitions the channel to RUNNING.
 *
 * @param mgr            Channel manager
 * @param topic_id       Topic ID string
 * @param responder_addr Responder IPv4 address
 * @param responder_port Responder port
 * @param timestamp_us   Timestamp from the bootstrap request
 * @param seed_addrs     Seed node addresses
 * @param seed_ports     Seed node ports
 * @param num_seeds      Number of seed nodes
 * @return               0 on success, -1 if pending bootstrap not found
 */
int poseidon_channel_manager_handle_bootstrap_reply(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds);

/**
 * Handles a bootstrap request from a peer.
 * If this node is a member of the requested channel, publishes a bootstrap
 * reply on the dial channel with local connection info.
 *
 * @param mgr             Channel manager
 * @param topic_id        Topic ID string
 * @param sender_node_id  Sender node ID string
 * @return                0 on success, -1 if not a member or publish failed
 */
int poseidon_channel_manager_handle_bootstrap_request(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id);

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_manager_tick_all(poseidon_channel_manager_t* mgr);
int poseidon_channel_manager_gossip_all(poseidon_channel_manager_t* mgr);

// ============================================================================
// SESSION REGISTRY
// ============================================================================

int poseidon_channel_manager_register_session(poseidon_channel_manager_t* mgr, void* session);
int poseidon_channel_manager_unregister_session(poseidon_channel_manager_t* mgr, void* session);

// ============================================================================
// TOMBSTONE OPERATIONS
// ============================================================================

int poseidon_channel_manager_add_tombstone(poseidon_channel_manager_t* mgr,
                                            const poseidon_tombstone_t* tombstone);

const poseidon_tombstone_t* poseidon_channel_manager_find_tombstone(
    const poseidon_channel_manager_t* mgr, const char* topic_id);

void poseidon_channel_manager_expire_tombstones(poseidon_channel_manager_t* mgr);

int poseidon_tombstone_from_delete_notice(const meridian_channel_delete_notice_t* notice,
                                             poseidon_tombstone_t* tombstone);

int poseidon_tombstone_from_modify_notice(const meridian_channel_modify_notice_t* notice,
                                            poseidon_tombstone_t* tombstone);

// ============================================================================
// DELETE/MODIFY NOTICE HANDLERS
// ============================================================================

/**
 * Handles a verified delete notice received from the network.
 * Stores a tombstone, deletes the channel locally if present, and
 * redistributes the notice on the dial channel.
 */
void poseidon_channel_manager_handle_delete_notice(
    poseidon_channel_manager_t* mgr,
    const meridian_channel_delete_notice_t* notice);

/**
 * Handles a verified modify notice received from the network.
 * Stores a tombstone, triggers rejoin with new config if channel is local,
 * and redistributes the notice on the dial channel.
 */
void poseidon_channel_manager_handle_modify_notice(
    poseidon_channel_manager_t* mgr,
    const meridian_channel_modify_notice_t* notice);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_MANAGER_H
