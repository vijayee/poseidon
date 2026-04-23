//
// Created by victor on 4/22/26.
//

#define _POSIX_C_SOURCE 200809L
#include "poseidon_client.h"
#include "Crypto/key_pair.h"
#include "Channel/channel_config.h"
#include "ClientAPIs/client_protocol.h"
#include "Network/Meridian/msquic_singleton.h"
#include "Util/allocator.h"
#include "Util/log.h"
#include "Util/threadding.h"
#include <cbor.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define FRAME_HEADER_SIZE 4
#define CLIENT_READ_BUF_SIZE 65536
#define CLIENT_MAX_RESULT 1024

struct poseidon_client_t {
    int fd;
    bool connected;
    bool is_quic;
    uint32_t next_request_id;
    poseidon_message_cb_t message_cb;
    void* message_cb_ctx;
    poseidon_event_cb_t event_cb;
    void* event_cb_ctx;
    poseidon_response_cb_t response_cb;
    void* response_cb_ctx;
    PLATFORMLOCKTYPE(lock);
    PLATFORMTHREADTYPE(recv_thread);
    volatile bool running;
    uint8_t read_buf[CLIENT_READ_BUF_SIZE];
    size_t read_len;

    // QUIC-specific fields
    const struct QUIC_API_TABLE* quic_api;
    HQUIC quic_registration;
    HQUIC quic_configuration;
    HQUIC quic_connection;
    HQUIC quic_stream;
    volatile bool quic_stream_ready;
    PLATFORMCONDITIONTYPE(quic_handshake_done);
    PLATFORMLOCKTYPE(quic_lock);
};

// ============================================================================
// FRAME I/O
// ============================================================================

static bool write_frame_header(uint8_t* buf, uint32_t len) {
    buf[0] = (uint8_t)(len >> 24);
    buf[1] = (uint8_t)(len >> 16);
    buf[2] = (uint8_t)(len >> 8);
    buf[3] = (uint8_t)(len);
    return true;
}

static bool read_frame_header(const uint8_t* buf, uint32_t* len) {
    *len = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    return *len > 0 && *len <= CLIENT_READ_BUF_SIZE;
}

static int send_frame(poseidon_client_t* client, const uint8_t* data, size_t len) {
    if (client->is_quic) {
        if (!client->quic_stream_ready || client->quic_stream == NULL) return -1;

        size_t total_len = FRAME_HEADER_SIZE + len;
        uint8_t* send_buf = get_clear_memory(total_len);
        if (send_buf == NULL) return -1;

        write_frame_header(send_buf, (uint32_t)len);
        memcpy(send_buf + FRAME_HEADER_SIZE, data, len);

        QUIC_BUFFER buf;
        buf.Buffer = send_buf;
        buf.Length = total_len;

        QUIC_STATUS status = client->quic_api->StreamSend(
            client->quic_stream, &buf, 1, QUIC_SEND_FLAG_NONE, send_buf);
        if (QUIC_FAILED(status)) {
            free(send_buf);
            return -1;
        }
        // send_buf is freed in QUIC_STREAM_EVENT_SEND_COMPLETE callback
        return 0;
    }

    uint8_t header[FRAME_HEADER_SIZE];
    write_frame_header(header, (uint32_t)len);

    platform_lock(&client->lock);
    ssize_t h = send(client->fd, header, FRAME_HEADER_SIZE, MSG_NOSIGNAL);
    ssize_t d = send(client->fd, data, len, MSG_NOSIGNAL);
    platform_unlock(&client->lock);

    return (h == FRAME_HEADER_SIZE && (size_t)d == len) ? 0 : -1;
}

static uint32_t next_id(poseidon_client_t* client) {
    platform_lock(&client->lock);
    uint32_t id = client->next_request_id++;
    platform_unlock(&client->lock);
    return id;
}

