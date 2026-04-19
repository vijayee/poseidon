//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_QUERY_H
#define POSEIDON_MERIDIAN_QUERY_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "../../Util/threadding.h"
#include "meridian.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

/** Maximum number of target nodes a query can track */
#define MERIDIAN_QUERY_MAX_TARGETS 256

// ============================================================================
// QUERY TYPES
// ============================================================================

/**
 * Types of queries supported by the Meridian protocol.
 * Each type corresponds to a different network operation.
 */
typedef enum {
    MERIDIAN_QUERY_TYPE_GOSSIP,     /**< Node exchange with peers */
    MERIDIAN_QUERY_TYPE_MEASURE,    /**< Latency measurement to target */
    MERIDIAN_QUERY_TYPE_CLOSEST,    /**< Find lowest-latency node among targets */
    MERIDIAN_QUERY_TYPE_CONSTRAINT, /**< Find node meeting constraints */
    MERIDIAN_QUERY_TYPE_INFO        /**< Info request/response */
} meridian_query_type_t;

// ============================================================================
// QUERY STATUS
// ============================================================================

/**
 * Lifecycle states for a query.
 * Transitions: INIT -> WAITING -> FINISHED/FAILED
 */
typedef enum {
    MERIDIAN_QUERY_STATUS_INIT,     /**< Created but not yet sent */
    MERIDIAN_QUERY_STATUS_WAITING,  /**< Sent, awaiting response */
    MERIDIAN_QUERY_STATUS_FINISHED, /**< Completed successfully */
    MERIDIAN_QUERY_STATUS_FAILED   /**< Failed or timed out */
} meridian_query_status_t;

// ============================================================================
// QUERY STRUCTURE
// ============================================================================

/**
 * Represents an in-flight query to network peers.
 * Tracks multiple targets and their measured latencies.
 *
 * Lifecycle:
 * 1. Create with timeout
 * 2. Add targets
 * 3. Send and wait (or measure)
 * 4. Check expired/finished
 * 5. Get closest result or destroy
 */
typedef struct meridian_query_t {
    refcounter_t refcounter;              /**< Reference counting for lifetime */
    uint64_t query_id;                    /**< Unique identifier for this query */
    meridian_query_type_t type;           /**< Type of query operation */
    meridian_query_status_t status;       /**< Current state of the query */
    struct timeval start_time;            /**< When query was created */
    struct timeval timeout;               /**< Deadline for this query */
    meridian_node_t* source;              /**< Node that initiated this query */
    meridian_node_t* targets[MERIDIAN_QUERY_MAX_TARGETS]; /**< Target nodes */
    uint32_t latencies[MERIDIAN_QUERY_MAX_TARGETS];        /**< Measured latencies */
    size_t num_targets;                   /**< Number of targets currently set */
    size_t num_measured;                  /**< Number of targets with latency data */
    void* user_ctx;                       /**< User-supplied context data */
} meridian_query_t;

/** Callback type for query completion notifications */
typedef void (*meridian_query_callback_t)(void* ctx, meridian_query_t* query);

// ============================================================================
// QUERY TABLE
// ============================================================================

/**
 * Thread-safe table for tracking all in-flight queries.
 * Provides O(1) lookup by query_id and manages expiration.
 *
 * The table owns references to all inserted queries.
 * When a query expires, ownership is transferred to the caller via tick().
 */
typedef struct meridian_query_table_t {
    refcounter_t refcounter;              /**< Reference counting for lifetime */
    meridian_query_t** queries;           /**< Array of query pointers */
    size_t capacity;                      /**< Allocated capacity */
    size_t count;                         /**< Current number of queries */
    PLATFORMLOCKTYPE(lock);              /**< Thread-safe access lock */
} meridian_query_table_t;

// ============================================================================
// QUERY LIFECYCLE
// ============================================================================

/**
 * Creates a new query with the given parameters.
 *
 * @param query_id   Unique identifier for this query
 * @param type       Query type (GOSSIP, CLOSEST, MEASURE, etc.)
 * @param timeout_ms Timeout in milliseconds
 * @return           New query with refcount=1, or NULL on failure
 */
meridian_query_t* meridian_query_create(uint64_t query_id,
                                        meridian_query_type_t type,
                                        uint32_t timeout_ms);

/**
 * Destroys a query, releasing all target and source references.
 * Only frees memory when refcount reaches zero.
 *
 * @param query  Query to destroy
 */
void meridian_query_destroy(meridian_query_t* query);

// ============================================================================
// TARGET MANAGEMENT
// ============================================================================

/**
 * Adds a target node to the query.
 * Takes a reference to the target node.
 *
 * @param query  Query to add target to
 * @param node   Target node to add
 * @return       0 on success, -1 on failure (NULL, full, etc.)
 */
int meridian_query_add_target(meridian_query_t* query, meridian_node_t* node);

/**
 * Records a latency measurement for a target by index.
 *
 * @param query       Query to record measurement for
 * @param target_idx  Index of target (0-based)
 * @param latency     Measured latency in microseconds
 * @return            0 on success, -1 on failure
 */
