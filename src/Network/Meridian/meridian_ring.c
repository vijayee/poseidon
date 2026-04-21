//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_ring.h"
#include "../../Util/allocator.h"
#include "../../Crypto/node_id.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// RING SET LIFECYCLE
// ============================================================================

/**
 * Creates a ring set with specified sizes and bucketing parameters.
 * Initializes MERIDIAN_MAX_RINGS rings, each with primary and secondary vectors.
 *
 * @param primary_size    Max nodes in each primary ring
 * @param secondary_size  Max nodes in each secondary ring
 * @param exponent_base   Logarithmic base for latency bucketing (e.g., 2 = powers of 2)
 * @return                New ring set with refcount=1, or NULL on failure
 */
meridian_ring_set_t* meridian_ring_set_create(uint32_t primary_size,
                                               uint32_t secondary_size,
                                               int32_t exponent_base) {
    meridian_ring_set_t* set = (meridian_ring_set_t*) get_clear_memory(
        sizeof(meridian_ring_set_t));
    set->primary_ring_size = primary_size;
    set->secondary_ring_size = secondary_size;
    set->exponent_base = exponent_base;

    // Initialize all rings with empty vectors and default bounds
    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        vec_init(&set->rings[i].primary);
        vec_init(&set->rings[i].secondary);
        set->rings[i].frozen = false;
        set->rings[i].latency_min_us = 0;
        set->rings[i].latency_max_us = 0;
    }

    platform_lock_init(&set->lock);
    refcounter_init(&set->refcounter);
    return set;
}

/**
 * Destroys a ring set, releasing all nodes in all rings.
 * Only frees memory when refcount reaches zero.
 *
 * @param set  Ring set to destroy
 */
void meridian_ring_set_destroy(meridian_ring_set_t* set) {
    if (set == NULL) return;
    refcounter_dereference(&set->refcounter);
    if (refcounter_count(&set->refcounter) == 0) {
        for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
            vec_deinit(&set->rings[i].primary);
            vec_deinit(&set->rings[i].secondary);
        }
        platform_lock_destroy(&set->lock);
        free(set);
    }
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

/**
 * Compares two nodes by address for sorting purposes.
 * Used internally for maintaining sorted node arrays.
 */
static int ring_latency_cmp(const void* a, const void* b) {
    const meridian_node_t* node_a = *(const meridian_node_t* const*) a;
    const meridian_node_t* node_b = *(const meridian_node_t* const*) b;
    if (node_a->addr < node_b->addr) return -1;
    if (node_a->addr > node_b->addr) return 1;
    return 0;
}

static bool node_identity_match(const meridian_node_t* a, const meridian_node_t* b) {
    if (!poseidon_node_id_is_null(&a->id) && !poseidon_node_id_is_null(&b->id)) {
        return meridian_node_equals_by_id(a, b);
    }
    return meridian_node_equals_by_addr(a, b);
}

// ============================================================================
// NODE INSERTION AND REMOVAL
// ============================================================================

/**
 * Inserts a node into the appropriate ring based on measured latency.
 * Uses logarithmic bucketing: ring = floor(log(latency) / log(base))
 *
 * Nodes fill the primary ring first. When primary is full, new nodes
 * go to secondary. This creates replacement candidates for when primary
 * nodes leave.
 *
 * @param set        Ring set to insert into
 * @param node       Node to insert (ownership stays with caller)
 * @param latency_us  Measured latency in microseconds
 * @param rendv      Rendezvous point for the node (may be NULL)
 * @return           0 on success, -1 on failure (ring full, frozen, or invalid)
 */
int meridian_ring_set_insert(meridian_ring_set_t* set, meridian_node_t* node,
                             uint32_t latency_us, meridian_node_t* rendv) {
    (void)rendv; // Reserved for future proximity-aware insertion
    if (set == NULL || node == NULL) return -1;

    // Determine which ring this latency belongs to
    int ring_num = meridian_ring_set_get_ring(set, latency_us);
    if (ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) return -1;

    meridian_ring_t* ring = &set->rings[ring_num];
    if (ring->frozen) return -1;  // Frozen rings reject new insertions

    // Fill primary first, then secondary
    if (ring->primary.length < (int) set->primary_ring_size) {
        vec_push(&ring->primary, node);
    } else if (ring->secondary.length < (int) set->secondary_ring_size) {
        vec_push(&ring->secondary, node);
    } else {
        return -1;  // Both rings full
    }

    return 0;
}

/**
 * Removes a node from all rings in the set.
 * Searches both primary and secondary rings for all rings.
 *
 * @param set   Ring set to remove from
 * @param node  Node to remove
 * @return      0 if found and removed, -1 if not found
 */
int meridian_ring_set_erase(meridian_ring_set_t* set, meridian_node_t* node) {
    if (set == NULL || node == NULL) return -1;

    // Scan all rings looking for the node
    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        int idx = -1;
        for (int j = 0; j < set->rings[i].primary.length; j++) {
            meridian_node_t* n = set->rings[i].primary.data[j];
            if (node_identity_match(n, node)) {
                idx = j;
                break;
            }
        }
        if (idx >= 0) {
            vec_splice(&set->rings[i].primary, idx, 1);
            return 0;
        }

        // Check secondary ring
        idx = -1;
        for (int j = 0; j < set->rings[i].secondary.length; j++) {
            meridian_node_t* n = set->rings[i].secondary.data[j];
            if (node_identity_match(n, node)) {
                idx = j;
                break;
            }
        }
        if (idx >= 0) {
            vec_splice(&set->rings[i].secondary, idx, 1);
            return 0;
        }
    }
    return -1;
}

