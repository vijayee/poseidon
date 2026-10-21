//
// unix_connection.c
//
// Per-connection actor for the Unix-domain client API. Each accepted
// socket becomes one poseidon_unix_connection_t whose mailbox is
// drained by a scheduler worker.
//
// The actor receives a small set of message types (data / write /
// close / writable) and the I/O thread invokes the connection's read
// callback to feed bytes and hangup/error notifications into the
// actor.
//
// Wire protocol on the socket: 4-byte big-endian length prefix followed
// by a payload of that length. The connection reassembles
// length-prefixed frames from a stream of read() calls before handing
// the framed payload to the on_message callback.
//
// The connection owns its watcher. The watcher may only be registered
// and unregistered on the I/O thread, so a teardown that is requested
// from the actor thread pushes the watcher onto the transport's destroy
// stack; the I/O thread drains that stack and frees the watcher there.
// The socket is closed only after the watcher has been unregistered, so
// the descriptor number cannot be recycled under a live registration.
//

#include "unix_connection.h"
#include "../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "../Util/log.h"
#include "../Buffer/buffer.h"
#include "transport_internal.h"

#include <errno.h>
#include <endian.h>
#include <string.h>

// ============================================================================
// CONNECTION TYPE
// ============================================================================
//
// The struct body lives in unix_connection.h so that the listener can
// walk the connection table during teardown. The field notes are
// documented there.

#define UNIX_READ_BUF_SIZE    65536
#define UNIX_FRAME_HEADER_SIZE 4
#define UNIX_MAX_FRAME_SIZE   UNIX_READ_BUF_SIZE

// ============================================================================
// PAYLOAD HELPERS
// ============================================================================

static void _data_payload_destroy(void* p) {
  poseidon_unix_connection_data_payload_t* payload =
      (poseidon_unix_connection_data_payload_t*)p;
  if (payload == NULL) return;
  if (payload->buffer != NULL) {
    buffer_destroy(payload->buffer);
  }
  free(payload);
}

