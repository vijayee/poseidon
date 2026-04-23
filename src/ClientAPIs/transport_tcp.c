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

#define TCP_MAX_CLIENTS 256
#define TCP_READ_BUF_SIZE 65536
#define TCP_FRAME_HEADER_SIZE 4

// Per-client connection state
typedef struct tcp_client_t {
    int fd;
    SSL* ssl;
    client_session_t* session;
    pd_watcher_t* watcher;
    uint8_t read_buf[TCP_READ_BUF_SIZE];
    size_t read_len;
    poseidon_transport_t* transport;
    bool ssl_connected;
} tcp_client_t;

// TCP transport internal state
typedef struct tcp_transport_data_t tcp_transport_data_t;

typedef struct {
    poseidon_transport_t* transport;
    tcp_transport_data_t* data;
} tcp_accept_ctx_t;

struct tcp_transport_data_t {
    int listen_fd;
    SSL_CTX* ssl_ctx;
    pd_loop_t* loop;
    pd_watcher_t* listen_watcher;
    tcp_accept_ctx_t* accept_ctx;
    tcp_client_t* clients[TCP_MAX_CLIENTS];
    size_t num_clients;
};

// ============================================================================
// FRAME PROTOCOL: same 4-byte length prefix as Unix transport
// ============================================================================

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= TCP_READ_BUF_SIZE;
}

static void write_frame_header(uint8_t* buf, uint32_t len) {
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
}

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

static void tcp_client_destroy(tcp_client_t* client) {
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

// ============================================================================
// FRAME PROCESSING
// ============================================================================

static void process_client_data(tcp_client_t* client) {
    while (client->read_len >= TCP_FRAME_HEADER_SIZE) {
        uint32_t frame_len = 0;
        if (!read_frame_header(client->read_buf, &frame_len)) {
            log_warn("tcp transport: invalid frame header, disconnecting client");
            return;
        }

        size_t total_len = TCP_FRAME_HEADER_SIZE + frame_len;
        if (client->read_len < total_len) break;

        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(client->read_buf + TCP_FRAME_HEADER_SIZE,
                                       frame_len, &result);
        if (item == NULL) {
            log_warn("tcp transport: failed to decode CBOR frame");
            memmove(client->read_buf, client->read_buf + total_len,
                    client->read_len - total_len);
            client->read_len -= total_len;
            continue;
        }

        client_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        if (client_protocol_decode(item, &frame) == 0) {
            cbor_item_t* response = NULL;
            int rc = client_session_handle_request(client->session, &frame, &response);
            if (response != NULL) {
                uint8_t* resp_buf = NULL;
                size_t resp_len = 0;
                if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
                    uint8_t header[TCP_FRAME_HEADER_SIZE];
                    write_frame_header(header, (uint32_t)resp_len);
                    if (client->ssl != NULL && client->ssl_connected) {
                        SSL_write(client->ssl, header, TCP_FRAME_HEADER_SIZE);
                        SSL_write(client->ssl, resp_buf, (int)resp_len);
                    } else {
                        send(client->fd, header, TCP_FRAME_HEADER_SIZE, MSG_NOSIGNAL);
                        send(client->fd, resp_buf, resp_len, MSG_NOSIGNAL);
                    }
                    free(resp_buf);
                }
                cbor_decref(&response);
            }
            (void)rc;
        }

        cbor_decref(&item);
        memmove(client->read_buf, client->read_buf + total_len,
                client->read_len - total_len);
        client->read_len -= total_len;
    }
}

// ============================================================================
// CLIENT DISCONNECT
// ============================================================================

static void tcp_client_disconnect(tcp_client_t* client) {
    if (client == NULL) return;

    poseidon_transport_t* transport = client->transport;
    tcp_transport_data_t* data = (tcp_transport_data_t*)transport->loop;

    // Clean up Quasar subscriptions before unregistering from session registry
    client_session_cleanup_subscriptions(client->session);
    poseidon_channel_manager_unregister_session(transport->manager, client->session);

    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] == client) {
            data->clients[i] = data->clients[data->num_clients - 1];
            data->clients[data->num_clients - 1] = NULL;
            data->num_clients--;
            break;
        }
    }

    tcp_client_destroy(client);
}

// ============================================================================
// READ CALLBACK
// ============================================================================

static void tcp_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                               pd_event_t events, void* user_data) {
    (void)loop;
    tcp_client_t* client = (tcp_client_t*)user_data;

    if (events & PD_EVENT_READ) {
        int fd = pd_watcher_get_fd(watcher);
        ssize_t bytes;

        if (client->ssl != NULL && client->ssl_connected) {
            bytes = SSL_read(client->ssl, client->read_buf + client->read_len,
                            TCP_READ_BUF_SIZE - client->read_len);
        } else {
            bytes = recv(fd, client->read_buf + client->read_len,
                        TCP_READ_BUF_SIZE - client->read_len, 0);
        }

        if (bytes <= 0) {
            tcp_client_disconnect(client);
            return;
        }

        client->read_len += bytes;
        process_client_data(client);
    }

    if (events & PD_EVENT_HANGUP) {
        tcp_client_disconnect(client);
        return;
    }

    if (events & PD_EVENT_ERROR) {
        log_warn("tcp transport: socket error");
        tcp_client_disconnect(client);
        return;
    }
}

// ============================================================================
// ACCEPT CALLBACK — TLS handshake then register read watcher
// ============================================================================

