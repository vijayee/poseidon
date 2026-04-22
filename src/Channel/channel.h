//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_CHANNEL_H
#define POSEIDON_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "../RefCounter/refcounter.h"
#include "../Crypto/key_pair.h"
#include "../Crypto/node_id.h"
#include "../Network/Meridian/meridian_protocol.h"
#include "../Network/Quasar/quasar.h"
#include "../Workers/pool.h"
#include "../Time/wheel.h"
#include "subtopic.h"
#include "topic_alias.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POSEIDON_CHANNEL_MAX_NAME 64

// ============================================================================
// CHANNEL STATES
// ============================================================================

typedef enum {
    POSEIDON_CHANNEL_STATE_INIT,
    POSEIDON_CHANNEL_STATE_BOOTSTRAPPING,
    POSEIDON_CHANNEL_STATE_RUNNING,
    POSEIDON_CHANNEL_STATE_SHUTTING_DOWN
} poseidon_channel_state_t;

// ============================================================================
// CHANNEL CONFIGURATION
// ============================================================================

typedef struct poseidon_channel_config_t {
    uint32_t ring_sizes[MERIDIAN_MAX_RINGS];   /**< Meridian ring sizes */
    uint32_t gossip_init_interval_s;     /**< Gossip interval during bootstrap */
    uint32_t gossip_steady_interval_s;   /**< Gossip interval in steady state */
    uint32_t gossip_num_init_intervals;  /**< Number of init-phase gossip cycles */
    uint32_t quasar_max_hops;            /**< Quasar routing filter depth */
    uint32_t quasar_alpha;               /**< Quasar random walk fan-out */
    uint32_t quasar_seen_size;           /**< Quasar dedup filter size (bits) */
    uint32_t quasar_seen_hashes;         /**< Quasar dedup filter hash count */
} poseidon_channel_config_t;

/** Returns default channel configuration values */
poseidon_channel_config_t poseidon_channel_config_defaults(void);

// ============================================================================
// CHANNEL
// ============================================================================

typedef struct poseidon_channel_t poseidon_channel_t;

typedef void (*poseidon_channel_delivery_cb_t)(void* ctx, const uint8_t* topic,
                                                size_t topic_len,
                                                const char* subtopic,
                                                const uint8_t* data, size_t data_len);

/** Intercept callback: return true if packet was handled (skip normal delivery) */
typedef bool (*poseidon_channel_intercept_cb_t)(void* ctx, const uint8_t* data, size_t data_len);

typedef struct poseidon_channel_t {
    refcounter_t refcounter;
    poseidon_channel_state_t state;
    poseidon_key_pair_t* key_pair;           /**< Channel identity (owns ref) */
    poseidon_node_id_t node_id;              /**< BLAKE3(pub_key) -> Base58 */
    char name[POSEIDON_CHANNEL_MAX_NAME];    /**< Human-readable name */
    poseidon_channel_config_t config;        /**< Mutable configuration */
    meridian_protocol_t* protocol;           /**< Meridian P2P network */
    quasar_t* quasar;                        /**< Quasar pub/sub overlay */
    uint16_t listen_port;
    bool is_dial;
    bool owns_key_pair;
    subtopic_table_t* subtopic_subs;    /**< Per-granularity subtopic subscriptions */
    topic_alias_registry_t* aliases;     /**< Human-readable name → Base58 ID map */
    poseidon_channel_delivery_cb_t delivery_cb;
    void* delivery_cb_ctx;
    poseidon_channel_intercept_cb_t intercept_cb;  /**< Intercepts Quasar deliveries (e.g. bootstrap) */
    void* intercept_ctx;
    PLATFORMLOCKTYPE(lock);
} poseidon_channel_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

/**
 * Creates a channel from an existing key pair.
 * Derives node_id, generates TLS files, and creates protocol + quasar.
 *
 * @param key_pair    Key pair for channel identity (caller retains ref)
 * @param name        Human-readable channel name
 * @param listen_port Port for the Meridian protocol
 * @param config      Channel configuration
 * @param pool        Work pool for async operations
 * @param wheel       Timing wheel for scheduling
 * @return            New channel with refcount=1, or NULL on failure
 */
poseidon_channel_t* poseidon_channel_create(poseidon_key_pair_t* key_pair,
                                             const char* name,
                                             uint16_t listen_port,
                                             const poseidon_channel_config_t* config,
                                             work_pool_t* pool,
                                             hierarchical_timing_wheel_t* wheel);

void poseidon_channel_destroy(poseidon_channel_t* channel);

int poseidon_channel_start(poseidon_channel_t* channel,
                            meridian_node_t** seed_nodes, size_t num_seeds);
int poseidon_channel_stop(poseidon_channel_t* channel);

// ============================================================================
// ACCESSORS
// ============================================================================

const poseidon_node_id_t* poseidon_channel_get_node_id(const poseidon_channel_t* channel);
const char* poseidon_channel_get_topic(const poseidon_channel_t* channel);

// ============================================================================
// QUASAR OPERATIONS
// ============================================================================

int poseidon_channel_subscribe(poseidon_channel_t* channel,
                                const uint8_t* topic, size_t topic_len, uint32_t ttl);
int poseidon_channel_unsubscribe(poseidon_channel_t* channel,
                                  const uint8_t* topic, size_t topic_len);
int poseidon_channel_publish(poseidon_channel_t* channel,
                              const uint8_t* topic, size_t topic_len,
                              const uint8_t* data, size_t data_len);
int poseidon_channel_publish_subtopic(poseidon_channel_t* channel,
                                       const uint8_t* topic, size_t topic_len,
                                       const char* subtopic,
                                       const uint8_t* data, size_t data_len);

int poseidon_channel_set_delivery_callback(poseidon_channel_t* channel,
                                            poseidon_channel_delivery_cb_t cb, void* ctx);

/** Enables Quasar delivery on a channel without setting an application callback. */
int poseidon_channel_enable_quasar_delivery(poseidon_channel_t* channel);

// ============================================================================
// SUBTOPIC OPERATIONS
// ============================================================================

int poseidon_channel_subscribe_subtopic(poseidon_channel_t* channel,
                                         const char* subtopic, uint32_t ttl);
int poseidon_channel_unsubscribe_subtopic(poseidon_channel_t* channel,
                                            const char* subtopic);

// ============================================================================
// TOPIC ALIASES
// ============================================================================

int poseidon_channel_register_alias(poseidon_channel_t* channel,
                                     const char* name, const char* topic);
int poseidon_channel_unregister_alias(poseidon_channel_t* channel,
                                       const char* name);
const char* poseidon_channel_resolve_alias(const poseidon_channel_t* channel,
                                            const char* name);

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int poseidon_channel_tick(poseidon_channel_t* channel);
int poseidon_channel_gossip(poseidon_channel_t* channel);

// ============================================================================
// CONFIGURATION
// ============================================================================

/**
 * Updates channel configuration. The caller must hold the channel's private key.
 * Compares the provided key_pair against the channel's key_pair to verify ownership.
 *
 * @param channel    Channel to update
 * @param new_config New configuration values
 * @param key_pair   Key pair proving ownership (must match channel's key_pair)
 * @return           0 on success, -1 if key mismatch or invalid args
 */
int poseidon_channel_update_config(poseidon_channel_t* channel,
                                    const poseidon_channel_config_t* new_config,
                                    poseidon_key_pair_t* key_pair);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CHANNEL_H