//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_CONN_H
#define POSEIDON_MERIDIAN_CONN_H

#include <stdint.h>
#include <stdbool.h>
#include "meridian.h"
#include "meridian_rendv.h"
#include "../../RefCounter/refcounter.h"
#include "../../Util/threadding.h"
#include "msquic.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

struct meridian_relay_t;

// ============================================================================
// CONNECTION STATE
// ============================================================================

/**
 * Connection state for a Meridian peer connection.
 * Tracks the progression from connection attempt to active data transfer.
 */
typedef enum {
    MERIDIAN_CONN_STATE_DIRECT,        /**< Direct QUIC connection active */
    MERIDIAN_CONN_STATE_TRYING_DIRECT,  /**< Attempting direct, relay as backup */
    MERIDIAN_CONN_STATE_RELAY,          /**< Relay only, direct failed or not tried */
    MERIDIAN_CONN_STATE_RELAY_ONLY     /**< Symmetric NAT — never try direct */
} meridian_conn_state_t;

// ============================================================================
// CONNECTION PATH
// ============================================================================

/**
 * Represents a network path to a peer.
 * Contains addresses and metrics for a particular route to the peer.
 */
typedef struct meridian_conn_path_t {
    uint32_t addr;                      /**< Peer address */
    uint16_t port;                       /**< Peer port */
    uint32_t reflexive_addr;             /**< Peer's discovered public address */
    uint16_t reflexive_port;             /**< Peer's discovered public port */
    uint32_t rtt_ms;                     /**< Measured round-trip time */
    uint64_t last_activity_ms;           /**< Last activity timestamp */
    bool active;                         /**< Path is currently usable */
} meridian_conn_path_t;

// ============================================================================
// CONNECTION
// ============================================================================

/**
 * Represents an active connection to a Meridian peer.
 * Manages both direct and relayed paths, automatically selecting the best route.
 *
 * Lifecycle:
 * 1. Create via meridian_conn_create()
 * 2. Initiate connection via meridian_conn_connect()
 * 3. Send data via meridian_conn_send()
 * 4. Destroy via meridian_conn_destroy()
 */
typedef struct meridian_conn_t {
    refcounter_t refcounter;            /**< Reference counting for lifetime */
    meridian_node_t* peer;               /**< Peer we're connecting to */
    HQUIC direct_connection;             /**< Direct QUIC connection (NULL if relay-only) */
    struct meridian_relay_t* relay;      /**< Relay client (NULL if direct-only) */
    uint32_t relay_endpoint_id;          /**< Peer's relay endpoint ID (0 if unknown) */
    meridian_conn_state_t state;         /**< Current connection state */
    meridian_conn_path_t direct_path;    /**< Direct path info */
    meridian_conn_path_t relay_path;     /**< Relay path info */
    meridian_nat_type_t local_nat_type;  /**< Our NAT type */
    meridian_nat_type_t peer_nat_type;   /**< Peer's NAT type */
    uint32_t direct_attempts;            /**< Count of direct connection attempts */
    uint64_t last_direct_attempt_ms;     /**< Timestamp of last direct attempt */
    PLATFORMLOCKTYPE(lock);              /**< Thread-safe access */
} meridian_conn_t;

// ============================================================================
// CONNECTION LIFECYCLE
// ============================================================================

/**
 * Creates a new connection to a peer.
 *
 * @param peer           Peer node to connect to
 * @param relay          Relay client (NULL if direct-only)
 * @param local_nat_type Our NAT type for this connection
 * @return               New connection with refcount=1, or NULL on failure
 */
meridian_conn_t* meridian_conn_create(meridian_node_t* peer, struct meridian_relay_t* relay,
                                       meridian_nat_type_t local_nat_type);

/**
 * Destroys a connection.
 * Closes both direct and relay connections, frees all resources.
 *
 * @param conn  Connection to destroy
 */
void meridian_conn_destroy(meridian_conn_t* conn);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

/**
 * Initiates a connection to the peer.
 * Tries direct first if the peer's NAT type permits, otherwise uses relay.
 *
 * @param conn  Connection to initiate
 * @return      0 on success, -1 on failure
 */
int meridian_conn_connect(meridian_conn_t* conn);

/**
 * Closes the connection.
 * Tears down both direct and relay paths.
 *
 * @param conn  Connection to close
 */
void meridian_conn_disconnect(meridian_conn_t* conn);

/**
 * Sends data via the best available path.
 * Prefers direct path if active, falls back to relay if needed.
 *
 * @param conn  Connection to send on
 * @param data  Data to send
 * @param len   Length of data in bytes
 * @return      Bytes sent, or -1 on failure
 */
int meridian_conn_send(meridian_conn_t* conn, const uint8_t* data, uint32_t len);

/**
 * Attempts to upgrade to a direct connection (call-me-maybe).
 * Used when we learn the peer's reflexive address.
 *
 * @param conn  Connection to upgrade
 * @return      0 on success, -1 on failure
 */
int meridian_conn_upgrade_to_direct(meridian_conn_t* conn);

// ============================================================================
// PEER INFORMATION UPDATES
// ============================================================================

/**
 * Updates the peer's NAT type.
 * Used when NAT type is discovered or changed.
 *
 * @param conn  Connection to update
 * @param type  Peer's NAT type
 */
void meridian_conn_set_peer_nat_type(meridian_conn_t* conn, meridian_nat_type_t type);

/**
 * Updates the peer's discovered reflexive address.
 * Used when we learn the peer's public address from rendezvous.
 *
 * @param conn  Connection to update
 * @param addr  Peer's reflexive IPv4 address
 * @param port  Peer's reflexive port
 */
void meridian_conn_set_peer_reflexive(meridian_conn_t* conn, uint32_t addr, uint16_t port);

// ============================================================================
// STATE QUERIES
// ============================================================================

/**
 * Gets the current connection state.
 *
 * @param conn  Connection to query
 * @return      Current state enum value
 */
meridian_conn_state_t meridian_conn_get_state(const meridian_conn_t* conn);

/**
 * Checks if using a direct path.
 *
 * @param conn  Connection to check
 * @return      True if direct path is active
 */
bool meridian_conn_is_direct(const meridian_conn_t* conn);

/**
 * Checks if using a relay path.
 *
 * @param conn  Connection to check
 * @return      True if relay path is active
 */
bool meridian_conn_is_relay(const meridian_conn_t* conn);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_CONN_H
