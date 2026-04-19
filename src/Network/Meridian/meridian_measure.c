//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_measure.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>

// ============================================================================
// LATENCY CACHE
// ============================================================================

/**
 * Creates a latency cache with the specified capacity.
 * The cache stores recent latency measurements to avoid redundant probes.
 *
 * @param capacity  Maximum number of entries (0 = default MERIDIAN_PROBE_CACHE_SIZE)
 * @return          New cache with refcount=1, or NULL on failure
 */
meridian_latency_cache_t* meridian_latency_cache_create(size_t capacity) {
    if (capacity == 0) capacity = MERIDIAN_PROBE_CACHE_SIZE;

    meridian_latency_cache_t* cache = (meridian_latency_cache_t*)
        get_clear_memory(sizeof(meridian_latency_cache_t));

    cache->entries = (meridian_measure_entry_t*)
        get_clear_memory(capacity * sizeof(meridian_measure_entry_t));

    cache->capacity = capacity;
    cache->count = 0;
    platform_lock_init(&cache->lock);
    refcounter_init(&cache->refcounter);

    return cache;
}

/**
 * Destroys a latency cache, freeing all memory.
 * Only frees when refcount reaches zero.
 *
 * @param cache  Cache to destroy
 */
void meridian_latency_cache_destroy(meridian_latency_cache_t* cache) {
    if (cache == NULL) return;

    refcounter_dereference(&cache->refcounter);
    if (refcounter_count(&cache->refcounter) == 0) {
        platform_lock(&cache->lock);
        if (cache->entries) {
            free(cache->entries);
            cache->entries = NULL;
        }
        platform_unlock(&cache->lock);
        platform_lock_destroy(&cache->lock);
        free(cache);
    }
}

/**
 * Inserts or updates a latency measurement for a node.
 * If the node already exists in the cache, its entry is updated.
 * If the cache is full, the oldest entry (by timestamp) is evicted.
 *
 * @param cache       Cache to insert into
 * @param node        Node whose latency was measured
 * @param latency_us  Measured latency in microseconds
 * @return            0 on success, -1 on failure
 */
int meridian_latency_cache_insert(meridian_latency_cache_t* cache,
                                   const meridian_node_t* node, uint32_t latency_us) {
    if (cache == NULL || node == NULL) return -1;

    platform_lock(&cache->lock);

    // Check if node already exists - update in place
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].addr == node->addr &&
            cache->entries[i].port == node->port) {
            gettimeofday(&cache->entries[i].timestamp, NULL);
            cache->entries[i].latency_us = latency_us;
            platform_unlock(&cache->lock);
            return 0;
        }
    }

    // Cache not full - append new entry
    if (cache->count < cache->capacity) {
        cache->entries[cache->count].addr = node->addr;
        cache->entries[cache->count].port = node->port;
        cache->entries[cache->count].latency_us = latency_us;
        gettimeofday(&cache->entries[cache->count].timestamp, NULL);
        cache->count++;
    } else {
        // Cache full - evict oldest entry
        size_t oldest_idx = 0;
        struct timeval oldest;
        gettimeofday(&oldest, NULL);

        for (size_t i = 0; i < cache->count; i++) {
            if (timercmp(&cache->entries[i].timestamp, &oldest, <)) {
                oldest = cache->entries[i].timestamp;
                oldest_idx = i;
            }
        }

        // Replace oldest with new entry
        cache->entries[oldest_idx].addr = node->addr;
        cache->entries[oldest_idx].port = node->port;
        cache->entries[oldest_idx].latency_us = latency_us;
        gettimeofday(&cache->entries[oldest_idx].timestamp, NULL);
    }

    platform_unlock(&cache->lock);
    return 0;
}

/**
 * Retrieves a cached latency measurement for a node.
 *
 * @param cache        Cache to query
 * @param node         Node to look up
 * @param latency_us   Output: latency in microseconds (only valid if return is 0)
 * @return             0 if found, -1 if not found
 */
int meridian_latency_cache_get(meridian_latency_cache_t* cache,
                                 const meridian_node_t* node, uint32_t* latency_us) {
    if (cache == NULL || node == NULL || latency_us == NULL) return -1;

    platform_lock(&cache->lock);

    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].addr == node->addr &&
            cache->entries[i].port == node->port) {
            *latency_us = cache->entries[i].latency_us;
            platform_unlock(&cache->lock);
            return 0;
        }
    }

    platform_unlock(&cache->lock);
    return -1;
}

