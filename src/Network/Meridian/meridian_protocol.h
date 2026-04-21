//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_PROTOCOL_H
#define POSEIDON_MERIDIAN_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../../Util/threadding.h"
#include "meridian.h"
#include "meridian_ring.h"
#include "meridian_query.h"
#include "meridian_gossip.h"
#include "meridian_rendv.h"
#include "meridian_measure.h"
#include "meridian_packet.h"
#include "meridian_conn.h"
#include "../../Workers/pool.h"
#include "../../Time/wheel.h"
#include "msquic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct meridian_protocol_t meridian_protocol_t;

// ============================================================================
// PROTOCOL STATES
// ============================================================================

/**
 * Lifecycle states for the Meridian protocol instance.
 * State transitions: INIT -> BOOTSTRAPPING -> RUNNING -> SHUTTING_DOWN
 */
typedef enum {
    MERIDIAN_PROTOCOL_STATE_INIT,         /**< Created, not yet started */
    MERIDIAN_PROTOCOL_STATE_BOOTSTRAPPING, /**< Connecting to seed nodes */
    MERIDIAN_PROTOCOL_STATE_RUNNING,      /**< Normal operation */
    MERIDIAN_PROTOCOL_STATE_SHUTTING_DOWN /**< Cleaning up */
} meridian_protocol_state_t;

// ============================================================================
// PROTOCOL CALLBACKS (must be before meridian_protocol_t)
// ============================================================================

/** Callback type for protocol events */
typedef void (*meridian_protocol_event_cb_t)(void* ctx,
                                             meridian_protocol_t* protocol,
                                             const uint8_t* data, size_t len);

/**
 * Callbacks structure for protocol events.
 * Allows embedding application logic into the protocol.
 */
typedef struct meridian_protocol_callbacks_t {
    void* user_ctx;                    /**< User context passed to callbacks */
    meridian_protocol_event_cb_t on_packet;    /**< Called for each received packet */
    meridian_protocol_event_cb_t on_node_joined; /**< Called when a node joins */
    meridian_protocol_event_cb_t on_node_left;   /**< Called when a node leaves */
} meridian_protocol_callbacks_t;

// ============================================================================
// PROTOCOL CONFIGURATION
// ============================================================================

/**
 * Configuration parameters for creating a protocol instance.
 * Contains all tunable parameters for the Meridian protocol.
 */
typedef struct meridian_protocol_config_t {
    uint16_t listen_port;                     /**< Local port for incoming connections */
    uint16_t info_port;                       /**< Port for info/status messages */
    uint32_t primary_ring_size;              /**< Max nodes per primary ring */
    uint32_t secondary_ring_size;            /**< Max nodes per secondary ring */
    int32_t ring_exponent_base;               /**< Log base for latency bucketing */
    uint32_t init_gossip_interval_s;          /**< Gossip interval during init phase */
    uint32_t num_init_gossip_intervals;       /**< Number of init intervals */
    uint32_t steady_state_gossip_interval_s;  /**< Gossip interval during steady-state */
    uint32_t replace_interval_s;              /**< Ring replacement check interval */
    uint32_t gossip_timeout_ms;              /**< Timeout for gossip replies */
    uint32_t measure_timeout_ms;             /**< Timeout for measure replies */
    work_pool_t* pool;                       /**< Work pool for async operations */
    hierarchical_timing_wheel_t* wheel;       /**< Timing wheel for scheduling */
} meridian_protocol_config_t;

// ============================================================================
// PROTOCOL STRUCTURE
// ============================================================================

/**
 * Main Meridian protocol instance.
 * Orchestrates all Meridian subsystems and manages QUIC connectivity.
 *
 * Lifecycle:
 * 1. Create via meridian_protocol_create()
 * 2. Configure with add_seed_node(), set_callbacks()
 * 3. Start via meridian_protocol_start()
 * 4. Use find_closest(), gossip(), ring_management() during operation
 * 5. Stop via meridian_protocol_stop()
 * 6. Destroy via meridian_protocol_destroy()
 */
