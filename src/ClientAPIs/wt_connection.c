//
// wt_connection.c
//
// WebTransport per-connection actor.
//
// Every accepted WebTransport session becomes one
// poseidon_wt_connection_t whose actor runs on the transport's
// scheduler pool. The actor owns the session's stream, its frame
// decoder and its pending outbound frames.
//
// Threading model:
//   - libwtf calls the transport's session and stream callbacks on its
//     own worker threads. The callbacks only copy what they received
//     and hand it to the actor;
//   - the actor decodes the frames, answers the client session and
//     writes to the stream;
//   - every cross-thread hand-off goes through actor_send, so the
//     stream is only ever touched by the actor's own thread.
//
// Wire protocol: 4-byte big-endian length prefix followed by a CBOR
// payload, the same framing the other backends use.
//

#include "wt_connection.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "client_session.h"
#include "transport_internal.h"

#include <wtf.h>

#include <endian.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define WT_READ_BUF_SIZE     65536
#define WT_FRAME_HEADER_SIZE 4
#define WT_MAX_FRAME_SIZE    WT_READ_BUF_SIZE

// ============================================================================
// PAYLOAD HELPERS
// ============================================================================

static void _data_payload_destroy(void* p) {
  poseidon_wt_connection_data_payload_t* payload =
      (poseidon_wt_connection_data_payload_t*)p;
  if (payload == NULL) return;
  if (payload->data != NULL) {
    free(payload->data);
  }
  free(payload);
}

static void _write_payload_destroy(void* p) {
  poseidon_wt_connection_write_payload_t* payload =
      (poseidon_wt_connection_write_payload_t*)p;
  if (payload == NULL) return;
  if (payload->data != NULL) {
    free(payload->data);
  }
  free(payload);
}

static void _close_payload_destroy(void* p) {
  free(p);
}

static void _stream_payload_destroy(void* p) {
  free(p);
}

// ============================================================================
// HELPERS
// ============================================================================

// Tears the connection down: the stream is closed, the connection is
// marked closed and the actor stops accepting work. The connection's
// memory is released by the listener's teardown, once the actor has
// been quiesced.
static void _connection_teardown(poseidon_wt_connection_t* connection) {
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }
  ATOMIC_STORE(&connection->closed, 1);
  connection->is_closing = 1;

  if (connection->stream != NULL) {
    wtf_stream_close(connection->stream);
    connection->stream = NULL;
  }
}

// ============================================================================
// FRAME HANDLING (actor thread)
// ============================================================================
//
// A frame is a CBOR document that the client session answers. The
// session's answer is framed again and queued on the connection's own
// write path, so the reply leaves through the same serialized writer
// as every other outbound frame.

