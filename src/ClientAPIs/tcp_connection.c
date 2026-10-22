//
// tcp_connection.c
//
// TCP per-connection actor.
//
// Every accepted TCP socket becomes one poseidon_tcp_connection_t whose
// actor runs on the transport's scheduler pool. The actor owns the
// connection's TLS session, its frame decoder and its pending outbound
// frames.
//
// Threading model:
//   - the I/O thread only reports that the socket became readable or
//     writable; it never touches the socket itself;
//   - the actor performs the read, the write and the TLS handshake,
//     because an SSL object may only be used from one thread;
//   - every cross-thread hand-off (the session actor's outbound frame,
//     a close request) goes through actor_send, so the connection's
//     socket is only ever touched by its own actor.
//
// Wire protocol: 4-byte big-endian length prefix followed by a CBOR
// payload. The connection strips the prefix, decodes the frame and
// hands it to the client session; the send path adds the prefix back.
//

#include "tcp_connection.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "client_session.h"
#include "transport_internal.h"

#include <poll-dancer/poll-dancer.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define TCP_READ_BUF_SIZE      65536
#define TCP_FRAME_HEADER_SIZE  4
#define TCP_MAX_FRAME_SIZE     TCP_READ_BUF_SIZE

// How many times the actor drains the socket before it hands control
// back to the scheduler. The poll-dancer epoll backend is level
// triggered, so a socket that still holds data fires the watcher again
// and the next round picks the rest up. The cap keeps a busy peer from
// monopolising a pool worker.
#define TCP_MAX_READ_ITERATIONS 16

// ============================================================================
// PAYLOAD HELPERS
// ============================================================================

static void _write_payload_destroy(void* p) {
  poseidon_tcp_connection_write_payload_t* payload =
      (poseidon_tcp_connection_write_payload_t*)p;
  if (payload == NULL) return;
  if (payload->buffer != NULL) {
    buffer_destroy(payload->buffer);
  }
  free(payload);
}

static void _close_payload_destroy(void* p) {
  free(p);
}

// ============================================================================
// STATIC WATCHER HELPERS
// ============================================================================

static void _connection_update_watcher(poseidon_tcp_connection_t* connection,
                                       pd_event_t events) {
  if (connection->watcher == NULL) {
    return;
  }
  pd_watcher_update(connection->watcher, events);
}