/**
 * Evicts all expired entries from the cache.
 * Entries older than MERIDIAN_PROBE_CACHE_TIMEOUT_US are removed.
 * Uses write_idx compaction to efficiently remove stale entries.
 *
 * @param cache  Cache to evict from
 */
void meridian_latency_cache_evict_expired(meridian_latency_cache_t* cache) {
    if (cache == NULL) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    platform_lock(&cache->lock);

    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < cache->count; read_idx++) {
        struct timeval age;
        timersub(&now, &cache->entries[read_idx].timestamp, &age);

        // Keep entry if not expired
        if (age.tv_sec * 1000000 + age.tv_usec < MERIDIAN_PROBE_CACHE_TIMEOUT_US) {
            if (write_idx != read_idx) {
                cache->entries[write_idx] = cache->entries[read_idx];
            }
            write_idx++;
        }
        // Expired entries are simply skipped (not copied)
    }

    cache->count = write_idx;
    platform_unlock(&cache->lock);
}

// ============================================================================
// MEASURE REQUEST
// ============================================================================

/**
 * Creates a measure request to probe a target node's latency.
 * The request starts timing immediately and must be executed before timeout.
 *
 * @param query_id  Unique identifier for this measure operation
 * @param target    Node to measure latency to
 * @param type      Measurement type (TCP, PING, DNS)
 * @param timeout_ms Timeout in milliseconds
 * @param callback  Function called when measurement completes
 * @param ctx       User context passed to callback
 * @return          New request with refcount=1, or NULL on failure
 */
meridian_measure_request_t* meridian_measure_request_create(
    uint64_t query_id,
    meridian_node_t* target,
    meridian_measure_type_t type,
    uint32_t timeout_ms,
    meridian_measure_callback_t callback,
    void* ctx) {
    if (target == NULL) return NULL;

    meridian_measure_request_t* req = (meridian_measure_request_t*)
        get_clear_memory(sizeof(meridian_measure_request_t));

    req->query_id = query_id;
    req->target = (meridian_node_t*) refcounter_reference(&target->refcounter);
    req->type = type;
    gettimeofday(&req->start_time, NULL);
    req->timeout.tv_sec = timeout_ms / 1000;
    req->timeout.tv_usec = (timeout_ms % 1000) * 1000;
    req->callback = callback;
    req->ctx = ctx;
    req->completed = false;

    return req;
}

/**
 * Destroys a measure request, releasing the target node reference.
 *
 * @param req  Request to destroy
 */
void meridian_measure_request_destroy(meridian_measure_request_t* req) {
    if (req == NULL) return;
    if (req->target) {
        refcounter_dereference(&req->target->refcounter);
        if (refcounter_count(&req->target->refcounter) == 0) {
            free(req->target);
        }
    }
    free(req);
}

/**
 * Updates the timeout for a measure request.
 *
 * @param req        Request to update
 * @param timeout_ms New timeout in milliseconds
 * @return           0 on success, -1 on failure
 */
int meridian_measure_request_set_timeout(meridian_measure_request_t* req, uint32_t timeout_ms) {
    if (req == NULL) return -1;
    req->timeout.tv_sec = timeout_ms / 1000;
    req->timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return 0;
}

/**
 * Checks if a measure request has expired (timed out).
 *
 * @param req  Request to check
 * @return    true if expired or req is NULL, false otherwise
 */
bool meridian_measure_request_is_expired(const meridian_measure_request_t* req) {
    if (req == NULL) return true;

    struct timeval now;
    gettimeofday(&now, NULL);

    struct timeval deadline;
    timeradd(&req->start_time, &req->timeout, &deadline);

    return timercmp(&now, &deadline, >);
}

/**
 * Executes a measure request, calculating elapsed time and invoking callback.
 * This is a placeholder implementation that measures time-to-execute
 * rather than actual network latency.
 *
 * @param req  Request to execute
 * @return     0 on success, -1 on failure (already completed or NULL)
 */
int meridian_measure_request_execute(meridian_measure_request_t* req) {
    if (req == NULL || req->completed) return -1;

    meridian_measure_result_t result = {
        .node = req->target,
        .latency_us = 0,
        .success = false
    };

    struct timeval now;
    gettimeofday(&now, NULL);

    struct timeval elapsed;
    timersub(&now, &req->start_time, &elapsed);
    result.latency_us = (uint32_t)(elapsed.tv_sec * 1000000 + elapsed.tv_usec);
    result.success = true;

    req->completed = true;

    // Invoke callback with results
    if (req->callback) {
        req->callback(req->ctx, &result);
    }

    return 0;
}