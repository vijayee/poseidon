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
#include <sys/un.h>
#include <errno.h>
#include <pthread.h>

#define UNIX_MAX_CLIENTS 128
#define UNIX_READ_BUF_SIZE 65536
#define UNIX_FRAME_HEADER_SIZE 4

// Per-client connection state
typedef struct unix_client_t {
    int fd;
    client_session_t* session;
    pd_watcher_t* watcher;
    uint8_t read_buf[UNIX_READ_BUF_SIZE];
    size_t read_len;
    poseidon_transport_t* transport;
} unix_client_t;

// Unix transport state (forward-declared for accept context)
typedef struct unix_transport_data_t unix_transport_data_t;

// Accept callback context
typedef struct {
    poseidon_transport_t* transport;
    unix_transport_data_t* data;
} unix_accept_ctx_t;

// Unix transport state
struct unix_transport_data_t {
    int listen_fd;
    pd_loop_t* loop;
    pd_watcher_t* listen_watcher;
    unix_accept_ctx_t* accept_ctx;
    unix_client_t* clients[UNIX_MAX_CLIENTS];
    size_t num_clients;
};

// ============================================================================
// FRAME PROTOCOL: 4-byte length prefix (network byte order) + CBOR payload
// ============================================================================

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= UNIX_READ_BUF_SIZE;
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

static unix_client_t* unix_client_create(int fd, poseidon_transport_t* transport) {
    static uint32_t next_session_id = 1;
    unix_client_t* client = get_clear_memory(sizeof(unix_client_t));
    if (client == NULL) return NULL;

    client->fd = fd;
    client->transport = transport;
    client->session = client_session_create(next_session_id++, transport->manager);
    if (client->session == NULL) {
        free(client);
        return NULL;
    }
    client->session->client_fd = fd;
    client->session->transport = transport;
    client->read_len = 0;

    return client;
}

static void unix_client_destroy(unix_client_t* client) {
    if (client == NULL) return;
    if (client->watcher != NULL) {
        pd_watcher_stop(client->watcher);
        pd_watcher_destroy(client->watcher);
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    client_session_destroy(client->session);
    free(client);
}

// ============================================================================
// READ CALLBACK — process incoming CBOR frames
// ============================================================================

static void unix_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                pd_event_t events, void* user_data);

static void process_client_data(unix_client_t* client) {
    while (client->read_len >= UNIX_FRAME_HEADER_SIZE) {
        uint32_t frame_len = 0;
        if (!read_frame_header(client->read_buf, &frame_len)) {
            log_warn("unix transport: invalid frame header, disconnecting client");
            return;
        }

        size_t total_len = UNIX_FRAME_HEADER_SIZE + frame_len;
        if (client->read_len < total_len) break;

        // Decode CBOR frame
        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(client->read_buf + UNIX_FRAME_HEADER_SIZE,
                                       frame_len, &result);
        if (item == NULL) {
            log_warn("unix transport: failed to decode CBOR frame");
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
                // Serialize and send response
                uint8_t* resp_buf = NULL;
                size_t resp_len = 0;
                if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
                    uint8_t header[UNIX_FRAME_HEADER_SIZE];
                    write_frame_header(header, (uint32_t)resp_len);
                    send(client->fd, header, UNIX_FRAME_HEADER_SIZE, MSG_NOSIGNAL);
                    send(client->fd, resp_buf, resp_len, MSG_NOSIGNAL);
                    free(resp_buf);
                }
                cbor_decref(&response);
            }
            (void)rc;
        }

        cbor_decref(&item);

        // Shift remaining data
        memmove(client->read_buf, client->read_buf + total_len,
                client->read_len - total_len);
        client->read_len -= total_len;
    }
}