// Claims the connection's watcher and hands it to the transport's
// destroy stack. The stack is drained on the I/O thread, so the
// watcher is unregistered and freed where the loop expects it. The
// socket is closed by the caller after this call, because the
// unregister has to precede the close.
static void _connection_stop_watcher(poseidon_tcp_connection_t* connection) {
  pd_watcher_t* watcher = connection->watcher;
  if (watcher == NULL) return;
  connection->watcher = NULL;

  if (connection->transport != NULL) {
    // Push the watcher onto the transport's destroy stack. The I/O
    // thread drains the stack and calls pd_watcher_destroy there.
    destroy_stack_push(&connection->transport->destroy_stack, watcher);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

// Closes the connection's socket. The closed flag is raised before the
// descriptor goes away, so the listener never resolves a recycled
// descriptor number to this connection.
static void _connection_close_fd(poseidon_tcp_connection_t* connection) {
  if (connection->sock == NULL) {
    return;
  }
  platform_socket_t* sock = connection->sock;
  connection->sock = NULL;
  ATOMIC_STORE(&connection->closed, 1);
  platform_socket_destroy(sock);
}

// Releases the connection's TLS session. It is only ever called from
// the actor's own thread, because the SSL object must not be used
// concurrently.
static void _connection_free_ssl(poseidon_tcp_connection_t* connection) {
  if (connection->ssl == NULL) {
    return;
  }
  SSL_shutdown(connection->ssl);
  SSL_free(connection->ssl);
  connection->ssl = NULL;
}

// Tears the connection down for good: the pending frames are dropped,
// the watcher is released, the TLS session goes away and the socket is
// closed. Safe to call more than once.
static void _connection_teardown(poseidon_tcp_connection_t* connection) {
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }

  if (connection->sock != NULL) {
    platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
  }

  // The watcher is released through the transport's destroy stack, so
  // it is unregistered and freed on the I/O thread. The socket is
  // closed afterwards: the unregister has to precede the close, or the
  // closed descriptor number can be reused by an accepted connection
  // whose registration the deferred unregister then removes.
  _connection_stop_watcher(connection);

  // The TLS session goes before the pending frames: a write that could
  // not be completed leaves the frame inside the SSL object, so the
  // frame's memory has to outlive it.
  _connection_free_ssl(connection);

  if (connection->write_buffer != NULL) {
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
  }
  if (connection->write_queue != NULL) {
    buffer_destroy(connection->write_queue);
    connection->write_queue = NULL;
  }
  connection->write_pending = 0;

  _connection_close_fd(connection);
}

// ============================================================================
// READ CALLBACK (runs on I/O thread)
// ============================================================================
//
// The callback never touches the socket: a TLS connection's SSL object
// may only be used from one thread, so the callback only reports that
// the socket became readable or writable and the actor does the work.

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                      pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  poseidon_tcp_connection_t* connection =
      (poseidon_tcp_connection_t*)user_data;
  if (connection == NULL) {
    return;
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    // The peer is gone. Tell the actor so it can run the close path.
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? POSEIDON_TCP_CONNECTION_HANGUP
                                           : POSEIDON_TCP_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    return;
  }

  if (events & PD_EVENT_READ) {
    // The socket is readable: the actor pulls the bytes off it.
    message_t msg;
    msg.type = POSEIDON_TCP_CONNECTION_READ;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
  }

  if (events & PD_EVENT_WRITE) {
    // The socket drained: retry the pending frame.
    message_t msg;
    msg.type = POSEIDON_TCP_CONNECTION_WRITABLE;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
  }
}

// ============================================================================
// TLS HANDSHAKE (actor thread)
// ============================================================================
//
// The handshake runs on the actor's thread against a non-blocking
// socket. When OpenSSL needs more data it reports SSL_ERROR_WANT_READ
// (or WANT_WRITE) and the watcher is re-armed; the next readiness
// notification drives the handshake forward.
//
// Returns 0 when the handshake is done (or still in progress) and -1
// when it failed for good.

static int _connection_handshake(poseidon_tcp_connection_t* connection) {
  if (connection->ssl_ctx == NULL) {
    connection->handshake_done = 1;
    return 0;
  }

  if (connection->ssl == NULL) {
    connection->ssl = SSL_new(connection->ssl_ctx);
    if (connection->ssl == NULL) {
      log_warn("tcp connection: SSL_new failed");
      return -1;
    }
    SSL_set_fd(connection->ssl, platform_socket_fd(connection->sock));
    // The buffer handed to SSL_write must not move between a failed
    // attempt and its retry, so the mode that allows the caller to move
    // it is deliberately NOT enabled: the connection keeps the pending
    // frame in place instead.
  }

  int rc = SSL_accept(connection->ssl);
  if (rc == 1) {
    connection->handshake_done = 1;
    _connection_update_watcher(connection, PD_EVENT_READ);
    return 0;
  }

  int err = SSL_get_error(connection->ssl, rc);
  if (err == SSL_ERROR_WANT_READ) {
    _connection_update_watcher(connection, PD_EVENT_READ);
    return 0;
  }
  if (err == SSL_ERROR_WANT_WRITE) {
    _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
    return 0;
  }

  log_warn("tcp connection: TLS handshake failed (err=%d)", err);
  return -1;
}

// ============================================================================
// FRAME HANDLING (actor thread)
// ============================================================================
//
// A frame is a CBOR document that the client session answers. The
// session's answer is framed again and queued on the connection's own
// write path, so the reply leaves through the same serialized writer
// as every other outbound frame.

