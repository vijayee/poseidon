//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_TRANSPORT_H
#define POSEIDON_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include "client_session.h"
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TRANSPORT TYPES
// ============================================================================

typedef enum {
    POSEIDON_TRANSPORT_UNIX_SOCKET,
    POSEIDON_TRANSPORT_TCP,
    POSEIDON_TRANSPORT_WEBSOCKET,
    POSEIDON_TRANSPORT_QUIC,
    POSEIDON_TRANSPORT_WEBTRANSPORT,
    POSEIDON_TRANSPORT_BINDER,
    POSEIDON_TRANSPORT_XPC
} poseidon_transport_type_t;

// ============================================================================
// TRANSPORT CONFIG
// ============================================================================

typedef struct poseidon_transport_config_t {
    bool enable_unix;
    bool enable_tcp;
    bool enable_ws;
    bool enable_quic;
    bool enable_wt;
    bool enable_binder;
    bool enable_xpc;
    const char* unix_socket_path;
    uint16_t tcp_port;
    uint16_t ws_port;
    uint16_t quic_port;
    uint16_t wt_port;
    const char* tls_cert_path;
    const char* tls_key_path;
} poseidon_transport_config_t;

poseidon_transport_config_t poseidon_transport_config_defaults(void);

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

typedef struct poseidon_transport_t poseidon_transport_t;

typedef void (*poseidon_transport_on_message_cb)(poseidon_transport_t* transport,
                                                  int client_id,
                                                  const uint8_t* data,
                                                  size_t len);

struct poseidon_transport_t {
    const char* name;
    poseidon_transport_type_t type;
    PLATFORMTHREADTYPE thread;
    void* loop;
    poseidon_channel_manager_t* manager;
    poseidon_transport_on_message_cb on_message;
    PLATFORMLOCKTYPE(lock);
    volatile bool running;

    int (*start)(poseidon_transport_t* self);
    int (*stop)(poseidon_transport_t* self);
    int (*send)(poseidon_transport_t* self, int client_id,
                const uint8_t* data, size_t len);
    void (*destroy)(poseidon_transport_t* self);
};

// ============================================================================
// TRANSPORT LIFECYCLE
// ============================================================================

/**
 * Creates a Unix domain socket transport.
 *
 * @param socket_path  Path for the Unix socket (e.g., "/var/run/poseidond.sock")
 * @param manager       Shared channel manager
 * @return              New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_unix_create(const char* socket_path,
                                                      poseidon_channel_manager_t* manager);

/**
 * Creates a TCP+TLS transport.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (NULL for no TLS)
 * @param key_path   TLS private key PEM file (NULL for no TLS)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_tcp_create(uint16_t port,
                                                      const char* cert_path,
                                                      const char* key_path,
                                                      poseidon_channel_manager_t* manager);

/**
 * Creates a WebSocket+TLS transport.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (NULL for no TLS)
 * @param key_path   TLS private key PEM file (NULL for no TLS)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_ws_create(uint16_t port,
                                                    const char* cert_path,
                                                    const char* key_path,
                                                    poseidon_channel_manager_t* manager);

/**
 * Creates a QUIC transport.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (required for QUIC)
 * @param key_path   TLS private key PEM file (required for QUIC)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_quic_create(uint16_t port,
                                                      const char* cert_path,
                                                      const char* key_path,
                                                      poseidon_channel_manager_t* manager);

/**
 * Creates a WebTransport server transport.
 *
 * Uses libwtf (HTTP/3 + WebTransport over QUIC) for browser-compatible
 * connections. Requires TLS certificate and key.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (required)
 * @param key_path   TLS private key PEM file (required)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_webtransport_create(uint16_t port,
                                                             const char* cert_path,
                                                             const char* key_path,
                                                             poseidon_channel_manager_t* manager);

#ifdef __ANDROID__
/**
 * Creates an Android Binder transport.
 *
 * @param service_name  Binder service name
 * @param manager       Shared channel manager
 * @return              New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_binder_create(const char* service_name,
                                                      poseidon_channel_manager_t* manager);
#endif

#ifdef __APPLE__
/**
 * Creates an iOS/macOS XPC transport.
 *
 * @param service_name  XPC service name
 * @param manager       Shared channel manager
 * @return              New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_xpc_create(const char* service_name,
                                                    poseidon_channel_manager_t* manager);
#endif

/**
 * Destroys a transport. Stops the transport thread if running.
 *
 * @param transport  Transport to destroy
 */
void poseidon_transport_destroy(poseidon_transport_t* transport);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_TRANSPORT_H