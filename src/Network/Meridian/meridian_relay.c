//
// Created by victor on 4/19/26.
//

#include "meridian_relay.h"
#include "meridian_packet.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <cbor.h>

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayConnectionCallback(HQUIC Connection, void* Context,
                        QUIC_CONNECTION_EVENT* Event);

static QUIC_STATUS QUIC_API
relay_uni_stream_callback(HQUIC Stream, void* Context,
                          QUIC_STREAM_EVENT* Event);

static QUIC_STATUS QUIC_API
relay_bidi_stream_callback(HQUIC Stream, void* Context,
                           QUIC_STREAM_EVENT* Event);

// ============================================================================
// STREAM TRACKING HELPERS
// ============================================================================

/**
 * Adds a stream entry to the relay's active_streams tracking array.
 * Caller must hold relay->lock.
 */
static int relay_track_stream(meridian_relay_t* relay,
                              meridian_stream_entry_t* entry) {
    if (relay->num_active_streams >= relay->active_streams_cap) {
        size_t new_cap = relay->active_streams_cap * 2;
        meridian_stream_entry_t** new_arr = (meridian_stream_entry_t**)
            realloc(relay->active_streams, new_cap * sizeof(meridian_stream_entry_t*));
        if (new_arr == NULL) return -1;
        relay->active_streams = new_arr;
        relay->active_streams_cap = new_cap;
    }
    relay->active_streams[relay->num_active_streams++] = entry;
    return 0;
}

// ============================================================================
// RELAY CLIENT LIFECYCLE
// ============================================================================

meridian_relay_t* meridian_relay_create(const struct QUIC_API_TABLE* msquic,
                                        HQUIC registration,
                                        meridian_rendv_t* server,
                                        const meridian_relay_config_t* config) {
    if (msquic == NULL || server == NULL) return NULL;

    meridian_relay_t* relay = (meridian_relay_t*)
        get_clear_memory(sizeof(meridian_relay_t));
    if (relay == NULL) return NULL;

    relay->msquic = msquic;
    relay->registration = registration;
    relay->server = server;
    relay->configuration = NULL;
    relay->connection = NULL;
    relay->local_endpoint_id = 0;
    relay->connected = false;
    relay->on_datagram = NULL;
    relay->on_datagram_ctx = NULL;
    relay->on_addr_response = NULL;
    relay->on_addr_response_ctx = NULL;

    relay->active_streams_cap = 16;
    relay->active_streams = (meridian_stream_entry_t**)
        get_clear_memory(relay->active_streams_cap * sizeof(meridian_stream_entry_t*));
    if (relay->active_streams == NULL) {
        free(relay);
        return NULL;
    }
    relay->num_active_streams = 0;

    if (config != NULL) {
        relay->config = *config;
    } else {
        relay->config.alpn = NULL;
        relay->config.idle_timeout_ms = 30000;
        relay->config.max_datagram_size = 4096;
        relay->config.keepalive_interval_ms = 10000;
    }

    platform_lock_init(&relay->lock);
    refcounter_init(&relay->refcounter);

    // Create QUIC configuration if ALPN is set
    if (relay->config.alpn != NULL) {
        QUIC_BUFFER Alpn = {
            (uint32_t)strlen(relay->config.alpn),
            (uint8_t*)relay->config.alpn
        };

        QUIC_SETTINGS Settings = {0};
        Settings.IdleTimeoutMs = relay->config.idle_timeout_ms;
        Settings.IsSet.IdleTimeoutMs = TRUE;
        Settings.KeepAliveIntervalMs = relay->config.keepalive_interval_ms;
        Settings.IsSet.KeepAliveIntervalMs = TRUE;
        Settings.PeerBidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
        Settings.IsSet.PeerBidiStreamCount = TRUE;
        Settings.PeerUnidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
        Settings.IsSet.PeerUnidiStreamCount = TRUE;

        QUIC_STATUS Status = relay->msquic->ConfigurationOpen(
            relay->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &relay->configuration);

        if (QUIC_FAILED(Status)) {
            free(relay->active_streams);
            platform_lock_destroy(&relay->lock);
            free(relay);
            return NULL;
        }

        // Load credentials
        QUIC_CREDENTIAL_CONFIG CredConfig = {0};
        QUIC_CERTIFICATE_FILE CertFile = {0};
        if (config->tls_key_path != NULL && config->tls_cert_path != NULL) {
            CertFile.PrivateKeyFile = config->tls_key_path;
            CertFile.CertificateFile = config->tls_cert_path;
            CredConfig.CertificateFile = &CertFile;
            CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        } else {
            CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
            CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
        }

        Status = relay->msquic->ConfigurationLoadCredential(
            relay->configuration,
            &CredConfig);

        if (QUIC_FAILED(Status)) {
            relay->msquic->ConfigurationClose(relay->configuration);
            relay->configuration = NULL;
            free(relay->active_streams);
            platform_lock_destroy(&relay->lock);
            free(relay);
            return NULL;
        }
    }

    return relay;
}

