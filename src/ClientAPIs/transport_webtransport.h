/* Poseidon — WebTransport transport backend
 *
 * The WebTransport backend is split into a listener
 * (transport_webtransport.c) and a per-connection actor
 * (wt_connection.c). The listener owns the libwtf context, the server
 * and the session and stream callbacks; every accepted WebTransport
 * session becomes a poseidon_wt_connection_t whose actor runs on the
 * transport's scheduler pool.
 *
 * libwtf owns the I/O: it runs its own worker threads and delivers the
 * data through the transport's callbacks. There is no poll-dancer loop
 * behind this backend, and no watcher to tear down.
 *
 * The public transport object is still the vtable-shaped
 * poseidon_transport_t declared in transport.h; the actor-shaped body
 * behind it is the internal transport body from transport_internal.h.
 * The listener's own state (libwtf context, server, port, certificate
 * paths, connection lock) lives in a backend-private struct whose
 * first member is the shared body, so the backend is recovered from
 * the internal body with a plain upcast and from the public pointer
 * through offsetof.
 *
 * A client is identified by the id of its client session, the same way
 * the other backends identify their clients by descriptor.
 */

#ifndef POSEIDON_TRANSPORT_WEBTRANSPORT_H
#define POSEIDON_TRANSPORT_WEBTRANSPORT_H

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
 * Creates a WebTransport transport.
 *
 * The libwtf context and server are created during start, so a NULL
 * return means the arguments were rejected. The transport is not
 * serving yet: call poseidon_transport_webtransport_start to bring
 * the server up.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (required)
 * @param key_path   TLS private key PEM file (required)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_webtransport_create(
    uint16_t port,
    const char* cert_path,
    const char* key_path,
    poseidon_channel_manager_t* manager);

/**
 * Stops the server and releases every resource the transport owns: the
 * libwtf context and server, the accepted connections and the
 * transport actor. The transport pointer is invalid after this call.
 */
void poseidon_transport_webtransport_destroy(poseidon_transport_t* transport);

/**
 * Starts the libwtf server.
 *
 * @return 0 on success, -1 on failure
 */
int poseidon_transport_webtransport_start(poseidon_transport_t* transport);

/**
 * Stops the libwtf server and waits for it to exit. Safe to call on a
 * transport that was never started.
 */
int poseidon_transport_webtransport_stop(poseidon_transport_t* transport);

/**
 * Sends a payload to the client identified by client_id (the id of
 * the client's session). The payload is framed with the 4-byte
 * big-endian length prefix by the transport and queued on the
 * connection's write actor.
 *
 * @return 0 on success, -1 if the client is unknown
 */
int poseidon_transport_webtransport_send(poseidon_transport_t* transport,
                                         int client_id,
                                         const uint8_t* data,
                                         size_t len);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_TRANSPORT_WEBTRANSPORT_H */
