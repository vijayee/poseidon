//
// Created by victor on 4/22/26.
//
// QUIC transport for ClientAPI — uses msquic for connections.
// No poll-dancer event loop; msquic manages its own worker threads
// and delivers data via callbacks. The transport stores msquic handles
// in transport->loop as quic_transport_data_t*.
//

#include "transport.h"
#include "client_protocol.h"
#include "../Network/Meridian/msquic_singleton.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/vec.h"
#include <cbor.h>
#include <string.h>
#include <stdlib.h>

#define QUIC_MAX_CLIENTS 256
#define QUIC_READ_BUF_SIZE 65536
#define QUIC_FRAME_HEADER_SIZE 4

#define QUIC_ALPN "poseidon_client"

// ============================================================================
// FRAME PROTOCOL: same 4-byte big-endian length prefix as TCP/Unix
// ============================================================================

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= QUIC_READ_BUF_SIZE;
}

static void write_frame_header(uint8_t* buf, uint32_t len) {
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
}

// ============================================================================
// PER-CLIENT STATE
// ============================================================================

typedef struct quic_client_t {
    HQUIC connection;
    HQUIC stream;
    client_session_t* session;
    poseidon_transport_t* transport;
    uint8_t* read_buf;
    size_t read_len;
    size_t read_cap;
    bool stream_ready;
    volatile bool shutting_down;
} quic_client_t;

// ============================================================================
// TRANSPORT INTERNAL DATA
// ============================================================================

typedef struct quic_transport_data_t {
    const struct QUIC_API_TABLE* api;
    HQUIC registration;
    HQUIC configuration;
    HQUIC listener;
    uint16_t port;
    char* cert_path;
    char* key_path;
    quic_client_t* clients[QUIC_MAX_CLIENTS];
    size_t num_clients;
    PLATFORMLOCKTYPE(lock);
    volatile bool stopping;
} quic_transport_data_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API quic_listener_callback(HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);
static QUIC_STATUS QUIC_API quic_connection_callback(HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);
static QUIC_STATUS QUIC_API quic_stream_callback(HQUIC stream, void* context, QUIC_STREAM_EVENT* event);

static void quic_client_destroy(quic_client_t* client);
static void process_stream_data(quic_client_t* client);

// ============================================================================
// CLIENT LOOKUP — uses session_id as the client_id for send routing,
// avoiding truncation of pointer-sized HQUIC handles to int
// ============================================================================

static quic_client_t* quic_find_client_by_id(quic_transport_data_t* data, uint32_t session_id) {
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL &&
            data->clients[i]->session != NULL &&
            data->clients[i]->session->session_id == session_id) {
            return data->clients[i];
        }
    }
    return NULL;
}

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

static quic_client_t* quic_client_create(HQUIC connection, poseidon_transport_t* transport) {
    quic_client_t* client = get_clear_memory(sizeof(quic_client_t));
    if (client == NULL) return NULL;

    client->connection = connection;
    client->transport = transport;
    client->read_cap = QUIC_READ_BUF_SIZE;
    client->read_buf = get_clear_memory(client->read_cap);
    if (client->read_buf == NULL) {
        free(client);
        return NULL;
    }
    client->read_len = 0;
    client->stream_ready = false;
    client->shutting_down = false;

    static uint32_t next_session_id = 1;
    client->session = client_session_create(next_session_id++, transport->manager);
    if (client->session == NULL) {
        free(client->read_buf);
        free(client);
        return NULL;
    }

    return client;
}

static void quic_client_destroy(quic_client_t* client) {
    if (client == NULL) return;

    // Clean up Quasar subscriptions before unregistering from session registry
    if (client->session != NULL) {
        client_session_cleanup_subscriptions(client->session);
        poseidon_channel_manager_unregister_session(
            client->transport->manager, client->session);
        client_session_destroy(client->session);
        client->session = NULL;
    }

    client->stream = NULL;
    client->connection = NULL;

    if (client->read_buf != NULL) {
        free(client->read_buf);
        client->read_buf = NULL;
    }

    free(client);
}