static void _connection_handle_frame(poseidon_tcp_connection_t* connection) {
  struct cbor_load_result result;
  cbor_item_t* item =
      cbor_load(connection->read_buf, connection->read_len, &result);
  if (item == NULL) {
    log_warn("tcp connection: failed to decode CBOR frame");
    return;
  }

  client_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  if (client_protocol_decode(item, &frame) == 0) {
    cbor_item_t* response = NULL;
    int rc = client_session_handle_request(connection->session, &frame,
                                            &response);
    (void)rc;
    if (response != NULL) {
      uint8_t* resp_buf = NULL;
      size_t resp_len = 0;
      if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
        poseidon_tcp_connection_write(connection, resp_buf, resp_len);
        free(resp_buf);
      }
      cbor_decref(&response);
    }
  }

  cbor_decref(&item);
}

// ============================================================================
// FRAME DECODER (actor thread)
// ============================================================================
//
// Decode a stream of bytes into one or more length-prefixed frames. On
// a partial frame the state is kept in the connection's header_buf /
// header_len / read_buf / read_len.

static void _connection_decode_frames(poseidon_tcp_connection_t* connection,
                                      const uint8_t* data, size_t length) {
  size_t consumed = 0;
  while (consumed < length) {
    // Phase 1: collect the 4-byte big-endian length header.
    while (connection->header_len < TCP_FRAME_HEADER_SIZE &&
           consumed < length) {
      connection->header_buf[connection->header_len++] = data[consumed++];
    }
    if (connection->header_len < TCP_FRAME_HEADER_SIZE) {
      return;
    }

    // Decode the header once we have all four bytes.
    if (connection->expected_len == 0) {
      uint32_t len = ((uint32_t)connection->header_buf[0] << 24) |
                     ((uint32_t)connection->header_buf[1] << 16) |
                     ((uint32_t)connection->header_buf[2] << 8) |
                     ((uint32_t)connection->header_buf[3]);
      if (len == 0 || len > TCP_MAX_FRAME_SIZE) {
        log_error("tcp connection: frame length %u out of range", len);
        connection->is_closing = 1;
        _connection_teardown(connection);
        return;
      }
      connection->expected_len = len;
    }

    // Phase 2: collect frame bytes into the read buffer.
    size_t needed = (size_t)connection->expected_len - connection->read_len;
    size_t available = length - consumed;
    size_t to_copy = (available < needed) ? available : needed;
    memcpy(connection->read_buf + connection->read_len, data + consumed,
           to_copy);
    connection->read_len += to_copy;
    consumed += to_copy;

    if (connection->read_len < (size_t)connection->expected_len) {
      // Still partial; need more bytes.
      return;
    }

    // Frame complete.
    _connection_handle_frame(connection);

    // Reset decoder state for the next frame.
    connection->expected_len = 0;
    connection->header_len = 0;
    connection->read_len = 0;

    if (ATOMIC_LOAD(&connection->closed)) {
      return;
    }
  }
}

// ============================================================================
// SOCKET DRAIN (actor thread)
// ============================================================================
//
// Pulls the bytes off the socket and hands them to the frame decoder.
// The loop stops when the socket has nothing more to offer, when the
// connection is torn down, or when the read budget for one turn is
// exhausted.