static void _write_payload_destroy(void* p) {
  poseidon_unix_connection_write_payload_t* payload =
      (poseidon_unix_connection_write_payload_t*)p;
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

static void _connection_update_watcher(poseidon_unix_connection_t* connection,
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
static void _connection_stop_watcher(poseidon_unix_connection_t* connection) {
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

static void _connection_close_fd(poseidon_unix_connection_t* connection) {
  if (connection->sock == NULL) {
    return;
  }
  platform_socket_t* sock = connection->sock;
  connection->sock = NULL;
  platform_socket_destroy(sock);
}

// ============================================================================
// READ CALLBACK (runs on I/O thread)
// ============================================================================

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                      pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  poseidon_unix_connection_t* connection =
      (poseidon_unix_connection_t*)user_data;

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    // The peer is gone. Tell the actor so it can run the close path
    // (drain pending writes, shut the fd down, tear the watcher down).
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? POSEIDON_UNIX_CONNECTION_HANGUP
                                           : POSEIDON_UNIX_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    return;
  }

  if (events & PD_EVENT_WRITE) {
    // The socket is writable again: retry the pending frame.
    message_t msg;
    msg.type = POSEIDON_UNIX_CONNECTION_WRITABLE;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    return;
  }

  if (events & PD_EVENT_READ) {
    // POSIX read. We avoid partial frame accumulators on the I/O
    // thread and just hand the raw bytes to the actor, which owns
    // the frame reassembly state.
    if (connection->sock == NULL) {
      return;
    }
    uint8_t buffer[UNIX_READ_BUF_SIZE];
    ssize_t bytes_read =
        platform_socket_recv(connection->sock, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
      if (bytes_read == 0) {
        // Clean EOF: the peer closed its side.
        message_t msg;
        msg.type = POSEIDON_UNIX_CONNECTION_HANGUP;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        message_t msg;
        msg.type = POSEIDON_UNIX_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
      }
      return;
    }

    buffer_t* data = buffer_create_from_pointer_copy(buffer, (size_t)bytes_read);
    if (data == NULL) {
      return;
    }
    poseidon_unix_connection_data_payload_t* payload =
        (poseidon_unix_connection_data_payload_t*)
            malloc(sizeof(poseidon_unix_connection_data_payload_t));
    if (payload == NULL) {
      buffer_destroy(data);
      return;
    }
    payload->buffer = data;

    message_t msg;
    msg.type = POSEIDON_UNIX_CONNECTION_DATA;
    msg.payload = payload;
    msg.payload_destroy = _data_payload_destroy;
    actor_send(&connection->actor, &msg);
  }
}

// ============================================================================
// FRAME DECODER (actor thread)
// ============================================================================

// Decode a stream of bytes into one or more length-prefixed frames.
// On a complete frame, invokes the on_message callback with the frame's
// payload. On a partial frame, stores the state in the connection's
// header_buf / header_len / read_buf / read_len.
static size_t _connection_decode_frames(poseidon_unix_connection_t* connection,
                                        const uint8_t* data, size_t length) {
  size_t consumed = 0;
  while (consumed < length) {
    // Phase 1: collect the 4-byte big-endian length header.
    while (connection->header_len < UNIX_FRAME_HEADER_SIZE &&
           consumed < length) {
      connection->header_buf[connection->header_len++] = data[consumed++];
    }
    if (connection->header_len < UNIX_FRAME_HEADER_SIZE) {
      return consumed;
    }

    // Decode the header once we have all four bytes.
    if (connection->expected_len == 0) {
      uint32_t len = ((uint32_t)connection->header_buf[0] << 24) |
                     ((uint32_t)connection->header_buf[1] << 16) |
                     ((uint32_t)connection->header_buf[2] << 8) |
                     ((uint32_t)connection->header_buf[3]);
      if (len == 0 || len > UNIX_MAX_FRAME_SIZE) {
        log_error("unix_connection: frame length %u out of range", len);
        connection->is_closing = 1;
        return consumed;
      }
      connection->expected_len = len;
    }

    // Phase 2: collect frame bytes into the read buffer.
    size_t needed =
        (size_t)connection->expected_len - connection->read_len;
    size_t available = length - consumed;
    size_t to_copy = (available < needed) ? available : needed;
    memcpy(connection->read_buf + connection->read_len, data + consumed,
           to_copy);
    connection->read_len += to_copy;
    consumed += to_copy;

    if (connection->read_len < (size_t)connection->expected_len) {
      // Still partial; need more bytes.
      return consumed;
    }

    // Frame complete. Hand it to the on_message callback as a fresh
    // buffer. The buffer takes a copy of the frame bytes; the
    // decoder's read_buf is reused for the next frame.
    buffer_t* frame = buffer_create_from_pointer_copy(
        connection->read_buf, connection->read_len);
    if (frame != NULL) {
      if (connection->on_message != NULL && connection->transport != NULL) {
        // client_id is the underlying file descriptor for this
        // transport. on_message is 4-arg (transport, client_id, data, len).
        int client_id = platform_socket_fd(connection->sock);
        connection->on_message(connection->transport, client_id, frame->data,
                               frame->size);
      }
      buffer_destroy(frame);
    }

    // Reset decoder state for the next frame.
    connection->expected_len = 0;
    connection->header_len = 0;
    connection->read_len = 0;
  }
  return consumed;
}

// ============================================================================
// WRITE PATH (actor thread)
// ============================================================================

// Pushes the connection's pending write buffer at the socket. Returns
// true when the buffer is empty afterwards (everything was written),
// false when bytes are still pending or the connection was closed.
static bool _connection_flush(poseidon_unix_connection_t* connection) {
  if (connection->sock == NULL || connection->write_buffer == NULL) {
    return true;
  }

  const uint8_t* out = connection->write_buffer->data;
  size_t out_len = connection->write_buffer->size;
  ssize_t sent = platform_socket_send(connection->sock, out, out_len);

  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // The kernel's send buffer is full. Keep the bytes and wait for
      // the socket to become writable again.
      connection->write_pending = 1;
      _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
      return false;
    }
    // A real error: drop the pending frame and close the connection.
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
    connection->write_pending = 0;
    connection->is_closing = 1;
    _connection_stop_watcher(connection);
    _connection_close_fd(connection);
    return true;
  }

  if ((size_t)sent == out_len) {
    // Everything went out.
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
    connection->write_pending = 0;
    _connection_update_watcher(connection, PD_EVENT_READ);
    return true;
  }

  if (sent == 0) {
    // The peer closed its side: nothing more will be accepted.
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
    connection->write_pending = 0;
    connection->is_closing = 1;
    _connection_stop_watcher(connection);
    _connection_close_fd(connection);
    return true;
  }

  // Partial write. Slide the un-sent tail to the front of a fresh
  // buffer of the right size; replace write_buffer with it. Arm WRITE
  // so the loop re-invokes us when the socket is writable again.
  size_t remaining = out_len - (size_t)sent;
  buffer_t* tail = buffer_create(remaining);
  if (tail == NULL) {
    // Allocation failure: drop the buffer and close.
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
    connection->write_pending = 0;
    connection->is_closing = 1;
    _connection_stop_watcher(connection);
    _connection_close_fd(connection);
    return true;
  }
  memcpy(tail->data, connection->write_buffer->data + sent, remaining);
  buffer_destroy(connection->write_buffer);
  connection->write_buffer = tail;
  connection->write_pending = 1;
  _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
  return false;
}

