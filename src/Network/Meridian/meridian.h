//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_MERIDIAN_H
#define POSEIDON_MERIDIAN_MERIDIAN_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "../../RefCounter/refcounter.h"
#include "../../Util/log.h"
#include "../../Workers/pool.h"
#include "../../Time/wheel.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

/** Maximum number of latency rings in the ring set */
#define MERIDIAN_MAX_RINGS 10

/** Maximum payload size in bytes (MTU consideration) */
#define MERIDIAN_MAX_PAYLOAD_SIZE 1400

// ============================================================================
// NODE FLAGS
// ============================================================================

/**
 * Flags for a meridian node.
 * Indicates special properties or roles of the node.
 */
typedef enum {
    MERIDIAN_NODE_FLAG_NONE = 0,           /**< Regular node */
    MERIDIAN_NODE_FLAG_RENDEZVOUS = (1 << 0) /**< Node is a rendezvous point */
} meridian_node_flags_t;

// ============================================================================
// MERIDIAN NODE
// ============================================================================

/**
 * Represents a peer node in the Meridian network.
 * Contains network address information and optional rendezvous point.
 *
 * Lifecycle:
 * 1. Create via meridian_node_create() or meridian_node_create_rendv()
 * 2. Reference when adding to queries, rings, or tables
 * 3. Release via meridian_node_destroy() when no longer needed
 */
typedef struct meridian_node_t {
    refcounter_t refcounter;  /**< Reference counting for lifetime management */
    uint32_t addr;            /**< IPv4 address in network byte order */
    uint16_t port;           /**< Port number in network byte order */
    uint32_t rendv_addr;     /**< Rendezvous point address (for NAT traversal) */
    uint16_t rendv_port;      /**< Rendezvous point port (for NAT traversal) */
    meridian_node_flags_t flags; /**< Node properties (e.g., RENDEZVOUS) */
} meridian_node_t;

// ============================================================================
// MERIDIAN CONFIGURATION
// ============================================================================

/**
 * Configuration for a Meridian protocol instance.
 * Passed to meridian_protocol_create() to configure behavior.
 */
typedef struct meridian_config_t {
    uint16_t meridian_port;              /**< Port for Meridian protocol traffic */
    uint16_t info_port;                 /**< Port for info/status messages */
    uint32_t primary_ring_size;         /**< Max nodes per primary ring */
    uint32_t secondary_ring_size;       /**< Max nodes per secondary ring */
    int32_t ring_exponent_base;         /**< Logarithmic base for latency bucketing */
    uint32_t init_gossip_interval_s;     /**< Gossip interval during initialization */
    uint32_t num_init_gossip_intervals;  /**< Number of init intervals before steady-state */
    uint32_t steady_state_gossip_interval_s; /**< Gossip interval during steady-state */
    uint32_t replace_interval_s;         /**< Interval for ring replacement checks */
} meridian_config_t;

// ============================================================================
// NODE LIFECYCLE
// ============================================================================

/**
 * Creates a new meridian node with the given address and port.
 * Rendezvous fields are initialized to zero (no rendezvous point).
 *
 * @param addr  IPv4 address in network byte order
 * @param port  Port number in network byte order
 * @return      New node with refcount=1, or NULL on allocation failure
 */
meridian_node_t* meridian_node_create(uint32_t addr, uint16_t port);

/**
 * Creates a new meridian node that acts as a rendezvous point for NAT traversal.
 * The node stores its own address/port plus the rendezvous point it's associated with.
 *
 * @param addr         Node's IPv4 address
 * @param port         Node's port
 * @param rendv_addr   Rendezvous server address
 * @param rendv_port   Rendezvous server port
 * @return             New rendezvous node with refcount=1
 */
meridian_node_t* meridian_node_create_rendv(uint32_t addr, uint16_t port,
                                            uint32_t rendv_addr, uint16_t rendv_port);

/**
 * Releases a node's reference. If this was the last reference, the node
 * memory is freed. Follows standard reference-counting destroy pattern.
 *
 * @param node  Node to release (may be NULL)
 */
void meridian_node_destroy(meridian_node_t* node);

/**
 * Compares two nodes by address:port for sorting/lookup.
 * Used as a comparator for qsort/bsearch on node arrays.
 *
 * @param a  Pointer to first node pointer
 * @param b  Pointer to second node pointer
 * @return   -1/0/1 for less/equal/greater
 */
int meridian_node_latency_cmp(const void* a, const void* b);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_MERIDIAN_H