static int send_request(poseidon_client_t* client, uint8_t method,
                        const char* topic_path, const uint8_t* payload, size_t payload_len) {
    uint32_t id = next_id(client);

    cbor_item_t* frame = client_protocol_encode_request(
        id, method, topic_path, payload, payload_len);
    if (frame == NULL) return -1;

    uint8_t* buf = NULL;
    size_t buf_len = 0;
    if (client_protocol_serialize(frame, &buf, &buf_len) != 0) {
        cbor_decref(&frame);
        return -1;
    }

    int rc = send_frame(client, buf, buf_len);
    free(buf);
    cbor_decref(&frame);
    return rc;
}

static int send_admin_request(poseidon_client_t* client, uint8_t method,
                              const char* topic_path,
                              const uint8_t* signature, size_t sig_len,
                              const uint8_t* config_data, size_t config_len) {
    uint32_t id = next_id(client);

    cbor_item_t* frame = client_protocol_encode_admin_request(
        id, method, topic_path, signature, sig_len, config_data, config_len);
    if (frame == NULL) return -1;

    uint8_t* buf = NULL;
    size_t buf_len = 0;
    if (client_protocol_serialize(frame, &buf, &buf_len) != 0) {
        cbor_decref(&frame);
        return -1;
    }

    int rc = send_frame(client, buf, buf_len);
    free(buf);
    cbor_decref(&frame);
    return rc;
}

// ============================================================================
// QUIC CALLBACKS
// ============================================================================

static void process_received_frame(poseidon_client_t* client, client_frame_t* frame);