// ============================================================================
// DISPATCH (actor thread)
// ============================================================================

static void _unix_connection_dispatch(void* state, message_t* msg) {
  poseidon_unix_connection_t* connection = (poseidon_unix_connection_t*)state;
  if (connection == NULL) return;

  if (connection->is_closing) {
    return;
  }

  switch (msg->type) {
    case POSEIDON_UNIX_CONNECTION_DATA: {
      poseidon_unix_connection_data_payload_t* payload =
          (poseidon_unix_connection_data_payload_t*)msg->payload;
      if (payload == NULL || payload->buffer == NULL) {
        break;
      }
      const uint8_t* bytes = payload->buffer->data;
      size_t length = payload->buffer->size;
      _connection_decode_frames(connection, bytes, length);
      break;
    }

    case POSEIDON_UNIX_CONNECTION_HANGUP:
    case POSEIDON_UNIX_CONNECTION_ERROR: {
      // The peer is gone. Drain what is pending, then tear the
      // connection down.
      connection->is_closing = 1;
      if (connection->write_buffer != NULL) {
        buffer_destroy(connection->write_buffer);
        connection->write_buffer = NULL;
      }
      connection->write_pending = 0;
      if (connection->sock != NULL) {
        platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case POSEIDON_UNIX_CONNECTION_WRITABLE: {
      // The socket drained: retry the pending frame.
      if (connection->sock == NULL) {
        break;
      }
      bool drained = _connection_flush(connection);
      if (drained && connection->is_closing) {
        if (connection->sock != NULL) {
          platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
        }
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case POSEIDON_UNIX_CONNECTION_WRITE: {
      poseidon_unix_connection_write_payload_t* payload =
          (poseidon_unix_connection_write_payload_t*)msg->payload;
      if (payload == NULL || payload->buffer == NULL) {
        break;
      }
      buffer_t* buf = payload->buffer;
      payload->buffer = NULL;  // ownership transferred

      if (connection->sock == NULL) {
        buffer_destroy(buf);
        break;
      }

      // Coalesce with a pending frame: the bytes go out in one send.
      if (connection->write_buffer != NULL) {
        buffer_t* combined = buffer_concat(connection->write_buffer, buf);
        buffer_destroy(connection->write_buffer);
        buffer_destroy(buf);
        connection->write_buffer = combined;
      } else {
        connection->write_buffer = buf;
      }
      connection->write_pending = 1;

      bool drained = _connection_flush(connection);
      if (drained && connection->is_closing) {
        if (connection->sock != NULL) {
          platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
        }
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case POSEIDON_UNIX_CONNECTION_CLOSE: {
      poseidon_unix_connection_close_payload_t* payload =
          (poseidon_unix_connection_close_payload_t*)msg->payload;
      bool graceful = (payload != NULL) ? payload->graceful : true;

      connection->is_closing = 1;
      if (graceful && connection->write_buffer != NULL) {
        // Drain pending writes before closing: the frame must not be
        // truncated mid-transmission.
        _connection_update_watcher(connection,
                                   PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      if (connection->write_buffer != NULL) {
        buffer_destroy(connection->write_buffer);
        connection->write_buffer = NULL;
      }
      connection->write_pending = 0;
      if (connection->sock != NULL) {
        platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    default:
      log_warn("unix_connection: unknown message type %u", msg->type);
      break;
  }
}

// ============================================================================
// CREATE / DESTROY
// ============================================================================

bool poseidon_unix_connection_create(poseidon_transport_internal_t* transport,
                                     platform_socket_t* sock,
                                     client_session_t* session,
                                     poseidon_unix_connection_t** out_conn) {
  if (out_conn == NULL) {
    return false;
  }
  *out_conn = NULL;

  if (transport == NULL || sock == NULL) {
    return false;
  }

  poseidon_unix_connection_t* connection =
      (poseidon_unix_connection_t*)
          calloc(1, sizeof(poseidon_unix_connection_t));
  if (connection == NULL) {
    return false;
  }

  connection->transport = transport;
  connection->sock = sock;
  connection->session = session;
  connection->on_message = transport->on_message;
  connection->expected_len = 0;
  connection->header_len = 0;
  connection->read_len = 0;
  connection->write_buffer = NULL;
  connection->write_pending = 0;
  connection->is_closing = 0;

  actor_init(&connection->actor, connection, _unix_connection_dispatch,
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

void poseidon_unix_connection_destroy(poseidon_unix_connection_t* connection) {
  if (connection == NULL) {
    return;
  }

  // Stop the actor first: no dispatch can be running once this
  // returns, so the fields below are not touched concurrently.
  actor_destroy(&connection->actor);

  // The watcher is released through the transport's destroy stack, so
  // it is unregistered and freed on the I/O thread. The socket is
  // closed afterwards: the unregister has to precede the close, or the
  // closed descriptor number can be reused by an accepted connection
  // whose registration the deferred unregister then removes.
  _connection_stop_watcher(connection);

  if (connection->sock != NULL) {
    platform_socket_destroy(connection->sock);
    connection->sock = NULL;
  }
  if (connection->write_buffer != NULL) {
    buffer_destroy(connection->write_buffer);
    connection->write_buffer = NULL;
  }
  free(connection);
}

// ============================================================================
// PUBLIC WRITE / CLOSE
// ============================================================================

void poseidon_unix_connection_write(poseidon_unix_connection_t* connection,
                                    buffer_t* buffer) {
  if (connection == NULL || buffer == NULL) {
    if (buffer != NULL) {
      buffer_destroy(buffer);
    }
    return;
  }

  poseidon_unix_connection_write_payload_t* payload =
      (poseidon_unix_connection_write_payload_t*)
          malloc(sizeof(poseidon_unix_connection_write_payload_t));
  if (payload == NULL) {
    buffer_destroy(buffer);
    return;
  }
  payload->buffer = buffer;

  message_t msg;
  msg.type = POSEIDON_UNIX_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&connection->actor, &msg);
}

void poseidon_unix_connection_close(poseidon_unix_connection_t* connection,
                                    bool graceful) {
  if (connection == NULL) {
    return;
  }

  poseidon_unix_connection_close_payload_t* payload =
      (poseidon_unix_connection_close_payload_t*)
          malloc(sizeof(poseidon_unix_connection_close_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->graceful = graceful;

  message_t msg;
  msg.type = POSEIDON_UNIX_CONNECTION_CLOSE;
  msg.payload = payload;
  msg.payload_destroy = _close_payload_destroy;
  actor_send(&connection->actor, &msg);
}