static void quic_remove_client(quic_transport_data_t* data, quic_client_t* client) {
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
// FRAME PROCESSING — same CBOR protocol as TCP/Unix
// ============================================================================

static void process_stream_data(quic_client_t* client) {
    while (client->read_len >= QUIC_FRAME_HEADER_SIZE) {
        uint32_t frame_len = 0;
        if (!read_frame_header(client->read_buf, &frame_len)) {
            log_warn("quic transport: invalid frame header, clearing read buffer");
            client->read_len = 0;
            return;
        }

        size_t total_len = QUIC_FRAME_HEADER_SIZE + frame_len;
        if (client->read_len < total_len) break;

        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(client->read_buf + QUIC_FRAME_HEADER_SIZE,
                                      frame_len, &result);
        if (item == NULL) {
            log_warn("quic transport: failed to decode CBOR frame");
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
            if (response != NULL && client->stream != NULL && client->stream_ready && !client->shutting_down) {
                uint8_t* resp_buf = NULL;
                size_t resp_len = 0;
                if (client_protocol_serialize(response, &resp_buf, &resp_len) == 0) {
                    quic_transport_data_t* data = (quic_transport_data_t*)client->transport->loop;
                    uint8_t* send_buf = get_clear_memory(QUIC_FRAME_HEADER_SIZE + resp_len);
                    if (send_buf != NULL) {
                        write_frame_header(send_buf, (uint32_t)resp_len);
                        memcpy(send_buf + QUIC_FRAME_HEADER_SIZE, resp_buf, resp_len);

                        QUIC_BUFFER buf;
                        buf.Buffer = send_buf;
                        buf.Length = QUIC_FRAME_HEADER_SIZE + resp_len;

                        // Do NOT hold data->lock across StreamSend — msquic may
                        // invoke SEND_COMPLETE synchronously on the same thread
                        QUIC_STATUS send_status = data->api->StreamSend(
                            client->stream, &buf, 1, QUIC_SEND_FLAG_NONE, send_buf);
                        if (QUIC_FAILED(send_status)) {
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
// STREAM CALLBACK — handles data on a bidirectional QUIC stream
// ============================================================================

static QUIC_STATUS QUIC_API quic_stream_callback(HQUIC stream, void* context,
                                                   QUIC_STREAM_EVENT* event) {
    quic_client_t* client = (quic_client_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        client->stream_ready = true;
        log_info("quic transport: stream started for client %u",
                 client->session ? client->session->session_id : 0);
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (client->shutting_down) {
            quic_transport_data_t* data = (quic_transport_data_t*)client->transport->loop;
            data->api->StreamReceiveComplete(stream, 0);
            break;
        }

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        size_t total_received = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            total_received += buffers[i].Length;
        }

        // Grow buffer if needed
        if (client->read_len + total_received > client->read_cap) {
            size_t new_cap = client->read_cap;
            while (new_cap < client->read_len + total_received) {
                new_cap *= 2;
            }
            uint8_t* new_buf = realloc(client->read_buf, new_cap);
            if (new_buf == NULL) {
                log_error("quic transport: failed to grow read buffer");
                quic_transport_data_t* data = (quic_transport_data_t*)client->transport->loop;
                data->api->StreamReceiveComplete(stream, total_received);
                break;
            }
            client->read_buf = new_buf;
            client->read_cap = new_cap;
        }

        for (uint32_t i = 0; i < buffer_count; i++) {
            memcpy(client->read_buf + client->read_len,
                   buffers[i].Buffer, buffers[i].Length);
            client->read_len += buffers[i].Length;
        }

        process_stream_data(client);

        if (client->transport != NULL) {
            quic_transport_data_t* data = (quic_transport_data_t*)client->transport->loop;
            data->api->StreamReceiveComplete(stream, total_received);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        if (client->transport != NULL && !client->shutting_down) {
            quic_transport_data_t* data = (quic_transport_data_t*)client->transport->loop;
            data->api->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        log_info("quic transport: stream shutdown complete for client %u",
                 client->session ? client->session->session_id : 0);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// CONNECTION CALLBACK — handles QUIC connection lifecycle
// ============================================================================

static QUIC_STATUS QUIC_API quic_connection_callback(HQUIC connection, void* context,
                                                       QUIC_CONNECTION_EVENT* event) {
    poseidon_transport_t* transport = (poseidon_transport_t*)context;
    quic_transport_data_t* data = (quic_transport_data_t*)transport->loop;

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        log_info("quic transport: client connected");
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        if (data->stopping) {
            return QUIC_STATUS_CONNECTION_REFUSED;
        }

        if (data->num_clients >= QUIC_MAX_CLIENTS) {
            log_warn("quic transport: max clients reached, rejecting stream");
            data->api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            return QUIC_STATUS_SUCCESS;
        }

        HQUIC stream = event->PEER_STREAM_STARTED.Stream;

        quic_client_t* client = quic_client_create(connection, transport);
        if (client == NULL) {
            log_error("quic transport: failed to create client state");
            data->api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            return QUIC_STATUS_SUCCESS;
        }

        data->api->SetCallbackHandler(stream, (void*)quic_stream_callback, client);
        client->stream = stream;

        // Use session_id as the client_id for send routing instead of
        // truncating the pointer-sized HQUIC handle to int
        client->session->client_fd = (int)client->session->session_id;
        client->session->transport = transport;

        platform_lock(&data->lock);
        data->clients[data->num_clients++] = client;
        poseidon_channel_manager_register_session(transport->manager, client->session);
        platform_unlock(&data->lock);

        log_info("quic transport: stream started, client %u (total=%zu)",
                 client->session->session_id, data->num_clients);
        break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        // Find and remove all clients associated with this connection
        platform_lock(&data->lock);
        vec_t(quic_client_t*) to_remove;
        vec_init(&to_remove);

        for (size_t i = 0; i < data->num_clients; i++) {
            if (data->clients[i] != NULL && data->clients[i]->connection == connection) {
                data->clients[i]->shutting_down = true;
                vec_push(&to_remove, data->clients[i]);
            }
        }

        for (int i = 0; i < to_remove.length; i++) {
            quic_client_t* client = to_remove.data[i];
            quic_remove_client(data, client);
            quic_client_destroy(client);
        }
        vec_deinit(&to_remove);
        platform_unlock(&data->lock);

        data->api->ConnectionClose(connection);
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// LISTENER CALLBACK — accepts incoming QUIC connections
// ============================================================================

static QUIC_STATUS QUIC_API quic_listener_callback(HQUIC listener, void* context,
                                                     QUIC_LISTENER_EVENT* event) {
    (void)listener;
    poseidon_transport_t* transport = (poseidon_transport_t*)context;
    quic_transport_data_t* data = (quic_transport_data_t*)transport->loop;

    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
        const QUIC_NEW_CONNECTION_INFO* info = event->NEW_CONNECTION.Info;

        static const uint8_t poseidon_alpn_len = sizeof(QUIC_ALPN) - 1;
        if (info->NegotiatedAlpnLength != poseidon_alpn_len ||
            memcmp(info->NegotiatedAlpn, QUIC_ALPN, poseidon_alpn_len) != 0) {
            log_warn("quic transport: rejecting connection with wrong ALPN");
            return QUIC_STATUS_CONNECTION_REFUSED;
        }

        HQUIC connection = event->NEW_CONNECTION.Connection;

        data->api->SetCallbackHandler(connection, (void*)quic_connection_callback, transport);

        QUIC_STATUS status = data->api->ConnectionSetConfiguration(connection, data->configuration);
        if (QUIC_FAILED(status)) {
            log_error("quic transport: ConnectionSetConfiguration failed: 0x%x", status);
            return status;
        }

        log_info("quic transport: accepted new connection");
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        log_info("quic transport: listener stopped");
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

static int quic_transport_start(poseidon_transport_t* self) {
    quic_transport_data_t* data = (quic_transport_data_t*)self->loop;
    const struct QUIC_API_TABLE* api = data->api;

    QUIC_STATUS status = api->RegistrationOpen(NULL, &data->registration);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: RegistrationOpen failed: 0x%x", status);
        return -1;
    }

    QUIC_BUFFER alpn;
    alpn.Buffer = (uint8_t*)QUIC_ALPN;
    alpn.Length = sizeof(QUIC_ALPN) - 1;

    status = api->ConfigurationOpen(data->registration, &alpn, 1, NULL, 0, NULL, &data->configuration);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ConfigurationOpen failed: 0x%x", status);
        api->RegistrationClose(data->registration);
        data->registration = NULL;
        return -1;
    }

    QUIC_CERTIFICATE_FILE cert_file;
    cert_file.CertificateFile = data->cert_path;
    cert_file.PrivateKeyFile = data->key_path;

    QUIC_CREDENTIAL_CONFIG cred_config;
    memset(&cred_config, 0, sizeof(cred_config));
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    status = api->ConfigurationLoadCredential(data->configuration, &cred_config);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ConfigurationLoadCredential failed: 0x%x", status);
        api->ConfigurationClose(data->configuration);
        data->configuration = NULL;
        api->RegistrationClose(data->registration);
        data->registration = NULL;
        return -1;
    }

    status = api->ListenerOpen(data->registration, quic_listener_callback, self, &data->listener);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ListenerOpen failed: 0x%x", status);
        api->ConfigurationClose(data->configuration);
        data->configuration = NULL;
        api->RegistrationClose(data->registration);
        data->registration = NULL;
        return -1;
    }

    QUIC_ADDR listen_addr;
    QuicAddrSetFamily(&listen_addr, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&listen_addr, data->port);

    status = api->ListenerStart(data->listener, &alpn, 1, &listen_addr);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ListenerStart failed: 0x%x", status);
        api->ListenerClose(data->listener);
        data->listener = NULL;
        api->ConfigurationClose(data->configuration);
        data->configuration = NULL;
        api->RegistrationClose(data->registration);
        data->registration = NULL;
        return -1;
    }

    self->running = true;
    log_info("quic transport: listening on port %u", data->port);
    return 0;
}

static int quic_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;

    self->running = false;
    quic_transport_data_t* data = (quic_transport_data_t*)self->loop;
    const struct QUIC_API_TABLE* api = data->api;

    data->stopping = true;

    // Stop listener first to prevent new connections
    if (data->listener != NULL) {
        api->ListenerStop(data->listener);
        api->ListenerClose(data->listener);
        data->listener = NULL;
    }

    // Signal all clients to stop accepting data, then shutdown connections
    platform_lock(&data->lock);
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            data->clients[i]->shutting_down = true;
        }
    }
    platform_unlock(&data->lock);

    // Shutdown all connections (may trigger SHUTDOWN_COMPLETE callbacks)
    platform_lock(&data->lock);
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL && data->clients[i]->connection != NULL) {
            api->ConnectionShutdown(data->clients[i]->connection,
                                    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        }
    }
    platform_unlock(&data->lock);

    // Wait for msquic to deliver SHUTDOWN_COMPLETE callbacks
    platform_usleep(100000);

    // Destroy remaining clients that weren't cleaned up by callbacks
    platform_lock(&data->lock);
    for (size_t i = 0; i < data->num_clients; i++) {
        if (data->clients[i] != NULL) {
            quic_client_destroy(data->clients[i]);
            data->clients[i] = NULL;
        }
    }
    data->num_clients = 0;
    platform_unlock(&data->lock);

    if (data->configuration != NULL) {
        api->ConfigurationClose(data->configuration);
        data->configuration = NULL;
    }
    if (data->registration != NULL) {
        api->RegistrationClose(data->registration);
        data->registration = NULL;
    }

    return 0;
}

static int quic_transport_send(poseidon_transport_t* self, int client_id,
                                const uint8_t* send_data, size_t len) {
    quic_transport_data_t* data = (quic_transport_data_t*)self->loop;

    // Find the client by session_id stored in client_fd
    quic_client_t* target = NULL;
    platform_lock(&data->lock);
    target = quic_find_client_by_id(data, (uint32_t)client_id);
    platform_unlock(&data->lock);

    if (target == NULL || !target->stream_ready || target->shutting_down) {
        return -1;
    }

    size_t total_len = QUIC_FRAME_HEADER_SIZE + len;
    uint8_t* send_buf = get_clear_memory(total_len);
    if (send_buf == NULL) return -1;

    write_frame_header(send_buf, (uint32_t)len);
    memcpy(send_buf + QUIC_FRAME_HEADER_SIZE, send_data, len);

    QUIC_BUFFER buf;
    buf.Buffer = send_buf;
    buf.Length = total_len;

    QUIC_STATUS status = data->api->StreamSend(target->stream, &buf, 1,
                                                 QUIC_SEND_FLAG_NONE, send_buf);
    if (QUIC_FAILED(status)) {
        free(send_buf);
        return -1;
    }

    return 0;
}

// ============================================================================
// DESTROY — clean up all msquic handles, free resources, release ref
// ============================================================================

static void quic_transport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    quic_transport_data_t* data = (quic_transport_data_t*)transport->loop;
    if (data != NULL) {
        if (transport->running) {
            quic_transport_stop(transport);
        }

        free(data->cert_path);
        free(data->key_path);
        platform_lock_destroy(&data->lock);
        free(data);
    }

    platform_lock_destroy(&transport->lock);
    free(transport);

    poseidon_msquic_close();
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_quic_create(uint16_t port,
                                                      const char* cert_path,
                                                      const char* key_path,
                                                      poseidon_channel_manager_t* manager) {
    if (manager == NULL || cert_path == NULL || key_path == NULL) return NULL;

    const struct QUIC_API_TABLE* api = poseidon_msquic_open();
    if (api == NULL) {
        log_error("quic transport: failed to open msquic");
        return NULL;
    }

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) {
        poseidon_msquic_close();
        return NULL;
    }

    quic_transport_data_t* data = get_clear_memory(sizeof(quic_transport_data_t));
    if (data == NULL) {
        free(transport);
        poseidon_msquic_close();
        return NULL;
    }

    data->api = api;
    data->port = port;
    data->cert_path = strdup(cert_path);
    data->key_path = strdup(key_path);
    if (data->cert_path == NULL || data->key_path == NULL) {
        free(data->cert_path);
        free(data->key_path);
        free(data);
        free(transport);
        poseidon_msquic_close();
        return NULL;
    }
    data->num_clients = 0;
    data->stopping = false;
    platform_lock_init(&data->lock);

    transport->name = "quic";
    transport->type = POSEIDON_TRANSPORT_QUIC;
    transport->manager = manager;
    transport->loop = data;
    transport->start = quic_transport_start;
    transport->stop = quic_transport_stop;
    transport->send = quic_transport_send;
    transport->destroy = quic_transport_destroy;
    platform_lock_init(&transport->lock);

    return transport;
}