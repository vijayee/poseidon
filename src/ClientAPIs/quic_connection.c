//
// quic_connection.c
//
// QUIC per-connection actor.
//
// Every accepted client stream becomes one poseidon_quic_connection_t
// whose actor runs on the transport's scheduler pool. The actor owns
// the stream, its frame decoder and its pending outbound frames.
//
// Threading model:
//   - msquic calls the transport's callbacks on its own worker threads.
//     The callbacks only copy what msquic handed them and hand it to
//     the actor;
//   - the actor decodes the frames, answers the client session and
//     writes to the stream;
//   - every cross-thread hand-off goes through actor_send, so the
//     stream is only ever touched by the actor's own thread.
//
// Wire protocol: 4-byte big-endian length prefix followed by a CBOR
// payload, the same framing the other backends use.
//

#include "quic_connection.h"

#include "../Platform/platform.h"
#include "../Platform/poseidon_msquic.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "client_session.h"
#include "transport.h"

#include <msquic.h>

#include <endian.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define QUIC_READ_BUF_SIZE     65536
#define QUIC_FRAME_HEADER_SIZE 4
#define QUIC_MAX_FRAME_SIZE    QUIC_READ_BUF_SIZE

// ============================================================================
// PAYLOAD HELPERS
// ============================================================================

static void _data_payload_destroy(void* p) {
  poseidon_quic_connection_data_payload_t* payload =
      (poseidon_quic_connection_data_payload_t*)p;
  if (payload == NULL) return;
  if (payload->data != NULL) {
    free(payload->data);
  }
  free(payload);
}

static void _write_payload_destroy(void* p) {
  poseidon_quic_connection_write_payload_t* payload =
      (poseidon_quic_connection_write_payload_t*)p;
  if (payload == NULL) return;
  if (payload->data != NULL) {
    free(payload->data);
  }
  free(payload);
}

static void _close_payload_destroy(void* p) {
  free(p);
}

// ============================================================================
// HELPERS
// ============================================================================

