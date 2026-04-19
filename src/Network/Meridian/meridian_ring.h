//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_RING_H
#define POSEIDON_MERIDIAN_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "meridian.h"
#include "../../Util/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// RING STRUCTURE
// ============================================================================

/**
 * A single latency ring containing primary and secondary node lists.
 *
 * Primary ring holds preferred nodes for routing.
 * Secondary ring holds candidates for replacement.
 *
 * Latency bucketing: nodes are placed in rings based on latency using
 * logarithmic bucketing: ring = floor(log(latency) / log(base))
 *
 * For example, with base=2:
 *   1μs -> ring 0, 2-3μs -> ring 1, 4-7μs -> ring 2, etc.
 */
typedef struct meridian_ring_t {
    vec_t(meridian_node_t*) primary;   /**< Active nodes (replacement candidates) */
    vec_t(meridian_node_t*) secondary; /**< Candidates waiting for promotion */
    bool frozen;                       /**< If true, reject new insertions */
    uint32_t latency_min_us;           /**< Minimum latency for this ring (inclusive) */
    uint32_t latency_max_us;           /**< Maximum latency for this ring (exclusive) */
} meridian_ring_t;

// ============================================================================
// RING SET
// ============================================================================

/**
 * Collection of latency rings organized by latency buckets.
 * Supports O(log M) ring selection where M is max latency.
 *
 * Thread-safe: all operations are protected by a lock.
 */
typedef struct meridian_ring_set_t {
    refcounter_t refcounter;           /**< Reference counting for lifetime */
    uint32_t primary_ring_size;        /**< Max nodes in each primary ring */
    uint32_t secondary_ring_size;       /**< Max nodes in each secondary ring */
    int32_t exponent_base;             /**< Logarithmic base for latency bucketing */
    meridian_ring_t rings[MERIDIAN_MAX_RINGS]; /**< Array of latency rings */
    PLATFORMLOCKTYPE(lock);          /**< Thread-safe access */
} meridian_ring_set_t;

// ============================================================================
// RING SET LIFECYCLE
// ============================================================================

/**
 * Creates a ring set with specified sizes and bucketing parameters.
 *
 * @param primary_size    Max nodes in each primary ring
 * @param secondary_size  Max nodes in each secondary ring
 * @param exponent_base   Logarithmic base for latency bucketing (e.g., 2 = powers of 2)
 * @return                New ring set with refcount=1, or NULL on failure
 */
meridian_ring_set_t* meridian_ring_set_create(uint32_t primary_size,
                                               uint32_t secondary_size,
                                               int32_t exponent_base);

/**
 * Destroys a ring set, releasing all nodes in all rings.
 * Only frees memory when refcount reaches zero.
 *
 * @param set  Ring set to destroy
 */
void meridian_ring_set_destroy(meridian_ring_set_t* set);

// ============================================================================
// NODE INSERTION AND REMOVAL
// ============================================================================

/**
 * Inserts a node into the appropriate ring based on measured latency.
 *
 * Uses logarithmic bucketing: ring = floor(log(latency) / log(base))
 * Primary ring fills first, then secondary when primary is full.
 *
 * @param set        Ring set to insert into
 * @param node       Node to insert (ownership stays with caller)
 * @param latency_us Measured latency in microseconds
 * @param rendv      Rendezvous point for the node (may be NULL)
 * @return           0 on success, -1 on failure (ring full or frozen)
 */
int meridian_ring_set_insert(meridian_ring_set_t* set, meridian_node_t* node,
                             uint32_t latency_us, meridian_node_t* rendv);

/**
 * Removes a node from all rings in the set.
 * Searches both primary and secondary rings.
 *
 * @param set   Ring set to remove from
 * @param node  Node to remove
 * @return      0 if found and removed, -1 if not found
 */
int meridian_ring_set_erase(meridian_ring_set_t* set, meridian_node_t* node);

/**
 * Removes a node from a specific ring only.
 * More efficient than erase() when ring is known.
 *
 * @param set      Ring set to remove from
 * @param node     Node to remove
 * @param ring_num Specific ring to remove from
 * @return         0 if found and removed, -1 if not found
 */
int meridian_ring_set_erase_ring(meridian_ring_set_t* set, meridian_node_t* node,
                                  int ring_num);

// ============================================================================
// LATENCY BUCKETING
// ============================================================================

/**
 * Determines which ring a latency value belongs to.
 *
 * Formula: ring = floor(log(latency) / log(base))
 *
 * With base=2:
 *   1μs -> ring 0
 *   2-3μs -> ring 1
 *   4-7μs -> ring 2
 *   8-15μs -> ring 3
 *   etc.
 *
 * @param set        Ring set containing the exponent_base
 * @param latency_us Latency to classify
 * @return           Ring number (0 to MERIDIAN_MAX_RINGS-1)
 */
int meridian_ring_set_get_ring(meridian_ring_set_t* set, uint32_t latency_us);

// ============================================================================
// CLOSEST NODE LOOKUP
// ============================================================================

/**
 * Finds the closest node to a target address/port across all rings.
 *
 * Uses a heuristic to find nodes between current best and target.
 * Note: This is a placeholder; full impl would use actual latency data.
 *
 * @param set         Ring set to search
 * @param target_addr Target IPv4 address
 * @param target_port Target port
 * @return            Closest node found, or NULL if none exist
 */
meridian_node_t* meridian_ring_set_find_closest(meridian_ring_set_t* set,
                                                 uint32_t target_addr,
                                                 uint16_t target_port);

// ============================================================================
// RING FREEZE/UNFREEZE
// ============================================================================

/**
 * Freezes a ring, preventing new insertions.
 * Used during ring maintenance or when replacing nodes.
 *
 * @param set      Ring set containing the ring
 * @param ring_num Ring to freeze
 */
void meridian_ring_set_freeze(meridian_ring_set_t* set, int ring_num);

/**
 * Unfreezes a ring, allowing new insertions again.
 *
 * @param set      Ring set containing the ring
 * @param ring_num Ring to unfreeze
 */
void meridian_ring_set_unfreeze(meridian_ring_set_t* set, int ring_num);

// ============================================================================
// REPLACEMENT ELIGIBILITY
// ============================================================================

/**
 * Determines if a ring has capacity to accept replacement candidates.
 *
 * A ring is eligible when:
 *   1. Not frozen
 *   2. Primary ring is full
 *   3. Secondary ring has candidates
 *   4. More than primary_ring_size non-rendezvous nodes exist
 *
 * This supports the replacement algorithm where secondary nodes
 * get promoted to primary when primary nodes leave.
 *
 * @param set      Ring set
 * @param ring_num Ring to check
 * @return         true if eligible for replacement
 */
bool meridian_ring_set_eligible_for_replacement(meridian_ring_set_t* set, int ring_num);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_RING_H