static QUIC_STATUS QUIC_API quic_client_stream_callback(HQUIC stream, void* context,
                                                          QUIC_STREAM_EVENT* event) {
    poseidon_client_t* client = (poseidon_client_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        platform_lock(&client->quic_lock);
        client->quic_stream_ready = true;
        platform_unlock(&client->quic_lock);
        platform_signal_condition(&client->quic_handshake_done);
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        size_t total_received = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            total_received += buffers[i].Length;
        }

        // Grow read buffer if needed
        if (client->read_len + total_received > CLIENT_READ_BUF_SIZE) {
            // For simplicity, just process what we can and discard overflow
            log_warn("quic client: read buffer overflow, discarding data");
            client->quic_api->StreamReceiveComplete(stream, total_received);
            break;
        }

        for (uint32_t i = 0; i < buffer_count; i++) {
            memcpy(client->read_buf + client->read_len,
                   buffers[i].Buffer, buffers[i].Length);
            client->read_len += buffers[i].Length;
        }

        // Process complete frames
        while (client->read_len >= FRAME_HEADER_SIZE) {
            uint32_t frame_len = 0;
            if (!read_frame_header(client->read_buf, &frame_len)) {
                client->connected = false;
                break;
            }

            size_t total = FRAME_HEADER_SIZE + frame_len;
            if (client->read_len < total) break;

            struct cbor_load_result result;
            cbor_item_t* item = cbor_load(client->read_buf + FRAME_HEADER_SIZE,
                                          frame_len, &result);
            if (item != NULL) {
                client_frame_t frame;
                memset(&frame, 0, sizeof(frame));
                if (client_protocol_decode(item, &frame) == 0) {
                    process_received_frame(client, &frame);
                }
                cbor_decref(&item);
            }

            memmove(client->read_buf, client->read_buf + total,
                    client->read_len - total);
            client->read_len -= total;
        }

        client->quic_api->StreamReceiveComplete(stream, total_received);
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        client->connected = false;
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        client->connected = false;
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API quic_client_connection_callback(HQUIC connection, void* context,
                                                              QUIC_CONNECTION_EVENT* event) {
    poseidon_client_t* client = (poseidon_client_t*)context;

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        client->connected = true;
        // Open a bidirectional stream once connected
        {
            HQUIC stream = NULL;
            QUIC_STATUS status = client->quic_api->StreamOpen(
                connection, QUIC_STREAM_OPEN_FLAG_NONE,
                (void*)quic_client_stream_callback, client, &stream);
            if (QUIC_FAILED(status)) {
                log_error("quic client: StreamOpen failed: 0x%x", status);
                client->connected = false;
                platform_signal_condition(&client->quic_handshake_done);
                return QUIC_STATUS_SUCCESS;
            }

            status = client->quic_api->StreamStart(stream, QUIC_STREAM_OPEN_FLAG_NONE);
            if (QUIC_FAILED(status)) {
                log_error("quic client: StreamStart failed: 0x%x", status);
                client->quic_api->StreamClose(stream);
                client->connected = false;
                platform_signal_condition(&client->quic_handshake_done);
                return QUIC_STATUS_SUCCESS;
            }

            client->quic_stream = stream;
        }
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        client->connected = false;
        platform_signal_condition(&client->quic_handshake_done);
        client->quic_api->ConnectionClose(connection);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// RECEIVE THREAD (Unix/TCP only — QUIC uses callbacks)
// ============================================================================

static void process_received_frame(poseidon_client_t* client, client_frame_t* frame) {
    if (frame->frame_type == CLIENT_FRAME_RESPONSE) {
        if (client->response_cb != NULL) {
            client->response_cb(client->response_cb_ctx, frame->request_id,
                               frame->error_code, frame->result_data);
        }
    } else if (frame->frame_type == CLIENT_FRAME_EVENT) {
        if (frame->event_type == CLIENT_EVENT_MESSAGE && client->message_cb != NULL) {
            client->message_cb(client->message_cb_ctx, frame->topic_path,
                               frame->subtopic, frame->payload, frame->payload_len);
        } else if (client->event_cb != NULL) {
            client->event_cb(client->event_cb_ctx, frame->event_type,
                             frame->payload, frame->payload_len);
        }
    }
}

static void* recv_thread_func(void* arg) {
    poseidon_client_t* client = (poseidon_client_t*)arg;

    while (client->running) {
        ssize_t bytes = recv(client->fd, client->read_buf + client->read_len,
                           CLIENT_READ_BUF_SIZE - client->read_len, 0);
        if (bytes <= 0) {
            client->connected = false;
            break;
        }

        client->read_len += bytes;

        while (client->read_len >= FRAME_HEADER_SIZE) {
            uint32_t frame_len = 0;
            if (!read_frame_header(client->read_buf, &frame_len)) {
                client->connected = false;
                goto done;
            }

            size_t total = FRAME_HEADER_SIZE + frame_len;
            if (client->read_len < total) break;

            struct cbor_load_result result;
            cbor_item_t* item = cbor_load(client->read_buf + FRAME_HEADER_SIZE,
                                           frame_len, &result);
            if (item != NULL) {
                client_frame_t frame;
                memset(&frame, 0, sizeof(frame));
                if (client_protocol_decode(item, &frame) == 0) {
                    process_received_frame(client, &frame);
                }
                cbor_decref(&item);
            }

            memmove(client->read_buf, client->read_buf + total,
                    client->read_len - total);
            client->read_len -= total;
        }
    }

done:
    return NULL;
}

// ============================================================================
// CONNECTION
// ============================================================================

poseidon_client_t* poseidon_client_connect(const char* transport_url) {
    if (transport_url == NULL) return NULL;

    // QUIC transport
    if (strncmp(transport_url, "quic://", 7) == 0) {
        const char* host_port = transport_url + 7;
        char host[256] = {0};
        uint16_t port = 0;

        const char* colon = strrchr(host_port, ':');
        if (colon == NULL) return NULL;
        size_t host_len = colon - host_port;
        if (host_len >= sizeof(host)) return NULL;
        memcpy(host, host_port, host_len);
        host[host_len] = '\0';
        port = (uint16_t)atoi(colon + 1);

        // Open msquic
        const struct QUIC_API_TABLE* api = poseidon_msquic_open();
        if (api == NULL) {
            log_error("quic client: failed to open msquic");
            return NULL;
        }

        poseidon_client_t* client = get_clear_memory(sizeof(poseidon_client_t));
        if (client == NULL) {
            poseidon_msquic_close();
            return NULL;
        }

        client->is_quic = true;
        client->quic_api = api;
        client->next_request_id = 1;
        client->connected = false; // Set true in CONNECTED callback
        client->running = true;
        platform_lock_init(&client->lock);
        platform_lock_init(&client->quic_lock);
        platform_condition_init(&client->quic_handshake_done);

        // Open registration
        HQUIC registration = NULL;
        QUIC_STATUS status = api->RegistrationOpen(NULL, &registration);
        if (QUIC_FAILED(status)) {
            log_error("quic client: RegistrationOpen failed: 0x%x", status);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }
        client->quic_registration = registration;

        // Open configuration
        QUIC_BUFFER alpn;
        static const uint8_t alpn_str[] = "poseidon_client";
        alpn.Buffer = (uint8_t*)alpn_str;
        alpn.Length = sizeof(alpn_str) - 1;

        HQUIC configuration = NULL;
        status = api->ConfigurationOpen(registration, &alpn, 1, NULL, 0, NULL, &configuration);
        if (QUIC_FAILED(status)) {
            log_error("quic client: ConfigurationOpen failed: 0x%x", status);
            api->RegistrationClose(registration);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }
        client->quic_configuration = configuration;

        // Load credentials — no cert validation for development
        QUIC_CREDENTIAL_CONFIG cred_config;
        memset(&cred_config, 0, sizeof(cred_config));
        cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                            QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

        status = api->ConfigurationLoadCredential(configuration, &cred_config);
        if (QUIC_FAILED(status)) {
            log_error("quic client: ConfigurationLoadCredential failed: 0x%x", status);
            api->ConfigurationClose(configuration);
            api->RegistrationClose(registration);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }

        // Open connection with callback
        HQUIC connection = NULL;
        status = api->ConnectionOpen(registration,
                                     (void*)quic_client_connection_callback,
                                     client, &connection);
        if (QUIC_FAILED(status)) {
            log_error("quic client: ConnectionOpen failed: 0x%x", status);
            api->ConfigurationClose(configuration);
            api->RegistrationClose(registration);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }
        client->quic_connection = connection;

        // Start connection
        status = api->ConnectionStart(connection, configuration,
                                       QUIC_ADDRESS_FAMILY_INET, host, port);
        if (QUIC_FAILED(status)) {
            log_error("quic client: ConnectionStart failed: 0x%x", status);
            api->ConnectionClose(connection);
            api->ConfigurationClose(configuration);
            api->RegistrationClose(registration);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }

        // Wait for handshake + stream to be ready
        platform_lock(&client->quic_lock);
        while (!client->quic_stream_ready && client->connected) {
            platform_condition_wait(&client->quic_lock, &client->quic_handshake_done);
        }
        platform_unlock(&client->quic_lock);

        if (!client->quic_stream_ready) {
            log_error("quic client: failed to establish stream");
            api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(connection);
            api->ConfigurationClose(configuration);
            api->RegistrationClose(registration);
            platform_condition_destroy(&client->quic_handshake_done);
            platform_lock_destroy(&client->quic_lock);
            platform_lock_destroy(&client->lock);
            free(client);
            poseidon_msquic_close();
            return NULL;
        }

        client->connected = true;
        // No recv_thread for QUIC — msquic callbacks handle incoming data
        return client;
    }

    // Unix domain socket transport
    int fd = -1;

    if (strncmp(transport_url, "unix://", 7) == 0) {
        const char* path = transport_url + 7;
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return NULL;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return NULL;
        }
    } else if (strncmp(transport_url, "tcp://", 6) == 0) {
        const char* host_port = transport_url + 6;
        char host[256] = {0};
        uint16_t port = 0;

        const char* colon = strrchr(host_port, ':');
        if (colon == NULL) {
            return NULL;
        }
        size_t host_len = colon - host_port;
        if (host_len >= sizeof(host)) return NULL;
        memcpy(host, host_port, host_len);
        host[host_len] = '\0';
        port = (uint16_t)atoi(colon + 1);

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return NULL;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return NULL;
        }
    } else {
        return NULL;
    }

    poseidon_client_t* client = get_clear_memory(sizeof(poseidon_client_t));
    if (client == NULL) {
        close(fd);
        return NULL;
    }

    client->fd = fd;
    client->connected = true;
    client->next_request_id = 1;
    client->running = true;
    platform_lock_init(&client->lock);

    if (pthread_create(&client->recv_thread, NULL, recv_thread_func, client) != 0) {
        platform_lock_destroy(&client->lock);
        free(client);
        close(fd);
        return NULL;
    }

    return client;
}