static void _connection_drain(poseidon_tcp_connection_t* connection) {
  uint8_t chunk[TCP_READ_BUF_SIZE];

  for (int iteration = 0; iteration < TCP_MAX_READ_ITERATIONS; iteration++) {
    if (connection->sock == NULL) {
      return;
    }

    ssize_t bytes_read;
    if (connection->ssl != NULL) {
      bytes_read = SSL_read(connection->ssl, chunk, sizeof(chunk));
    } else {
      bytes_read = platform_socket_recv(connection->sock, chunk, sizeof(chunk));
    }

    if (bytes_read > 0) {
      _connection_decode_frames(connection, chunk, (size_t)bytes_read);
      if (ATOMIC_LOAD(&connection->closed)) {
        return;
      }
      continue;
    }

    if (bytes_read == 0) {
      // Clean EOF: the peer closed its side.
      connection->is_closing = 1;
      _connection_teardown(connection);
      return;
    }

    // bytes_read < 0
    if (connection->ssl != NULL) {
      int err = SSL_get_error(connection->ssl, (int)bytes_read);
      if (err == SSL_ERROR_WANT_READ) {
        return;
      }
      if (err == SSL_ERROR_WANT_WRITE) {
        _connection_update_watcher(connection,
                                   PD_EVENT_READ | PD_EVENT_WRITE);
        return;
      }
      if (err == SSL_ERROR_ZERO_RETURN) {
        // The peer sent close_notify: the TLS session is over.
        connection->is_closing = 1;
        _connection_teardown(connection);
        return;
      }
      log_warn("tcp connection: SSL_read failed (err=%d)", err);
      connection->is_closing = 1;
      _connection_teardown(connection);
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Nothing more to read right now.
      return;
    }
    log_warn("tcp connection: recv failed (errno=%d)", errno);
    connection->is_closing = 1;
    _connection_teardown(connection);
    return;
  }
}

// ============================================================================
// WRITE PATH (actor thread)
// ============================================================================
//
// Pushes the connection's pending frames at the socket. Returns true
// when nothing is pending any more, false when bytes are still queued
// or the connection was closed.
//
// A TLS write that cannot be completed keeps the pending frame exactly
// where it is: OpenSSL has to be retried with the very same buffer and
// length, so the frame is never moved while a write is in flight.

static bool _connection_flush(poseidon_tcp_connection_t* connection) {
  if (connection->sock == NULL || connection->write_buffer == NULL) {
    return true;
  }

  while (connection->write_buffer != NULL) {
    buffer_t* buf = connection->write_buffer;
    ssize_t written;

    if (connection->ssl != NULL) {
      int rc = SSL_write(connection->ssl, buf->data, (int)buf->size);
      if (rc <= 0) {
        int err = SSL_get_error(connection->ssl, rc);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
          // The socket could not take the frame yet. Keep the buffer
          // untouched and retry it once the socket is writable again.
          connection->write_pending = 1;
          _connection_update_watcher(connection,
                                     PD_EVENT_READ | PD_EVENT_WRITE);
          return false;
        }
        log_warn("tcp connection: SSL_write failed (err=%d)", err);
        connection->is_closing = 1;
        _connection_teardown(connection);
        return true;
      }
      written = rc;
    } else {
      written = platform_socket_send(connection->sock, buf->data, buf->size);
      if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // The kernel's send buffer is full. Keep the bytes and wait
          // for the socket to become writable again.
          connection->write_pending = 1;
          _connection_update_watcher(connection,
                                     PD_EVENT_READ | PD_EVENT_WRITE);
          return false;
        }
        log_warn("tcp connection: send failed (errno=%d)", errno);
        connection->is_closing = 1;
        _connection_teardown(connection);
        return true;
      }
    }

    if ((size_t)written == buf->size) {
      // Everything went out; the next queued frame follows.
      connection->write_buffer = connection->write_queue;
      connection->write_queue = NULL;
      buffer_destroy(buf);
      continue;
    }

    if (written == 0) {
      // The peer closed its side: nothing more will be accepted.
      connection->is_closing = 1;
      _connection_teardown(connection);
      return true;
    }

    // Partial write. Slide the un-sent tail to the front of a fresh
    // buffer of the right size; replace write_buffer with it. Arm WRITE
    // so the loop re-invokes us when the socket is writable again.
    size_t remaining = buf->size - (size_t)written;
    buffer_t* tail = buffer_create(remaining);
    if (tail == NULL) {
      // Allocation failure: drop the frame and close.
      buffer_destroy(connection->write_buffer);
      connection->write_buffer = NULL;
      connection->is_closing = 1;
      _connection_teardown(connection);
      return true;
    }
    memcpy(tail->data, buf->data + written, remaining);
    buffer_destroy(buf);
    connection->write_buffer = tail;
    connection->write_pending = 1;
    _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
    return false;
  }

  connection->write_pending = 0;
  _connection_update_watcher(connection, PD_EVENT_READ);
  return true;
}

