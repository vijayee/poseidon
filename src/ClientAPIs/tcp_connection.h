/* Poseidon — TCP per-connection actor public interface
 *
 * The TCP backend is split into a listener (transport_tcp.c) and a
 * per-connection actor (this file). Each accepted connection gets its
 * own actor that runs on the transport's scheduler pool. The
 * connection owns its watcher, its TLS session, its frame decoder and
 * its pending outbound frames.
 *
 * The struct body is visible here so that the listener can walk the
 * connection table during teardown: the listener has to stop each
 * connection's actor, release its watcher and free it, and that is
 * done by touching the fields directly.
 *
 * Threading: a TLS connection's SSL object may only be used from a
 * single thread, so the connection's actor is the only thread that
 * touches the socket, the SSL session and the frame decoder. The I/O
 * thread only ever calls the watcher callback, which reports that the
 * socket became readable or writable; the actor then performs the
 * read, the write and the TLS handshake.
 */
#ifndef POSEIDON_TCP_CONNECTION_H
#define POSEIDON_TCP_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../deps/poll-dancer/include/poll-dancer/types.h"
#include "../Actor/actor.h"
#include "../Buffer/buffer.h"

struct client_session_t;
struct platform_socket_t;
struct pd_watcher;
struct ssl_ctx_st;
struct ssl_st;

#include "transport.h"
#include "tcp_connection_messages.h"

/* Per-connection actor. One instance per accepted socket. The actor_t
 * is first so the struct follows the poseidon convention "the struct IS
 * the actor".
 *
 * Field notes:
 *  - transport: pointer to the parent transport. It carries the
 *    scheduler pool the actor is bound to and the destroy stack the
 *    watcher is released through.
 *  - ssl_ctx: the listener's TLS context, borrowed. NULL when the
 *    transport runs without TLS.
 *  - ssl: the connection's own SSL session. Created on the actor
 *    thread when the handshake starts, because an SSL object may only
 *    be used from one thread.
 *  - watcher: plain pd_watcher_t*. The watcher is created on the I/O
 *    thread and only ever destroyed on the I/O thread (through the
 *    transport's destroy stack), so the pointer is only ever changed
 *    by the connection's own actor.
 *  - client_fd: the descriptor the connection was accepted on. It is
 *    the connection's identity on the wire and never changes, so the
 *    listener can look a connection up without touching the socket.
 *  - closed: set once the socket has been closed. The listener skips
 *    closed connections when it looks a client up, so a recycled
 *    descriptor number is never mistaken for a live connection.
 *  - frame decoder state: header_buf holds the 4 BE length prefix
 *    bytes, header_len counts how many have been read so far, and the
 *    contiguous read_buf is the working frame buffer. expected_len is
 *    set from the decoded header and is the target size of the next
 *    frame.
 *  - write_buffer / write_queue: the pending outbound bytes. The head
 *    (write_buffer) is the buffer being written to the socket; the
 *    queue holds the frames that follow it. A TLS write that could not
 *    be completed keeps the head buffer untouched, because the SSL
 *    object has to be retried with the very same buffer.
 *  - write_pending: non-zero while a write is in flight; serialized by
 *    the actor's dispatch.
 *  - is_closing: set by the CLOSE path to defer the actual socket
 *    shutdown / watcher stop / fd close until any in-flight write
 *    completes (so the FRAME is not truncated mid-transmission).
 */
typedef struct poseidon_tcp_connection_t {
  actor_t actor;
  poseidon_transport_t* transport;
  platform_socket_t* sock;
  client_session_t* session;
  struct ssl_ctx_st* ssl_ctx;
  struct ssl_st* ssl;
  pd_watcher_t* watcher;
  int client_fd;
  ATOMIC(uint8_t) closed;
  int handshake_done;
  size_t expected_len;
  uint8_t header_len;
  uint8_t header_buf[4];
  uint8_t read_buf[65536];
  size_t read_len;
  buffer_t* write_buffer;
  buffer_t* write_queue;
  int write_pending;
  int is_closing;
} poseidon_tcp_connection_t;

/* Create a new per-connection actor and bind it to the given transport's
 * scheduler pool. ssl_ctx is the listener's TLS context (NULL for a
 * plaintext transport) and is borrowed. On success, ownership of the
 * platform_socket_t transfers to the connection (the caller must NOT
 * destroy it). On failure (out of memory, watcher creation failed), the
 * socket is destroyed by this function and false is returned. The
 * connection's watcher is registered with the I/O loop; PD_EVENT_READ is
 * armed before return so the first event arrives as soon as the I/O loop
 * dispatches.
 *
 * Returns false on failure; the caller must not call any other function
 * on a NULL connection pointer. */
bool poseidon_tcp_connection_create(
    poseidon_transport_t* transport,
    struct platform_socket_t* sock,
    struct client_session_t* session,
    struct ssl_ctx_st* ssl_ctx,
    poseidon_tcp_connection_t** out_conn);

/* Destroy a per-connection actor. The actor is stopped, its mailbox is
 * drained and the wrapper is freed. The connection's watcher, SSL
 * session and socket are released as well. Safe to call from any
 * thread. The pointer is invalid after return. */
void poseidon_tcp_connection_destroy(poseidon_tcp_connection_t* conn);

/* Enqueue an outbound payload. The payload is framed with the 4-byte
 * big-endian length prefix by the connection and copied before this
 * call returns, so the caller keeps its buffer. The connection's actor
 * serializes the writes, so the caller can rely on sequential frame
 * delivery. */
void poseidon_tcp_connection_write(poseidon_tcp_connection_t* conn,
                                   const uint8_t* data,
                                   size_t len);

/* Request a graceful (drain pending writes) or hard (drop the pending
 * write and tear down immediately) close. The actual fd teardown is
 * performed by the connection actor on its own thread. Idempotent. */
void poseidon_tcp_connection_close(poseidon_tcp_connection_t* conn,
                                   bool graceful);

#endif /* POSEIDON_TCP_CONNECTION_H */
