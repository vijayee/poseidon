//
// Created by victor on 4/23/26.
//
// WebTransport server transport — uses libwtf (HTTP/3 + WebTransport over QUIC)
// for browser-compatible connections. No poll-dancer event loop; libwtf manages
// its own msquic worker threads and delivers data via callbacks.
//

#include "transport.h"
#include "client_protocol.h"
#include <wtf.h>
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>

#define WT_MAX_CLIENTS 256
#define WT_READ_BUF_SIZE 65536
#define WT_FRAME_HEADER_SIZE 4

// ============================================================================
// FRAME PROTOCOL: same 4-byte big-endian length prefix as TCP/Unix/QUIC
// ============================================================================

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= WT_READ_BUF_SIZE;
}

static void write_frame_header(uint8_t* buf, uint32_t len) {
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
}

// Forward-declare so wt_client_t can reference wt_transport_data_t
typedef struct wt_transport_data_t wt_transport_data_t;

// ============================================================================
// PER-CLIENT STATE — one per WebTransport session (browser tab)
// ============================================================================

typedef struct wt_client_t {
    wtf_session_t* session;
    wtf_stream_t* stream;
    client_session_t* client_session;
    poseidon_transport_t* transport;
    wt_transport_data_t* data;
    uint8_t* read_buf;
    size_t read_len;
    size_t read_cap;
    volatile bool shutting_down;
} wt_client_t;

// ============================================================================
// TRANSPORT INTERNAL DATA
// ============================================================================

struct wt_transport_data_t {
    wtf_context_t* context;
    wtf_server_t* server;
    uint16_t port;
    char* cert_path;
    char* key_path;
    wt_client_t* clients[WT_MAX_CLIENTS];
    size_t num_clients;
    PLATFORMLOCKTYPE(lock);
    volatile bool stopping;
};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void wt_session_callback(const wtf_session_event_t* event);
static void wt_stream_callback(const wtf_stream_event_t* event);
static wtf_connection_decision_t wt_connection_validator(
    const wtf_connection_request_t* request, void* user_context);

static void wt_client_destroy(wt_client_t* client);
static void process_stream_data(wt_client_t* client);

// ============================================================================
// CLIENT LOOKUP — uses session_id as the client_id for send routing
// ============================================================================

static wt_client_t* wt_find_client_by_id(wt_transport_data_t* data,
                                          uint32_t session_id) {
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL &&
            data->clients[i]->client_session != NULL &&
            data->clients[i]->client_session->session_id == session_id) {
            return data->clients[i];
        }
    }
    return NULL;
}

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

static wt_client_t* wt_client_create(wtf_session_t* session,
                                      poseidon_transport_t* transport,
                                      wt_transport_data_t* data) {
    if (data->num_clients >= WT_MAX_CLIENTS) {
        log_warn("webtransport: max clients reached, rejecting session");
        return NULL;
    }

    wt_client_t* client = get_clear_memory(sizeof(wt_client_t));
    if (client == NULL) return NULL;

    client->session = session;
    client->transport = transport;
    client->data = data;
    client->read_cap = WT_READ_BUF_SIZE;
    client->read_buf = get_clear_memory(client->read_cap);
    if (client->read_buf == NULL) {
        free(client);
        return NULL;
    }
    client->read_len = 0;
    client->stream = NULL;
    client->shutting_down = false;

    static uint32_t next_session_id = 1;
    client->client_session = client_session_create(next_session_id++,
                                                     transport->manager);
    if (client->client_session == NULL) {
        free(client->read_buf);
        free(client);
        return NULL;
    }

    // Store wt_client_t on the session so future callbacks retrieve it
    wtf_session_set_context(session, client);

    // Use session_id as the client_id for send routing
    client->client_session->client_fd = (int)client->client_session->session_id;
    client->client_session->transport = transport;

    // Add to client list and register with channel manager
    platform_lock(&data->lock);
    data->clients[data->num_clients++] = client;
    platform_unlock(&data->lock);

    poseidon_channel_manager_register_session(transport->manager,
                                              client->client_session);

    return client;
}

