//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_MEASURE_H
#define POSEIDON_MERIDIAN_MEASURE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "../../Util/threadding.h"
#include "meridian.h"
#include "../../Util/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

/** Default size for the latency cache */
#define MERIDIAN_PROBE_CACHE_SIZE 1024

/**
 * Timeout for cached latency entries in microseconds.
 * Default: 5 seconds (5 * 1000 * 1000)
 */
#define MERIDIAN_PROBE_CACHE_TIMEOUT_US (5 * 1000 * 1000)

// ============================================================================
// LATENCY CACHE ENTRY
// ============================================================================

/**
 * A single entry in the latency cache.
 * Stores a node's address/port and its measured latency.
 */
typedef struct meridian_measure_entry_t {
    uint32_t addr;          /**< Node IPv4 address */
    uint16_t port;         /**< Node port */
    uint32_t latency_us;   /**< Measured latency in microseconds */
    struct timeval timestamp; /**< When this measurement was taken */
} meridian_measure_entry_t;

// ============================================================================
// LATENCY CACHE
// ============================================================================

/**
 * Thread-safe cache for recent latency measurements.
 * Avoids redundant probes to the same nodes.
 *
 * Eviction policy: oldest entry (by timestamp) when full.
 */
typedef struct meridian_latency_cache_t {
    refcounter_t refcounter;              /**< Reference counting for lifetime */
    meridian_measure_entry_t* entries;    /**< Array of cache entries */
    size_t capacity;                      /**< Maximum entries */
    size_t count;                         /**< Current number of entries */
    PLATFORMLOCKTYPE(lock);             /**< Thread-safe access */
} meridian_latency_cache_t;

// ============================================================================
// MEASUREMENT TYPES
// ============================================================================

/**
 * Methods for measuring latency to a node.
 */
typedef enum {
    MERIDIAN_MEASURE_TYPE_TCP,   /**< TCP ping measurement */
    MERIDIAN_MEASURE_TYPE_PING,  /**< ICMP ping measurement */
    MERIDIAN_MEASURE_TYPE_DNS    /**< DNS resolution measurement */
} meridian_measure_type_t;

// ============================================================================
// MEASUREMENT RESULT
// ============================================================================

/**
 * Result of a latency measurement operation.
 * Passed to the completion callback.
 */
typedef struct meridian_measure_result_t {
    meridian_node_t* node;     /**< Node that was measured */
    uint32_t latency_us;       /**< Measured latency in microseconds */
    bool success;               /**< Whether measurement succeeded */
} meridian_measure_result_t;

// ============================================================================
// LATENCY CACHE OPERATIONS
// ============================================================================

/**
 * Creates a latency cache with the specified capacity.
 *
 * @param capacity  Maximum entries (0 = default MERIDIAN_PROBE_CACHE_SIZE)
 * @return          New cache with refcount=1, or NULL on failure
 */
meridian_latency_cache_t* meridian_latency_cache_create(size_t capacity);

/**
 * Destroys a latency cache, freeing all memory.
 *
 * @param cache  Cache to destroy
 */
void meridian_latency_cache_destroy(meridian_latency_cache_t* cache);

/**
 * Inserts or updates a latency measurement.
 * If node exists, updates timestamp and latency.
 * If cache is full, evicts oldest entry first.
 *
 * @param cache       Cache to insert into
 * @param node        Node that was measured
 * @param latency_us  Measured latency in microseconds
 * @return            0 on success, -1 on failure
 */
int meridian_latency_cache_insert(meridian_latency_cache_t* cache,
                                   const meridian_node_t* node, uint32_t latency_us);

/**
 * Retrieves a cached latency measurement for a node.
 *
 * @param cache        Cache to query
 * @param node         Node to look up
 * @param latency_us   Output: latency in microseconds (valid if return is 0)
 * @return             0 if found, -1 if not found
 */
int meridian_latency_cache_get(meridian_latency_cache_t* cache,
                                 const meridian_node_t* node, uint32_t* latency_us);

/**
 * Evicts all expired entries from the cache.
 * Entries older than MERIDIAN_PROBE_CACHE_TIMEOUT_US are removed.
 * Uses compacting algorithm to fill gaps.
 *
 * @param cache  Cache to evict from
 */
void meridian_latency_cache_evict_expired(meridian_latency_cache_t* cache);

// ============================================================================
// MEASUREMENT REQUEST
// ============================================================================

/** Callback type for measurement completion */
typedef void (*meridian_measure_callback_t)(void* ctx, const meridian_measure_result_t* result);

/**
 * Represents a pending latency measurement request.
 * Tracks timeout and invokes callback on completion.
 */
typedef struct meridian_measure_request_t {
    uint64_t query_id;              /**< Unique identifier for this measure */
    meridian_node_t* target;        /**< Node to measure */
    meridian_measure_type_t type;   /**< Measurement method to use */
    struct timeval start_time;      /**< When request was created */
    struct timeval timeout;         /**< Deadline for completion */
    meridian_measure_callback_t callback; /**< Called when measurement finishes */
    void* ctx;                       /**< User context for callback */
    bool completed;                 /**< Whether measurement has finished */
} meridian_measure_request_t;

// ============================================================================
// MEASURE REQUEST LIFECYCLE
// ============================================================================

/**
 * Creates a new measure request for probing a target node.
 *
 * @param query_id   Unique identifier for this measure
 * @param target     Node to measure latency to
 * @param type       Measurement type (TCP, PING, DNS)
 * @param timeout_ms Timeout in milliseconds
 * @param callback   Function called when measurement completes
 * @param ctx        User context passed to callback
 * @return           New request with refcount=1, or NULL on failure
 */
meridian_measure_request_t* meridian_measure_request_create(
    uint64_t query_id,
    meridian_node_t* target,
    meridian_measure_type_t type,
    uint32_t timeout_ms,
    meridian_measure_callback_t callback,
    void* ctx);

/**
 * Destroys a measure request, releasing the target reference.
 *
 * @param req  Request to destroy
 */
void meridian_measure_request_destroy(meridian_measure_request_t* req);

/**
 * Updates the timeout for a measure request.
 *
 * @param req        Request to update
 * @param timeout_ms New timeout in milliseconds
 * @return           0 on success, -1 on failure
 */
int meridian_measure_request_set_timeout(meridian_measure_request_t* req, uint32_t timeout_ms);

/**
 * Checks if a measure request has timed out.
 *
 * @param req  Request to check
 * @return     true if expired, false otherwise (NULL returns true)
 */
bool meridian_measure_request_is_expired(const meridian_measure_request_t* req);

/**
 * Executes a measure request and invokes the completion callback.
 * Calculates elapsed time from start to execution.
 *
 * @param req  Request to execute
 * @return     0 on success, -1 on failure
 */
int meridian_measure_request_execute(meridian_measure_request_t* req);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_MEASURE_H