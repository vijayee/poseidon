//
// Created by victor on 4/19/26.
//

#include "meridian_query.h"
#include "../../Util/allocator.h"
#include "../../Util/threadding.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// ============================================================================
// QUERY LIFECYCLE
// ============================================================================

/**
 * Creates a new query with the given parameters.
 * Initializes timeout based on current time + timeout_ms.
 *
 * @param query_id   Unique identifier for this query
 * @param type       Query type (GOSSIP, CLOSEST, MEASURE)
 * @param timeout_ms Timeout in milliseconds
 * @return           New query with refcount=1, or NULL on failure
 */
meridian_query_t* meridian_query_create(uint64_t query_id,
                                        meridian_query_type_t type,
                                        uint32_t timeout_ms) {
    meridian_query_t* query = (meridian_query_t*)
        get_clear_memory(sizeof(meridian_query_t));

    query->query_id = query_id;
    query->type = type;
    query->status = MERIDIAN_QUERY_STATUS_INIT;
    gettimeofday(&query->start_time, NULL);
    query->timeout.tv_sec = timeout_ms / 1000;
    query->timeout.tv_usec = (timeout_ms % 1000) * 1000;
    query->source = NULL;
    query->num_targets = 0;
    query->num_measured = 0;
    query->user_ctx = NULL;

    refcounter_init(&query->refcounter);
    return query;
}

/**
 * Destroys a query, releasing all references.
 * Only frees memory when refcount reaches zero.
 * Destroys source node and all target nodes if owned.
 *
 * @param query  Query to destroy
 */
void meridian_query_destroy(meridian_query_t* query) {
    if (query == NULL) return;

    refcounter_dereference(&query->refcounter);
    if (refcounter_count(&query->refcounter) == 0) {
        // Release source node
        if (query->source) {
            refcounter_dereference(&query->source->refcounter);
            if (refcounter_count(&query->source->refcounter) == 0) {
                free(query->source);
            }
        }
        // Release all target nodes
        for (size_t i = 0; i < query->num_targets; i++) {
            if (query->targets[i]) {
                refcounter_dereference(&query->targets[i]->refcounter);
                if (refcounter_count(&query->targets[i]->refcounter) == 0) {
                    free(query->targets[i]);
                }
            }
        }
        free(query);
    }
}

/**
 * Adds a target node to the query.
 * Takes a reference to the target node.
 *
 * @param query  Query to add target to
 * @param node   Target node to add
 * @return       0 on success, -1 on failure
 */
int meridian_query_add_target(meridian_query_t* query, meridian_node_t* node) {
    if (query == NULL || node == NULL) return -1;
    if (query->num_targets >= MERIDIAN_QUERY_MAX_TARGETS) return -1;

    query->targets[query->num_targets++] = (meridian_node_t*)
        refcounter_reference(&node->refcounter);
    return 0;
}

// ============================================================================
// LATENCY MEASUREMENT
// ============================================================================

/**
 * Records a latency measurement for a target by index.
 *
 * @param query       Query to record measurement for
 * @param target_idx  Index of target (0-based)
 * @param latency     Measured latency in microseconds
 * @return            0 on success, -1 on failure
 */
int meridian_query_set_latency(meridian_query_t* query, size_t target_idx, uint32_t latency) {
    if (query == NULL) return -1;
    if (target_idx >= query->num_targets) return -1;

    query->latencies[target_idx] = latency;
    query->num_measured++;
    return 0;
}

/**
 * Finds the target with the lowest latency.
 * Scans all measured latencies and returns the closest target.
 *
 * @param query  Query to search
 * @return       Closest target node, or NULL if no targets
 */
meridian_node_t* meridian_query_get_closest(meridian_query_t* query) {
    if (query == NULL || query->num_targets == 0) return NULL;

    size_t closest_idx = 0;
    uint32_t closest_latency = query->latencies[0];

    for (size_t i = 1; i < query->num_targets; i++) {
        if (query->latencies[i] < closest_latency) {
            closest_latency = query->latencies[i];
            closest_idx = i;
        }
    }

    return query->targets[closest_idx];
}

// ============================================================================
// QUERY EXPIRATION
// ============================================================================

/**
 * Checks if a query has expired based on its timeout.
 *
 * @param query  Query to check
 * @return       true if expired or query is NULL, false otherwise
 */
bool meridian_query_is_expired(const meridian_query_t* query) {
    if (query == NULL) return true;

    struct timeval now;
    gettimeofday(&now, NULL);

    struct timeval deadline;
    timeradd(&query->start_time, &query->timeout, &deadline);

    return timercmp(&now, &deadline, >);
}

/**
 * Checks if a query has finished (completed or failed).
 *
 * @param query  Query to check
 * @return       true if finished or query is NULL, false otherwise
 */