void poseidon_client_disconnect(poseidon_client_t* client) {
    if (client == NULL) return;

    client->running = false;
    client->connected = false;

    if (client->is_quic) {
        if (client->quic_stream != NULL) {
            client->quic_api->StreamShutdown(client->quic_stream,
                                              QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
            client->quic_api->StreamClose(client->quic_stream);
            client->quic_stream = NULL;
        }
        if (client->quic_connection != NULL) {
            client->quic_api->ConnectionShutdown(client->quic_connection,
                                                  QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            client->quic_api->ConnectionClose(client->quic_connection);
            client->quic_connection = NULL;
        }
        if (client->quic_configuration != NULL) {
            client->quic_api->ConfigurationClose(client->quic_configuration);
            client->quic_configuration = NULL;
        }
        if (client->quic_registration != NULL) {
            client->quic_api->RegistrationClose(client->quic_registration);
            client->quic_registration = NULL;
        }
        poseidon_msquic_close();
        platform_condition_destroy(&client->quic_handshake_done);
        platform_lock_destroy(&client->quic_lock);
    } else {
        shutdown(client->fd, SHUT_RDWR);
        pthread_join(client->recv_thread, NULL);
        close(client->fd);
    }

    platform_lock_destroy(&client->lock);
    free(client);
}

// ============================================================================
// CHANNEL LIFECYCLE
// ============================================================================

int poseidon_client_channel_create(poseidon_client_t* client, const char* name,
                                    char* out_topic_id, size_t buf_size) {
    if (client == NULL || name == NULL) return -1;
    return send_request(client, CLIENT_METHOD_CHANNEL_CREATE, name, NULL, 0);
}

int poseidon_client_channel_join(poseidon_client_t* client, const char* topic_or_alias,
                                  char* out_topic_id, size_t buf_size) {
    if (client == NULL || topic_or_alias == NULL) return -1;
    return send_request(client, CLIENT_METHOD_CHANNEL_JOIN, topic_or_alias, NULL, 0);
}

int poseidon_client_channel_leave(poseidon_client_t* client, const char* topic_id) {
    if (client == NULL || topic_id == NULL) return -1;
    return send_request(client, CLIENT_METHOD_CHANNEL_LEAVE, topic_id, NULL, 0);
}

int poseidon_client_channel_destroy(poseidon_client_t* client, const char* topic_id,
                                     const poseidon_key_pair_t* owner_key) {
    if (client == NULL || topic_id == NULL || owner_key == NULL) return -1;

    // Sign: method_code || topic_id || timestamp
    size_t topic_len = strlen(topic_id);
    if (topic_len > CLIENT_MAX_TOPIC_PATH) return -1;
    uint8_t data_to_sign[1 + CLIENT_MAX_TOPIC_PATH + 8];
    size_t pos = 0;
    data_to_sign[pos++] = CLIENT_METHOD_CHANNEL_DESTROY;
    memcpy(data_to_sign + pos, topic_id, topic_len);
    pos += topic_len;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    for (int i = 7; i >= 0; i--) {
        data_to_sign[pos++] = (uint8_t)(timestamp_us >> (i * 8));
    }

    uint8_t signature[64];
    size_t sig_len = 0;
    if (poseidon_key_pair_sign((poseidon_key_pair_t*)owner_key, data_to_sign, pos,
                                signature, &sig_len) != 0) {
        return -1;
    }

    return send_admin_request(client, CLIENT_METHOD_CHANNEL_DESTROY, topic_id,
                               signature, sig_len, NULL, 0);
}

int poseidon_client_channel_modify(poseidon_client_t* client, const char* topic_id,
                                    const poseidon_channel_config_t* config,
                                    const poseidon_key_pair_t* owner_key) {
    if (client == NULL || topic_id == NULL || config == NULL || owner_key == NULL) return -1;

    size_t topic_len = strlen(topic_id);
    if (topic_len > CLIENT_MAX_TOPIC_PATH) return -1;
    uint8_t data_to_sign[1 + CLIENT_MAX_TOPIC_PATH + 8];
    size_t pos = 0;
    data_to_sign[pos++] = CLIENT_METHOD_CHANNEL_MODIFY;
    memcpy(data_to_sign + pos, topic_id, topic_len);
    pos += topic_len;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    for (int i = 7; i >= 0; i--) {
        data_to_sign[pos++] = (uint8_t)(timestamp_us >> (i * 8));
    }

    uint8_t signature[64];
    size_t sig_len = 0;
    if (poseidon_key_pair_sign((poseidon_key_pair_t*)owner_key, data_to_sign, pos,
                                signature, &sig_len) != 0) {
        return -1;
    }

    return send_admin_request(client, CLIENT_METHOD_CHANNEL_MODIFY, topic_id,
                               signature, sig_len, (const uint8_t*)config, sizeof(*config));
}

// ============================================================================
// PUB/SUB
// ============================================================================

int poseidon_client_subscribe(poseidon_client_t* client, const char* topic_path) {
    if (client == NULL || topic_path == NULL) return -1;
    return send_request(client, CLIENT_METHOD_SUBSCRIBE, topic_path, NULL, 0);
}

int poseidon_client_unsubscribe(poseidon_client_t* client, const char* topic_path) {
    if (client == NULL || topic_path == NULL) return -1;
    return send_request(client, CLIENT_METHOD_UNSUBSCRIBE, topic_path, NULL, 0);
}

int poseidon_client_publish(poseidon_client_t* client, const char* topic_path,
                             const uint8_t* data, size_t len) {
    if (client == NULL || topic_path == NULL) return -1;
    return send_request(client, CLIENT_METHOD_PUBLISH, topic_path, data, len);
}

// ============================================================================
// ALIASES
// ============================================================================

int poseidon_client_alias_register(poseidon_client_t* client, const char* name,
                                    const char* topic_id) {
    if (client == NULL || name == NULL || topic_id == NULL) return -1;
    return send_request(client, CLIENT_METHOD_ALIAS_REGISTER, name,
                        (const uint8_t*)topic_id, strlen(topic_id));
}

int poseidon_client_alias_unregister(poseidon_client_t* client, const char* name) {
    if (client == NULL || name == NULL) return -1;
    return send_request(client, CLIENT_METHOD_ALIAS_UNREGISTER, name, NULL, 0);
}

// ============================================================================
// EVENTS
// ============================================================================

void poseidon_client_on_message(poseidon_client_t* client,
                                  poseidon_message_cb_t cb, void* ctx) {
    if (client == NULL) return;
    client->message_cb = cb;
    client->message_cb_ctx = ctx;
}

void poseidon_client_on_event(poseidon_client_t* client,
                               poseidon_event_cb_t cb, void* ctx) {
    if (client == NULL) return;
    client->event_cb = cb;
    client->event_cb_ctx = ctx;
}

void poseidon_client_on_response(poseidon_client_t* client,
                                  poseidon_response_cb_t cb, void* ctx) {
    if (client == NULL) return;
    client->response_cb = cb;
    client->response_cb_ctx = ctx;
}