static void wt_client_destroy(wt_client_t* client) {
    if (client == NULL) return;

    if (client->client_session != NULL) {
        client_session_cleanup_subscriptions(client->client_session);
        poseidon_channel_manager_unregister_session(
            client->transport->manager, client->client_session);
        client_session_destroy(client->client_session);
        client->client_session = NULL;
    }

    client->stream = NULL;
    client->session = NULL;

    if (client->read_buf != NULL) {
        free(client->read_buf);
        client->read_buf = NULL;
    }

    free(client);
}

static void wt_remove_client(wt_transport_data_t* data, wt_client_t* client) {
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] == client) {
            data->clients[i] = data->clients[data->num_clients - 1];
            data->clients[data->num_clients - 1] = NULL;
            data->num_clients--;
            return;
        }
    }
}

// ============================================================================
// FRAME PROCESSING — same CBOR protocol as TCP/Unix/QUIC
// ============================================================================

static void process_stream_data(wt_client_t* client) {
    while (client->read_len >= WT_FRAME_HEADER_SIZE) {
        uint32_t frame_len = 0;
        if (!read_frame_header(client->read_buf, &frame_len)) {
            log_warn("webtransport: invalid frame header, clearing read buffer");
            client->read_len = 0;
            return;
        }

        size_t total_len = WT_FRAME_HEADER_SIZE + frame_len;
        if (client->read_len < total_len) break;

        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(client->read_buf + WT_FRAME_HEADER_SIZE,
                                      frame_len, &result);
        if (item == NULL) {
            log_warn("webtransport: failed to decode CBOR frame");
            memmove(client->read_buf, client->read_buf + total_len,
                    client->read_len - total_len);
            client->read_len -= total_len;
            continue;
        }

        client_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        if (client_protocol_decode(item, &frame) == 0) {
            cbor_item_t* response = NULL;
            int rc = client_session_handle_request(client->client_session,
                                                     &frame, &response);
            if (response != NULL && client->stream != NULL &&
                !client->shutting_down) {
                uint8_t* resp_buf = NULL;
                size_t resp_len = 0;
                if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
                    size_t send_total = WT_FRAME_HEADER_SIZE + resp_len;
                    uint8_t* send_buf = get_clear_memory(send_total);
                    if (send_buf != NULL) {
                        write_frame_header(send_buf, (uint32_t)resp_len);
                        memcpy(send_buf + WT_FRAME_HEADER_SIZE, resp_buf, resp_len);

                        wtf_buffer_t buf;
                        buf.data = send_buf;
                        buf.length = (uint32_t)send_total;

                        wtf_result_t wr = wtf_stream_send(client->stream, &buf, 1, false);
                        if (wr != WTF_SUCCESS) {
                            free(send_buf);
                        }
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
// STREAM CALLBACK — handles data on a WebTransport bidirectional stream
// ============================================================================

static void wt_stream_callback(const wtf_stream_event_t* event) {
    wt_client_t* client = (wt_client_t*)event->user_context;
    if (client == NULL) return;

    switch (event->type) {
    case WTF_STREAM_EVENT_DATA_RECEIVED: {
        if (client->shutting_down) break;

        wtf_buffer_t* buffers = event->data_received.buffers;
        uint32_t buffer_count = event->data_received.buffer_count;

        size_t total_received = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            total_received += buffers[i].length;
        }

        // Grow buffer if needed
        if (client->read_len + total_received > client->read_cap) {
            size_t new_cap = client->read_cap;
            while (new_cap < client->read_len + total_received) {
                new_cap *= 2;
            }
            uint8_t* new_buf = realloc(client->read_buf, new_cap);
            if (new_buf == NULL) {
                log_error("webtransport: failed to grow read buffer");
                break;
            }
            client->read_buf = new_buf;
            client->read_cap = new_cap;
        }

        for (uint32_t i = 0; i < buffer_count; i++) {
            memcpy(client->read_buf + client->read_len,
                   buffers[i].data, buffers[i].length);
            client->read_len += buffers[i].length;
        }

        process_stream_data(client);
        break;
    }

    case WTF_STREAM_EVENT_SEND_COMPLETE: {
        // Free send buffer data — caller retains ownership and must free
        // in SEND_COMPLETE (including cancelled sends)
        wtf_buffer_t* buffers = event->send_complete.buffers;
        if (buffers != NULL && buffers->data != NULL) {
            free(buffers->data);
        }
        break;
    }

    case WTF_STREAM_EVENT_PEER_CLOSED:
        log_debug("webtransport: peer closed stream for client %u",
                  client->client_session ? client->client_session->session_id : 0);
        if (!client->shutting_down && client->stream != NULL) {
            wtf_stream_close(client->stream);
        }
        break;

    case WTF_STREAM_EVENT_CLOSED:
        log_info("webtransport: stream closed for client %u",
                 client->client_session ? client->client_session->session_id : 0);
        client->stream = NULL;
        break;

    case WTF_STREAM_EVENT_ABORTED:
        log_warn("webtransport: stream aborted (error=0x%x) for client %u",
                 event->aborted.error_code,
                 client->client_session ? client->client_session->session_id : 0);
        client->stream = NULL;
        break;

    default:
        break;
    }
}

// ============================================================================
// SESSION CALLBACK — handles WebTransport session lifecycle
// ============================================================================

static void wt_session_callback(const wtf_session_event_t* event) {
    if (event == NULL) return;

    switch (event->type) {
    case WTF_SESSION_EVENT_CONNECTED: {
        // user_context is the server's user_context (poseidon_transport_t*)
        // because no wt_client_t exists yet for this session
        poseidon_transport_t* transport = (poseidon_transport_t*)event->user_context;
        if (transport == NULL) {
            log_error("webtransport: CONNECTED with no transport context");
            wtf_session_close(event->session, 0, "internal error");
            break;
        }

        wt_transport_data_t* data = (wt_transport_data_t*)transport->loop;
        if (data->stopping) {
            wtf_session_close(event->session, 0, "server stopping");
            break;
        }

        wt_client_t* client = wt_client_create(event->session, transport, data);
        if (client == NULL) {
            log_error("webtransport: failed to create client for session");
            wtf_session_close(event->session, 0, "server full");
            break;
        }

        log_info("webtransport: session connected, client %u (total=%zu)",
                 client->client_session->session_id, data->num_clients);
        break;
    }

    case WTF_SESSION_EVENT_DISCONNECTED: {
        wt_client_t* client = (wt_client_t*)event->user_context;
        if (client == NULL) break;

        wt_transport_data_t* data = client->data;
        if (data == NULL) break;

        log_info("webtransport: session disconnected (error=0x%x, reason=%s)",
                 event->disconnected.error_code,
                 event->disconnected.reason ? event->disconnected.reason : "");

        client->shutting_down = true;

        // If the transport is stopping, webtransport_stop will clean up
        // remaining clients — avoid double-destroy
        if (data->stopping) break;

        platform_lock(&data->lock);
        wt_remove_client(data, client);
        wt_client_destroy(client);
        platform_unlock(&data->lock);
        break;
    }

    case WTF_SESSION_EVENT_DRAINING: {
        wt_client_t* client = (wt_client_t*)event->user_context;
        if (client != NULL) {
            log_info("webtransport: session draining for client %u",
                     client->client_session ? client->client_session->session_id : 0);
        }
        break;
    }

    case WTF_SESSION_EVENT_STREAM_OPENED: {
        wt_client_t* client = (wt_client_t*)event->user_context;
        if (client == NULL) break;

        wtf_stream_t* stream = event->stream_opened.stream;
        wtf_stream_type_t stream_type = event->stream_opened.stream_type;

        if (stream_type == WTF_STREAM_UNIDIRECTIONAL) {
            log_debug("webtransport: ignoring unidirectional stream");
            wtf_stream_close(stream);
            break;
        }

        if (client->stream != NULL) {
            log_debug("webtransport: closing extra bidi stream");
            wtf_stream_close(stream);
            break;
        }

        client->stream = stream;
        wtf_stream_set_callback(stream, wt_stream_callback);
        wtf_stream_set_context(stream, client);

        log_info("webtransport: bidi stream opened for client %u",
                 client->client_session ? client->client_session->session_id : 0);
        break;
    }

    case WTF_SESSION_EVENT_DATAGRAM_SEND_STATE_CHANGE:
    case WTF_SESSION_EVENT_DATAGRAM_RECEIVED:
        // Not using datagrams
        break;

    default:
        break;
    }
}

// ============================================================================
// CONNECTION VALIDATOR — accept all connections for now
// ============================================================================

static wtf_connection_decision_t wt_connection_validator(
    const wtf_connection_request_t* request, void* user_context) {
    (void)user_context;
    if (request != NULL) {
        log_info("webtransport: connection request from %s%s",
                 request->origin ? request->origin : "",
                 request->path ? request->path : "");
    }
    return WTF_CONNECTION_ACCEPT;
}

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

static int webtransport_start(poseidon_transport_t* self) {
    wt_transport_data_t* data = (wt_transport_data_t*)self->loop;

    // Create libwtf context
    wtf_context_config_t ctx_config;
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.log_level = WTF_LOG_LEVEL_WARN;
    ctx_config.worker_thread_count = 2;
    ctx_config.enable_load_balancing = false;
    ctx_config.disable_encryption = false;
    ctx_config.execution_profile = WTF_EXECUTION_PROFILE_LOW_LATENCY;

    wtf_result_t wr = wtf_context_create(&ctx_config, &data->context);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: context create failed: %s",
                  wtf_result_to_string(wr));
        return -1;
    }

    // Configure certificate
    wtf_certificate_config_t cert_config;
    memset(&cert_config, 0, sizeof(cert_config));
    cert_config.cert_type = WTF_CERT_TYPE_FILE;
    cert_config.cert_data.file.cert_path = data->cert_path;
    cert_config.cert_data.file.key_path = data->key_path;

    // Create server — pass transport as user_context so the session
    // callback can retrieve it on the first CONNECTED event before
    // any wt_client_t exists
    wtf_server_config_t srv_config;
    memset(&srv_config, 0, sizeof(srv_config));
    srv_config.host = NULL;
    srv_config.port = data->port;
    srv_config.cert_config = &cert_config;
    srv_config.max_sessions_per_connection = 32;
    srv_config.max_streams_per_session = 256;
    srv_config.max_data_per_session = 0;
    srv_config.idle_timeout_ms = 60000;
    srv_config.handshake_timeout_ms = 10000;
    srv_config.enable_0rtt = false;
    srv_config.enable_migration = false;
    srv_config.connection_validator = wt_connection_validator;
    srv_config.session_callback = wt_session_callback;
    srv_config.user_context = self;  // poseidon_transport_t*

    wr = wtf_server_create(data->context, &srv_config, &data->server);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: server create failed: %s",
                  wtf_result_to_string(wr));
        wtf_context_destroy(data->context);
        data->context = NULL;
        return -1;
    }

    // Start server
    wr = wtf_server_start(data->server);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: server start failed: %s",
                  wtf_result_to_string(wr));
        wtf_server_destroy(data->server);
        data->server = NULL;
        wtf_context_destroy(data->context);
        data->context = NULL;
        return -1;
    }

    self->running = true;
    log_info("webtransport: listening on port %u", data->port);
    return 0;
}