static void _connection_handle_frame(poseidon_wt_connection_t* connection) {
  struct cbor_load_result result;
  cbor_item_t* item =
      cbor_load(connection->read_buf + WT_FRAME_HEADER_SIZE,
                connection->read_len - WT_FRAME_HEADER_SIZE, &result);
  if (item == NULL) {
    log_warn("wt connection: failed to decode CBOR frame");
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
        poseidon_wt_connection_write(connection, resp_buf, resp_len);
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
// Consumes the length-prefixed frames in the connection's receive
// buffer. The bytes are consumed in place, so a frame that is only
// half in the buffer stays there until the rest of it arrives.

static void _connection_decode_frames(poseidon_wt_connection_t* connection) {
  while (connection->read_len >= WT_FRAME_HEADER_SIZE) {
    uint32_t frame_len =
        ((uint32_t)connection->read_buf[0] << 24) |
        ((uint32_t)connection->read_buf[1] << 16) |
        ((uint32_t)connection->read_buf[2] << 8) |
        (uint32_t)connection->read_buf[3];
    if (frame_len == 0 || frame_len > WT_MAX_FRAME_SIZE) {
      log_error("wt connection: frame length %u out of range", frame_len);
      connection->read_len = 0;
      return;
    }

    size_t total_len = WT_FRAME_HEADER_SIZE + (size_t)frame_len;
    if (connection->read_len < total_len) {
      // Still partial; need more bytes.
      return;
    }

    _connection_handle_frame(connection);

    memmove(connection->read_buf, connection->read_buf + total_len,
            connection->read_len - total_len);
    connection->read_len -= total_len;
  }
}

// ============================================================================
// WRITE PATH (actor thread)
// ============================================================================
//
// Hands the pending bytes to libwtf. The buffer is handed over to
// libwtf, which frees it when the send completes.

static void _connection_send(poseidon_wt_connection_t* connection,
                             uint8_t* data, size_t len) {
  if (connection->stream == NULL || data == NULL) {
    return;
  }

  wtf_buffer_t buf;
  buf.data = data;
  buf.length = (uint32_t)len;

  // libwtf keeps the buffer until the send completes and frees it in
  // the SEND_COMPLETE event, so the buffer must not be touched here.
  wtf_result_t wr = wtf_stream_send(connection->stream, &buf, 1, false);
  if (wr != WTF_SUCCESS) {
    free(data);
  }
}

// ============================================================================
// DISPATCH (actor thread)
// ============================================================================

static void _wt_connection_dispatch(void* state, message_t* msg) {
  poseidon_wt_connection_t* connection = (poseidon_wt_connection_t*)state;
  if (connection == NULL) return;

  // The connection is gone: there is nothing left to do for it, the
  // queued messages are dropped.
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }

  switch (msg->type) {
    case POSEIDON_WT_CONNECTION_DATA: {
      poseidon_wt_connection_data_payload_t* payload =
          (poseidon_wt_connection_data_payload_t*)msg->payload;
      if (payload == NULL || payload->data == NULL || payload->len == 0) {
        break;
      }
      if (connection->is_closing) break;

      // Append the bytes to the receive buffer, growing it when needed.
      if (connection->read_len + payload->len > connection->read_cap) {
        size_t new_cap = connection->read_cap;
        while (new_cap < connection->read_len + payload->len) {
          new_cap *= 2;
        }
        uint8_t* new_buf = (uint8_t*)realloc(connection->read_buf, new_cap);
        if (new_buf == NULL) {
          log_error("wt connection: failed to grow the receive buffer");
          break;
        }
        connection->read_buf = new_buf;
        connection->read_cap = new_cap;
      }
      memcpy(connection->read_buf + connection->read_len, payload->data,
             payload->len);
      connection->read_len += payload->len;

      _connection_decode_frames(connection);
      break;
    }

    case POSEIDON_WT_CONNECTION_WRITE: {
      poseidon_wt_connection_write_payload_t* payload =
          (poseidon_wt_connection_write_payload_t*)msg->payload;
      if (payload == NULL || payload->data == NULL) {
        break;
      }
      uint8_t* data = payload->data;
      payload->data = NULL;  // ownership transferred

      if (connection->is_closing || connection->stream == NULL) {
        free(data);
        break;
      }
      _connection_send(connection, data, payload->len);
      break;
    }

    case POSEIDON_WT_CONNECTION_STREAM_OPENED: {
      poseidon_wt_connection_stream_payload_t* payload =
          (poseidon_wt_connection_stream_payload_t*)msg->payload;
      if (payload == NULL || payload->stream == NULL) {
        break;
      }
      // The actor owns the stream from now on.
      connection->stream = payload->stream;
      break;
    }

    case POSEIDON_WT_CONNECTION_STREAM_CLOSED: {
      // The stream is gone: drop it and tear the connection down.
      connection->stream = NULL;
      _connection_teardown(connection);
      break;
    }

    case POSEIDON_WT_CONNECTION_CLOSE: {
      poseidon_wt_connection_close_payload_t* payload =
          (poseidon_wt_connection_close_payload_t*)msg->payload;
      (void)payload;
      _connection_teardown(connection);
      break;
    }

    default:
      log_warn("wt connection: unknown message type %u", msg->type);
      break;
  }
}

// ============================================================================
// CREATE / DESTROY
// ============================================================================

bool poseidon_wt_connection_create(poseidon_transport_internal_t* transport,
                                    client_session_t* session,
                                    uint32_t session_id,
                                    poseidon_wt_connection_t** out_conn) {
  if (out_conn == NULL) {
    return false;
  }
  *out_conn = NULL;

  if (transport == NULL || session == NULL) {
    return false;
  }

  poseidon_wt_connection_t* connection =
      (poseidon_wt_connection_t*)calloc(1, sizeof(poseidon_wt_connection_t));
  if (connection == NULL) {
    return false;
  }

  connection->transport = transport;
  connection->session = session;
  connection->stream = NULL;
  connection->session_id = session_id;
  connection->read_buf = (uint8_t*)get_clear_memory(WT_READ_BUF_SIZE);
  if (connection->read_buf == NULL) {
    free(connection);
    return false;
  }
  connection->read_len = 0;
  connection->read_cap = WT_READ_BUF_SIZE;
  connection->is_closing = 0;
  ATOMIC_STORE(&connection->closed, 0);

  actor_init(&connection->actor, connection, _wt_connection_dispatch,
             transport->pool);

  *out_conn = connection;
  return true;
}

void poseidon_wt_connection_destroy(poseidon_wt_connection_t* connection) {
  if (connection == NULL) {
    return;
  }

  // Stop the actor first: no dispatch can be running once this
  // returns, so the fields below are not touched concurrently.
  actor_destroy(&connection->actor);

  if (connection->read_buf != NULL) {
    free(connection->read_buf);
    connection->read_buf = NULL;
  }
  free(connection);
}

// ============================================================================
// PUBLIC WRITE / CLOSE
// ============================================================================

void poseidon_wt_connection_write(poseidon_wt_connection_t* connection,
                                   const uint8_t* data, size_t len) {
  if (connection == NULL) {
    return;
  }
  if (data == NULL && len > 0) {
    return;
  }
  if (len > WT_MAX_FRAME_SIZE) {
    log_warn("wt connection: refusing to send a %zu byte frame", len);
    return;
  }

  // Build the framed payload: 4 header bytes plus the body. The
  // connection's write actor owns the buffer from here on, and libwtf
  // frees it once the send completes.
  size_t total_len = WT_FRAME_HEADER_SIZE + len;
  uint8_t* framed = (uint8_t*)get_clear_memory(total_len);
  if (framed == NULL) {
    return;
  }
  uint32_t be_len = htobe32((uint32_t)len);
  memcpy(framed, &be_len, WT_FRAME_HEADER_SIZE);
  if (len > 0) {
    memcpy(framed + WT_FRAME_HEADER_SIZE, data, len);
  }

  poseidon_wt_connection_write_payload_t* payload =
      (poseidon_wt_connection_write_payload_t*)
          malloc(sizeof(poseidon_wt_connection_write_payload_t));
  if (payload == NULL) {
    free(framed);
    return;
  }
  payload->data = framed;
  payload->len = total_len;

  message_t msg;
  msg.type = POSEIDON_WT_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&connection->actor, &msg);
}

