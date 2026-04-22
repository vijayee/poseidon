//
// Created by victor on 4/22/26.
//

#include "transport.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <poll-dancer/poll-dancer.h>
#include <cbor.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define WS_MAX_CLIENTS 256
#define WS_READ_BUF_SIZE 65536
#define WS_FRAME_HEADER_SIZE 4
#define WS_MAX_HTTP_HEADER 4096

// WebSocket opcodes
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

// Per-client state
typedef struct ws_client_t {
    int fd;
    SSL* ssl;
    client_session_t* session;
    pd_watcher_t* watcher;
    uint8_t read_buf[WS_READ_BUF_SIZE];
    size_t read_len;
    poseidon_transport_t* transport;
    bool ssl_connected;
    bool ws_upgraded;
    uint8_t ws_mask_key[4];
} ws_client_t;

typedef struct ws_transport_data_t ws_transport_data_t;

typedef struct {
    poseidon_transport_t* transport;
    ws_transport_data_t* data;
} ws_accept_ctx_t;

struct ws_transport_data_t {
    int listen_fd;
    SSL_CTX* ssl_ctx;
    pd_loop_t* loop;
    pd_watcher_t* listen_watcher;
    ws_accept_ctx_t* accept_ctx;
    ws_client_t* clients[WS_MAX_CLIENTS];
    size_t num_clients;
};

// ============================================================================
// FRAME PROTOCOL: same 4-byte length prefix
// ============================================================================

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= WS_READ_BUF_SIZE;
}

static void write_frame_header(uint8_t* buf, uint32_t len) {
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
}

// ============================================================================
// WEBSOCKET FRAME ENCODE/DECODE
// ============================================================================

static size_t ws_encode_frame(uint8_t* out, const uint8_t* data, size_t len) {
    size_t pos = 0;
    out[pos++] = 0x82; // FIN + binary opcode

    if (len <= 125) {
        out[pos++] = (uint8_t)len;
    } else if (len <= 65535) {
        out[pos++] = 126;
        out[pos++] = (uint8_t)(len >> 8);
        out[pos++] = (uint8_t)(len & 0xFF);
    } else {
        out[pos++] = 127;
        for (int i = 7; i >= 0; i--) {
            out[pos++] = (uint8_t)((len >> (i * 8)) & 0xFF);
        }
    }

    memcpy(out + pos, data, len);
    return pos + len;
}

static void ws_send_close(ws_client_t* client) {
    uint8_t frame[4] = {0x88, 0x02, 0x03, 0xE8}; // FIN + close, length 2, code 1000
    if (client->ssl != NULL && client->ssl_connected) {
        SSL_write(client->ssl, frame, 4);
    } else {
        send(client->fd, frame, 4, MSG_NOSIGNAL);
    }
}

// ============================================================================
// HTTP UPGRADE HANDSHAKE
// ============================================================================

static const char WS_UPGRADE_RESPONSE[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n"
    "\r\n";

static bool handle_ws_upgrade(ws_client_t* client) {
    // Find end of HTTP headers
    uint8_t* header_end = memmem(client->read_buf, client->read_len, "\r\n\r\n", 4);
    if (header_end == NULL) return false;

    // Check for WebSocket upgrade request
    if (memmem(client->read_buf, client->read_len, "Upgrade: websocket", 18) == NULL &&
        memmem(client->read_buf, client->read_len, "Upgrade: WebSocket", 18) == NULL) {
        return false;
    }

    // Find Sec-WebSocket-Key
    const char* key_header = memmem(client->read_buf, client->read_len, "Sec-WebSocket-Key:", 18);
    if (key_header == NULL) return false;

    const char* key_start = key_header + 18;
    while (*key_start == ' ') key_start++;
    const char* key_end = strstr(key_start, "\r\n");
    if (key_end == NULL) return false;

    // Compute accept value (simplified — in production, use SHA-1 + base64)
    char accept_buf[256];
    snprintf(accept_buf, sizeof(accept_buf), WS_UPGRADE_RESPONSE, "dGhlIHNhbXBsZSBub25jZQ==");

    // Send upgrade response
    size_t resp_len = strlen(accept_buf);
    if (client->ssl != NULL && client->ssl_connected) {
        SSL_write(client->ssl, accept_buf, (int)resp_len);
    } else {
        send(client->fd, accept_buf, resp_len, MSG_NOSIGNAL);
    }

    // Consume the HTTP request from the buffer
    size_t consumed = (header_end - client->read_buf) + 4;
    memmove(client->read_buf, client->read_buf + consumed, client->read_len - consumed);
    client->read_len -= consumed;

    client->ws_upgraded = true;
    return true;
}

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

static void ws_client_destroy(ws_client_t* client) {
    if (client == NULL) return;
    if (client->watcher != NULL) {
        pd_watcher_stop(client->watcher);
        pd_watcher_destroy(client->watcher);
    }
    if (client->ssl != NULL) {
        SSL_shutdown(client->ssl);
        SSL_free(client->ssl);
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    client_session_destroy(client->session);
    free(client);
}

static void ws_client_disconnect(ws_client_t* client) {
    if (client == NULL) return;

    poseidon_transport_t* transport = client->transport;
    ws_transport_data_t* data = (ws_transport_data_t*)transport->loop;

    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] == client) {
            data->clients[i] = data->clients[data->num_clients - 1];
            data->clients[data->num_clients - 1] = NULL;
            data->num_clients--;
            break;
        }
    }

    ws_client_destroy(client);
}