static int webtransport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;

    self->running = false;
    wt_transport_data_t* data = (wt_transport_data_t*)self->loop;
    data->stopping = true;

    // Signal all clients to stop
    platform_lock(&data->lock);
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            data->clients[i]->shutting_down = true;
        }
    }
    platform_unlock(&data->lock);

    // Stop server — libwtf handles closing sessions
    if (data->server != NULL) {
        wtf_server_stop(data->server);
    }

    // Destroy remaining clients that weren't cleaned up by session callbacks
    platform_lock(&data->lock);
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            wt_client_destroy(data->clients[i]);
            data->clients[i] = NULL;
        }
    }
    data->num_clients = 0;
    platform_unlock(&data->lock);

    if (data->server != NULL) {
        wtf_server_destroy(data->server);
        data->server = NULL;
    }
    if (data->context != NULL) {
        wtf_context_destroy(data->context);
        data->context = NULL;
    }

    return 0;
}

static int webtransport_send(poseidon_transport_t* self, int client_id,
                              const uint8_t* send_data, size_t len) {
    wt_transport_data_t* data = (wt_transport_data_t*)self->loop;

    // Find the client by session_id stored in client_fd
    wt_client_t* target = NULL;
    platform_lock(&data->lock);
    target = wt_find_client_by_id(data, (uint32_t)client_id);
    platform_unlock(&data->lock);

    if (target == NULL || target->stream == NULL || target->shutting_down) {
        return -1;
    }

    size_t total_len = WT_FRAME_HEADER_SIZE + len;
    uint8_t* send_buf = get_clear_memory(total_len);
    if (send_buf == NULL) return -1;

    write_frame_header(send_buf, (uint32_t)len);
    memcpy(send_buf + WT_FRAME_HEADER_SIZE, send_data, len);

    wtf_buffer_t buf;
    buf.data = send_buf;
    buf.length = (uint32_t)total_len;

    // Do NOT hold data->lock across wtf_stream_send — libwtf may invoke
    // SEND_COMPLETE synchronously on the same thread
    wtf_result_t wr = wtf_stream_send(target->stream, &buf, 1, false);
    if (wr != WTF_SUCCESS) {
        free(send_buf);
        return -1;
    }

    return 0;
}