void meridian_relay_destroy(meridian_relay_t* relay) {
    if (relay == NULL) return;

    refcounter_dereference(&relay->refcounter);
    if (refcounter_count(&relay->refcounter) == 0) {
        // Disconnect if connected
        if (relay->connected) {
            meridian_relay_disconnect(relay);
        }

        // Free all active stream entries
        for (size_t i = 0; i < relay->num_active_streams; i++) {
            meridian_stream_entry_destroy(relay->active_streams[i]);
        }
        free(relay->active_streams);

        // Close configuration if owned
        if (relay->configuration) {
            relay->msquic->ConfigurationClose(relay->configuration);
            relay->configuration = NULL;
        }

        platform_lock_destroy(&relay->lock);
        free(relay);
    }
}

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

int meridian_relay_connect(meridian_relay_t* relay) {
    if (relay == NULL || relay->connected) return -1;

    platform_lock(&relay->lock);

    // Open QUIC connection
    QUIC_STATUS Status = relay->msquic->ConnectionOpen(
        relay->registration,
        RelayConnectionCallback,
        relay,
        &relay->connection);

    if (QUIC_FAILED(Status)) {
        platform_unlock(&relay->lock);
        return -1;
    }

    // Set connection callback
    relay->msquic->SetCallbackHandler(relay->connection, RelayConnectionCallback, relay);

    // Build server address
    QUIC_ADDR RemoteAddr = {0};
#ifdef QUIC_ADDRESS_FAMILY_UNSPEC
    RemoteAddr.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET;
#else
    RemoteAddr.Ipv4.sin_family = AF_INET;
#endif
    RemoteAddr.Ipv4.sin_addr.s_addr = relay->server->addr;
    RemoteAddr.Ipv4.sin_port = relay->server->port;

    // Start connection
    Status = relay->msquic->ConnectionStart(
        relay->connection,
        relay->configuration,
        AF_INET,
        NULL,
        relay->server->port);

    if (QUIC_FAILED(Status)) {
        relay->msquic->ConnectionClose(relay->connection);
        relay->connection = NULL;
        platform_unlock(&relay->lock);
        return -1;
    }

    platform_unlock(&relay->lock);
    return 0;
}

int meridian_relay_disconnect(meridian_relay_t* relay) {
    if (relay == NULL) return -1;

    platform_lock(&relay->lock);

    if (!relay->connected && relay->connection == NULL) {
        platform_unlock(&relay->lock);
        return 0;
    }

    if (relay->connection) {
        relay->msquic->ConnectionShutdown(
            relay->connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
            0);
        relay->msquic->ConnectionClose(relay->connection);
        relay->connection = NULL;
    }

    relay->connected = false;

    platform_unlock(&relay->lock);
    return 0;
}

// ============================================================================
// MESSAGE TRANSMISSION (STREAM-BASED)
// ============================================================================

int meridian_relay_send_datagram(meridian_relay_t* relay, const uint8_t* data,
                                  size_t len, uint32_t dest_endpoint_id) {
    if (relay == NULL || data == NULL || !relay->connected) return -1;

    // Wire format for relay stream: [4 bytes dest_endpoint_id BE] + [4 bytes src_endpoint_id BE] + [payload]
    size_t envelope_len = 8 + len;
    uint8_t* envelope = (uint8_t*)get_memory(envelope_len);
    if (envelope == NULL) return -1;

    // dest_endpoint_id in big-endian
    envelope[0] = (dest_endpoint_id >> 24) & 0xFF;
    envelope[1] = (dest_endpoint_id >> 16) & 0xFF;
    envelope[2] = (dest_endpoint_id >> 8) & 0xFF;
    envelope[3] = dest_endpoint_id & 0xFF;

    // src_endpoint_id in big-endian
    uint32_t src_id = relay->local_endpoint_id;
    envelope[4] = (src_id >> 24) & 0xFF;
    envelope[5] = (src_id >> 16) & 0xFF;
    envelope[6] = (src_id >> 8) & 0xFF;
    envelope[7] = src_id & 0xFF;

    memcpy(envelope + 8, data, len);

    int result = meridian_stream_send_uni(relay->msquic, relay->connection,
                                           envelope, envelope_len, NULL);
    free(envelope);
    return result;
}