static void tcp_accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                pd_event_t events, void* user_data) {
    tcp_accept_ctx_t* ctx = (tcp_accept_ctx_t*)user_data;
    tcp_transport_data_t* data = ctx->data;
    poseidon_transport_t* transport = ctx->transport;
    int listen_fd = pd_watcher_get_fd(watcher);

    if (!(events & PD_EVENT_READ)) return;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        log_warn("tcp transport: accept failed: %s", strerror(errno));
        return;
    }

    if (data->num_clients >= TCP_MAX_CLIENTS) {
        log_warn("tcp transport: max clients reached, rejecting connection");
        close(client_fd);
        return;
    }

    // Create client
    static uint32_t next_session_id = 1;
    tcp_client_t* client = get_clear_memory(sizeof(tcp_client_t));
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
    client->session->client_fd = client_fd;
    client->session->transport = transport;
    client->read_len = 0;

    // Wrap with SSL if TLS is configured
    if (data->ssl_ctx != NULL) {
        client->ssl = SSL_new(data->ssl_ctx);
        if (client->ssl == NULL) {
            log_warn("tcp transport: SSL_new failed");
            client_session_destroy(client->session);
            free(client);
            close(client_fd);
            return;
        }
        SSL_set_fd(client->ssl, client_fd);
        int ssl_rc = SSL_accept(client->ssl);
        if (ssl_rc <= 0) {
            int ssl_err = SSL_get_error(client->ssl, ssl_rc);
            log_warn("tcp transport: TLS handshake failed (err=%d)", ssl_err);
            SSL_free(client->ssl);
            client_session_destroy(client->session);
            free(client);
            close(client_fd);
            return;
        }
        client->ssl_connected = true;
    }

    client->watcher = pd_watcher_create(loop, client_fd, PD_EVENT_READ,
                                         tcp_read_callback, client);
    if (client->watcher == NULL) {
        log_warn("tcp transport: failed to create watcher for client");
        tcp_client_destroy(client);
        return;
    }

    data->clients[data->num_clients++] = client;
    poseidon_channel_manager_register_session(transport->manager, client->session);
    log_info("tcp transport: client connected (fd=%d, total=%zu)",
             client_fd, data->num_clients);
}

// ============================================================================
// TRANSPORT THREAD
// ============================================================================

static void* tcp_transport_thread(void* arg) {
    poseidon_transport_t* transport = (poseidon_transport_t*)arg;
    tcp_transport_data_t* data = (tcp_transport_data_t*)transport->loop;

    log_info("tcp transport: event loop starting");
    pd_loop_run(data->loop);
    log_info("tcp transport: event loop stopped");

    return NULL;
}

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

static int tcp_transport_start(poseidon_transport_t* self) {
    tcp_transport_data_t* data = (tcp_transport_data_t*)self->loop;

    data->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data->listen_fd < 0) {
        log_error("tcp transport: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(data->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)(uintptr_t)self->name);

    if (bind(data->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("tcp transport: bind() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    if (listen(data->listen_fd, 16) < 0) {
        log_error("tcp transport: listen() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    tcp_accept_ctx_t* ctx = get_clear_memory(sizeof(tcp_accept_ctx_t));
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
                                              tcp_accept_callback, ctx);
    if (data->listen_watcher == NULL) {
        log_error("tcp transport: failed to create accept watcher");
        free(ctx);
        data->accept_ctx = NULL;
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    self->running = true;

    if (pthread_create(&self->thread, NULL, tcp_transport_thread, self) != 0) {
        log_error("tcp transport: failed to create thread");
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

static int tcp_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;

    self->running = false;
    tcp_transport_data_t* data = (tcp_transport_data_t*)self->loop;

    pd_loop_stop(data->loop);
    platform_join(self->thread);

    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            tcp_client_destroy(data->clients[i]);
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

static int tcp_transport_send(poseidon_transport_t* self, int client_id,
                                const uint8_t* data, size_t len) {
    (void)self;
    uint8_t header[TCP_FRAME_HEADER_SIZE];
    write_frame_header(header, (uint32_t)len);

    ssize_t h = send(client_id, header, TCP_FRAME_HEADER_SIZE, MSG_NOSIGNAL);
    ssize_t d = send(client_id, data, len, MSG_NOSIGNAL);
    return (h == TCP_FRAME_HEADER_SIZE && (size_t)d == len) ? 0 : -1;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_tcp_create(uint16_t port,
                                                      const char* cert_path,
                                                      const char* key_path,
                                                      poseidon_channel_manager_t* manager) {
    if (manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    tcp_transport_data_t* data = get_clear_memory(sizeof(tcp_transport_data_t));
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

    // Set up TLS if cert/key provided
    if (cert_path != NULL && key_path != NULL) {
        SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (ssl_ctx == NULL) {
            log_error("tcp transport: SSL_CTX_new failed");
            pd_loop_destroy(data->loop);
            free(data);
            free(transport);
            return NULL;
        }

        if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0 ||
            SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
            log_error("tcp transport: failed to load TLS cert/key");
            SSL_CTX_free(ssl_ctx);
            pd_loop_destroy(data->loop);
            free(data);
            free(transport);
            return NULL;
        }
        data->ssl_ctx = ssl_ctx;
    }

    transport->name = (const char*)(uintptr_t)port;
    transport->type = POSEIDON_TRANSPORT_TCP;
    transport->manager = manager;
    transport->loop = data;
    transport->start = tcp_transport_start;
    transport->stop = tcp_transport_stop;
    transport->send = tcp_transport_send;
    platform_lock_init(&transport->lock);

    return transport;
}