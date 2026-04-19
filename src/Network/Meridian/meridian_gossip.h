//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_GOSSIP_H
#define POSEIDON_MERIDIAN_GOSSIP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "../../Util/threadding.h"
#include "meridian.h"
#include "meridian_query.h"
#include "meridian_packet.h"

// ============================================================================
// GOSSIP STATES
// ============================================================================

/**
 * Lifecycle states for a gossip exchange.
 */
typedef enum {
    MERIDIAN_GOSSIP_STATE_INIT,       /**< Created, not yet active */
    MERIDIAN_GOSSIP_STATE_ACTIVE,     /**< In progress */
    MERIDIAN_GOSSIP_STATE_WAIT_REPLY  /**< Sent request, waiting for response */
} meridian_gossip_state_t;

// ============================================================================
// GOSSIP STRUCTURE
// ============================================================================

/**
 * Represents a gossip exchange with a specific target node.
 * Tracks timing and state for the exchange lifecycle.
 */
typedef struct meridian_gossip_t {
    refcounter_t refcounter;          /**< Reference counting for lifetime */
    uint64_t query_id;                /**< Unique identifier for this exchange */
    meridian_gossip_state_t state;    /**< Current state of the exchange */
    meridian_node_t* target;         /**< Node to exchange gossip with */
    struct timeval start_time;        /**< When gossip was initiated */
    struct timeval reply_timeout;     /**< Deadline for receiving reply */
    uint32_t gossip_interval_s;       /**< Interval between gossip cycles (init) */
    uint32_t steady_state_interval_s; /**< Interval between gossip cycles (steady) */
    bool active;                      /**< Whether gossip is currently active */
} meridian_gossip_t;

// ============================================================================
// GOSSIP SCHEDULER
// ============================================================================

/**
 * Scheduler for controlling gossip frequency.
 * Implements two-phase timing: aggressive init, relaxed steady-state.
 */
typedef struct meridian_gossip_scheduler_t {
    refcounter_t refcounter;         /**< Reference counting for lifetime */
    uint32_t init_interval_s;         /**< Gossip interval during initialization */
    uint32_t num_init_intervals;      /**< Number of init intervals before steady-state */
    uint32_t steady_state_interval_s; /**< Gossip interval during steady-state */
    uint32_t interval_idx;            /**< Current interval index (init phase) */
    bool is_initial_phase;            /**< true = init phase, false = steady-state */
    struct timeval last_gossip;        /**< Timestamp of last gossip sent */
    PLATFORMLOCKTYPE(lock);          /**< Thread-safe access */
} meridian_gossip_scheduler_t;

// ============================================================================
// GOSSIP LIFECYCLE
// ============================================================================

/**
 * Creates a new gossip exchange with a target node.
 *
 * @param query_id   Unique identifier for this gossip exchange
 * @param target     Node to exchange gossip with
 * @param timeout_ms Timeout in milliseconds
 * @return           New gossip with refcount=1, or NULL on failure
 */
meridian_gossip_t* meridian_gossip_create(uint64_t query_id, meridian_node_t* target,
                                           uint32_t timeout_ms);

/**
 * Destroys a gossip, releasing the target node reference.
 *
 * @param gossip  Gossip to destroy
 */
void meridian_gossip_destroy(meridian_gossip_t* gossip);

/**
 * Activates a gossip exchange for sending/receiving.
 *
 * @param gossip  Gossip to activate
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_start(meridian_gossip_t* gossip);

/**
 * Checks if a gossip has timed out waiting for a reply.
 *
 * @param gossip  Gossip to check
 * @return        true if expired, false otherwise
 */
bool meridian_gossip_is_expired(const meridian_gossip_t* gossip);

/**
 * Handles an incoming reply packet to this gossip.
 * Updates state from WAIT_REPLY to ACTIVE on success.
 *
 * @param gossip  Gossip that received the reply
 * @param pkt     Reply packet received
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_response(meridian_gossip_t* gossip,
                                     const meridian_gossip_packet_t* pkt);

/**
 * Builds a gossip packet for transmission.
 * Populates packet fields from the gossip state.
 *
 * @param gossip   Gossip to build from
 * @param out_pkt  Output: newly created packet
 * @return         0 on success, -1 on failure
 */
int meridian_gossip_build_packet(meridian_gossip_t* gossip,
                                  meridian_gossip_packet_t** out_pkt);

// ============================================================================
// SCHEDULER LIFECYCLE
// ============================================================================

/**
 * Creates a gossip scheduler with specified intervals.
 * Handles two-phase scheduling (init vs steady-state).
 *
 * @param init_interval_s        Interval during initialization phase
 * @param num_init_intervals     Number of init intervals before steady-state
 * @param steady_state_interval_s Interval during steady-state
 * @return                       New scheduler with refcount=1, or NULL on failure
 */
meridian_gossip_scheduler_t* meridian_gossip_scheduler_create(
    uint32_t init_interval_s, uint32_t num_init_intervals,
    uint32_t steady_state_interval_s);

/**
 * Destroys a gossip scheduler.
 *
 * @param sched  Scheduler to destroy
 */
