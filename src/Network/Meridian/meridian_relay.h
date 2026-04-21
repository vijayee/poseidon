//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_RELAY_H
#define POSEIDON_MERIDIAN_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include "meridian_rendv.h"
#include "msquic.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CALLBACK TYPES
// ============================================================================

/**
 * Callback invoked when a datagram is received from a peer via the relay.
 *
 * @param ctx            User context from meridian_relay_on_datagram
 * @param data           Received datagram data
 * @param len            Length of received data in bytes
 * @param src_endpoint_id Endpoint ID of the source peer
 */
typedef void (*meridian_relay_datagram_cb_t)(void* ctx, const uint8_t* data, size_t len,
                                             uint32_t src_endpoint_id);

/**
 * Callback invoked when a reflexive address response is received from the relay.
 *
 * @param ctx            User context from meridian_relay_on_addr_response
 * @param reflexive_addr Discovered public IPv4 address
 * @param reflexive_port Discovered public port
 * @param endpoint_id    Assigned endpoint ID for this peer
 */
typedef void (*meridian_relay_addr_cb_t)(void* ctx, uint32_t reflexive_addr,
                                         uint16_t reflexive_port, uint32_t endpoint_id);

// ============================================================================
// RELAY CLIENT CONFIGURATION
// ============================================================================

/**
 * Configuration for the relay client.
 * Controls connection parameters and protocol settings.
 */
typedef struct meridian_relay_config_t {
    const char* alpn;                 /**< ALPN protocol identifier */
    uint32_t idle_timeout_ms;         /**< Connection idle timeout in milliseconds */
    uint32_t max_datagram_size;       /**< Maximum datagram payload size */
    uint32_t keepalive_interval_ms;   /**< Keepalive ping interval in milliseconds */
    const char* tls_key_path;        /**< Path to TLS private key PEM (NULL = insecure) */
    const char* tls_cert_path;       /**< Path to TLS certificate PEM (NULL = insecure) */
} meridian_relay_config_t;

// ============================================================================
// RELAY CLIENT
// ============================================================================

/**
 * Relay client for establishing peer-to-peer communication through a relay server.
 * Maintains a QUIC connection to the relay and provides datagram forwarding.
 *
 * Thread safety: The relay client uses a platform lock to protect shared state.
 * All exported functions are thread-safe when called with proper synchronization.
 */
typedef struct meridian_relay_t {
    refcounter_t refcounter;                        /**< Reference counting for lifetime */
    const struct QUIC_API_TABLE* msquic;           /**< msquic function table */
    HQUIC registration;                            /**< msquic registration handle */
    HQUIC configuration;                           /**< msquic configuration handle */
    HQUIC connection;                              /**< QUIC connection to relay server */
    meridian_rendv_t* server;                       /**< Relay server rendezvous point */
    meridian_relay_config_t config;                /**< Client configuration */
    uint32_t local_endpoint_id;                     /**< Assigned endpoint ID from relay */
    bool connected;                                 /**< Connection state to relay */
    meridian_relay_datagram_cb_t on_datagram;       /**< Datagram received callback */
    void* on_datagram_ctx;                          /**< User context for datagram callback */
    meridian_relay_addr_cb_t on_addr_response;      /**< Address response callback */
    void* on_addr_response_ctx;                     /**< User context for address callback */
    PLATFORMLOCKTYPE(lock);                         /**< Thread-safe access */
} meridian_relay_t;

// ============================================================================
// RELAY CLIENT LIFECYCLE
// ============================================================================

/**
 * Creates a new relay client.
 * The client is initialized but not connected; call meridian_relay_connect to establish
 * the QUIC connection to the relay server.
 *
 * @param msquic      msquic function table (borrowed from parent protocol)
 * @param registration msquic registration handle (borrowed from parent protocol)
 * @param server      Relay server rendezvous point (caller retains ownership)
 * @param config      Client configuration parameters
 * @return            New relay client with refcount=1, or NULL on failure
 */
meridian_relay_t* meridian_relay_create(const struct QUIC_API_TABLE* msquic,
                                        HQUIC registration,
                                        meridian_rendv_t* server,
                                        const meridian_relay_config_t* config);

/**
 * Destroys a relay client.
 * Closes any active connection and frees all resources.
 *
 * @param relay  Relay client to destroy
 */
void meridian_relay_destroy(meridian_relay_t* relay);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

/**
 * Opens a QUIC connection to the relay server.
 * The connection is established asynchronously; use meridian_relay_is_connected to check status.
 *
 * @param relay  Relay client to connect
 * @return       0 on success, -1 on failure
 */
int meridian_relay_connect(meridian_relay_t* relay);

/**
 * Closes the QUIC connection to the relay server.
 *
 * @param relay  Relay client to disconnect
 * @return       0 on success, -1 on failure
 */
int meridian_relay_disconnect(meridian_relay_t* relay);

// ============================================================================
// DATAGRAM TRANSMISSION
// ============================================================================

/**
 * Sends a datagram to a peer through the relay server.
 *
 * @param relay            Relay client
 * @param data             Datagram payload data
 * @param len              Length of data in bytes
 * @param dest_endpoint_id Destination peer's endpoint ID
 * @return                 0 on success, -1 on failure
 */
int meridian_relay_send_datagram(meridian_relay_t* relay, const uint8_t* data,
                                  size_t len, uint32_t dest_endpoint_id);

// ============================================================================
// ADDRESS DISCOVERY
// ============================================================================

/**
 * Requests the client's reflexive address from the relay server.
 * The response is delivered via the callback registered with meridian_relay_on_addr_response.
 *
 * @param relay  Relay client
 * @return       0 on success, -1 on failure
 */
int meridian_relay_send_addr_request(meridian_relay_t* relay);

// ============================================================================
// CALLBACK REGISTRATION
// ============================================================================

/**
 * Registers a handler for incoming datagrams from peers.
 *
 * @param relay     Relay client
 * @param callback  Function to call when datagram is received
 * @param ctx       User context passed to callback
 */
void meridian_relay_on_datagram(meridian_relay_t* relay,
                                 meridian_relay_datagram_cb_t callback, void* ctx);

/**
 * Registers a handler for address discovery responses.
 *
 * @param relay     Relay client
 * @param callback  Function to call when reflexive address is discovered
 * @param ctx       User context passed to callback
 */
void meridian_relay_on_addr_response(meridian_relay_t* relay,
                                      meridian_relay_addr_cb_t callback, void* ctx);

// ============================================================================
// RELAY STATE QUERY
// ============================================================================

/**
 * Gets the endpoint ID assigned to this client by the relay server.
 *
 * @param relay  Relay client
 * @return       Assigned endpoint ID, or 0 if not connected
 */
uint32_t meridian_relay_get_endpoint_id(const meridian_relay_t* relay);

/**
 * Checks whether the relay client is connected to the relay server.
 *
 * @param relay  Relay client
 * @return       True if connected, false otherwise
 */
bool meridian_relay_is_connected(const meridian_relay_t* relay);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_RELAY_H