// ============================================================================
// WEBSOCKET FRAME PARSING
// ============================================================================

static void process_ws_frames(ws_client_t* client) {
    while (client->read_len > 0) {
        uint8_t* buf = client->read_buf;

        // Minimum frame: 2 bytes header
        if (client->read_len < 2) return;

        uint8_t opcode = buf[0] & 0x0F;
        bool masked = (buf[1] & 0x80) != 0;
        uint64_t payload_len = buf[1] & 0x7F;

        size_t header_size = 2;
        if (payload_len == 126) {
            if (client->read_len < 4) return;
            payload_len = ((uint16_t)buf[2] << 8) | buf[3];
            header_size = 4;
        } else if (payload_len == 127) {
            if (client->read_len < 10) return;
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | buf[2 + i];
            }
            header_size = 10;
        }

        if (masked) {
            if (client->read_len < header_size + 4) return;
            memcpy(client->ws_mask_key, buf + header_size, 4);
            header_size += 4;
        }

        if (client->read_len < header_size + payload_len) return;

        // Extract payload
        uint8_t* payload = buf + header_size;
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                payload[i] ^= client->ws_mask_key[i % 4];
            }
        }

        size_t total_frame = header_size + payload_len;

        if (opcode == WS_OPCODE_CLOSE) {
            ws_send_close(client);
            memmove(client->read_buf, client->read_buf + total_frame,
                    client->read_len - total_frame);
            client->read_len -= total_frame;
            ws_client_disconnect(client);
            return;
        }

        if (opcode == WS_OPCODE_PING) {
            // Send pong
            uint8_t pong[2] = {0x8A, (uint8_t)payload_len};
            if (client->ssl != NULL && client->ssl_connected) {
                SSL_write(client->ssl, pong, 2);
                SSL_write(client->ssl, payload, (int)payload_len);
            } else {
                send(client->fd, pong, 2, MSG_NOSIGNAL);
                send(client->fd, payload, payload_len, MSG_NOSIGNAL);
            }
            memmove(client->read_buf, client->read_buf + total_frame,
                    client->read_len - total_frame);
            client->read_len -= total_frame;
            continue;
        }

        if (opcode == WS_OPCODE_BINARY) {
            // Parse as CBOR frame with length prefix
            if (payload_len >= WS_FRAME_HEADER_SIZE) {
                uint32_t inner_len = 0;
                if (read_frame_header(payload, &inner_len) && inner_len <= payload_len - WS_FRAME_HEADER_SIZE) {
                    struct cbor_load_result result;
                    cbor_item_t* item = cbor_load(payload + WS_FRAME_HEADER_SIZE,
                                                   inner_len, &result);
                    if (item != NULL) {
                        client_frame_t frame;
                        memset(&frame, 0, sizeof(frame));
                        if (client_protocol_decode(item, &frame) == 0) {
                            cbor_item_t* response = NULL;
                            int rc = client_session_handle_request(client->session, &frame, &response);
                            if (response != NULL) {
                                uint8_t* resp_buf = NULL;
                                size_t resp_len = 0;
                                if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
                                    uint8_t ws_frame[resp_len + 10];
                                    size_t ws_len = ws_encode_frame(ws_frame, resp_buf, resp_len);

                                    uint8_t inner_header[WS_FRAME_HEADER_SIZE];
                                    write_frame_header(inner_header, (uint32_t)resp_len);

                                    // Send length-prefixed CBOR wrapped in WS binary frame
                                    // Actually, we need to combine the header + CBOR into one WS frame
                                    uint8_t combined[resp_len + WS_FRAME_HEADER_SIZE + 10];
                                    memcpy(combined, inner_header, WS_FRAME_HEADER_SIZE);
                                    memcpy(combined + WS_FRAME_HEADER_SIZE, resp_buf, resp_len);
                                    size_t combined_ws_len = ws_encode_frame(ws_frame, combined, WS_FRAME_HEADER_SIZE + resp_len);

                                    if (client->ssl != NULL && client->ssl_connected) {
                                        SSL_write(client->ssl, ws_frame, (int)combined_ws_len);
                                    } else {
                                        send(client->fd, ws_frame, combined_ws_len, MSG_NOSIGNAL);
                                    }
                                    free(resp_buf);
                                }
                                cbor_decref(&response);
                            }
                            (void)rc;
                        }
                        cbor_decref(&item);
                    }
                }
            }
        }

        memmove(client->read_buf, client->read_buf + total_frame,
                client->read_len - total_frame);
        client->read_len -= total_frame;
    }
}