void meridian_gossip_scheduler_destroy(meridian_gossip_scheduler_t* sched);

/**
 * Ticks the scheduler to determine if gossip should be sent.
 *
 * Algorithm:
 *   1. Determine interval based on phase (init vs steady-state)
 *   2. Check if enough time has passed since last gossip
 *   3. If yes, set should_gossip=true and update last_gossip
 *   4. Track init phase intervals; transition after num_init_intervals
 *
 * @param sched         Scheduler to tick
 * @param should_gossip Output: true if gossip should be sent now
 * @return              0 on success, -1 on failure
 */
int meridian_gossip_scheduler_tick(meridian_gossip_scheduler_t* sched,
                                    bool* should_gossip);

/**
 * Updates the steady-state gossip interval.
 *
 * @param sched       Scheduler to update
 * @param interval_s  New steady-state interval in seconds
 * @return            0 on success, -1 on failure
 */
int meridian_gossip_scheduler_set_steady_state(meridian_gossip_scheduler_t* sched,
                                                uint32_t interval_s);

// ============================================================================
// GOSSIP CONFIGURATION AND CALLBACKS
// ============================================================================

/** Callback for sending gossip packets to the network */
typedef void (*meridian_gossip_outbound_fn)(void* ctx, const uint8_t* data, size_t len,
                                             const meridian_node_t* target);

/** Callback when gossip exchange completes */
typedef void (*meridian_gossip_completed_fn)(void* ctx, uint64_t query_id,
                                              meridian_node_t** peers, size_t num_peers);

/**
 * Configuration for a gossip handle.
 * Contains callbacks and timing parameters.
 */
typedef struct meridian_gossip_config_t {
    void* user_ctx;                    /**< User context passed to callbacks */
    meridian_gossip_outbound_fn outbound_cb;    /**< Called to send data */
    meridian_gossip_completed_fn completed_cb;   /**< Called on completion */
    uint32_t init_interval_s;          /**< Init phase gossip interval */
    uint32_t num_init_intervals;       /**< Number of init intervals */
    uint32_t steady_state_interval_s;  /**< Steady-state gossip interval */
    uint32_t timeout_ms;               /**< Timeout for gossip replies */
} meridian_gossip_config_t;

// ============================================================================
// GOSSIP HANDLE
// ============================================================================

/**
 * Handle for managing all gossip operations.
 * Owns a scheduler, query table, and active gossip list.
 */
typedef struct meridian_gossip_handle_t {
    meridian_gossip_config_t config;    /**< Configuration and callbacks */
    meridian_gossip_scheduler_t* scheduler; /**< Timing scheduler */
    meridian_query_table_t* query_table;   /**< Tracks in-flight gossip queries */
    PLATFORMLOCKTYPE(lock);           /**< Thread-safe access */
    vec_t(meridian_gossip_t*) active_gossips; /**< Currently active gossip exchanges */
    bool running;                      /**< Whether handle is active */
} meridian_gossip_handle_t;

// ============================================================================
// HANDLE LIFECYCLE
// ============================================================================

/**
 * Creates a gossip handle from configuration.
 * Creates internal scheduler and query table.
 *
 * @param config  Configuration with callbacks and intervals
 * @return        New handle, or NULL on failure
 */
meridian_gossip_handle_t* meridian_gossip_handle_create(const meridian_gossip_config_t* config);

/**
 * Destroys a gossip handle, releasing all resources.
 *
 * @param handle  Handle to destroy
 */
void meridian_gossip_handle_destroy(meridian_gossip_handle_t* handle);

/**
 * Starts the gossip handle for sending/receiving.
 *
 * @param handle  Handle to start
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_start(meridian_gossip_handle_t* handle);

/**
 * Stops the gossip handle.
 *
 * @param handle  Handle to stop
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_stop(meridian_gossip_handle_t* handle);

/**
 * Handles an incoming packet from a peer.
 *
 * @param handle  Handle that received packet
 * @param data    Raw packet data
 * @param len     Length of data
 * @param from    Node that sent the packet
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_on_packet(meridian_gossip_handle_t* handle,
                                      const uint8_t* data, size_t len,
                                      const meridian_node_t* from);

/**
 * Sends gossip to multiple target nodes.
 *
 * Algorithm:
 *   1. Create gossip for each target with unique query ID
 *   2. Build CBOR-encoded gossip packet
 *   3. Invoke outbound_cb to send
 *   4. Track active gossip for response handling
 *
 * @param handle      Handle to send from
 * @param targets     Array of target nodes
 * @param num_targets Number of targets
 * @return            0 on success, -1 on failure
 */
int meridian_gossip_handle_send_gossip(meridian_gossip_handle_t* handle,
                                       meridian_node_t** targets, size_t num_targets);

/**
 * Gets a peer for this gossip handle.
 * (Currently a placeholder - returns NULL)
 *
 * @return  NULL (placeholder)
 */
meridian_gossip_handle_t* meridian_gossip_handle_get_peer(void);

#ifdef __cplusplus
}
#endif
#endif // POSEIDON_MERIDIAN_GOSSIP_H