// ============================================================================
// DESTROY — clean up libwtf handles, free resources
// ============================================================================

static void webtransport_transport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    wt_transport_data_t* data = (wt_transport_data_t*)transport->loop;
    if (data != NULL) {
        if (transport->running) {
            webtransport_stop(transport);
        }

        free(data->cert_path);
        free(data->key_path);
        platform_lock_destroy(&data->lock);
        free(data);
    }

    platform_lock_destroy(&transport->lock);
    free(transport);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_webtransport_create(
    uint16_t port, const char* cert_path, const char* key_path,
    poseidon_channel_manager_t* manager) {
    if (manager == NULL || cert_path == NULL || key_path == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    wt_transport_data_t* data = get_clear_memory(sizeof(wt_transport_data_t));
    if (data == NULL) {
        free(transport);
        return NULL;
    }

    data->port = port;
    data->cert_path = strdup(cert_path);
    data->key_path = strdup(key_path);
    if (data->cert_path == NULL || data->key_path == NULL) {
        free(data->cert_path);
        free(data->key_path);
        free(data);
        free(transport);
        return NULL;
    }
    data->num_clients = 0;
    data->stopping = false;
    data->context = NULL;
    data->server = NULL;
    platform_lock_init(&data->lock);

    transport->name = "webtransport";
    transport->type = POSEIDON_TRANSPORT_WEBTRANSPORT;
    transport->manager = manager;
    transport->loop = data;
    transport->start = webtransport_start;
    transport->stop = webtransport_stop;
    transport->send = webtransport_send;
    transport->destroy = webtransport_transport_destroy;
    platform_lock_init(&transport->lock);

    return transport;
}