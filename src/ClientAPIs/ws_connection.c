//
// ws_connection.c
//
// WebSocket per-connection actor.
//
// Every accepted socket becomes one poseidon_ws_connection_t whose actor
// runs on the transport's scheduler pool. The actor owns the
// connection's TLS session, its HTTP upgrade state, its WebSocket frame
// decoder and its pending outbound frames.
//
// Threading model:
//   - the I/O thread only reports that the socket became readable or
//     writable; it never touches the socket itself;
//   - the actor performs the read, the write, the TLS handshake and the
//     HTTP upgrade, because an SSL object may only be used from one
//     thread;
//   - every cross-thread hand-off (the session actor's outbound frame,
//     a close request) goes through actor_send, so the connection's
//     socket is only ever touched by its own actor.
//
// Wire protocol: a WebSocket binary frame carrying a 4-byte big-endian
// length prefix followed by a CBOR payload. The connection strips the
// WebSocket framing and the length prefix, decodes the CBOR document
// and hands it to the client session; the send path puts both back.
//

#include "ws_connection.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "client_session.h"
#include "transport_internal.h"

#include <poll-dancer/poll-dancer.h>

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define WS_READ_BUF_SIZE      65536
#define WS_FRAME_HEADER_SIZE  4
#define WS_MAX_FRAME_SIZE     WS_READ_BUF_SIZE

// The HTTP upgrade request may not grow past this. A client that sends
// more than this without the header terminator is not a client.
#define WS_MAX_HTTP_HEADER 4096

// WebSocket opcodes (RFC 6455 §5.2).
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

// The magic string of RFC 6455 §1.3: the accept value is the
// base64-encoded SHA-1 of the client's key concatenated with it.
#define WS_HANDSHAKE_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// ============================================================================
// PAYLOAD HELPERS
// ============================================================================