// ============================================================================
// ADDRESS DISCOVERY (STREAM-BASED)
// ============================================================================

int meridian_relay_send_addr_request(meridian_relay_t* relay) {
    if (relay == NULL || !relay->connected) return -1;

    // Build CBOR ADDR_REQUEST: [type, query_id_hi, query_id_lo]
    uint64_t query_id = (uint64_t)relay->local_endpoint_id << 32;

    cbor_item_t* array = cbor_new_definite_array(3);
    if (array == NULL) return -1;

    uint64_t qid_1 = query_id >> 32;
    uint64_t qid_2 = query_id & 0xFFFFFFFF;

    if (!cbor_array_push(array, cbor_build_uint8(MERIDIAN_PACKET_TYPE_ADDR_REQUEST)) ||
        !cbor_array_push(array, cbor_build_uint64(qid_1)) ||
        !cbor_array_push(array, cbor_build_uint64(qid_2))) {
        cbor_decref(&array);
        return -1;
    }

    // Serialize CBOR payload
    unsigned char* cbor_buf = NULL;
    size_t cbor_buf_sz = 0;
    size_t cbor_len = cbor_serialize_alloc(array, &cbor_buf, &cbor_buf_sz);
    cbor_decref(&array);

    if (cbor_len == 0 || cbor_buf == NULL) {
        return -1;
    }

    // Wire format for bidi stream: [4 bytes dest_endpoint_id=0 BE] + [CBOR payload]
    size_t msg_len = 4 + cbor_len;
    uint8_t* msg_buf = (uint8_t*)get_memory(msg_len);
    if (msg_buf == NULL) {
        free(cbor_buf);
        return -1;
    }

    // dest_endpoint_id = 0 for ADDR_REQUEST
    msg_buf[0] = 0;
    msg_buf[1] = 0;
    msg_buf[2] = 0;
    msg_buf[3] = 0;

    memcpy(msg_buf + 4, cbor_buf, cbor_len);
    free(cbor_buf);

    // Create stream entry to track the response
    meridian_stream_entry_t* entry = meridian_stream_entry_create(NULL, true, relay);
    if (entry == NULL) {
        free(msg_buf);
        return -1;
    }

    // Open bidi stream and send with FIN
    HQUIC stream = meridian_stream_open_bidi(relay->msquic, relay->connection,
                                               relay_bidi_stream_callback, entry);
    if (stream == NULL) {
        meridian_stream_entry_destroy(entry);
        free(msg_buf);
        return -1;
    }

    entry->stream = stream;

    platform_lock(&relay->lock);
    relay_track_stream(relay, entry);
    platform_unlock(&relay->lock);

    // Copy data into send buffer for SEND_COMPLETE callback
    uint8_t* send_buf = (uint8_t*)get_memory(msg_len);
    if (send_buf == NULL) {
        // Stream is already started; let shutdown_complete clean up the entry
        relay->msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        free(msg_buf);
        return -1;
    }
    memcpy(send_buf, msg_buf, msg_len);
    free(msg_buf);

    QUIC_BUFFER buf;
    buf.Buffer = send_buf;
    buf.Length = (uint32_t)msg_len;

    QUIC_STATUS status = relay->msquic->StreamSend(stream, &buf, 1,
                                                     QUIC_SEND_FLAG_FIN, send_buf);
    if (QUIC_FAILED(status)) {
        log_error("meridian relay: ADDR_REQUEST StreamSend failed: 0x%x", status);
        free(send_buf);
        relay->msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        return -1;
    }

    return 0;
}

// ============================================================================
// CALLBACK REGISTRATION
// ============================================================================

void meridian_relay_on_datagram(meridian_relay_t* relay,
                                 meridian_relay_datagram_cb_t callback, void* ctx) {
    if (relay == NULL) return;

    platform_lock(&relay->lock);
    relay->on_datagram = callback;
    relay->on_datagram_ctx = ctx;
    platform_unlock(&relay->lock);
}