int meridian_query_set_latency(meridian_query_t* query, size_t target_idx, uint32_t latency);

/**
 * Finds the target with the lowest recorded latency.
 * Scans all measured latencies and returns the closest target.
 *
 * @param query  Query to search
 * @return       Closest target node, or NULL if no targets
 */
meridian_node_t* meridian_query_get_closest(meridian_query_t* query);

// ============================================================================
// EXPIRATION AND STATUS
// ============================================================================

/**
 * Checks if a query has exceeded its timeout.
 *
 * @param query  Query to check
 * @return       true if expired, false otherwise (NULL returns true)
 */
bool meridian_query_is_expired(const meridian_query_t* query);

/**
 * Checks if a query has completed (success or failure).
 *
 * @param query  Query to check
 * @return       true if finished, false otherwise
 */
bool meridian_query_is_finished(const meridian_query_t* query);

/**
 * Marks a query as finished successfully.
 *
 * @param query  Query to mark complete
 * @return       0 on success, -1 on failure
 */
int meridian_query_finish(meridian_query_t* query);

/**
 * Gets the user context pointer from a query.
 *
 * @param query  Query to get context from
 * @return       User context, or NULL if query is NULL
 */
void* meridian_query_get_ctx(meridian_query_t* query);

// ============================================================================
// QUERY TABLE MANAGEMENT
// ============================================================================

/**
 * Creates a new query table for tracking in-flight queries.
 *
 * @param capacity  Initial capacity (0 = default 64)
 * @return          New table with refcount=1, or NULL on failure
 */
meridian_query_table_t* meridian_query_table_create(size_t capacity);

/**
 * Destroys a query table, freeing all tracked queries.
 * Only frees memory when refcount reaches zero.
 *
 * @param table  Table to destroy
 */
void meridian_query_table_destroy(meridian_query_table_t* table);

/**
 * Inserts a query into the table.
 * Table takes ownership of a reference to the query.
 *
 * @param table  Table to insert into
 * @param query  Query to insert
 * @return       0 on success, -1 on failure
 */
int meridian_query_table_insert(meridian_query_table_t* table, meridian_query_t* query);

/**
 * Removes a query from the table by ID.
 * Destroys the query and compacts the array.
 *
 * @param table     Table to remove from
 * @param query_id  ID of query to remove
 * @return          0 if found and removed, -1 if not found
 */
int meridian_query_table_remove(meridian_query_table_t* table, uint64_t query_id);

/**
 * Looks up a query by ID.
 * Returns a new reference; caller must eventually destroy.
 *
 * @param table     Table to search
 * @param query_id  ID to find
 * @return          Query with new reference, or NULL if not found
 */
meridian_query_t* meridian_query_table_lookup(meridian_query_table_t* table, uint64_t query_id);

/**
 * Ticks the table, finding and expiring timed-out queries.
 * Expired queries are transferred to caller ownership via CONSUME pattern.
 *
 * Algorithm:
 *   1. Count expired queries
 *   2. Allocate output array for expired queries
 *   3. Partition: non-expired -> front, expired -> caller via CONSUME
 *   4. Update count to exclude expired
 *
 * @param table        Table to tick
 * @param expired      Output: array of expired queries (caller owns them)
 * @param num_expired  Output: number of expired queries
 * @return             0 on success, -1 on failure
 */
int meridian_query_table_tick(meridian_query_table_t* table,
                               meridian_query_t*** expired,
                               size_t* num_expired);

// ============================================================================
// SPECIALIZED QUERY FACTORIES
// ============================================================================

/**
 * Creates a gossip query for node exchange with peers.
 *
 * @param query_id   Unique identifier
 * @param source     Source node initiating the gossip
 * @param targets    Array of target nodes to exchange with
 * @param num_targets Number of targets
 * @param timeout_ms Timeout in milliseconds
 * @return           New gossip query, or NULL on failure
 */
meridian_query_t* meridian_gossip_query_create(uint64_t query_id,
                                                meridian_node_t* source,
                                                meridian_node_t** targets,
                                                size_t num_targets,
                                                uint32_t timeout_ms);

/**
 * Creates a closest-node query to find the lowest-latency target.
 *
 * @param query_id   Unique identifier
 * @param source     Source node initiating query
 * @param targets    Array of candidate nodes
 * @param num_targets Number of targets
 * @param timeout_ms Timeout in milliseconds
 * @return           New closest query, or NULL on failure
 */
meridian_query_t* meridian_closest_query_create(uint64_t query_id,
                                                 meridian_node_t* source,
                                                 meridian_node_t** targets,
                                                 size_t num_targets,
                                                 uint32_t timeout_ms);

/**
 * Creates a single-target measurement query.
 *
 * @param query_id   Unique identifier
 * @param source     Source node
 * @param target     Single target to measure
 * @param timeout_ms Timeout in milliseconds
 * @return           New measure query, or NULL on failure
 */
meridian_query_t* meridian_measure_query_create(uint64_t query_id,
                                                  meridian_node_t* source,
                                                  meridian_node_t* target,
                                                  uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_QUERY_H