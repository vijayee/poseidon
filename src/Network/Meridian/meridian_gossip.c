//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_gossip.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>

// ============================================================================
// GOSSIP LIFECYCLE
// ============================================================================

/**
 * Creates a new gossip exchange with a target node.
 * Starts timing immediately for timeout tracking.
 *
 * @param query_id   Unique identifier for this gossip exchange
 * @param target     Node to exchange gossip with
 * @param timeout_ms Timeout in milliseconds
 * @return           New gossip with refcount=1, or NULL on failure
 */
meridian_gossip_t* meridian_gossip_create(uint64_t query_id, meridian_node_t* target,
                                           uint32_t timeout_ms) {
    if (target == NULL) return NULL;

    meridian_gossip_t* gossip = (meridian_gossip_t*)
        get_clear_memory(sizeof(meridian_gossip_t));

    gossip->query_id = query_id;
    gossip->state = MERIDIAN_GOSSIP_STATE_INIT;
    gossip->target = (meridian_node_t*) refcounter_reference(&target->refcounter);
    gettimeofday(&gossip->start_time, NULL);
    gossip->reply_timeout.tv_sec = timeout_ms / 1000;
    gossip->reply_timeout.tv_usec = (timeout_ms % 1000) * 1000;
    gossip->active = false;
    gossip->gossip_interval_s = 5;
    gossip->steady_state_interval_s = 5;

    refcounter_init(&gossip->refcounter);
    return gossip;
}

/**
 * Destroys a gossip, releasing target node reference.
 *
 * @param gossip  Gossip to destroy
 */
void meridian_gossip_destroy(meridian_gossip_t* gossip) {
    if (gossip == NULL) return;

    refcounter_dereference(&gossip->refcounter);
    if (refcounter_count(&gossip->refcounter) == 0) {
        if (gossip->target) {
            refcounter_dereference(&gossip->target->refcounter);
            if (refcounter_count(&gossip->target->refcounter) == 0) {
                free(gossip->target);
            }
        }
        free(gossip);
    }
}

/**
 * Activates a gossip exchange.
 *
 * @param gossip  Gossip to activate
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_start(meridian_gossip_t* gossip) {
    if (gossip == NULL) return -1;
    gossip->active = true;
    gossip->state = MERIDIAN_GOSSIP_STATE_ACTIVE;
    return 0;
}

/**
 * Checks if a gossip has timed out waiting for a reply.
 *
 * @param gossip  Gossip to check
 * @return        true if expired, false otherwise
 */
bool meridian_gossip_is_expired(const meridian_gossip_t* gossip) {
    if (gossip == NULL) return true;

    struct timeval now;
    gettimeofday(&now, NULL);

    struct timeval deadline;
    timeradd(&gossip->start_time, &gossip->reply_timeout, &deadline);

    return timercmp(&now, &deadline, >);
}

/**
 * Handles an incoming reply to the gossip.
 * Updates state from WAIT_REPLY to ACTIVE.
 *
 * @param gossip  Gossip that received reply
 * @param pkt     Reply packet
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_response(meridian_gossip_t* gossip,
                                     const meridian_gossip_packet_t* pkt) {
    (void)pkt; // Reserved for response validation
    if (gossip == NULL || pkt == NULL) return -1;

    if (gossip->state == MERIDIAN_GOSSIP_STATE_WAIT_REPLY) {
        gossip->state = MERIDIAN_GOSSIP_STATE_ACTIVE;
    }

    return 0;
}

/**
 * Builds a gossip packet for transmission.
 * Populates the packet with query ID and target node info.
 *
 * @param gossip   Gossip to build from
 * @param out_pkt  Output: newly created packet
 * @return         0 on success, -1 on failure
 */
int meridian_gossip_build_packet(meridian_gossip_t* gossip,
                                  meridian_gossip_packet_t** out_pkt) {
    if (gossip == NULL || out_pkt == NULL) return -1;

    meridian_gossip_packet_t* pkt = meridian_gossip_packet_create();
    if (pkt == NULL) return -1;

    pkt->base.query_id = gossip->query_id;
    pkt->base.rendv_addr = gossip->target->rendv_addr;
    pkt->base.rendv_port = gossip->target->rendv_port;

    if (gossip->target) {
        meridian_gossip_packet_add_target(pkt, gossip->target);
    }

    *out_pkt = pkt;
    return 0;
}