static void unix_client_disconnect(unix_client_t* client) {
    if (client == NULL) return;

    poseidon_transport_t* transport = client->transport;
    unix_transport_data_t* data = (unix_transport_data_t*)transport->loop;

    // Clean up Quasar subscriptions before unregistering from session registry
    client_session_cleanup_subscriptions(client->session);
    poseidon_channel_manager_unregister_session(transport->manager, client->session);

    // Remove from client list
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] == client) {
            data->clients[i] = data->clients[data->num_clients - 1];
            data->clients[data->num_clients - 1] = NULL;
            data->num_clients--;
            break;
        }
    }

    unix_client_destroy(client);
}

static void unix_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                pd_event_t events, void* user_data) {
    (void)loop;
    unix_client_t* client = (unix_client_t*)user_data;
    int fd = pd_watcher_get_fd(watcher);

    if (events & PD_EVENT_READ) {
        ssize_t bytes = recv(fd, client->read_buf + client->read_len,
                              UNIX_READ_BUF_SIZE - client->read_len, 0);
        if (bytes <= 0) {
            if (bytes == 0) {
                log_debug("unix transport: client disconnected (fd=%d)", fd);
            } else {
                log_debug("unix transport: recv error on fd=%d: %s", fd, strerror(errno));
            }
            unix_client_disconnect(client);
            return;
        }

        client->read_len += bytes;
        process_client_data(client);
    }

    if (events & PD_EVENT_HANGUP) {
        log_debug("unix transport: client hangup (fd=%d)", fd);
        unix_client_disconnect(client);
        return;
    }

    if (events & PD_EVENT_ERROR) {
        log_warn("unix transport: socket error on fd=%d", fd);
        unix_client_disconnect(client);
        return;
    }
}

// ============================================================================
// ACCEPT CALLBACK
// ============================================================================

static void unix_accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                  pd_event_t events, void* user_data) {
    unix_accept_ctx_t* ctx = (unix_accept_ctx_t*)user_data;
    unix_transport_data_t* data = ctx->data;
    poseidon_transport_t* transport = ctx->transport;
    int listen_fd = pd_watcher_get_fd(watcher);

    if (!(events & PD_EVENT_READ)) return;

    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        log_warn("unix transport: accept failed: %s", strerror(errno));
        return;
    }

    if (data->num_clients >= UNIX_MAX_CLIENTS) {
        log_warn("unix transport: max clients reached, rejecting connection");
        close(client_fd);
        return;
    }

    unix_client_t* client = unix_client_create(client_fd, transport);
    if (client == NULL) {
        log_warn("unix transport: failed to create client session");
        close(client_fd);
        return;
    }

    client->watcher = pd_watcher_create(loop, client_fd, PD_EVENT_READ,
                                         unix_read_callback, client);
    if (client->watcher == NULL) {
        log_warn("unix transport: failed to create watcher for client fd=%d", client_fd);
        unix_client_destroy(client);
        return;
    }

    data->clients[data->num_clients++] = client;
    poseidon_channel_manager_register_session(transport->manager, client->session);
    log_info("unix transport: client connected (fd=%d, total=%zu)",
             client_fd, data->num_clients);
}

// ============================================================================
// TRANSPORT THREAD
// ============================================================================

static void* unix_transport_thread(void* arg) {
    poseidon_transport_t* transport = (poseidon_transport_t*)arg;
    unix_transport_data_t* data = (unix_transport_data_t*)transport->loop;

    log_info("unix transport: event loop starting");
    pd_loop_run(data->loop);
    log_info("unix transport: event loop stopped");

    return NULL;
}

// ============================================================================
// TRANSPORT INTERFACE IMPLEMENTATION
// ============================================================================