bool meridian_query_is_finished(const meridian_query_t* query) {
    if (query == NULL) return true;
    return query->status == MERIDIAN_QUERY_STATUS_FINISHED ||
           query->status == MERIDIAN_QUERY_STATUS_FAILED;
}

/**
 * Marks a query as finished successfully.
 *
 * @param query  Query to finish
 * @return       0 on success, -1 on failure
 */
int meridian_query_finish(meridian_query_t* query) {
    if (query == NULL) return -1;
    query->status = MERIDIAN_QUERY_STATUS_FINISHED;
    return 0;
}

/**
 * Gets the user context from a query.
 *
 * @param query  Query to get context from
 * @return       User context pointer, or NULL if query is NULL
 */
void* meridian_query_get_ctx(meridian_query_t* query) {
    return query ? query->user_ctx : NULL;
}

// ============================================================================
// QUERY TABLE
// ============================================================================

/**
 * Creates a query table for tracking in-flight queries.
 * The table manages its own memory and provides thread-safe access.
 *
 * @param capacity  Initial capacity (0 = default 64)
 * @return          New table with refcount=1, or NULL on failure
 */
meridian_query_table_t* meridian_query_table_create(size_t capacity) {
    if (capacity == 0) capacity = 64;

    meridian_query_table_t* table = (meridian_query_table_t*)
        get_clear_memory(sizeof(meridian_query_table_t));

    table->queries = (meridian_query_t**) get_clear_memory(
        capacity * sizeof(meridian_query_t*));

    table->capacity = capacity;
    table->count = 0;
    platform_lock_init(&table->lock);
    refcounter_init(&table->refcounter);

    return table;
}

/**
 * Destroys a query table, freeing all tracked queries.
 * Only frees memory when refcount reaches zero.
 *
 * @param table  Table to destroy
 */
void meridian_query_table_destroy(meridian_query_table_t* table) {
    if (table == NULL) return;

    refcounter_dereference(&table->refcounter);
    if (refcounter_count(&table->refcounter) == 0) {
        for (size_t i = 0; i < table->count; i++) {
            if (table->queries[i]) {
                meridian_query_destroy(table->queries[i]);
            }
        }
        free(table->queries);
        platform_lock_destroy(&table->lock);
        free(table);
    }
}

/**
 * Inserts a query into the table.
 * Takes a reference to the query. Table grows automatically if needed.
 *
 * @param table  Table to insert into
 * @param query  Query to insert
 * @return       0 on success, -1 on failure
 */
int meridian_query_table_insert(meridian_query_table_t* table, meridian_query_t* query) {
    if (table == NULL || query == NULL) return -1;

    platform_lock(&table->lock);

    // Grow table if needed (double capacity)
    if (table->count >= table->capacity) {
        size_t new_capacity = table->capacity * 2;
        meridian_query_t** new_queries = (meridian_query_t**)
            get_clear_memory(new_capacity * sizeof(meridian_query_t*));

        if (new_queries == NULL) {
            platform_unlock(&table->lock);
            return -1;
        }

        memcpy(new_queries, table->queries, table->count * sizeof(meridian_query_t*));
        free(table->queries);
        table->queries = new_queries;
        table->capacity = new_capacity;
    }

    // Add query with reference
    table->queries[table->count++] = (meridian_query_t*)
        refcounter_reference(&query->refcounter);

    platform_unlock(&table->lock);
    return 0;
}

/**
 * Removes a query from the table by query_id.
 * The query is destroyed using meridian_query_destroy().
 *
 * @param table     Table to remove from
 * @param query_id  ID of query to remove
 * @return          0 if found and removed, -1 if not found
 */
int meridian_query_table_remove(meridian_query_table_t* table, uint64_t query_id) {
    if (table == NULL) return -1;

    platform_lock(&table->lock);

    for (size_t i = 0; i < table->count; i++) {
        if (table->queries[i]->query_id == query_id) {
            // Destroy the query (releases table's reference)
            meridian_query_destroy(table->queries[i]);

            // Compact array (remove gap at index i)
            for (size_t j = i; j < table->count - 1; j++) {
                table->queries[j] = table->queries[j + 1];
            }
            table->count--;
            platform_unlock(&table->lock);
            return 0;
        }
    }

    platform_unlock(&table->lock);
    return -1;
}

/**
 * Looks up a query by ID.
 * Returns a new reference that caller must eventually destroy.
 *
 * @param table     Table to search
 * @param query_id  ID to find
 * @return          Query with new reference, or NULL if not found
 */
meridian_query_t* meridian_query_table_lookup(meridian_query_table_t* table, uint64_t query_id) {
    if (table == NULL) return NULL;

    platform_lock(&table->lock);

    for (size_t i = 0; i < table->count; i++) {
        if (table->queries[i]->query_id == query_id) {
            platform_unlock(&table->lock);
            return (meridian_query_t*) refcounter_reference(&table->queries[i]->refcounter);
        }
    }

    platform_unlock(&table->lock);
    return NULL;
}