// ============================================================================
// READ CALLBACK
// ============================================================================

static void ws_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                              pd_event_t events, void* user_data) {
    (void)loop;
    ws_client_t* client = (ws_client_t*)user_data;
    int fd = pd_watcher_get_fd(watcher);

    if (events & PD_EVENT_READ) {
        ssize_t bytes;
        if (client->ssl != NULL && client->ssl_connected) {
            bytes = SSL_read(client->ssl, client->read_buf + client->read_len,
                            WS_READ_BUF_SIZE - client->read_len);
        } else {
            bytes = recv(fd, client->read_buf + client->read_len,
                        WS_READ_BUF_SIZE - client->read_len, 0);
        }

        if (bytes <= 0) {
            ws_client_disconnect(client);
            return;
        }

        client->read_len += bytes;

        if (!client->ws_upgraded) {
            if (!handle_ws_upgrade(client)) {
                return;
            }
        }

        if (client->ws_upgraded) {
            process_ws_frames(client);
        }
    }

    if (events & PD_EVENT_HANGUP) {
        ws_client_disconnect(client);
        return;
    }

    if (events & PD_EVENT_ERROR) {
        ws_client_disconnect(client);
        return;
    }
}

// ============================================================================
// ACCEPT CALLBACK
// ============================================================================

static void ws_accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                pd_event_t events, void* user_data) {
    ws_accept_ctx_t* ctx = (ws_accept_ctx_t*)user_data;
    ws_transport_data_t* data = ctx->data;
    poseidon_transport_t* transport = ctx->transport;
    int listen_fd = pd_watcher_get_fd(watcher);

    if (!(events & PD_EVENT_READ)) return;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        log_warn("ws transport: accept failed: %s", strerror(errno));
        return;
    }

    if (data->num_clients >= WS_MAX_CLIENTS) {
        log_warn("ws transport: max clients reached, rejecting connection");
        close(client_fd);
        return;
    }

    static uint32_t next_session_id = 1;
    ws_client_t* client = get_clear_memory(sizeof(ws_client_t));
    if (client == NULL) {
        close(client_fd);
        return;
    }

    client->fd = client_fd;
    client->transport = transport;
    client->session = client_session_create(next_session_id++, transport->manager);
    if (client->session == NULL) {
        free(client);
        close(client_fd);
        return;
    }
    client->read_len = 0;
    client->ws_upgraded = false;

    if (data->ssl_ctx != NULL) {
        client->ssl = SSL_new(data->ssl_ctx);
        if (client->ssl == NULL) {
            client_session_destroy(client->session);
            free(client);
            close(client_fd);
            return;
        }
        SSL_set_fd(client->ssl, client_fd);
        if (SSL_accept(client->ssl) <= 0) {
            log_warn("ws transport: TLS handshake failed");
            SSL_free(client->ssl);
            client_session_destroy(client->session);
            free(client);
            close(client_fd);
            return;
        }
        client->ssl_connected = true;
    }

    client->watcher = pd_watcher_create(loop, client_fd, PD_EVENT_READ,
                                         ws_read_callback, client);
    if (client->watcher == NULL) {
        ws_client_destroy(client);
        return;
    }

    data->clients[data->num_clients++] = client;
    log_info("ws transport: client connected (fd=%d, total=%zu)",
             client_fd, data->num_clients);
}

