//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_RENDV_H
#define POSEIDON_MERIDIAN_RENDV_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include "../../Util/threadding.h"
#include "meridian.h"
#include "msquic.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CONSTANTS
// ============================================================================

/** Maximum number of simultaneous tunnel connections */
#define MERIDIAN_RENDV_MAX_CONNECTIONS 16

// ============================================================================
// NAT TYPES
// ============================================================================

/**
 * NAT (Network Address Translation) traversal types.
 * Determines how peer-to-peer connections can be established.
 */
typedef enum {
    MERIDIAN_NAT_TYPE_UNKNOWN,           /**< Not yet determined */
    MERIDIAN_NAT_TYPE_OPEN,              /**< No NAT - direct connection possible */
    MERIDIAN_NAT_TYPE_FULL_CONE,         /**< Full cone NAT - any external can send */
    MERIDIAN_NAT_TYPE_RESTRICTED_CONE,   /**< Restricted cone - must have sent to target */
    MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE, /**< Port-restricted cone */
    MERIDIAN_NAT_TYPE_SYMMETRIC          /**< Symmetric NAT - different ports per destination */
} meridian_nat_type_t;

// ============================================================================
// RENDEZVOUS POINT
// ============================================================================

/**
 * Represents a rendezvous point for NAT traversal.
 * Rendezvous points help establish peer-to-peer connections through NAT.
 */
typedef struct meridian_rendv_t {
    refcounter_t refcounter;   /**< Reference counting for lifetime */
    uint32_t addr;             /**< Rendezvous server IPv4 address */
    uint16_t port;             /**< Rendezvous server port */
    meridian_nat_type_t nat_type; /**< NAT type of this rendezvous */
    bool is_active;            /**< Whether this rendezvous is currently active */
} meridian_rendv_t;

// ============================================================================
// RENDEZVOUS CONNECTION
// ============================================================================

/**
 * Represents an active connection through a rendezvous point.
 * Used for maintaining connections through NAT'd networks.
 */
typedef struct meridian_rendv_connection_t {
    meridian_rendv_t* rendv;      /**< Rendezvous point for this connection */
    HQUIC connection;            /**< QUIC connection handle */
    bool connected;             /**< Connection state */
    struct sockaddr_storage remote_addr; /**< Remote peer address */
} meridian_rendv_connection_t;

// ============================================================================
// RENDEZVOUS CONFIGURATION
// ============================================================================

/**
 * Configuration for rendezvous QUIC connections.
 * Contains TLS credentials and connection settings.
 */
typedef struct meridian_rendv_config_t {
    const char* alpn;                        /**< ALPN protocol identifier */
    QUIC_CREDENTIAL_CONFIG* cred_config;   /**< TLS credential configuration */
    uint32_t idle_timeout_ms;               /**< Connection idle timeout */
    bool datagram_recv_enabled;              /**< Enable receiving datagrams */
} meridian_rendv_config_t;

// ============================================================================
// RENDEZVOUS HANDLE
// ============================================================================

/**
 * Handle for managing rendezvous operations.
 * Manages local and peer rendezvous points and tunneled connections.
 */
typedef struct meridian_rendv_handle_t {
    refcounter_t refcounter;                    /**< Reference counting for lifetime */
    meridian_rendv_t* local_rendv;               /**< Local rendezvous point (if any) */
    vec_t(meridian_rendv_t*) peer_rendvs;       /**< Known peer rendezvous points */
    vec_t(meridian_rendv_connection_t*) connections; /**< Active tunneled connections */
    const struct QUIC_API_TABLE* msquic;     /**< msquic function table */
    HQUIC registration;                       /**< msquic registration for tunnels */
    HQUIC configuration;                     /**< msquic configuration for tunnels */
    PLATFORMLOCKTYPE(lock);                    /**< Thread-safe access */
} meridian_rendv_handle_t;

// ============================================================================
// RENDEZVOUS LIFECYCLE
// ============================================================================

/**
 * Creates a rendezvous point with given address and port.
 *
 * @param addr  Rendezvous server IPv4 address
 * @param port  Rendezvous server port
 * @return      New rendezvous with refcount=1, or NULL on failure
 */
meridian_rendv_t* meridian_rendv_create(uint32_t addr, uint16_t port);

/**
 * Destroys a rendezvous point.
 *
 * @param rendv  Rendezvous to destroy
 */
void meridian_rendv_destroy(meridian_rendv_t* rendv);

/**
 * Sets the NAT type for a rendezvous point.
 *
 * @param rendv  Rendezvous to update
 * @param type   NAT type enum value
 * @return       0 on success, -1 on failure
 */
int meridian_rendv_set_nat_type(meridian_rendv_t* rendv, meridian_nat_type_t type);

/**
 * Gets the NAT type of a rendezvous point.
 *
 * @param rendv  Rendezvous to query (NULL returns UNKNOWN)
 * @return       NAT type enum value
 */
meridian_nat_type_t meridian_rendv_get_nat_type(const meridian_rendv_t* rendv);

// ============================================================================
// HANDLE LIFECYCLE
// ============================================================================