void poseidon_wt_connection_close(poseidon_wt_connection_t* connection,
                                   bool graceful) {
  if (connection == NULL) {
    return;
  }
  (void)graceful;

  poseidon_wt_connection_close_payload_t* payload =
      (poseidon_wt_connection_close_payload_t*)
          malloc(sizeof(poseidon_wt_connection_close_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->graceful = graceful;

  message_t msg;
  msg.type = POSEIDON_WT_CONNECTION_CLOSE;
  msg.payload = payload;
  msg.payload_destroy = _close_payload_destroy;
  actor_send(&connection->actor, &msg);
}

// ============================================================================
// LISTENER HOOKS
// ============================================================================
//
// The transport's libwtf callbacks run on libwtf's own threads. They
// only hand the connection's actor what it needs; every decision is
// taken on the actor's thread.

// Announces the session's bidirectional stream to the connection.
void poseidon_wt_connection_stream_opened(poseidon_wt_connection_t* connection,
                                           struct wtf_stream* stream) {
  if (connection == NULL || stream == NULL) {
    return;
  }

  poseidon_wt_connection_stream_payload_t* payload =
      (poseidon_wt_connection_stream_payload_t*)
          malloc(sizeof(poseidon_wt_connection_stream_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->stream = stream;

  message_t msg;
  msg.type = POSEIDON_WT_CONNECTION_STREAM_OPENED;
  msg.payload = payload;
  msg.payload_destroy = _stream_payload_destroy;
  actor_send(&connection->actor, &msg);
}

// Announces that the stream is gone.
void poseidon_wt_connection_stream_closed(poseidon_wt_connection_t* connection) {
  if (connection == NULL) {
    return;
  }

  message_t msg;
  msg.type = POSEIDON_WT_CONNECTION_STREAM_CLOSED;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}

// Hands the bytes libwtf received to the connection's actor. The
// bytes must be a heap copy: the buffers libwtf hands over belong to
// libwtf. The connection takes ownership of the copy.
void poseidon_wt_connection_received(poseidon_wt_connection_t* connection,
                                      uint8_t* data, size_t len) {
  if (connection == NULL || data == NULL || len == 0) {
    if (data != NULL) {
      free(data);
    }
    return;
  }

  poseidon_wt_connection_data_payload_t* payload =
      (poseidon_wt_connection_data_payload_t*)
          malloc(sizeof(poseidon_wt_connection_data_payload_t));
  if (payload == NULL) {
    free(data);
    return;
  }
  payload->data = data;
  payload->len = len;

  message_t msg;
  msg.type = POSEIDON_WT_CONNECTION_DATA;
  msg.payload = payload;
  msg.payload_destroy = _data_payload_destroy;
  actor_send(&connection->actor, &msg);
}