void meridian_relay_on_addr_response(meridian_relay_t* relay,
                                      meridian_relay_addr_cb_t callback, void* ctx) {
    if (relay == NULL) return;

    platform_lock(&relay->lock);
    relay->on_addr_response = callback;
    relay->on_addr_response_ctx = ctx;
    platform_unlock(&relay->lock);
}

// ============================================================================
// RELAY STATE QUERY
// ============================================================================

uint32_t meridian_relay_get_endpoint_id(const meridian_relay_t* relay) {
    if (relay == NULL) return 0;
    return relay->local_endpoint_id;
}

bool meridian_relay_is_connected(const meridian_relay_t* relay) {
    if (relay == NULL) return false;
    return relay->connected;
}

// ============================================================================
// STREAM CALLBACKS
// ============================================================================

/**
 * Processes a complete message received on a unidirectional stream.
 * Wire format: [4 bytes dest_endpoint_id BE] + [4 bytes src_endpoint_id BE] + [payload]
 *
 * If dest_endpoint_id == 0, the payload is a relay control message (e.g., ADDR_RESPONSE).
 * Otherwise, the payload is a datagram forwarded from src_endpoint_id.
 */
static void relay_process_uni_message(meridian_relay_t* relay,
                                       const uint8_t* data, size_t len) {
    if (len < 8) {
        log_warn("meridian relay: uni stream message too short (%zu bytes)", len);
        return;
    }

    // Parse big-endian endpoint IDs
    uint32_t dest_endpoint_id = ((uint32_t)data[0] << 24) |
                                 ((uint32_t)data[1] << 16) |
                                 ((uint32_t)data[2] << 8) |
                                 (uint32_t)data[3];
    uint32_t src_endpoint_id = ((uint32_t)data[4] << 24) |
                                ((uint32_t)data[5] << 16) |
                                ((uint32_t)data[6] << 8) |
                                (uint32_t)data[7];

    const uint8_t* payload = data + 8;
    size_t payload_len = len - 8;

    if (dest_endpoint_id == 0) {
        // Control message for us — decode as CBOR and dispatch
        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(payload, payload_len, &result);
        if (item == NULL || !cbor_array_is_definite(item)) {
            if (item) cbor_decref(&item);
            return;
        }

        if (cbor_array_size(item) < 1) {
            cbor_decref(&item);
            return;
        }

        cbor_item_t** items = cbor_array_handle(item);
        uint8_t packet_type = cbor_get_uint8(items[0]);

        if (packet_type == MERIDIAN_PACKET_TYPE_ADDR_RESPONSE) {
            meridian_addr_response_t* addr_resp = meridian_addr_response_decode(item);
            cbor_decref(&item);

            if (addr_resp != NULL) {
                platform_lock(&relay->lock);
                relay->local_endpoint_id = addr_resp->endpoint_id;
                platform_unlock(&relay->lock);

                if (relay->on_addr_response != NULL) {
                    relay->on_addr_response(
                        relay->on_addr_response_ctx,
                        addr_resp->reflexive_addr,
                        addr_resp->reflexive_port,
                        addr_resp->endpoint_id);
                }

                meridian_addr_response_destroy(addr_resp);
            }
        } else if (packet_type == MERIDIAN_PACKET_TYPE_PUNCH_REQUEST ||
                   packet_type == MERIDIAN_PACKET_TYPE_PUNCH_SYNC) {
            // Forward punch messages to datagram callback
            if (relay->on_datagram != NULL) {
                relay->on_datagram(
                    relay->on_datagram_ctx,
                    payload,
                    payload_len,
                    src_endpoint_id);
            }
            cbor_decref(&item);
        } else {
            cbor_decref(&item);
        }
    } else {
        // Forwarded datagram from a peer
        if (relay->on_datagram != NULL) {
            relay->on_datagram(
                relay->on_datagram_ctx,
                payload,
                payload_len,
                src_endpoint_id);
        }
    }
}

/**
 * Processes a complete message received on a bidirectional stream.
 * Wire format: [4 bytes dest_endpoint_id BE] + [CBOR payload]
 *
 * For ADDR_RESPONSE, dest_endpoint_id will be 0 and we parse the CBOR payload.
 */