/**
 * Removes a node from a specific ring only.
 * More efficient than erase() when you know the ring.
 *
 * @param set      Ring set to remove from
 * @param node     Node to remove
 * @param ring_num Specific ring to remove from
 * @return         0 if found and removed, -1 if not found
 */
int meridian_ring_set_erase_ring(meridian_ring_set_t* set, meridian_node_t* node,
                                  int ring_num) {
    if (set == NULL || node == NULL || ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) {
        return -1;
    }

    int idx = -1;
    for (int j = 0; j < set->rings[ring_num].primary.length; j++) {
        meridian_node_t* n = set->rings[ring_num].primary.data[j];
        if (node_identity_match(n, node)) {
            idx = j;
            break;
        }
    }
    if (idx >= 0) {
        vec_splice(&set->rings[ring_num].primary, idx, 1);
        return 0;
    }

    idx = -1;
    for (int j = 0; j < set->rings[ring_num].secondary.length; j++) {
        meridian_node_t* n = set->rings[ring_num].secondary.data[j];
        if (node_identity_match(n, node)) {
            idx = j;
            break;
        }
    }
    if (idx >= 0) {
        vec_splice(&set->rings[ring_num].secondary, idx, 1);
        return 0;
    }
    return -1;
}

// ============================================================================
// LATENCY BUCKETING
// ============================================================================

/**
 * Determines which ring a latency value belongs to using logarithmic bucketing.
 * Formula: ring = floor(log(latency) / log(base))
 *
 * With base=2:
 *   1μs   → ring 0
 *   2-3μs → ring 1
 *   4-7μs → ring 2
 *   8-15μs → ring 3
 *   etc.
 *
 * @param set        Ring set containing the exponent_base
 * @param latency_us Latency to classify
 * @return           Ring number (0 to MERIDIAN_MAX_RINGS-1)
 */
int meridian_ring_set_get_ring(meridian_ring_set_t* set, uint32_t latency_us) {
    if (set == NULL || set->exponent_base <= 0) return 0;

    int ring = (int) floor(log((double) latency_us) / log((double) set->exponent_base));
    if (ring < 0) ring = 0;
    if (ring >= MERIDIAN_MAX_RINGS) ring = MERIDIAN_MAX_RINGS - 1;
    return ring;
}

// ============================================================================
// CLOSEST NODE LOOKUP
// ============================================================================

/**
 * Finds the closest node by scanning rings from lowest to highest latency.
 * Returns the first primary node found in the lowest non-empty ring.
 * This is O(MERIDIAN_MAX_RINGS) and reflects actual latency ordering.
 *
 * @param set         Ring set to search
 * @param target_addr Unused (reserved for future tiebreaking)
 * @param target_port Unused (reserved for future tiebreaking)
 * @return            Lowest-latency node found, or NULL if no primary nodes exist
 */
meridian_node_t* meridian_ring_set_find_closest(meridian_ring_set_t* set,
                                                 uint32_t target_addr,
                                                 uint16_t target_port) {
    (void)target_addr;
    (void)target_port;
    if (set == NULL) return NULL;

    for (int i = 0; i < MERIDIAN_MAX_RINGS; i++) {
        if (set->rings[i].primary.length > 0) {
            return set->rings[i].primary.data[0];
        }
    }
    return NULL;
}

// ============================================================================
// RING FREEZE/UNFREEZE
// ============================================================================

/**
 * Freezes a ring, preventing new insertions.
 * Used during ring maintenance or when a ring is being replaced.
 *
 * @param set      Ring set containing the ring
 * @param ring_num Ring to freeze
 */
void meridian_ring_set_freeze(meridian_ring_set_t* set, int ring_num) {
    if (set == NULL || ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) return;
    set->rings[ring_num].frozen = true;
}

/**
 * Unfreezes a ring, allowing new insertions again.
 *
 * @param set      Ring set containing the ring
 * @param ring_num Ring to unfreeze
 */
void meridian_ring_set_unfreeze(meridian_ring_set_t* set, int ring_num) {
    if (set == NULL || ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) return;
    set->rings[ring_num].frozen = false;
}

// ============================================================================
// REPLACEMENT ELIGIBILITY
// ============================================================================

/**
 * Determines if a ring has capacity to accept replacement candidates.
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
bool meridian_ring_set_eligible_for_replacement(meridian_ring_set_t* set, int ring_num) {
    if (set == NULL || ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) return false;

    meridian_ring_t* ring = &set->rings[ring_num];
    if (ring->frozen) return false;
    if ((int) ring->primary.length != (int) set->primary_ring_size) return false;
    if (ring->secondary.length == 0) return false;

    // Count non-rendezvous nodes (candidates for promotion)
    int eligible_count = 0;
    for (int i = 0; i < ring->primary.length; i++) {
        if (!(ring->primary.data[i]->flags & MERIDIAN_NODE_FLAG_RENDEZVOUS)) {
            eligible_count++;
        }
    }
    for (int i = 0; i < ring->secondary.length; i++) {
        if (!(ring->secondary.data[i]->flags & MERIDIAN_NODE_FLAG_RENDEZVOUS)) {
            eligible_count++;
        }
    }

    // Eligible only if we have surplus candidates
    return eligible_count > (int) set->primary_ring_size;
}

int meridian_ring_set_promote_secondary(meridian_ring_set_t* set, int ring_num) {
    if (set == NULL || ring_num < 0 || ring_num >= MERIDIAN_MAX_RINGS) return -1;

    meridian_ring_t* ring = &set->rings[ring_num];
    if (ring->secondary.length == 0) return -1;
    if (ring->primary.length >= set->primary_ring_size) return -1;

    // Take first node from secondary
    meridian_node_t* to_promote = ring->secondary.data[0];
    vec_splice(&ring->secondary, 0, 1);

    // Add to primary
    vec_push(&ring->primary, to_promote);

    return 0;
}