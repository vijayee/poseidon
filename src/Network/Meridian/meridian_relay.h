//
// Created by victor on 4/19/26.
//

#ifndef POSEIDON_MERIDIAN_RELAY_H
#define POSEIDON_MERIDIAN_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include "meridian_rendv.h"
#include "meridian_stream.h"
#include "../../Actor/actor.h"
#include "../../Time/wheel.h"
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
    uint8_t max_retries;               /**< How many times to reconnect after the
                                relay drops the connection. Upstream's relay
                                client takes the same budget
                                (liboffs/src/Network/relay_client.h). */
    uint32_t retry_delay_ms;          /**< The first backoff; each retry doubles
                                it. */
    hierarchical_timing_wheel_t* wheel; /**< The wheel that fires the backoff, so
                                no pool worker sleeps through it. */
} meridian_relay_config_t;

// ============================================================================
// RELAY CLIENT
// ============================================================================

/**
 * Relay client for establishing peer-to-peer communication through a relay server.
 * Maintains a QUIC connection to the relay and provides datagram forwarding.
 *
 * Thread safety: the relay follows the shape upstream's relay client uses.
 * The flags that are read across threads are atomic, the connection table
 * and the registered callbacks are guarded by one small mutex, and the
 * actor carries the outbound work that must not run on a callback thread.
 * An msquic call is never made while the mutex is held: msquic may answer
 * it with a callback on the same thread, and that callback takes the mutex.
 */
typedef struct meridian_relay_t {
    refcounter_t refcounter;                        /**< Reference counting for lifetime */
    const struct QUIC_API_TABLE* msquic;           /**< msquic function table */
    HQUIC registration;                            /**< msquic registration handle */
    HQUIC configuration;                           /**< msquic configuration handle */
    HQUIC connection;                              /**< QUIC connection to relay server */
    meridian_rendv_t* server;                       /**< Relay server rendezvous point */
    struct meridian_rendv_handle_t* handle;        /**< The rendezvous handle that owns the NAT
                                detection. The address response goes to its
                                actor as an event, the way upstream's relay
                                client hands it to the NAT detector's actor. */
    meridian_relay_config_t config;                /**< Client configuration */
    ATOMIC(uint32_t) local_endpoint_id;             /**< Assigned endpoint ID from relay */
    ATOMIC(uint8_t) connected;                      /**< Connection state to relay */
    ATOMIC(uint8_t) running;                        /**< True while the client is up. The
                                retry never fires once this is clear — upstream
                                keeps the same flag
                                (liboffs/src/Network/relay_client.h). */
    uint32_t reflexive_addr;                        /**< The address the relay saw
                                (server-reflexive, 0 if unknown). Upstream's
                                relay client keeps it here. */
    uint16_t reflexive_port;                        /**< The port the relay saw. */
    /* The retry budget: a relay that bounces orphans the client until the
     * process restarts, so the client reconnects on its own — a bounded
     * number of times, with an exponential backoff. Upstream's relay
     * client has the same state and the same reason
     * (liboffs/src/Network/relay_client.c). */
    uint8_t max_retries;                            /**< How many times to retry. */
    uint8_t retry_count;                            /**< How many have been tried. */
    uint32_t retry_delay_ms;                        /**< The first backoff; each retry doubles it. */
    uint8_t shutdown_pending;                        /**< 1 when the application asked to
                                disconnect: a shutdown never retries. */
    hierarchical_timing_wheel_t* wheel;             /**< The wheel that fires the retry's
                                backoff, so no worker sleeps through it. */
    meridian_relay_datagram_cb_t on_datagram;       /**< Datagram received callback */
    void* on_datagram_ctx;                          /**< User context for datagram callback */
    meridian_stream_entry_t** active_streams;        /**< Tracking array for open streams */
    size_t num_active_streams;                       /**< Number of active streams */
    size_t active_streams_cap;                       /**< Capacity of active_streams array */
    platform_mutex_t* lock;                         /**< Guards the stream table, the callbacks and the connection handle */
    scheduler_pool_t* pool;                          /**< Scheduler pool owning this actor */
    scheduler_pool_t* own_pool;                      /**< The pool this relay created for itself, when it was given none */
    actor_t actor;                                   /**< Work queue (embedded by value) */
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
 * @param pool        Scheduler pool for actor dispatch. NULL means the relay
 *                    creates and owns a pool of its own — an actor with no
 *                    pool is never scheduled, and a mailbox that is never
 *                    drained is a leak, not a fallback.
 * @return            New relay client with refcount=1, or NULL on failure
 */
meridian_relay_t* meridian_relay_create(const struct QUIC_API_TABLE* msquic,
                                        HQUIC registration,
                                        meridian_rendv_t* server,
                                        struct meridian_rendv_handle_t* handle,
                                        const meridian_relay_config_t* config,
                                        scheduler_pool_t* pool);

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
 *
 * The response is not a callback: the relay client posts it to the
 * rendezvous handle's actor as an event, the way upstream's relay client
 * hands the address response to the NAT detector's actor
 * (liboffs/src/Network/relay_client.c, NETWORK_ADDR_RESPONSE). A
 * callback would run on the relay's stream actor — a pool worker — and a
 * worker that waits is a worker that runs no actor; the event runs on
 * the actor that owns the state instead.
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


// ============================================================================
// RELAY STATE QUERY
// ============================================================================

/**
 * Gets the endpoint ID assigned to this client by the relay server.
 *
 * @param relay  Relay client
 * @return       Assigned endpoint ID, or 0 if not connected
 */
uint32_t meridian_relay_get_endpoint_id(meridian_relay_t* relay);

/**
 * Checks whether the relay client is connected to the relay server.
 *
 * @param relay  Relay client
 * @return       True if connected, false otherwise
 */
bool meridian_relay_is_connected(meridian_relay_t* relay);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_MERIDIAN_RELAY_H