// ============================================================================
// GOSSIP SCHEDULER
// ============================================================================

/**
 * Creates a gossip scheduler with the specified intervals.
 * Handles two-phase scheduling: initialization and steady-state.
 *
 * @param init_interval_s        Interval during initialization phase
 * @param num_init_intervals     Number of init intervals before steady-state
 * @param steady_state_interval_s Interval during steady-state
 * @return                       New scheduler with refcount=1, or NULL on failure
 */
meridian_gossip_scheduler_t* meridian_gossip_scheduler_create(
    uint32_t init_interval_s, uint32_t num_init_intervals,
    uint32_t steady_state_interval_s) {

    meridian_gossip_scheduler_t* sched = (meridian_gossip_scheduler_t*)
        get_clear_memory(sizeof(meridian_gossip_scheduler_t));

    sched->init_interval_s = init_interval_s;
    sched->num_init_intervals = num_init_intervals;
    sched->steady_state_interval_s = steady_state_interval_s;
    sched->interval_idx = 0;
    sched->is_initial_phase = true;
    gettimeofday(&sched->last_gossip, NULL);
    platform_lock_init(&sched->lock);
    refcounter_init(&sched->refcounter);

    return sched;
}

/**
 * Destroys a scheduler.
 *
 * @param sched  Scheduler to destroy
 */
void meridian_gossip_scheduler_destroy(meridian_gossip_scheduler_t* sched) {
    if (sched == NULL) return;

    refcounter_dereference(&sched->refcounter);
    if (refcounter_count(&sched->refcounter) == 0) {
        platform_lock_destroy(&sched->lock);
        free(sched);
    }
}

/**
 * Ticks the scheduler, determining if gossip should be sent.
 *
 * Algorithm:
 * 1. Determine interval based on current phase (init vs steady-state)
 * 2. Check if enough time has passed since last gossip
 * 3. If yes, set should_gossip=true and update last_gossip timestamp
 * 4. If in init phase, track interval count; transition to steady-state after num_init_intervals
 *
 * @param sched         Scheduler to tick
 * @param should_gossip Output: true if gossip should be sent now
 * @return              0 on success, -1 on failure
 */
int meridian_gossip_scheduler_tick(meridian_gossip_scheduler_t* sched,
                                    bool* should_gossip) {
    if (sched == NULL || should_gossip == NULL) return -1;

    *should_gossip = false;

    platform_lock(&sched->lock);

    struct timeval now;
    gettimeofday(&now, NULL);

    // Determine interval based on phase
    struct timeval interval;
    if (sched->is_initial_phase) {
        interval.tv_sec = sched->init_interval_s;
        interval.tv_usec = 0;
    } else {
        interval.tv_sec = sched->steady_state_interval_s;
        interval.tv_usec = 0;
    }

    // Check if it's time for next gossip
    struct timeval next_gossip;
    timeradd(&sched->last_gossip, &interval, &next_gossip);

    if (timercmp(&now, &next_gossip, >=)) {
        *should_gossip = true;
        sched->last_gossip = now;

        // Track init phase intervals
        if (sched->is_initial_phase) {
            sched->interval_idx++;
            if (sched->interval_idx >= sched->num_init_intervals) {
                sched->is_initial_phase = false;
            }
        }
    }

    platform_unlock(&sched->lock);
    return 0;
}

/**
 * Updates the steady-state interval.
 *
 * @param sched       Scheduler to update
 * @param interval_s  New steady-state interval in seconds
 * @return            0 on success, -1 on failure
 */
int meridian_gossip_scheduler_set_steady_state(meridian_gossip_scheduler_t* sched,
                                                uint32_t interval_s) {
    if (sched == NULL) return -1;
    sched->steady_state_interval_s = interval_s;
    return 0;
}

// ============================================================================
// GOSSIP HANDLE
// ============================================================================

/**
 * Creates a gossip handle managing all gossip operations.
 * Creates its own query table and scheduler.
 *
 * @param config  Configuration with callbacks and intervals
 * @return        New handle, or NULL on failure
 */