static void relay_process_bidi_message(meridian_relay_t* relay,
                                        const uint8_t* data, size_t len) {
    if (len < 4) {
        log_warn("meridian relay: bidi stream message too short (%zu bytes)", len);
        return;
    }

    uint32_t dest_endpoint_id = ((uint32_t)data[0] << 24) |
                                 ((uint32_t)data[1] << 16) |
                                 ((uint32_t)data[2] << 8) |
                                 (uint32_t)data[3];

    const uint8_t* payload = data + 4;
    size_t payload_len = len - 4;

    if (dest_endpoint_id == 0) {
        // ADDR_RESPONSE on a bidi stream
        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(payload, payload_len, &result);
        if (item == NULL || !cbor_array_is_definite(item)) {
            if (item) cbor_decref(&item);
            return;
        }

        if (cbor_array_size(item) < 1) {
            cbor_decref(&item);
            return;
        }

        meridian_addr_response_t* addr_resp = meridian_addr_response_decode(item);
        cbor_decref(&item);

        if (addr_resp != NULL) {
            platform_lock(&relay->lock);
            relay->local_endpoint_id = addr_resp->endpoint_id;
            platform_unlock(&relay->lock);

            if (relay->on_addr_response != NULL) {
                relay->on_addr_response(
                    relay->on_addr_response_ctx,
                    addr_resp->reflexive_addr,
                    addr_resp->reflexive_port,
                    addr_resp->endpoint_id);
            }

            meridian_addr_response_destroy(addr_resp);
        }
    }
}

/**
 * Unidirectional stream callback for relay.
 * Accumulates data until the peer finishes sending (PEER_SEND_SHUTDOWN),
 * then dispatches based on the 8-byte stream header.
 */
static QUIC_STATUS QUIC_API
relay_uni_stream_callback(HQUIC Stream, void* Context,
                          QUIC_STREAM_EVENT* Event) {
    (void)Stream;
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)Context;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("meridian relay: uni stream buffer overflow, dropping data");
                break;
            }
        }

        // Acknowledge all received bytes
        Event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Peer finished sending — process the complete message
        if (entry != NULL && entry->user_context != NULL) {
            meridian_relay_t* relay = (meridian_relay_t*)entry->user_context;
            relay_process_uni_message(relay, entry->recv_buf, entry->recv_len);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

/**
 * Bidirectional stream callback for relay.
 * Accumulates data until the peer finishes sending (PEER_SEND_SHUTDOWN),
 * then processes the response. Used for ADDR_REQUEST/ADDR_RESPONSE.
 */
static QUIC_STATUS QUIC_API
relay_bidi_stream_callback(HQUIC Stream, void* Context,
                           QUIC_STREAM_EVENT* Event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)Context;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("meridian relay: bidi stream buffer overflow, dropping data");
                break;
            }
        }

        Event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Peer finished sending — process the complete message
        if (entry != NULL && entry->user_context != NULL) {
            meridian_relay_t* relay = (meridian_relay_t*)entry->user_context;
            relay_process_bidi_message(relay, entry->recv_buf, entry->recv_len);
        }

        // Shut down our side of the bidi stream since we got our response
        if (Stream != NULL && entry != NULL && entry->user_context != NULL) {
            meridian_relay_t* relay_ctx = (meridian_relay_t*)entry->user_context;
            relay_ctx->msquic->StreamShutdown(
                Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// QUIC CONNECTION CALLBACK
// ============================================================================

static QUIC_STATUS QUIC_API
RelayConnectionCallback(HQUIC Connection, void* Context,
                        QUIC_CONNECTION_EVENT* Event) {
    (void)Connection;
    meridian_relay_t* relay = (meridian_relay_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        platform_lock(&relay->lock);
        relay->connected = true;
        platform_unlock(&relay->lock);

        // Send initial address request on a bidi stream
        meridian_relay_send_addr_request(relay);
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        platform_lock(&relay->lock);
        relay->connected = false;
        platform_unlock(&relay->lock);
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        platform_lock(&relay->lock);
        relay->connected = false;
        relay->connection = NULL;
        platform_unlock(&relay->lock);
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        // Peer initiated a stream — track it with the appropriate callback
        bool is_bidi = !(Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL);
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;

        meridian_stream_entry_t* entry = meridian_stream_entry_create(
            stream, is_bidi, relay);
        if (entry == NULL) {
            // Reject the stream if we can't allocate tracking state
            return QUIC_STATUS_OUT_OF_MEMORY;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? relay_bidi_stream_callback
            : relay_uni_stream_callback;

        relay->msquic->SetCallbackHandler(stream, callback, entry);

        platform_lock(&relay->lock);
        relay_track_stream(relay, entry);
        platform_unlock(&relay->lock);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