/**
 * Ticks the table, finding and expiring timed-out queries.
 * Expired queries are transferred to caller ownership via CONSUME pattern.
 *
 * Algorithm:
 * 1. Count expired queries
 * 2. Allocate output array
 * 3. Partition table: non-expired → front, expired → caller via CONSUME
 * 4. Update count to new size
 *
 * @param table        Table to tick
 * @param expired      Output: array of expired queries (caller owns them)
 * @param num_expired  Output: number of expired queries
 * @return             0 on success, -1 on failure
 */
int meridian_query_table_tick(meridian_query_table_t* table,
                               meridian_query_t*** expired,
                               size_t* num_expired) {
    if (table == NULL || expired == NULL || num_expired == NULL) return -1;

    *expired = NULL;
    *num_expired = 0;

    platform_lock(&table->lock);

    // Phase 1: Count expired queries
    size_t count = 0;
    for (size_t i = 0; i < table->count; i++) {
        if (meridian_query_is_expired(table->queries[i])) {
            count++;
        }
    }

    if (count == 0) {
        platform_unlock(&table->lock);
        return 0;
    }

    // Phase 2: Allocate output array
    *expired = (meridian_query_t**) get_clear_memory(count * sizeof(meridian_query_t*));
    if (*expired == NULL) {
        platform_unlock(&table->lock);
        return -1;
    }

    // Phase 3: Partition - separate expired from non-expired
    // Non-expired compact to front (write_idx), expired go to caller via CONSUME
    size_t idx = 0;
    size_t write_idx = 0;
    for (size_t i = 0; i < table->count; i++) {
        if (meridian_query_is_expired(table->queries[i])) {
            // CONSUME transfers table's reference to caller
            (*expired)[idx++] = CONSUME(table->queries[i], meridian_query_t);
            // Note: slot at [i] is now NULL but will be overwritten during compaction
        } else {
            table->queries[write_idx++] = table->queries[i];
        }
    }
    table->count = write_idx;  // Truncate to only non-expired

    platform_unlock(&table->lock);
    *num_expired = idx;
    return 0;
}

// ============================================================================
// SPECIALIZED QUERY FACTORIES
// ============================================================================

/**
 * Creates a gossip query for node exchange.
 *
 * @param query_id   Unique identifier
 * @param source     Source node for the gossip
 * @param targets    Array of target nodes
 * @param num_targets Number of targets
 * @param timeout_ms Timeout in milliseconds
 * @return           New gossip query, or NULL on failure
 */
meridian_query_t* meridian_gossip_query_create(uint64_t query_id,
                                                meridian_node_t* source,
                                                meridian_node_t** targets,
                                                size_t num_targets,
                                                uint32_t timeout_ms) {
    meridian_query_t* query = meridian_query_create(
        query_id, MERIDIAN_QUERY_TYPE_GOSSIP, timeout_ms);
    if (query == NULL) return NULL;

    if (source) {
        query->source = (meridian_node_t*) refcounter_reference(&source->refcounter);
    }

    for (size_t i = 0; i < num_targets && i < MERIDIAN_QUERY_MAX_TARGETS; i++) {
        meridian_query_add_target(query, targets[i]);
    }

    return query;
}

/**
 * Creates a closest-node query to find lowest-latency target.
 *
 * @param query_id   Unique identifier
 * @param source     Source node initiating query
 * @param targets    Array of candidates
 * @param num_targets Number of targets
 * @param timeout_ms Timeout in milliseconds
 * @return           New closest query, or NULL on failure
 */
meridian_query_t* meridian_closest_query_create(uint64_t query_id,
                                                 meridian_node_t* source,
                                                 meridian_node_t** targets,
                                                 size_t num_targets,
                                                 uint32_t timeout_ms) {
    meridian_query_t* query = meridian_query_create(
        query_id, MERIDIAN_QUERY_TYPE_CLOSEST, timeout_ms);
    if (query == NULL) return NULL;

    if (source) {
        query->source = (meridian_node_t*) refcounter_reference(&source->refcounter);
    }

    for (size_t i = 0; i < num_targets && i < MERIDIAN_QUERY_MAX_TARGETS; i++) {
        meridian_query_add_target(query, targets[i]);
    }

    return query;
}

/**
 * Creates a single-target measure query.
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
                                                  uint32_t timeout_ms) {
    meridian_query_t* query = meridian_query_create(
        query_id, MERIDIAN_QUERY_TYPE_MEASURE, timeout_ms);
    if (query == NULL) return NULL;

    if (source) {
        query->source = (meridian_node_t*) refcounter_reference(&source->refcounter);
    }

    if (target) {
        meridian_query_add_target(query, target);
    }

    return query;
}