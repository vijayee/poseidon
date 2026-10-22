/* Poseidon — TCP transport backend
 *
 * The TCP backend is split into a listener (transport_tcp.c) and a
 * per-connection actor (tcp_connection.c). The listener owns the
 * listening socket, the accept watcher, the TLS context and the
 * poll-dancer I/O loop; every accepted socket becomes a
 * poseidon_tcp_connection_t whose actor runs on the transport's
 * scheduler pool.
 *
 * The public transport object is still the vtable-shaped
 * poseidon_transport_t declared in transport.h; the actor-shaped body
 * behind it is the internal transport body from transport_internal.h.
 * The listener's own state (listening socket, accept watcher, TLS
 * context, port, connection lock) lives in a backend-private struct
 * whose first member is the shared body, so the backend is recovered
 * from the internal body with a plain upcast and from the public
 * pointer through offsetof.
 *
 * A client is identified by the file descriptor of its socket, the same
 * way the Unix backend identifies its clients.
 */

#ifndef POSEIDON_TRANSPORT_TCP_H
#define POSEIDON_TRANSPORT_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"

struct poseidon_transport_internal_t;
struct client_session_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a TCP transport.
 *
 * The listening socket is bound and listened on during creation, so a
 * NULL return means the port could not be claimed. The transport is
 * not serving yet: call poseidon_transport_tcp_start to spin up the
 * I/O thread.
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
 * Stops the I/O thread (if running) and releases every resource the
 * transport owns: the listening socket, the accepted connections, the
 * TLS context, the I/O loop and the transport actor. The transport
 * pointer is invalid after this call.
 */
void poseidon_transport_tcp_destroy(poseidon_transport_t* transport);

/**
 * Starts the I/O thread that drives the poll-dancer loop and accepts
 * new connections.
 *
 * @return 0 on success, -1 on failure
 */
int poseidon_transport_tcp_start(poseidon_transport_t* transport);

/**
 * Stops the I/O thread and waits for it to exit. The accepted
 * connections are asked to close gracefully, so their pending frames
 * are flushed while the scheduler pool is still alive. Safe to call on
 * a transport that was never started.
 */
int poseidon_transport_tcp_stop(poseidon_transport_t* transport);

/**
 * Sends a payload to the client identified by client_id (the file
 * descriptor of the accepted socket). The payload is framed with the
 * 4-byte big-endian length prefix by the transport and queued on the
 * connection's write actor.
 *
 * @return 0 on success, -1 if the client is unknown
 */
int poseidon_transport_tcp_send(poseidon_transport_t* transport,
                                int client_id,
                                const uint8_t* data,
                                size_t len);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_TRANSPORT_TCP_H */
