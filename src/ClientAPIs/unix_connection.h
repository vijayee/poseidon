/* Poseidon — Unix per-connection actor public interface
 *
 * The Unix backend is split into a listener (transport_unix.c) and a
 * per-connection actor (this file). Each accepted connection gets its
 * own actor that runs on the transport's scheduler pool. The
 * connection owns its watcher, its frame decoder and its pending
 * outbound frame.
 *
 * The struct body is visible here so that the listener can walk the
 * connection table during teardown: the listener has to stop each
 * connection's actor, release its watcher and free it, and that is
 * done by touching the fields directly.
 *
 * Threading: the connection's actor is the only thread that decodes
 * frames and writes to the socket. The I/O thread only ever calls the
 * read callback, which hands the raw bytes to the actor.
 */
#ifndef POSEIDON_UNIX_CONNECTION_H
#define POSEIDON_UNIX_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../deps/poll-dancer/include/poll-dancer/types.h"
#include "../Actor/actor.h"
#include "../Buffer/buffer.h"

struct client_session_t;
struct platform_socket_t;
struct pd_watcher;

#include "transport.h"
#include "unix_connection_messages.h"

/* Per-connection actor. One instance per accepted socket. The actor_t
 * is first so the struct follows the poseidon convention "the struct IS
 * the actor".
 *
 * Field notes:
 *  - transport: pointer to the parent transport. The on_message
 *    callback typed in transport.h takes struct
 *    poseidon_transport_t*, which is what this field is.
 *  - on_message: cached copy of the transport's on_message at _create
 *    time so the dispatch does not have to chase the parent pointer on
 *    every frame.
 *  - watcher: plain pd_watcher_t*. The watcher is created on the I/O
 *    thread and only ever destroyed on the I/O thread (through the
 *    transport's destroy stack), so the pointer is only ever changed
 *    by the connection's own actor.
 *  - frame decoder state: header_buf holds the 4 BE length prefix
 *    bytes, header_len counts how many have been read so far, and the
 *    contiguous read_buf is the working frame buffer. expected_len is
 *    set from the decoded header and is the target size of the next
 *    frame.
 *  - write_buffer: the pending outbound bytes. Held across partial
 *    sends. Owned by the connection; destroyed when the final write
 *    completes (or on connection teardown).
 *  - write_pending: non-zero while a write is in flight; serialized by
 *    the actor's dispatch.
 *  - is_closing: set by the CLOSE path to defer the actual socket
 *    shutdown / watcher stop / fd close until any in-flight write
 *    completes (so the FRAME is not truncated mid-transmission).
 */
typedef struct poseidon_unix_connection_t {
  actor_t actor;
  poseidon_transport_t* transport;
  platform_socket_t* sock;
  client_session_t* session;
  poseidon_transport_on_message_cb on_message;
  pd_watcher_t* watcher;
  // closed is raised once the socket is gone. The dispatch drops every
  // message for a closed connection; is_closing only means the
  // connection is winding down and still has to drain its pending
  // write.
  ATOMIC(uint8_t) closed;
  size_t expected_len;
  uint8_t header_len;
  uint8_t header_buf[4];
  uint8_t read_buf[65536];
  size_t read_len;
  buffer_t* write_buffer;
  int write_pending;
  int is_closing;
} poseidon_unix_connection_t;

/* Create a new per-connection actor and bind it to the given transport's
 * scheduler pool. On success, ownership of the platform_socket_t transfers
 * to the connection (the caller must NOT destroy it). On failure (out of
 * memory, watcher creation failed), the socket is destroyed by this
 * function and false is returned. The connection's watcher is registered
 * with the I/O loop; PD_EVENT_READ is armed before return so the first
 * event arrives as soon as the I/O loop dispatches.
 *
 * Returns false on failure; the caller must not call any other function
 * on a NULL connection pointer. */
bool poseidon_unix_connection_create(
    poseidon_transport_t* transport,
    struct platform_socket_t* sock,
    struct client_session_t* session,
    poseidon_unix_connection_t** out_conn);

/* Destroy a per-connection actor. The actor is stopped, its mailbox is
 * drained and the wrapper is freed. The connection's watcher and socket
 * are released as well. Safe to call from any thread. The pointer is
 * invalid after return. */
void poseidon_unix_connection_destroy(poseidon_unix_connection_t* conn);

/* Enqueue an outbound frame for delivery. The buffer is the framed
 * payload (length prefix already applied) and its ownership passes to
 * the connection. The connection's actor serializes the writes, so the
 * caller can rely on sequential frame delivery. */
void poseidon_unix_connection_write(
    poseidon_unix_connection_t* conn,
    struct buffer_t* buffer);

/* Request a graceful (drain pending writes) or hard (drop the pending
 * write and tear down immediately) close. The actual fd teardown is
 * performed by the connection actor on its own thread. Idempotent. */
void poseidon_unix_connection_close(
    poseidon_unix_connection_t* conn,
    bool graceful);

#endif /* POSEIDON_UNIX_CONNECTION_H */
