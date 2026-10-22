/* Poseidon — Unix domain socket transport backend
 *
 * The Unix backend is split into a listener (transport_unix.c) and a
 * per-connection actor (unix_connection.c). The listener owns the
 * listening socket, the accept watcher and the poll-dancer I/O loop;
 * every accepted socket becomes a poseidon_unix_connection_t whose
 * actor runs on the transport's scheduler pool.
 *
 * The transport object is the actor-shaped body declared in
 * transport.h; the backend's private state lives in a backend struct
 * whose first member is that body.
 * The listener's own state (listening socket, accept watcher, socket
 * path, connection lock) lives in a backend-private struct that the
 * shared body reaches through its backend_data field.
 *
 * The on_message callback is invoked with a fully decoded frame: the
 * 4-byte big-endian length prefix has already been stripped, and the
 * callback receives exactly one frame's payload. Replies go back
 * through poseidon_transport_unix_send_to_client, which frames the
 * payload again and hands it to the connection's write actor.
 */

#ifndef POSEIDON_TRANSPORT_UNIX_H
#define POSEIDON_TRANSPORT_UNIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"

struct client_session_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a Unix domain socket transport.
 *
 * The socket is bound and listened on during creation, so a NULL
 * return means the path could not be claimed. The transport is not
 * serving yet: call poseidon_transport_unix_start to spin up the I/O
 * thread.
 *
 * @param socket_path  Path of the Unix socket to listen on
 * @param manager      Shared channel manager (may be NULL)
 * @param on_message   Callback invoked with each decoded inbound frame
 * @param ctx          Opaque user context, retrievable through
 *                     poseidon_transport_unix_context
 * @return             New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_unix_create(const char* socket_path,
                                                      poseidon_channel_manager_t* manager,
                                                      poseidon_transport_on_message_cb on_message,
                                                      void* ctx);

/**
 * Stops the I/O thread (if running) and releases every resource the
 * transport owns: the listening socket, the accepted connections, the
 * I/O loop and the transport actor. The transport pointer is invalid
 * after this call.
 */
void poseidon_transport_unix_destroy(poseidon_transport_t* transport);

/**
 * Starts the I/O thread that drives the poll-dancer loop and accepts
 * new connections.
 *
 * @return 0 on success, -1 on failure
 */
int poseidon_transport_unix_start(poseidon_transport_t* transport);

/**
 * Stops the I/O thread and waits for it to exit. Safe to call on a
 * transport that was never started.
 */
int poseidon_transport_unix_stop(poseidon_transport_t* transport);

/**
 * Sends a payload to the client identified by client_id (the file
 * descriptor of the accepted socket). The payload is framed with the
 * 4-byte big-endian length prefix by the transport.
 *
 * @return 0 on success, -1 if the client is unknown
 */
int poseidon_transport_unix_send(poseidon_transport_t* transport,
                                  int client_id,
                                  const uint8_t* data,
                                  size_t len);

/**
 * Same as poseidon_transport_unix_send, but addressed through the
 * internal transport body. This is the form the on_message callback
 * uses, since the callback receives the internal body.
 */
int poseidon_transport_unix_send_to_client(struct poseidon_transport_t* transport,
                                            int client_id,
                                            const uint8_t* data,
                                            size_t len);

/**
 * Returns the client session bound to client_id, or NULL when the
 * client is unknown. The returned pointer is borrowed; it stays valid
 * until the connection is torn down.
 */
struct client_session_t* poseidon_transport_unix_session_for_client_id(
    struct poseidon_transport_t* transport,
    int client_id);

/**
 * Returns the opaque user context handed to
 * poseidon_transport_unix_create.
 */
void* poseidon_transport_unix_context(struct poseidon_transport_t* transport);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_TRANSPORT_UNIX_H */