// ============================================================================
// DISPATCH (actor thread)
// ============================================================================

static void _tcp_connection_dispatch(void* state, message_t* msg) {
  poseidon_tcp_connection_t* connection =
      (poseidon_tcp_connection_t*)state;
  if (connection == NULL) return;

  // The socket is gone: there is nothing left to do for this
  // connection, the queued messages are dropped.
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }

  switch (msg->type) {
    case POSEIDON_TCP_CONNECTION_READ: {
      if (connection->is_closing) break;

      if (!connection->handshake_done) {
        if (_connection_handshake(connection) != 0) {
          connection->is_closing = 1;
          _connection_teardown(connection);
          break;
        }
        if (!connection->handshake_done) {
          // The handshake needs more data; the watcher is re-armed.
          break;
        }
      }

      _connection_drain(connection);
      break;
    }

    case POSEIDON_TCP_CONNECTION_WRITABLE: {
      // The socket drained: retry the pending frame.
      bool drained = _connection_flush(connection);
      if (drained && connection->is_closing) {
        _connection_teardown(connection);
      }
      break;
    }

    case POSEIDON_TCP_CONNECTION_WRITE: {
      poseidon_tcp_connection_write_payload_t* payload =
          (poseidon_tcp_connection_write_payload_t*)msg->payload;
      if (payload == NULL || payload->buffer == NULL) {
        break;
      }
      buffer_t* buf = payload->buffer;
      payload->buffer = NULL;  // ownership transferred

      if (connection->is_closing || connection->sock == NULL) {
        buffer_destroy(buf);
        break;
      }

      // Queue the frame behind the one that is already pending. The
      // head buffer is never touched while a write is in flight: a TLS
      // write that could not be completed has to be retried with the
      // very same buffer.
      if (connection->write_buffer == NULL) {
        connection->write_buffer = buf;
      } else if (connection->write_queue == NULL) {
        connection->write_queue = buf;
      } else {
        buffer_t* combined =
            buffer_concat(connection->write_queue, buf);
        buffer_destroy(connection->write_queue);
        buffer_destroy(buf);
        connection->write_queue = combined;
      }
      connection->write_pending = 1;

      bool drained = _connection_flush(connection);
      if (drained && connection->is_closing) {
        _connection_teardown(connection);
      }
      break;
    }

    case POSEIDON_TCP_CONNECTION_CLOSE: {
      poseidon_tcp_connection_close_payload_t* payload =
          (poseidon_tcp_connection_close_payload_t*)msg->payload;
      bool graceful = (payload != NULL) ? payload->graceful : true;

      connection->is_closing = 1;
      if (graceful && connection->write_buffer != NULL) {
        // Drain pending writes before closing: the frame must not be
        // truncated mid-transmission.
        _connection_update_watcher(connection,
                                   PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      _connection_teardown(connection);
      break;
    }

    case POSEIDON_TCP_CONNECTION_HANGUP:
    case POSEIDON_TCP_CONNECTION_ERROR: {
      // The peer is gone. Drop what is pending and tear the connection
      // down.
      connection->is_closing = 1;
      _connection_teardown(connection);
      break;
    }

    default:
      log_warn("tcp connection: unknown message type %u", msg->type);
      break;
  }
}

// ============================================================================
// CREATE / DESTROY
// ============================================================================

bool poseidon_tcp_connection_create(poseidon_transport_internal_t* transport,
                                     platform_socket_t* sock,
                                     client_session_t* session,
                                     struct ssl_ctx_st* ssl_ctx,
                                     poseidon_tcp_connection_t** out_conn) {
  if (out_conn == NULL) {
    return false;
  }
  *out_conn = NULL;

  if (transport == NULL || sock == NULL) {
    return false;
  }

  poseidon_tcp_connection_t* connection =
      (poseidon_tcp_connection_t*)
          calloc(1, sizeof(poseidon_tcp_connection_t));
  if (connection == NULL) {
    return false;
  }

  connection->transport = transport;
  connection->sock = sock;
  connection->session = session;
  connection->ssl_ctx = ssl_ctx;
  connection->ssl = NULL;
  connection->watcher = NULL;
  connection->client_fd = platform_socket_fd(sock);
  connection->handshake_done = (ssl_ctx == NULL) ? 1 : 0;
  connection->expected_len = 0;
  connection->header_len = 0;
  connection->read_len = 0;
  connection->write_buffer = NULL;
  connection->write_queue = NULL;
  connection->write_pending = 0;
  connection->is_closing = 0;
  ATOMIC_STORE(&connection->closed, 0);

  actor_init(&connection->actor, connection, _tcp_connection_dispatch,
             transport->pool);

  // The watcher is started on READ; the connection re-arms it as
  // needed through the dispatch arm.
  connection->watcher =
      platform_socket_watcher_create(transport->loop, sock, PD_EVENT_READ,
                                     _connection_read_callback, connection);
  if (connection->watcher == NULL) {
    // The connection could never receive I/O, so returning it
    // half-built would leave the caller with an accepted-but-dead
    // socket. Tear down what create built and report the failure;
    // the caller destroys the socket.
    actor_destroy(&connection->actor);
    free(connection);
    return false;
  }
  pd_watcher_start(connection->watcher);

  *out_conn = connection;
  return true;
}

// ============================================================================
// PUBLIC WRITE / CLOSE
// ============================================================================

void poseidon_tcp_connection_write(poseidon_tcp_connection_t* connection,
                                    const uint8_t* data, size_t len) {
  if (connection == NULL) {
    return;
  }
  if (data == NULL && len > 0) {
    return;
  }
  if (len > TCP_MAX_FRAME_SIZE) {
    log_warn("tcp connection: refusing to send a %zu byte frame", len);
    return;
  }

  // Build the framed payload: 4 header bytes plus the body. The
  // connection's write actor owns the buffer from here on.
  buffer_t* framed = buffer_create(TCP_FRAME_HEADER_SIZE + len);
  if (framed == NULL) {
    return;
  }
  uint32_t be_len = htobe32((uint32_t)len);
  memcpy(framed->data, &be_len, TCP_FRAME_HEADER_SIZE);
  if (len > 0) {
    memcpy(framed->data + TCP_FRAME_HEADER_SIZE, data, len);
  }

  poseidon_tcp_connection_write_payload_t* payload =
      (poseidon_tcp_connection_write_payload_t*)
          malloc(sizeof(poseidon_tcp_connection_write_payload_t));
  if (payload == NULL) {
    buffer_destroy(framed);
    return;
  }
  payload->buffer = framed;

  message_t msg;
  msg.type = POSEIDON_TCP_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&connection->actor, &msg);
}

void poseidon_tcp_connection_close(poseidon_tcp_connection_t* connection,
                                    bool graceful) {
  if (connection == NULL) {
    return;
  }

  poseidon_tcp_connection_close_payload_t* payload =
      (poseidon_tcp_connection_close_payload_t*)
          malloc(sizeof(poseidon_tcp_connection_close_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->graceful = graceful;

  message_t msg;
  msg.type = POSEIDON_TCP_CONNECTION_CLOSE;
  msg.payload = payload;
  msg.payload_destroy = _close_payload_destroy;
  actor_send(&connection->actor, &msg);
}