/**
 * Creates a new rendezvous handle.
 * Initializes vectors for peers and connections.
 *
 * @return  New handle with refcount=1, or NULL on failure
 */
meridian_rendv_handle_t* meridian_rendv_handle_create(void);

/**
 * Sets the msquic handle and creates QUIC configuration on the rendezvous handle.
 * Must be called before creating QUIC tunnels.
 *
 * @param handle  Handle to update
 * @param msquic  msquic function table
 * @param registration  msquic registration handle
 * @param config  QUIC configuration with TLS credentials (NULL for defaults)
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_set_msquic(meridian_rendv_handle_t* handle,
                                    const struct QUIC_API_TABLE* msquic,
                                    HQUIC registration,
                                    const meridian_rendv_config_t* config);

/**
 * Destroys a rendezvous handle.
 * Closes all connections and frees all rendezvous points.
 *
 * @param handle  Handle to destroy
 */
void meridian_rendv_handle_destroy(meridian_rendv_handle_t* handle);

// ============================================================================
// LOCAL AND PEER MANAGEMENT
// ============================================================================

/**
 * Sets the local rendezvous point for this handle.
 * Takes ownership of a reference to the provided rendezvous.
 *
 * @param handle  Handle to update
 * @param rendv   Local rendezvous point
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_set_local(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv);

/**
 * Adds a peer rendezvous point to the handle.
 * No-op if peer already exists (by addr:port).
 *
 * @param handle  Handle to add to
 * @param rendv   Peer rendezvous to add
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_add_peer(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv);

/**
 * Removes a peer rendezvous by address:port.
 *
 * @param handle  Handle to remove from
 * @param addr    Address of peer to remove
 * @param port    Port of peer to remove
 * @return        0 if found and removed, -1 if not found
 */
int meridian_rendv_handle_remove_peer(meridian_rendv_handle_t* handle,
                                      uint32_t addr, uint16_t port);

/**
 * Finds a peer rendezvous by address:port.
 * Returns a new reference; caller must eventually destroy.
 *
 * @param handle  Handle to search
 * @param addr    Address to find
 * @param port    Port to find
 * @return        Peer rendezvous with new reference, or NULL if not found
 */
meridian_rendv_t* meridian_rendv_handle_find_peer(meridian_rendv_handle_t* handle,
                                                  uint32_t addr, uint16_t port);

// ============================================================================
// TUNNEL MANAGEMENT
// ============================================================================

/**
 * Creates a QUIC tunnel to a peer rendezvous point.
 * Used for hole-punching and direct communication.
 *
 * @param handle  Handle to create tunnel from
 * @param peer    Peer to connect to
 * @param out_conn  Output: QUIC connection handle for the tunnel
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_create_tunnel(meridian_rendv_handle_t* handle,
                                         meridian_rendv_t* peer,
                                         HQUIC* out_conn);

/**
 * Closes a QUIC tunnel connection.
 *
 * @param handle  Handle
 * @param conn    QUIC connection to close
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_close_tunnel(meridian_rendv_handle_t* handle, HQUIC conn);

// ============================================================================
// NAT DETECTION
// ============================================================================

/** Callback type for NAT detection results */
typedef void (*meridian_nat_callback_t)(void* ctx, uint32_t addr, uint16_t port,
                                         meridian_nat_type_t type);

/**
 * Detects NAT type for the local rendezvous.
 * Invokes callback with the detected information.
 *
 * @param handle    Handle to detect NAT for
 * @param callback  Function to receive NAT info
 * @param ctx      User context for callback
 * @return         0 on success, -1 on failure
 */
int meridian_rendv_handle_detect_nat(meridian_rendv_handle_t* handle,
                                      meridian_nat_callback_t callback, void* ctx);

// ============================================================================
// HOLE PUNCHING
// ============================================================================

/**
 * Sends a hole-punching packet to establish peer-to-peer connection.
 * Initiates NAT traversal by sending to the target rendezvous.
 *
 * @param handle  Handle to send from
 * @param target  Target rendezvous to punch
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_send_hole_punch(meridian_rendv_handle_t* handle,
                                    const meridian_rendv_t* target);

/**
 * Receives a hole-punching packet and reports sender info.
 *
 * @param handle     Handle to receive on
 * @param src_addr   Output: sender's address
 * @param src_port   Output: sender's port
 * @return           0 on success, -1 on failure
 */
int meridian_rendv_recv_hole_punch(meridian_rendv_handle_t* handle,
                                     uint32_t* src_addr, uint16_t* src_port);

// ============================================================================
// NAT TYPE UTILITIES
// ============================================================================

/**
 * Converts a string to a NAT type enum.
 *
 * @param str  String like "open", "full_cone", "symmetric", etc.
 * @return     Corresponding NAT type, or UNKNOWN if unrecognized
 */
meridian_nat_type_t meridian_nat_type_from_string(const char* str);

/**
 * Converts a NAT type enum to a string.
 *
 * @param type  NAT type to convert
 * @return      String representation, or "unknown" for unrecognized
 */
const char* meridian_nat_type_to_string(meridian_nat_type_t type);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_RENDV_H