meridian_gossip_handle_t* meridian_gossip_handle_create(const meridian_gossip_config_t* config) {
    if (config == NULL) return NULL;

    meridian_gossip_handle_t* handle = (meridian_gossip_handle_t*)
        get_clear_memory(sizeof(meridian_gossip_handle_t));

    memcpy(&handle->config, config, sizeof(meridian_gossip_config_t));
    vec_init(&handle->active_gossips);

    handle->scheduler = meridian_gossip_scheduler_create(
        config->init_interval_s,
        config->num_init_intervals,
        config->steady_state_interval_s
    );

    handle->query_table = meridian_query_table_create(64);
    handle->running = false;
    platform_lock_init(&handle->lock);

    return handle;
}

/**
 * Destroys a gossip handle, releasing all resources.
 *
 * @param handle  Handle to destroy
 */
void meridian_gossip_handle_destroy(meridian_gossip_handle_t* handle) {
    if (handle == NULL) return;

    if (handle->scheduler) {
        meridian_gossip_scheduler_destroy(handle->scheduler);
    }
    if (handle->query_table) {
        meridian_query_table_destroy(handle->query_table);
    }

    vec_deinit(&handle->active_gossips);
    platform_lock_destroy(&handle->lock);
    free(handle);
}

/**
 * Starts the gossip handle.
 *
 * @param handle  Handle to start
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_start(meridian_gossip_handle_t* handle) {
    if (handle == NULL) return -1;
    platform_lock(&handle->lock);
    handle->running = true;
    platform_unlock(&handle->lock);
    return 0;
}

/**
 * Stops the gossip handle.
 *
 * @param handle  Handle to stop
 * @return        0 on success, -1 on failure
 */
int meridian_gossip_handle_stop(meridian_gossip_handle_t* handle) {
    if (handle == NULL) return -1;
    platform_lock(&handle->lock);
    handle->running = false;
    platform_unlock(&handle->lock);
    return 0;
}

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
                                      const meridian_node_t* from) {
    (void)handle;
    (void)data;
    (void)len;
    (void)from;
    if (handle == NULL || data == NULL || from == NULL) return -1;

    return 0;
}

/**
 * Sends gossip to multiple target nodes.
 * Creates a gossip for each target and builds CBOR-encoded packets.
 *
 * Algorithm:
 * 1. For each target, create a gossip query
 * 2. Build gossip packet with target info
 * 3. CBOR-encode the packet
 * 4. Invoke outbound_cb to actually send the data
 * 5. Track active gossip for response handling
 *
 * @param handle      Handle to send from
 * @param targets     Array of target nodes
 * @param num_targets Number of targets
 * @return            0 on success, -1 on failure
 */
int meridian_gossip_handle_send_gossip(meridian_gossip_handle_t* handle,
                                       meridian_node_t** targets, size_t num_targets) {
    if (handle == NULL || targets == NULL || num_targets == 0) return -1;

    for (size_t i = 0; i < num_targets; i++) {
        // Create unique query ID for this gossip exchange
        uint64_t qid = (uint64_t) i + 1;
        qid = (qid << 32) | (uint64_t)((uintptr_t)targets[i] & 0xFFFFFFFF);

        meridian_gossip_t* gossip = meridian_gossip_create(
            qid, targets[i], handle->config.timeout_ms);
        if (gossip == NULL) continue;

        // Build and encode packet
        meridian_gossip_packet_t* pkt = NULL;
        if (meridian_gossip_build_packet(gossip, &pkt) == 0 && pkt != NULL) {
            cbor_item_t* encoded = meridian_gossip_encode(pkt);
            if (encoded != NULL) {
                unsigned char* buffer = NULL;
                size_t buf_sz = 0;
                size_t buffer_size = cbor_serialize_alloc(encoded, &buffer, &buf_sz);
                if (buffer_size > 0 && buffer != NULL) {
                    cbor_serialize(encoded, buffer, buffer_size);
                    // Invoke callback to send the actual data
                    handle->config.outbound_cb(handle->config.user_ctx,
                        buffer, buffer_size, targets[i]);
                    free(buffer);
                }
                cbor_decref(&encoded);
            }
            meridian_gossip_packet_destroy(pkt);
        }

        // Track active gossip
        vec_push(&handle->active_gossips, gossip);
    }

    return 0;
}