// Tears the connection down: the stream is shut down, the connection
// is marked closed and the actor stops accepting work. The
// connection's memory is released by the listener's teardown, once the
// actor has been quiesced.
static void _connection_teardown(poseidon_quic_connection_t* connection) {
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }
  ATOMIC_STORE(&connection->closed, 1);
  connection->is_closing = 1;

  const struct QUIC_API_TABLE* api = poseidon_msquic_api();
  if (api != NULL && connection->stream != NULL) {
    api->StreamShutdown(connection->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,
                        0);
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

static void _connection_handle_frame(poseidon_quic_connection_t* connection) {
  struct cbor_load_result result;
  cbor_item_t* item =
      cbor_load(connection->read_buf + QUIC_FRAME_HEADER_SIZE,
                connection->read_len - QUIC_FRAME_HEADER_SIZE, &result);
  if (item == NULL) {
    log_warn("quic connection: failed to decode CBOR frame");
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
        poseidon_quic_connection_write(connection, resp_buf, resp_len);
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

static void _connection_decode_frames(poseidon_quic_connection_t* connection) {
  while (connection->read_len >= QUIC_FRAME_HEADER_SIZE) {
    uint32_t frame_len =
        ((uint32_t)connection->read_buf[0] << 24) |
        ((uint32_t)connection->read_buf[1] << 16) |
        ((uint32_t)connection->read_buf[2] << 8) |
        (uint32_t)connection->read_buf[3];
    if (frame_len == 0 || frame_len > QUIC_MAX_FRAME_SIZE) {
      log_error("quic connection: frame length %u out of range", frame_len);
      connection->read_len = 0;
      return;
    }

    size_t total_len = QUIC_FRAME_HEADER_SIZE + (size_t)frame_len;
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
// Hands the pending bytes to msquic. The buffer is handed over to
// msquic, which frees it when the send completes.

/* The QUIC_BUFFER and the bytes it points at travel in one allocation:
 * msquic keeps a pointer to the buffer and reads it on its worker
 * thread, so a buffer on this stack would be read after the call
 * returned. The SEND_COMPLETE event frees the whole block. */
typedef struct quic_send_block_t {
    QUIC_BUFFER buffer;
    uint8_t data[];
} quic_send_block_t;

static void _connection_send(poseidon_quic_connection_t* connection,
                             uint8_t* data, size_t len) {
  if (connection->stream == NULL || data == NULL) {
    return;
  }

  const struct QUIC_API_TABLE* api = poseidon_msquic_api();
  if (api == NULL) {
    free(data);
    return;
  }

  quic_send_block_t* block = (quic_send_block_t*)
      get_memory(sizeof(quic_send_block_t) + len);
  if (block == NULL) {
    free(data);
    return;
  }
  memcpy(block->data, data, len);
  free(data);
  block->buffer.Buffer = block->data;
  block->buffer.Length = (uint32_t)len;

  // msquic keeps the buffer until the send completes and frees the
  // block in the SEND_COMPLETE event, so the buffer must not be touched
  // here.
  QUIC_STATUS status = api->StreamSend(connection->stream, &block->buffer, 1,
                                        QUIC_SEND_FLAG_NONE, block);
  if (QUIC_FAILED(status)) {
    free(block);
  }
}

// ============================================================================
// DISPATCH (actor thread)
// ============================================================================

static void _quic_connection_dispatch(void* state, message_t* msg) {
  poseidon_quic_connection_t* connection = (poseidon_quic_connection_t*)state;
  if (connection == NULL) return;

  // The connection is gone: there is nothing left to do for it, the
  // queued messages are dropped.
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }

  switch (msg->type) {
    case POSEIDON_QUIC_CONNECTION_DATA: {
      poseidon_quic_connection_data_payload_t* payload =
          (poseidon_quic_connection_data_payload_t*)msg->payload;
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
          log_error("quic connection: failed to grow the receive buffer");
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

    case POSEIDON_QUIC_CONNECTION_WRITE: {
      poseidon_quic_connection_write_payload_t* payload =
          (poseidon_quic_connection_write_payload_t*)msg->payload;
      if (payload == NULL || payload->data == NULL) {
        break;
      }
      uint8_t* data = payload->data;
      payload->data = NULL;  // ownership transferred

      if (connection->is_closing || connection->stream == NULL ||
          !connection->stream_ready) {
        free(data);
        break;
      }
      _connection_send(connection, data, payload->len);
      break;
    }

    case POSEIDON_QUIC_CONNECTION_STREAM_READY: {
      // The stream accepted the outbound frames from now on.
      connection->stream_ready = 1;
      break;
    }

    case POSEIDON_QUIC_CONNECTION_PEER_SHUTDOWN: {
      // The peer is done sending: answer with a graceful shutdown of
      // our own.
      const struct QUIC_API_TABLE* api = poseidon_msquic_api();
      if (api != NULL && connection->stream != NULL) {
        api->StreamShutdown(connection->stream,
                            QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
      }
      break;
    }

    case POSEIDON_QUIC_CONNECTION_CLOSE: {
      poseidon_quic_connection_close_payload_t* payload =
          (poseidon_quic_connection_close_payload_t*)msg->payload;
      (void)payload;
      _connection_teardown(connection);
      break;
    }

    default:
      log_warn("quic connection: unknown message type %u", msg->type);
      break;
  }
}

// ============================================================================
// CREATE / DESTROY
// ============================================================================

bool poseidon_quic_connection_create(poseidon_transport_t* transport,
                                     client_session_t* session,
                                     struct QUIC_HANDLE* connection,
                                     struct QUIC_HANDLE* stream,
                                     uint32_t session_id,
                                     poseidon_quic_connection_t** out_conn) {
  if (out_conn == NULL) {
    return false;
  }
  *out_conn = NULL;

  if (transport == NULL || session == NULL) {
    return false;
  }

  poseidon_quic_connection_t* conn =
      (poseidon_quic_connection_t*)calloc(1, sizeof(poseidon_quic_connection_t));
  if (conn == NULL) {
    return false;
  }

  conn->transport = transport;
  conn->session = session;
  conn->connection = connection;
  conn->stream = stream;
  conn->session_id = session_id;
  conn->read_buf = (uint8_t*)get_clear_memory(QUIC_READ_BUF_SIZE);
  if (conn->read_buf == NULL) {
    free(conn);
    return false;
  }
  conn->read_len = 0;
  conn->read_cap = QUIC_READ_BUF_SIZE;
  conn->stream_ready = 0;
  conn->is_closing = 0;
  ATOMIC_STORE(&conn->closed, 0);

  actor_init(&conn->actor, conn, _quic_connection_dispatch, transport->pool);

  *out_conn = conn;
  return true;
}

void poseidon_quic_connection_destroy(poseidon_quic_connection_t* conn) {
  if (conn == NULL) {
    return;
  }

  // Stop the actor first: no dispatch can be running once this
  // returns, so the fields below are not touched concurrently.
  actor_destroy(&conn->actor);

  if (conn->read_buf != NULL) {
    free(conn->read_buf);
    conn->read_buf = NULL;
  }
  free(conn);
}

// ============================================================================
// PUBLIC WRITE / CLOSE
// ============================================================================

void poseidon_quic_connection_write(poseidon_quic_connection_t* conn,
                                    const uint8_t* data, size_t len) {
  if (conn == NULL) {
    return;
  }
  if (data == NULL && len > 0) {
    return;
  }
  if (len > QUIC_MAX_FRAME_SIZE) {
    log_warn("quic connection: refusing to send a %zu byte frame", len);
    return;
  }

  // Build the framed payload: 4 header bytes plus the body. The
  // connection's write actor owns the buffer from here on, and msquic
  // frees it once the send completes.
  size_t total_len = QUIC_FRAME_HEADER_SIZE + len;
  uint8_t* framed = (uint8_t*)get_clear_memory(total_len);
  if (framed == NULL) {
    return;
  }
  uint32_t be_len = htobe32((uint32_t)len);
  memcpy(framed, &be_len, QUIC_FRAME_HEADER_SIZE);
  if (len > 0) {
    memcpy(framed + QUIC_FRAME_HEADER_SIZE, data, len);
  }

  poseidon_quic_connection_write_payload_t* payload =
      (poseidon_quic_connection_write_payload_t*)
          malloc(sizeof(poseidon_quic_connection_write_payload_t));
  if (payload == NULL) {
    free(framed);
    return;
  }
  payload->data = framed;
  payload->len = total_len;

  message_t msg;
  msg.type = POSEIDON_QUIC_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&conn->actor, &msg);
}

void poseidon_quic_connection_close(poseidon_quic_connection_t* conn,
                                    bool graceful) {
  if (conn == NULL) {
    return;
  }
  (void)graceful;

  poseidon_quic_connection_close_payload_t* payload =
      (poseidon_quic_connection_close_payload_t*)
          malloc(sizeof(poseidon_quic_connection_close_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->graceful = graceful;

  message_t msg;
  msg.type = POSEIDON_QUIC_CONNECTION_CLOSE;
  msg.payload = payload;
  msg.payload_destroy = _close_payload_destroy;
  actor_send(&conn->actor, &msg);
}

// ============================================================================
// LISTENER HOOKS
// ============================================================================
//
// The transport's msquic callbacks run on msquic's own worker threads.
// They only hand the connection's actor what it needs; every decision
// is taken on the actor's thread.

// Announces that the stream is ready to carry the outbound frames.
void poseidon_quic_connection_stream_ready(poseidon_quic_connection_t* conn) {
  if (conn == NULL) {
    return;
  }

  message_t msg;
  msg.type = POSEIDON_QUIC_CONNECTION_STREAM_READY;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&conn->actor, &msg);
}

// Announces that the peer shut down its sending side.
void poseidon_quic_connection_peer_shutdown(poseidon_quic_connection_t* conn) {
  if (conn == NULL) {
    return;
  }

  message_t msg;
  msg.type = POSEIDON_QUIC_CONNECTION_PEER_SHUTDOWN;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&conn->actor, &msg);
}

// Hands the bytes msquic received to the connection's actor. The bytes
// must be a heap copy: the buffers msquic hands over belong to msquic.
// The connection takes ownership of the copy.
void poseidon_quic_connection_received(poseidon_quic_connection_t* conn,
                                        uint8_t* data, size_t len) {
  if (conn == NULL || data == NULL || len == 0) {
    if (data != NULL) {
      free(data);
    }
    return;
  }

  poseidon_quic_connection_data_payload_t* payload =
      (poseidon_quic_connection_data_payload_t*)
          malloc(sizeof(poseidon_quic_connection_data_payload_t));
  if (payload == NULL) {
    free(data);
    return;
  }
  payload->data = data;
  payload->len = len;

  message_t msg;
  msg.type = POSEIDON_QUIC_CONNECTION_DATA;
  msg.payload = payload;
  msg.payload_destroy = _data_payload_destroy;
  actor_send(&conn->actor, &msg);
}
