//
// Created by victor on 4/19/26.
//

#include "meridian.h"
#include "../../Util/allocator.h"
#include <string.h>

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
meridian_node_t* meridian_node_create(uint32_t addr, uint16_t port) {
    meridian_node_t* node = (meridian_node_t*) get_clear_memory(sizeof(meridian_node_t));
    node->addr = addr;
    node->port = port;
    node->rendv_addr = 0;
    node->rendv_port = 0;
    node->flags = MERIDIAN_NODE_FLAG_NONE;
    refcounter_init(&node->refcounter);
    return node;
}

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
                                            uint32_t rendv_addr, uint16_t rendv_port) {
    meridian_node_t* node = (meridian_node_t*) get_clear_memory(sizeof(meridian_node_t));
    node->addr = addr;
    node->port = port;
    node->rendv_addr = rendv_addr;
    node->rendv_port = rendv_port;
    node->flags = MERIDIAN_NODE_FLAG_RENDEZVOUS;
    refcounter_init(&node->refcounter);
    return node;
}

/**
 * Releases a node's reference. If this was the last reference, the node
 * memory is freed. This follows the standard reference-counting destroy pattern.
 *
 * @param node  Node to release (may be NULL)
 */
void meridian_node_destroy(meridian_node_t* node) {
    if (node == NULL) return;
    refcounter_dereference(&node->refcounter);
    if (refcounter_count(&node->refcounter) == 0) {
        free(node);
    }
}

// ============================================================================
// COMPARISON
// ============================================================================

/**
 * Compares two nodes by address:port for sorting/lookup.
 * Used as a comparator for qsort/bsearch on node arrays.
 *
 * @param a  Pointer to first node pointer
 * @param b  Pointer to second node pointer
 * @return   -1/0/1 for less/equal/greater
 */
int meridian_node_latency_cmp(const void* a, const void* b) {
    const meridian_node_t* node_a = *(const meridian_node_t**) a;
    const meridian_node_t* node_b = *(const meridian_node_t**) b;
    if (node_a->addr < node_b->addr) return -1;
    if (node_a->addr > node_b->addr) return 1;
    if (node_a->port < node_b->port) return -1;
    if (node_a->port > node_b->port) return 1;
    return 0;
}