static void _write_payload_destroy(void* p) {
  poseidon_ws_connection_write_payload_t* payload =
      (poseidon_ws_connection_write_payload_t*)p;
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

static void _connection_update_watcher(poseidon_ws_connection_t* connection,
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
static void _connection_stop_watcher(poseidon_ws_connection_t* connection) {
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
static void _connection_close_fd(poseidon_ws_connection_t* connection) {
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
static void _connection_free_ssl(poseidon_ws_connection_t* connection) {
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
static void _connection_teardown(poseidon_ws_connection_t* connection) {
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
  poseidon_ws_connection_t* connection =
      (poseidon_ws_connection_t*)user_data;
  if (connection == NULL) {
    return;
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    // The peer is gone. Tell the actor so it can run the close path.
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? POSEIDON_WS_CONNECTION_HANGUP
                                           : POSEIDON_WS_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    return;
  }

  if (events & PD_EVENT_READ) {
    // The socket is readable: the actor pulls the bytes off it.
    message_t msg;
    msg.type = POSEIDON_WS_CONNECTION_READ;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
  }

  if (events & PD_EVENT_WRITE) {
    // The socket drained: retry the pending frame.
    message_t msg;
    msg.type = POSEIDON_WS_CONNECTION_WRITABLE;
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

static int _connection_handshake(poseidon_ws_connection_t* connection) {
  if (connection->ssl_ctx == NULL) {
    connection->handshake_done = 1;
    return 0;
  }

  if (connection->ssl == NULL) {
    connection->ssl = SSL_new(connection->ssl_ctx);
    if (connection->ssl == NULL) {
      log_warn("ws connection: SSL_new failed");
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

  log_warn("ws connection: TLS handshake failed (err=%d)", err);
  return -1;
}

// ============================================================================
// FRAME BUILDERS (actor thread)
// ============================================================================

// Builds a WebSocket frame: FIN + opcode, the length and the payload.
// The length form follows RFC 6455 §5.2: seven bits inline, 126 for a
// 16-bit length, 127 for a 64-bit one. A server never masks.
static buffer_t* _ws_build_frame(uint8_t opcode, const uint8_t* data,
                                 size_t len) {
  size_t header_size = 2;
  if (len > 65535) {
    header_size += 8;
  } else if (len > 125) {
    header_size += 2;
  }

  buffer_t* buf = buffer_create(header_size + len);
  if (buf == NULL) {
    return NULL;
  }

  uint8_t* out = buf->data;
  size_t pos = 0;
  out[pos++] = (uint8_t)(0x80 | opcode);
  if (len > 65535) {
    out[pos++] = 127;
    for (int i = 7; i >= 0; i--) {
      out[pos++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    }
  } else if (len > 125) {
    out[pos++] = 126;
    out[pos++] = (uint8_t)((len >> 8) & 0xFF);
    out[pos++] = (uint8_t)(len & 0xFF);
  } else {
    out[pos++] = (uint8_t)len;
  }
  if (len > 0 && data != NULL) {
    memcpy(out + pos, data, len);
  }
  return buf;
}

// Builds the binary frame that carries a client payload: the 4-byte
// big-endian length prefix, the payload, all inside one binary frame.
static buffer_t* _ws_build_binary(const uint8_t* data, size_t len) {
  size_t inner_len = WS_FRAME_HEADER_SIZE + len;
  uint8_t* inner = (uint8_t*)malloc(inner_len);
  if (inner == NULL) {
    return NULL;
  }
  uint32_t be_len = htobe32((uint32_t)len);
  memcpy(inner, &be_len, WS_FRAME_HEADER_SIZE);
  if (len > 0 && data != NULL) {
    memcpy(inner + WS_FRAME_HEADER_SIZE, data, len);
  }

  buffer_t* frame = _ws_build_frame(WS_OPCODE_BINARY, inner, inner_len);
  free(inner);
  return frame;
}

// Builds the close frame with the "normal closure" status code (1000).
static buffer_t* _ws_build_close(void) {
  uint8_t payload[2];
  payload[0] = 0x03;  // 1000, big endian
  payload[1] = 0xE8;
  return _ws_build_frame(WS_OPCODE_CLOSE, payload, sizeof(payload));
}

// ============================================================================
// HTTP UPGRADE (actor thread)
// ============================================================================
//
// Computes the Sec-WebSocket-Accept value of RFC 6455 §4.2.2: the
// base64-encoded SHA-1 of the client's Sec-WebSocket-Key concatenated
// with the handshake GUID.
static bool _ws_compute_accept(const char* key, char* out, size_t out_len) {
  char material[256];
  int material_len =
      snprintf(material, sizeof(material), "%s%s", key, WS_HANDSHAKE_GUID);
  if (material_len <= 0 || (size_t)material_len >= sizeof(material)) {
    return false;
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_Digest(material, (size_t)material_len, digest, &digest_len,
                 EVP_sha1(), NULL) != 1) {
    return false;
  }

  unsigned char encoded[4 * ((EVP_MAX_MD_SIZE + 2) / 3) + 1];
  int encoded_len = EVP_EncodeBlock(encoded, digest, digest_len);
  if (encoded_len < 0 || (size_t)encoded_len + 1 > out_len) {
    return false;
  }
  memcpy(out, encoded, (size_t)encoded_len);
  out[encoded_len] = '\0';
  return true;
}

// Builds the 101 response that completes the upgrade.
static buffer_t* _ws_build_upgrade_response(const char* accept) {
  char response[512];
  int len = snprintf(response, sizeof(response),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n",
                     accept);
  if (len <= 0 || (size_t)len >= sizeof(response)) {
    return NULL;
  }
  return buffer_create_from_pointer_copy((uint8_t*)response, (size_t)len);
}

// Runs the HTTP upgrade over the bytes collected in the connection's
// receive buffer.
//
// Returns 1 when the upgrade is done, 0 when more bytes are needed and
// -1 when the request is not a usable WebSocket upgrade.
static int _ws_handle_upgrade(poseidon_ws_connection_t* connection) {
  uint8_t* header_end =
      (uint8_t*)memmem(connection->read_buf, connection->read_len, "\r\n\r\n", 4);
  if (header_end == NULL) {
    if (connection->read_len > WS_MAX_HTTP_HEADER) {
      log_error("ws connection: the upgrade request is too large");
      return -1;
    }
    // The request has not arrived in full yet.
    return 0;
  }

  if (memmem(connection->read_buf, connection->read_len, "Upgrade: websocket", 18) == NULL &&
      memmem(connection->read_buf, connection->read_len, "Upgrade: WebSocket", 18) == NULL) {
    log_error("ws connection: the request is not a websocket upgrade");
    return -1;
  }

  const char* key_header =
      (const char*)memmem(connection->read_buf, connection->read_len,
                          "Sec-WebSocket-Key:", 18);
  if (key_header == NULL) {
    log_error("ws connection: the upgrade request has no Sec-WebSocket-Key");
    return -1;
  }

  // The receive buffer is not NUL-terminated, so every scan over it is
  // bounded by the number of bytes that are actually in it.
  const char* buffer_end =
      (const char*)connection->read_buf + connection->read_len;

  const char* key_start = key_header + 18;
  while (key_start < buffer_end && *key_start == ' ') key_start++;
  if (key_start >= buffer_end) {
    log_error("ws connection: the Sec-WebSocket-Key is malformed");
    return -1;
  }

  const char* key_end =
      (const char*)memmem(key_start, (size_t)(buffer_end - key_start), "\r\n", 2);
  if (key_end == NULL) {
    log_error("ws connection: the Sec-WebSocket-Key is malformed");
    return -1;
  }

  char key[128];
  size_t key_len = (size_t)(key_end - key_start);
  if (key_len == 0 || key_len >= sizeof(key)) {
    log_error("ws connection: the Sec-WebSocket-Key is malformed");
    return -1;
  }
  memcpy(key, key_start, key_len);
  key[key_len] = '\0';

  char accept[64];
  if (!_ws_compute_accept(key, accept, sizeof(accept))) {
    log_error("ws connection: failed to compute the Sec-WebSocket-Accept");
    return -1;
  }

  buffer_t* response = _ws_build_upgrade_response(accept);
  if (response == NULL) {
    return -1;
  }

  // Consume the HTTP request from the buffer. Whatever follows it in
  // the same read stays in place and is handed to the WebSocket frame
  // decoder.
  size_t consumed = (size_t)(header_end - connection->read_buf) + 4;
  memmove(connection->read_buf, connection->read_buf + consumed,
          connection->read_len - consumed);
  connection->read_len -= consumed;

  // The response goes on the write queue, so it is ordered before the
  // first WebSocket frame.
  poseidon_ws_connection_write_payload_t* payload =
      (poseidon_ws_connection_write_payload_t*)
          malloc(sizeof(poseidon_ws_connection_write_payload_t));
  if (payload == NULL) {
    buffer_destroy(response);
    return -1;
  }
  payload->buffer = response;

  message_t msg;
  msg.type = POSEIDON_WS_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&connection->actor, &msg);

  connection->upgraded = 1;
  return 1;
}

// ============================================================================
// FRAME HANDLING (actor thread)
// ============================================================================
//
// A binary frame carries a CBOR document that the client session
// answers. The session's answer is framed again and queued on the
// connection's own write path, so the reply leaves through the same
// serialized writer as every other outbound frame.

static void _ws_handle_binary(poseidon_ws_connection_t* connection,
                               uint8_t* payload, size_t payload_len) {
  if (payload_len < WS_FRAME_HEADER_SIZE) {
    return;
  }

  // The binary frame carries the same length-prefixed CBOR document the
  // other backends use.
  uint32_t inner_len = ((uint32_t)payload[0] << 24) |
                       ((uint32_t)payload[1] << 16) |
                       ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
  if (inner_len == 0 ||
      inner_len > payload_len - WS_FRAME_HEADER_SIZE) {
    log_warn("ws connection: invalid inner frame length %u", inner_len);
    return;
  }

  struct cbor_load_result result;
  cbor_item_t* item = cbor_load(payload + WS_FRAME_HEADER_SIZE, inner_len,
                                 &result);
  if (item == NULL) {
    log_warn("ws connection: failed to decode CBOR frame");
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
        poseidon_ws_connection_write(connection, resp_buf, resp_len);
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
// Decodes the WebSocket frames in the connection's receive buffer. The
// bytes are consumed in place, so a frame that is only half in the
// buffer stays there until the rest of it arrives.
//
// Returns 0 on success and -1 on a protocol error.

static int _ws_process_frames(poseidon_ws_connection_t* connection) {
  while (connection->read_len > 0) {
    uint8_t* buf = connection->read_buf;

    // The shortest frame is a two byte header.
    if (connection->read_len < 2) {
      return 0;
    }

    uint8_t opcode = buf[0] & 0x0F;
    bool masked = (buf[1] & 0x80) != 0;
    uint64_t payload_len = buf[1] & 0x7F;
    size_t header_size = 2;

    if (payload_len == 126) {
      if (connection->read_len < 4) {
        return 0;
      }
      payload_len = ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
      header_size = 4;
    } else if (payload_len == 127) {
      if (connection->read_len < 10) {
        return 0;
      }
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | (uint64_t)buf[2 + i];
      }
      header_size = 10;
    }

    // A length that does not fit in the receive buffer is a protocol
    // error, not a frame that is still arriving: the peer announced
    // more than this connection can ever hold.
    if (payload_len > WS_MAX_FRAME_SIZE) {
      log_error("ws connection: frame payload of %llu bytes is out of range",
                (unsigned long long)payload_len);
      return -1;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
      if (connection->read_len < header_size + 4) {
        return 0;
      }
      memcpy(mask, buf + header_size, 4);
      header_size += 4;
    }

    if (connection->read_len < header_size + (size_t)payload_len) {
      // The frame is only half in the buffer: wait for the rest.
      return 0;
    }

    uint8_t* payload = buf + header_size;
    if (masked) {
      // A client masks its frames; the mask is applied in place.
      for (size_t i = 0; i < (size_t)payload_len; i++) {
        payload[i] ^= mask[i % 4];
      }
    }

    size_t total_frame = header_size + (size_t)payload_len;

    if (opcode == WS_OPCODE_CLOSE) {
      // Answer with a close frame and tear the connection down.
      buffer_t* close_frame = _ws_build_close();
      if (close_frame != NULL) {
        poseidon_ws_connection_write_payload_t* payload_msg =
            (poseidon_ws_connection_write_payload_t*)
                malloc(sizeof(poseidon_ws_connection_write_payload_t));
        if (payload_msg != NULL) {
          payload_msg->buffer = close_frame;
          message_t msg;
          msg.type = POSEIDON_WS_CONNECTION_WRITE;
          msg.payload = payload_msg;
          msg.payload_destroy = _write_payload_destroy;
          actor_send(&connection->actor, &msg);
        } else {
          buffer_destroy(close_frame);
        }
      }
      memmove(buf, buf + total_frame, connection->read_len - total_frame);
      connection->read_len -= total_frame;
      return -1;
    }

    if (opcode == WS_OPCODE_PING) {
      // Answer with a pong carrying the same payload.
      buffer_t* pong = _ws_build_frame(WS_OPCODE_PONG, payload,
                                       (size_t)payload_len);
      if (pong != NULL) {
        poseidon_ws_connection_write_payload_t* payload_msg =
            (poseidon_ws_connection_write_payload_t*)
                malloc(sizeof(poseidon_ws_connection_write_payload_t));
        if (payload_msg != NULL) {
          payload_msg->buffer = pong;
          message_t msg;
          msg.type = POSEIDON_WS_CONNECTION_WRITE;
          msg.payload = payload_msg;
          msg.payload_destroy = _write_payload_destroy;
          actor_send(&connection->actor, &msg);
        } else {
          buffer_destroy(pong);
        }
      }
    } else if (opcode == WS_OPCODE_BINARY) {
      _ws_handle_binary(connection, payload, (size_t)payload_len);
    }

    memmove(buf, buf + total_frame, connection->read_len - total_frame);
    connection->read_len -= total_frame;
  }
  return 0;
}

// ============================================================================
// SOCKET DRAIN (actor thread)
// ============================================================================
//
// Pulls the bytes off the socket into the connection's receive buffer
// and runs the upgrade and the frame decoder over them.

static void _connection_drain(poseidon_ws_connection_t* connection) {
  if (connection->sock == NULL) {
    return;
  }

  if (connection->read_len >= WS_READ_BUF_SIZE) {
    // The receive buffer is full and no complete frame was consumed:
    // the peer is sending more than this connection can hold.
    log_error("ws connection: the receive buffer is full, dropping the "
              "connection");
    connection->is_closing = 1;
    _connection_teardown(connection);
    return;
  }

  ssize_t bytes_read;
  if (connection->ssl != NULL) {
    bytes_read = SSL_read(connection->ssl,
                          connection->read_buf + connection->read_len,
                          WS_READ_BUF_SIZE - connection->read_len);
  } else {
    bytes_read = platform_socket_recv(connection->sock,
                                      connection->read_buf + connection->read_len,
                                      WS_READ_BUF_SIZE - connection->read_len);
  }

  if (bytes_read > 0) {
    connection->read_len += (size_t)bytes_read;
  } else if (bytes_read == 0) {
    // Clean EOF: the peer closed its side.
    connection->is_closing = 1;
    _connection_teardown(connection);
    return;
  } else {
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
      log_warn("ws connection: SSL_read failed (err=%d)", err);
      connection->is_closing = 1;
      _connection_teardown(connection);
      return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Nothing more to read right now.
      return;
    }
    log_warn("ws connection: recv failed (errno=%d)", errno);
    connection->is_closing = 1;
    _connection_teardown(connection);
    return;
  }

  if (!connection->upgraded) {
    int rc = _ws_handle_upgrade(connection);
    if (rc < 0) {
      connection->is_closing = 1;
      _connection_teardown(connection);
      return;
    }
    if (rc == 0) {
      // The upgrade request has not arrived in full yet.
      return;
    }
  }

  if (_ws_process_frames(connection) != 0) {
    connection->is_closing = 1;
    _connection_teardown(connection);
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

static bool _connection_flush(poseidon_ws_connection_t* connection) {
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
        log_warn("ws connection: SSL_write failed (err=%d)", err);
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
        log_warn("ws connection: send failed (errno=%d)", errno);
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

static void _ws_connection_dispatch(void* state, message_t* msg) {
  poseidon_ws_connection_t* connection = (poseidon_ws_connection_t*)state;
  if (connection == NULL) return;

  // The socket is gone: there is nothing left to do for this
  // connection, the queued messages are dropped.
  if (ATOMIC_LOAD(&connection->closed)) {
    return;
  }

  switch (msg->type) {
    case POSEIDON_WS_CONNECTION_READ: {
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

    case POSEIDON_WS_CONNECTION_WRITABLE: {
      // The socket drained: retry the pending frame.
      bool drained = _connection_flush(connection);
      if (drained && connection->is_closing) {
        _connection_teardown(connection);
      }
      break;
    }

    case POSEIDON_WS_CONNECTION_WRITE: {
      poseidon_ws_connection_write_payload_t* payload =
          (poseidon_ws_connection_write_payload_t*)msg->payload;
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
        buffer_t* combined = buffer_concat(connection->write_queue, buf);
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

    case POSEIDON_WS_CONNECTION_CLOSE: {
      poseidon_ws_connection_close_payload_t* payload =
          (poseidon_ws_connection_close_payload_t*)msg->payload;
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

    case POSEIDON_WS_CONNECTION_HANGUP:
    case POSEIDON_WS_CONNECTION_ERROR: {
      // The peer is gone. Drop what is pending and tear the connection
      // down.
      connection->is_closing = 1;
      _connection_teardown(connection);
      break;
    }

    default:
      log_warn("ws connection: unknown message type %u", msg->type);
      break;
  }
}

// ============================================================================
// CREATE
// ============================================================================

bool poseidon_ws_connection_create(poseidon_transport_internal_t* transport,
                                    platform_socket_t* sock,
                                    client_session_t* session,
                                    struct ssl_ctx_st* ssl_ctx,
                                    poseidon_ws_connection_t** out_conn) {
  if (out_conn == NULL) {
    return false;
  }
  *out_conn = NULL;

  if (transport == NULL || sock == NULL) {
    return false;
  }

  poseidon_ws_connection_t* connection =
      (poseidon_ws_connection_t*)calloc(1, sizeof(poseidon_ws_connection_t));
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
  connection->upgraded = 0;
  connection->read_len = 0;
  connection->write_buffer = NULL;
  connection->write_queue = NULL;
  connection->write_pending = 0;
  connection->is_closing = 0;
  ATOMIC_STORE(&connection->closed, 0);

  actor_init(&connection->actor, connection, _ws_connection_dispatch,
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

void poseidon_ws_connection_write(poseidon_ws_connection_t* connection,
                                   const uint8_t* data, size_t len) {
  if (connection == NULL) {
    return;
  }
  if (data == NULL && len > 0) {
    return;
  }
  if (len > WS_MAX_FRAME_SIZE) {
    log_warn("ws connection: refusing to send a %zu byte frame", len);
    return;
  }

  // The binary frame carries the length prefix and the payload.
  buffer_t* frame = _ws_build_binary(data, len);
  if (frame == NULL) {
    return;
  }

  poseidon_ws_connection_write_payload_t* payload =
      (poseidon_ws_connection_write_payload_t*)
          malloc(sizeof(poseidon_ws_connection_write_payload_t));
  if (payload == NULL) {
    buffer_destroy(frame);
    return;
  }
  payload->buffer = frame;

  message_t msg;
  msg.type = POSEIDON_WS_CONNECTION_WRITE;
  msg.payload = payload;
  msg.payload_destroy = _write_payload_destroy;
  actor_send(&connection->actor, &msg);
}

void poseidon_ws_connection_close(poseidon_ws_connection_t* connection,
                                   bool graceful) {
  if (connection == NULL) {
    return;
  }

  poseidon_ws_connection_close_payload_t* payload =
      (poseidon_ws_connection_close_payload_t*)
          malloc(sizeof(poseidon_ws_connection_close_payload_t));
  if (payload == NULL) {
    return;
  }
  payload->graceful = graceful;

  message_t msg;
  msg.type = POSEIDON_WS_CONNECTION_CLOSE;
  msg.payload = payload;
  msg.payload_destroy = _close_payload_destroy;
  actor_send(&connection->actor, &msg);
}