typedef struct meridian_protocol_t {
    refcounter_t refcounter;                    /**< Reference counting for lifetime */
    meridian_protocol_state_t state;            /**< Current protocol state */
    meridian_protocol_config_t config;         /**< Configuration parameters */

    // QUIC handles (replace UDP socket)
    const struct QUIC_API_TABLE* msquic;     /**< msquic function table */
    HQUIC registration;                       /**< msquic registration handle */
    HQUIC listener;                           /**< QUIC listener for incoming */
    HQUIC configuration;                      /**< QUIC configuration (ALPN, TLS) */
    struct sockaddr_in local_addr;              /**< Local binding address */

    meridian_ring_set_t* ring_set;             /**< Latency ring management */
    meridian_rendv_handle_t* rendv_handle;     /**< Rendezvous/NAT handling */
    meridian_gossip_handle_t* gossip_handle;    /**< Gossip protocol handling */
    meridian_latency_cache_t* latency_cache;   /**< Recent latency measurements */

    // Pending measurement requests awaiting PONG responses
    meridian_measure_request_t** pending_measures; /**< Array of pending measure requests */
    size_t num_pending_measures;                    /**< Count of pending measure requests */
    size_t pending_measures_capacity;               /**< Allocated capacity */

    work_pool_t* pool;                          /**< Work pool for async tasks */
    hierarchical_timing_wheel_t* wheel;         /**< Timer scheduling */

    meridian_node_t* seed_nodes[16];           /**< Bootstrap nodes */
    size_t num_seed_nodes;                      /**< Number of seed nodes configured */

    // Connected peers via QUIC connections
    HQUIC connected_peers[64];                 /**< Active QUIC connections */
    meridian_node_t* peer_nodes[64];           /**< Peer address info corresponding to connections */
    size_t num_connected_peers;                 /**< Number of connected peers */

    // NAT traversal and connection management
    meridian_conn_t** connections;              /**< Managed connections (direct + relay) */
    size_t num_connections;                     /**< Number of managed connections */
    struct meridian_relay_t* default_relay;    /**< Default relay server client */

    meridian_protocol_callbacks_t callbacks;   /**< Protocol event callbacks */

    PLATFORMLOCKTYPE(lock);                    /**< Thread-safe state access */
    bool running;                              /**< True when protocol is active */
} meridian_protocol_t;

// ============================================================================
// PROTOCOL LIFECYCLE
// ============================================================================

/**
 * Creates a new protocol instance from configuration.
 * Initializes all subsystems but does not start network I/O.
 *
 * @param config  Configuration parameters
 * @return         New protocol with refcount=1, or NULL on failure
 */
meridian_protocol_t* meridian_protocol_create(const meridian_protocol_config_t* config);

/**
 * Destroys a protocol instance and all its subsystems.
 * Closes the QUIC connections and releases all allocated resources.
 *
 * @param protocol  Protocol to destroy
 */
void meridian_protocol_destroy(meridian_protocol_t* protocol);

/**
 * Starts the protocol, initializing QUIC listener and gossip.
 *
 * @param protocol  Protocol to start
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_start(meridian_protocol_t* protocol);

/**
 * Stops the protocol, closing connections and stopping gossip.
 *
 * @param protocol  Protocol to stop
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_stop(meridian_protocol_t* protocol);

// ============================================================================
// PEER MANAGEMENT
// ============================================================================

/**
 * Adds a seed node for initial bootstrap.
 * Seed nodes are contacted during bootstrapping to join the network.
 *
 * @param protocol  Protocol to add seed to
 * @param addr      Seed node IPv4 address
 * @param port      Seed node port
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_add_seed_node(meridian_protocol_t* protocol,
                                     uint32_t addr, uint16_t port);

/**
 * Connects to a peer node.
 * Adds the peer to the connected_peers list for gossip broadcast.
 *
 * @param protocol  Protocol to connect
 * @param addr      Peer IPv4 address
 * @param port      Peer port
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_connect(meridian_protocol_t* protocol,
                               uint32_t addr, uint16_t port);

/**
 * Disconnects from a peer node.
 * Removes the peer from the connected_peers list.
 *
 * @param protocol  Protocol to disconnect
 * @param addr      Peer IPv4 address
 * @param port      Peer port
 * @return          0 on success, -1 if peer not found
 */