static int unix_transport_start(poseidon_transport_t* self) {
    unix_transport_data_t* data = (unix_transport_data_t*)self->loop;

    // Create listening socket
    data->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (data->listen_fd < 0) {
        log_error("unix transport: socket() failed: %s", strerror(errno));
        return -1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(data->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, self->name, sizeof(addr.sun_path) - 1);

    // Remove existing socket file
    unlink(addr.sun_path);

    if (bind(data->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("unix transport: bind() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        return -1;
    }

    if (listen(data->listen_fd, 16) < 0) {
        log_error("unix transport: listen() failed: %s", strerror(errno));
        close(data->listen_fd);
        data->listen_fd = -1;
        unlink(self->name);
        return -1;
    }

    // Create accept context
    unix_accept_ctx_t* ctx = get_clear_memory(sizeof(unix_accept_ctx_t));
    if (ctx == NULL) {
        close(data->listen_fd);
        data->listen_fd = -1;
        unlink(self->name);
        return -1;
    }
    ctx->transport = self;
    ctx->data = data;
    data->accept_ctx = ctx;

    // Create accept watcher
    data->listen_watcher = pd_watcher_create(data->loop, data->listen_fd,
                                              PD_EVENT_READ,
                                              unix_accept_callback, ctx);
    if (data->listen_watcher == NULL) {
        log_error("unix transport: failed to create accept watcher");
        free(ctx);
        close(data->listen_fd);
        data->listen_fd = -1;
        unlink(self->name);
        return -1;
    }

    self->running = true;

    // Create thread
    if (pthread_create(&self->thread, NULL, unix_transport_thread, self) != 0) {
        log_error("unix transport: failed to create thread");
        pd_watcher_destroy(data->listen_watcher);
        data->listen_watcher = NULL;
        free(data->accept_ctx);
        data->accept_ctx = NULL;
        close(data->listen_fd);
        data->listen_fd = -1;
        unlink(self->name);
        self->running = false;
        return -1;
    }

    return 0;
}

static int unix_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;

    self->running = false;
    unix_transport_data_t* data = (unix_transport_data_t*)self->loop;

    // Stop the event loop
    pd_loop_stop(data->loop);

    // Wait for thread to finish
    platform_join(self->thread);

    // Clean up clients
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            unix_client_destroy(data->clients[i]);
            data->clients[i] = NULL;
        }
    }
    data->num_clients = 0;

    // Clean up listening socket
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
        unlink(self->name);
        data->listen_fd = -1;
    }

    return 0;
}

static int unix_transport_send(poseidon_transport_t* self, int client_id,
                                const uint8_t* data, size_t len) {
    (void)self;
    // client_id is the file descriptor in this implementation
    uint8_t header[UNIX_FRAME_HEADER_SIZE];
    write_frame_header(header, (uint32_t)len);

    ssize_t h = send(client_id, header, UNIX_FRAME_HEADER_SIZE, MSG_NOSIGNAL);
    ssize_t d = send(client_id, data, len, MSG_NOSIGNAL);
    return (h == UNIX_FRAME_HEADER_SIZE && (size_t)d == len) ? 0 : -1;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_config_t poseidon_transport_config_defaults(void) {
    poseidon_transport_config_t config;
    memset(&config, 0, sizeof(config));
    config.enable_unix = true;
    config.unix_socket_path = "/var/run/poseidond.sock";
    config.tcp_port = 9090;
    config.ws_port = 9091;
    config.quic_port = 9092;
    config.wt_port = 9093;
    return config;
}

poseidon_transport_t* poseidon_transport_unix_create(const char* socket_path,
                                                      poseidon_channel_manager_t* manager) {
    if (socket_path == NULL || manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    unix_transport_data_t* data = get_clear_memory(sizeof(unix_transport_data_t));
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

    transport->name = socket_path;
    transport->type = POSEIDON_TRANSPORT_UNIX_SOCKET;
    transport->manager = manager;
    transport->loop = data;
    transport->start = unix_transport_start;
    transport->stop = unix_transport_stop;
    transport->send = unix_transport_send;
    platform_lock_init(&transport->lock);

    return transport;
}

void poseidon_transport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    if (transport->destroy != NULL) {
        transport->destroy(transport);
        return;
    }

    if (transport->running) {
        transport->stop(transport);
    }

    unix_transport_data_t* data = (unix_transport_data_t*)transport->loop;
    if (data != NULL) {
        if (data->loop != NULL) {
            pd_loop_destroy(data->loop);
        }
        free(data);
    }

    platform_lock_destroy(&transport->lock);
    free(transport);
}