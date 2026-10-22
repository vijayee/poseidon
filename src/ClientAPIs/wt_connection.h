/* Poseidon — WebTransport per-connection actor public interface
 *
 * The WebTransport backend is split into a listener
 * (transport_webtransport.c) and a per-connection actor (this file).
 * Each accepted WebTransport session gets its own actor that runs on
 * the transport's scheduler pool. The connection owns its stream, its
 * frame decoder and its pending outbound frames.
 *
 * The struct body is visible here so that the listener can walk the
 * connection table during teardown: the listener has to stop each
 * connection's actor, release its resources and free it, and that is
 * done by touching the fields directly.
 *
 * Threading: libwtf owns the I/O and calls the transport's callbacks
 * on its own worker threads. Those callbacks only copy the bytes they
 * received and hand them to the connection's actor, so the actor is
 * the only thread that decodes frames, answers the client session and
 * touches the stream.
 */
#ifndef POSEIDON_WT_CONNECTION_H
#define POSEIDON_WT_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../deps/poll-dancer/include/poll-dancer/types.h"
#include "../Actor/actor.h"

struct poseidon_transport_internal_t;
struct client_session_t;
struct wtf_stream;

#include "transport_internal.h"
#include "wt_connection_messages.h"

/* Per-connection actor. One instance per WebTransport session. The
 * actor_t is first so the struct follows the poseidon convention "the
 * struct IS the actor".
 *
 * Field notes:
 *  - transport: pointer to the parent transport. It carries the
 *    scheduler pool the actor is bound to.
 *  - session: the client session bound to this connection.
 *  - stream: the session's bidirectional stream. It is announced with
 *    a STREAM_OPENED message, so only the actor's own thread ever
 *    touches it.
 *  - session_id: the client session's id. It is the connection's
 *    identity on the wire: the client session reports it back on
 *    every reply.
 *  - closed: set once the connection has been torn down. The listener
 *    skips closed connections when it looks a client up.
 *  - read_buf / read_len / read_cap: the connection's receive buffer.
 *    The bytes arrive in libwtf's chunks and are appended here; the
 *    frame decoder consumes them in place.
 *  - is_closing: set by the CLOSE path once the connection is winding
 *    down.
 */
typedef struct poseidon_wt_connection_t {
  actor_t actor;
  poseidon_transport_internal_t* transport;
  client_session_t* session;
  struct wtf_stream* stream;
  uint32_t session_id;
  ATOMIC(uint8_t) closed;
  uint8_t* read_buf;
  size_t read_len;
  size_t read_cap;
  int is_closing;
} poseidon_wt_connection_t;

/* Create a new per-connection actor and bind it to the given
 * transport's scheduler pool. On success the connection owns the
 * client session it was given. On failure (out of memory) nothing is
 * owned and false is returned.
 *
 * Returns false on failure; the caller must not call any other function
 * on a NULL connection pointer. */
bool poseidon_wt_connection_create(poseidon_transport_internal_t* transport,
                                    client_session_t* session,
                                    uint32_t session_id,
                                    poseidon_wt_connection_t** out_conn);

/* Destroy a per-connection actor. The actor is stopped, its mailbox is
 * drained and the wrapper is freed. The connection's receive buffer is
 * released as well. Safe to call from any thread. The pointer is
 * invalid after return. */
void poseidon_wt_connection_destroy(poseidon_wt_connection_t* conn);

/* Enqueue an outbound payload. The payload is framed with the 4-byte
 * big-endian length prefix by the connection and copied before this
 * call returns, so the caller keeps its buffer. The connection's actor
 * serializes the writes, so the caller can rely on sequential frame
 * delivery. */
void poseidon_wt_connection_write(poseidon_wt_connection_t* conn,
                                   const uint8_t* data,
                                   size_t len);

/* Request a close. The actual teardown is performed by the connection
 * actor on its own thread. Idempotent. */
void poseidon_wt_connection_close(poseidon_wt_connection_t* conn,
                                   bool graceful);

/* ---- LISTENER HOOKS ------------------------------------------------------
 * The transport's libwtf callbacks run on libwtf's own worker threads.
 * They only copy what libwtf handed them and hand it to the actor; every
 * decision is taken on the actor's thread.
 */

/* Announces the session's bidirectional stream to the connection. The
 * stream belongs to the actor from then on. */
void poseidon_wt_connection_stream_opened(poseidon_wt_connection_t* conn,
                                           struct wtf_stream* stream);

/* Announces that the stream is gone. */
void poseidon_wt_connection_stream_closed(poseidon_wt_connection_t* conn);

/* Hands the bytes libwtf received to the connection's actor. The data
 * must be a heap copy — the buffers libwtf hands over belong to libwtf —
 * and the connection takes ownership of it. */
void poseidon_wt_connection_received(poseidon_wt_connection_t* conn,
                                      uint8_t* data,
                                      size_t len);

#endif /* POSEIDON_WT_CONNECTION_H */