int meridian_protocol_disconnect(meridian_protocol_t* protocol,
                                  uint32_t addr, uint16_t port);

// ============================================================================
// NETWORK I/O
// ============================================================================

/**
 * Sends a raw packet to a specific target node.
 *
 * @param protocol  Protocol to send from
 * @param data      Raw packet bytes
 * @param len       Length of data
 * @param target    Destination node
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_send_packet(meridian_protocol_t* protocol,
                                   const uint8_t* data, size_t len,
                                   const meridian_node_t* target);

/**
 * Broadcasts a packet to all connected peers.
 *
 * @param protocol  Protocol to broadcast from
 * @param data      Raw packet bytes
 * @param len       Length of data
 * @return          0 on success, -1 if any send failed
 */
int meridian_protocol_broadcast(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len);

// ============================================================================
// NODE DISCOVERY
// ============================================================================

/**
 * Finds the closest node to a target address using the ring set.
 *
 * @param protocol    Protocol to search
 * @param target_addr Target IPv4 address
 * @param target_port Target port
 * @return            Closest node, or NULL if none found
 */
meridian_node_t* meridian_protocol_find_closest(meridian_protocol_t* protocol,
                                                uint32_t target_addr, uint16_t target_port);

/**
 * Gets the list of currently connected peers.
 *
 * @param protocol   Protocol to query
 * @param num_peers Output: number of peers returned
 * @return          Array of peer nodes (valid until next API call)
 */
meridian_node_t** meridian_protocol_get_connected_peers(meridian_protocol_t* protocol, size_t* num_peers);

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

/**
 * Performs periodic gossip operations.
 * Checks scheduler and sends gossip to peers if interval has passed.
 *
 * @param protocol  Protocol to tick
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_gossip(meridian_protocol_t* protocol);

/**
 * Performs periodic ring maintenance.
 * Checks for eligible rings and promotes secondary nodes.
 *
 * @param protocol  Protocol to manage
 * @return         0 on success, -1 on failure
 */
int meridian_protocol_ring_management(meridian_protocol_t* protocol);

// ============================================================================
// PACKET HANDLING
// ============================================================================

/**
 * Handles an incoming packet from the network.
 * Decodes the packet and routes to appropriate handler.
 *
 * @param protocol  Protocol that received packet
 * @param data      Raw packet data
 * @param len       Length of data
 * @param from      Node that sent the packet
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_on_packet(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len,
                                 const meridian_node_t* from);

/**
 * Handles a measurement result from the measure subsystem.
 * Updates the latency cache with the result.
 *
 * @param protocol  Protocol to notify
 * @param result   Measurement result
 * @return         0 on success, -1 on failure
 */
int meridian_protocol_on_measure_result(meridian_protocol_t* protocol,
                                         const meridian_measure_result_t* result);

/**
 * Sends a PING to measure latency to a target node and registers the request.
 * When the PONG response arrives, the callback in the request is invoked with
 * the measured RTT.
 *
 * @param protocol  Protocol to send from
 * @param req       Measure request (must have target, callback, and query_id set)
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_send_measure(meridian_protocol_t* protocol,
                                    meridian_measure_request_t* req);

// ============================================================================
// CALLBACKS
// ============================================================================

/**
 * Sets the callbacks for protocol events.
 *
 * @param protocol   Protocol to configure
 * @param callbacks  Callback structure
 * @return          0 on success, -1 on failure
 */
int meridian_protocol_set_callbacks(meridian_protocol_t* protocol,
                                     const meridian_protocol_callbacks_t* callbacks);

// ============================================================================
// LOCAL NODE INFO
// ============================================================================

/**
 * Gets the local node's address and port.
 *
 * @param protocol  Protocol to query
 * @param addr     Output: local IPv4 address (may be NULL)
 * @param port     Output: local port (may be NULL)
 * @return         0 on success, -1 on failure
 */
int meridian_protocol_get_local_node(meridian_protocol_t* protocol,
                                      uint32_t* addr, uint16_t* port);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_PROTOCOL_H