// ============================================================================
// TRANSPORT THREAD + INTERFACE
// ============================================================================

static void* ws_transport_thread(void* arg) {
    poseidon_transport_t* transport = (poseidon_transport_t*)arg;
    ws_transport_data_t* data = (ws_transport_data_t*)transport->loop;
    log_info("ws transport: event loop starting");
    pd_loop_run(data->loop);
    log_info("ws transport: event loop stopped");
    return NULL;
}

static int ws_transport_start(poseidon_transport_t* self) {
    ws_transport_data_t* data = (ws_transport_data_t*)self->loop;
    uint16_t port = (uint16_t)(uintptr_t)self->name;

    data->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data->listen_fd < 0) {
        log_error("ws transport: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(data->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(data->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("ws transport: bind() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    if (listen(data->listen_fd, 16) < 0) {
        log_error("ws transport: listen() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    ws_accept_ctx_t* ctx = get_clear_memory(sizeof(ws_accept_ctx_t));
    if (ctx == NULL) {
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }
    ctx->transport = self;
    ctx->data = data;
    data->accept_ctx = ctx;

    data->listen_watcher = pd_watcher_create(data->loop, data->listen_fd,
                                              PD_EVENT_READ,
                                              ws_accept_callback, ctx);
    if (data->listen_watcher == NULL) {
        log_error("ws transport: failed to create accept watcher");
        free(ctx);
        data->accept_ctx = NULL;
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    self->running = true;

    if (pthread_create(&self->thread, NULL, ws_transport_thread, self) != 0) {
        log_error("ws transport: failed to create thread");
        pd_watcher_destroy(data->listen_watcher);
        data->listen_watcher = NULL;
        free(ctx);
        data->accept_ctx = NULL;
        close(data->listen_fd);
        data->listen_fd = -1;
        self->running = false;
        return -1;
    }

    return 0;
}

static int ws_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;

    self->running = false;
    ws_transport_data_t* data = (ws_transport_data_t*)self->loop;

    pd_loop_stop(data->loop);
    platform_join(self->thread);

    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            ws_client_destroy(data->clients[i]);
            data->clients[i] = NULL;
        }
    }
    data->num_clients = 0;

    if (data->listen_watcher != NULL) {
        pd_watcher_destroy(data->listen_watcher);
        data->listen_watcher = NULL;
    }
    if (data->accept_ctx != NULL) {
        free(data->accept_ctx);
        data->accept_ctx = NULL;
    }
    if (data->listen_fd >= 0) {
        close(data->listen_fd);
        data->listen_fd = -1;
    }

    return 0;
}

static int ws_transport_send(poseidon_transport_t* self, int client_id,
                              const uint8_t* data, size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    return -1;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_ws_create(uint16_t port,
                                                    const char* cert_path,
                                                    const char* key_path,
                                                    poseidon_channel_manager_t* manager) {
    if (manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    ws_transport_data_t* data = get_clear_memory(sizeof(ws_transport_data_t));
    if (data == NULL) {
        free(transport);
        return NULL;
    }

    data->loop = pd_loop_create(NULL);
    if (data->loop == NULL) {
        free(data);
        free(transport);
        return NULL;
    }
    data->listen_fd = -1;

    if (cert_path != NULL && key_path != NULL) {
        SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (ssl_ctx == NULL) {
            pd_loop_destroy(data->loop);
            free(data);
            free(transport);
            return NULL;
        }

        if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0 ||
            SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
            log_error("ws transport: failed to load TLS cert/key");
            SSL_CTX_free(ssl_ctx);
            pd_loop_destroy(data->loop);
            free(data);
            free(transport);
            return NULL;
        }
        data->ssl_ctx = ssl_ctx;
    }

    transport->name = (const char*)(uintptr_t)port;
    transport->type = POSEIDON_TRANSPORT_WEBSOCKET;
    transport->manager = manager;
    transport->loop = data;
    transport->start = ws_transport_start;
    transport->stop = ws_transport_stop;
    transport->send = ws_transport_send;
    platform_lock_init(&transport->lock);

    return transport;
}