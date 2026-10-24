//
// Created by victor on 4/19/26.
//

#include "meridian_relay.h"
#include "meridian_packet.h"
#include "meridian_rendv.h"
#include "meridian_rendv_messages.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../Platform/platform_socket.h"
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

static void relay_process_uni_message(meridian_relay_t* relay,
                                       const uint8_t* data, size_t len);
static void relay_process_bidi_message(meridian_relay_t* relay,
                                        const uint8_t* data, size_t len);
static void relay_stream_message(void* ctx, meridian_stream_entry_t* entry,
                                 const uint8_t* data, size_t len);

static void meridian_relay_dispatch(void* state, message_t* msg);

// ============================================================================
// ACTOR DISPATCH
// ============================================================================

/**
 * The relay's actor is a work queue, not the owner of the relay's state.
 *
 * The relay is a foreign-thread subsystem: msquic delivers the
 * connection and the stream events on its own worker threads, and the
 * application calls the public API from its own thread. Upstream's relay
 * client solves this with the same split — the actor carries the
 * outbound work that must not run on a callback thread, the flags that
 * are read across threads are atomic, and the connection table is
 * guarded by a small mutex. This file follows that shape.
 *
 * The one message is the address request: the msquic connection
 * callback hands it over so the request is not built on msquic's worker
 * thread.
 */
static void meridian_relay_dispatch(void* state, message_t* msg) {
    meridian_relay_t* relay = (meridian_relay_t*)state;

    switch (msg->type) {
    case MERIDIAN_RELAY_ADDR_REQUEST:
        meridian_relay_send_addr_request(relay);
        break;
    default:
        break;
    }
}

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

/**
 * Removes a stream entry from the relay's active_streams tracking array.
 * Caller must hold relay->lock.
 */
static void relay_remove_stream(meridian_relay_t* relay,
                                 meridian_stream_entry_t* entry) {
    if (relay == NULL || entry == NULL) return;

    for (size_t i = 0; i < relay->num_active_streams; i++) {
        if (relay->active_streams[i] == entry) {
            relay->active_streams[i] =
                relay->active_streams[relay->num_active_streams - 1];
            relay->num_active_streams--;
            break;
        }
    }
}

// ============================================================================
// RELAY CLIENT LIFECYCLE
// ============================================================================

/**
 * @brief Releases the pool the relay created for itself, if it created one.
 *
 * Called on the failure paths of the create and on the way out of the
 * destroy. The pool is the relay's own, so the relay stops it before
 * it lets it go.
 */
static void relay_release_own_pool(meridian_relay_t* relay) {
    if (relay == NULL || relay->own_pool == NULL) return;
    scheduler_pool_stop(relay->own_pool);
    scheduler_pool_destroy(relay->own_pool);
    relay->own_pool = NULL;
    relay->pool = NULL;
}

meridian_relay_t* meridian_relay_create(const struct QUIC_API_TABLE* msquic,
                                        HQUIC registration,
                                        meridian_rendv_t* server,
                                        meridian_rendv_handle_t* handle,
                                        const meridian_relay_config_t* config,
                                        scheduler_pool_t* pool) {
    if (msquic == NULL || server == NULL) return NULL;

    meridian_relay_t* relay = (meridian_relay_t*)
        get_clear_memory(sizeof(meridian_relay_t));
    if (relay == NULL) return NULL;

    relay->msquic = msquic;
    relay->registration = registration;
    relay->server = server;
    relay->handle = handle;
    relay->configuration = NULL;
    relay->connection = NULL;
    ATOMIC_STORE(&relay->local_endpoint_id, 0);
    ATOMIC_STORE(&relay->connected, 0);
    relay->on_datagram = NULL;
    relay->on_datagram_ctx = NULL;
    relay->max_retries = (config != NULL) ? config->max_retries : 0;
    relay->retry_count = 0;
    relay->retry_delay_ms = (config != NULL) ? config->retry_delay_ms : 0;
    relay->shutdown_pending = 0;
    relay->wheel = (config != NULL) ? config->wheel : NULL;
    relay->pool = pool;
    relay->own_pool = NULL;

    // An actor with no pool is never scheduled, and a mailbox that is
    // never drained is a leak, not a fallback. The relay therefore
    // creates and owns a pool of its own when the caller gave none —
    // the same rule the unix transport follows.
    if (relay->pool == NULL) {
        relay->own_pool = scheduler_pool_create(1);
        if (relay->own_pool == NULL) {
            free(relay);
            return NULL;
        }
        scheduler_pool_start(relay->own_pool);
        relay->pool = relay->own_pool;
    }

    relay->active_streams_cap = 16;
    relay->active_streams = (meridian_stream_entry_t**)
        get_clear_memory(relay->active_streams_cap * sizeof(meridian_stream_entry_t*));
    if (relay->active_streams == NULL) {
        relay_release_own_pool(relay);
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

    relay->lock = platform_mutex_create();
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
            platform_mutex_destroy(relay->lock);
            relay_release_own_pool(relay);
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
            /* No certificate: the client does not validate the server's.
             * MsQuic v2 only accepts a NONE credential for a client —
             * the TLS layer returns QUIC_STATUS_INVALID_PARAMETER for a
             * server (deps/msquic/src/platform/tls_openssl.c). */
            CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
            CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
        }

        Status = relay->msquic->ConfigurationLoadCredential(
            relay->configuration,
            &CredConfig);

        if (QUIC_FAILED(Status)) {
            relay->msquic->ConfigurationClose(relay->configuration);
            relay->configuration = NULL;
            free(relay->active_streams);
            platform_mutex_destroy(relay->lock);
            relay_release_own_pool(relay);
            free(relay);
            return NULL;
        }
    }

    actor_init(&relay->actor, relay, meridian_relay_dispatch, relay->pool);

    return relay;
}

void meridian_relay_destroy(meridian_relay_t* relay) {
    if (relay == NULL) return;

    /* The client is going away: no retry may fire from here on. */
    relay->shutdown_pending = 1;
    ATOMIC_STORE(&relay->running, 0);

    refcounter_dereference((refcounter_t*)relay);
    if (refcounter_count(&relay->refcounter) == 0) {
        // Disconnect if connected
        if (ATOMIC_LOAD(&relay->connected)) {
            meridian_relay_disconnect(relay);
        }

        // Free all active stream entries
        platform_mutex_lock(relay->lock);
        for (size_t i = 0; i < relay->num_active_streams; i++) {
            meridian_stream_entry_destroy(relay->active_streams[i]);
        }
        free(relay->active_streams);
        relay->active_streams = NULL;
        relay->num_active_streams = 0;
        platform_mutex_unlock(relay->lock);

        // Close configuration if owned
        if (relay->configuration) {
            relay->msquic->ConfigurationClose(relay->configuration);
            relay->configuration = NULL;
        }

        actor_destroy(&relay->actor);
        platform_mutex_destroy(relay->lock);
        relay_release_own_pool(relay);
        free(relay);
    }
}

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

int meridian_relay_connect(meridian_relay_t* relay) {
    if (relay == NULL) return -1;

    if (ATOMIC_LOAD(&relay->connected)) return -1;

    ATOMIC_STORE(&relay->running, 1);
    /* A fresh attempt starts with a fresh budget: upstream resets the
     * counter when the connection comes up
     * (liboffs/src/Network/relay_client.c:255). */
    relay->retry_count = 0;

    // Open QUIC connection. The handle is kept in a local until the
    // connection is published under the lock: an msquic call must never
    // run while the lock is held, because msquic may answer it with a
    // callback on this same thread, and that callback takes the lock.
    HQUIC connection = NULL;
    QUIC_STATUS Status = relay->msquic->ConnectionOpen(
        relay->registration,
        RelayConnectionCallback,
        relay,
        &connection);

    if (QUIC_FAILED(Status)) {
        return -1;
    }

    platform_mutex_lock(relay->lock);
    relay->connection = connection;
    platform_mutex_unlock(relay->lock);

    // Set connection callback
    relay->msquic->SetCallbackHandler(connection, RelayConnectionCallback, relay);

    // Start the connection. MsQuic v2 resolves the server NAME, so the
    // address has to be a name and not a socket address. Upstream dials
    // the same way (liboffs/src/Network/quic_listener.c:1163 passes the
    // host string and QUIC_ADDRESS_FAMILY_UNSPEC).
    platform_address_t server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.family = PLATFORM_AF_INET;
    server_address.inet.addr = relay->server->addr;
    server_address.inet.port = relay->server->port;
    char host[64];
    if (platform_address_to_string(&server_address, host, sizeof(host)) != 0) {
        platform_mutex_lock(relay->lock);
        relay->connection = NULL;
        platform_mutex_unlock(relay->lock);
        relay->msquic->ConnectionClose(connection);
        return -1;
    }

    Status = relay->msquic->ConnectionStart(
        connection,
        relay->configuration,
        QUIC_ADDRESS_FAMILY_UNSPEC,
        host,
        relay->server->port);

    if (QUIC_FAILED(Status)) {
        platform_mutex_lock(relay->lock);
        relay->connection = NULL;
        platform_mutex_unlock(relay->lock);
        relay->msquic->ConnectionClose(connection);
        return -1;
    }

    return 0;
}

int meridian_relay_disconnect(meridian_relay_t* relay) {
    if (relay == NULL) return -1;

    /* The application asked for the shutdown: the retry must not fire
     * while the connection is going away. Upstream marks the same flag
     * before it tears the client down
     * (liboffs/src/Network/relay_client.c). */
    relay->shutdown_pending = 1;

    // Take the connection handle out of the relay and close it outside
    // the lock. msquic may fire SHUTDOWN_COMPLETE synchronously, and
    // that callback takes the lock itself.
    platform_mutex_lock(relay->lock);
    HQUIC connection = relay->connection;
    relay->connection = NULL;
    platform_mutex_unlock(relay->lock);

    if (connection != NULL) {
        relay->msquic->ConnectionShutdown(
            connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
            0);
        relay->msquic->ConnectionClose(connection);
    }

    ATOMIC_STORE(&relay->connected, 0);

    return 0;
}

// ============================================================================
// MESSAGE TRANSMISSION (STREAM-BASED)
// ============================================================================

int meridian_relay_send_datagram(meridian_relay_t* relay, const uint8_t* data,
                                  size_t len, uint32_t dest_endpoint_id) {
    if (relay == NULL || data == NULL) return -1;

    if (!ATOMIC_LOAD(&relay->connected)) return -1;

    platform_mutex_lock(relay->lock);
    HQUIC connection = relay->connection;
    uint32_t src_id = ATOMIC_LOAD(&relay->local_endpoint_id);
    platform_mutex_unlock(relay->lock);

    if (connection == NULL) return -1;

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
    envelope[4] = (src_id >> 24) & 0xFF;
    envelope[5] = (src_id >> 16) & 0xFF;
    envelope[6] = (src_id >> 8) & 0xFF;
    envelope[7] = src_id & 0xFF;

    memcpy(envelope + 8, data, len);

    int result = meridian_stream_send_uni(relay->msquic, connection,
                                           envelope, envelope_len, NULL);
    free(envelope);
    return result;
}

// ============================================================================
// ADDRESS DISCOVERY (STREAM-BASED)
// ============================================================================

int meridian_relay_send_addr_request(meridian_relay_t* relay) {
    if (relay == NULL) return -1;

    if (!ATOMIC_LOAD(&relay->connected)) return -1;

    platform_mutex_lock(relay->lock);
    HQUIC connection = relay->connection;
    uint64_t query_id = (uint64_t)ATOMIC_LOAD(&relay->local_endpoint_id) << 32;
    platform_mutex_unlock(relay->lock);

    if (connection == NULL) return -1;

    // Build CBOR ADDR_REQUEST: [type, query_id_hi, query_id_lo]
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
    meridian_stream_entry_t* entry = meridian_stream_entry_create(NULL, true, relay,
                                                                   relay->pool);
    if (entry != NULL) {
        meridian_stream_entry_on_message(entry, relay_stream_message);
    }
    if (entry == NULL) {
        free(msg_buf);
        return -1;
    }

    // Open bidi stream and send with FIN
    HQUIC stream = meridian_stream_open_bidi(relay->msquic, connection,
                                               relay_bidi_stream_callback, entry);
    if (stream == NULL) {
        meridian_stream_entry_destroy(entry);
        free(msg_buf);
        return -1;
    }

    entry->stream = stream;

    platform_mutex_lock(relay->lock);
    relay_track_stream(relay, entry);
    platform_mutex_unlock(relay->lock);

    // The request goes out with the send helper: the bytes and the
    // buffer that points at them live until the SEND_COMPLETE event.
    if (meridian_stream_send(relay->msquic, stream, msg_buf, msg_len,
                             QUIC_SEND_FLAG_FIN) != 0) {
        // Stream is already started; let shutdown_complete clean up the entry
        relay->msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        free(msg_buf);
        return -1;
    }
    free(msg_buf);

    return 0;
}

// ============================================================================
// CALLBACK REGISTRATION
// ============================================================================

void meridian_relay_on_datagram(meridian_relay_t* relay,
                                 meridian_relay_datagram_cb_t callback, void* ctx) {
    if (relay == NULL) return;

    platform_mutex_lock(relay->lock);
    relay->on_datagram = callback;
    relay->on_datagram_ctx = ctx;
    platform_mutex_unlock(relay->lock);
}

// ============================================================================
// RELAY STATE QUERY
// ============================================================================

uint32_t meridian_relay_get_endpoint_id(meridian_relay_t* relay) {
    if (relay == NULL) return 0;
    return ATOMIC_LOAD(&relay->local_endpoint_id);
}

bool meridian_relay_is_connected(meridian_relay_t* relay) {
    if (relay == NULL) return false;
    return ATOMIC_LOAD(&relay->connected) != 0;
}

/**
 * The backoff fired: the reconnect is the actor's work.
 *
 * The wheel calls this on its own thread, so the reconnect is handed to
 * the actor as an event — the same way every other piece of work reaches
 * the relay.
 */
static void relay_retry_fire(void* ctx) {
    meridian_relay_t* relay = (meridian_relay_t*)ctx;
    if (relay == NULL) return;

    message_t msg = {
        .type = MERIDIAN_RELAY_RETRY,
        .payload = NULL,
        .payload_destroy = NULL,
    };
    actor_send(&relay->actor, &msg);
}

/**
 * Hands the relay's address response to the rendezvous handle's actor.
 *
 * This is upstream's shape: the relay client posts the address response
 * to the NAT detector's actor as an event
 * (liboffs/src/Network/relay_client.c, NETWORK_ADDR_RESPONSE), and the
 * detector answers with its own event. A callback would run on the
 * relay's stream actor — a pool worker — and the worker that waits for
 * an answer is a worker that runs no actor.
 */
static void relay_post_addr_response(meridian_relay_t* relay,
                                      uint32_t reflexive_addr,
                                      uint16_t reflexive_port,
                                      uint32_t endpoint_id) {
    if (relay == NULL || relay->handle == NULL) return;

    meridian_rendv_addr_response_msg_t* request =
        (meridian_rendv_addr_response_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_addr_response_msg_t));
    if (request == NULL) return;
    request->relay = relay;
    request->reflexive_addr = reflexive_addr;
    request->reflexive_port = reflexive_port;
    request->endpoint_id = endpoint_id;

    /* The event is the worker's to free: the request carries no reply
     * block, because nobody waits for this one. */
    message_t msg = {
        .type = MERIDIAN_RENDV_ADDR_RESPONSE,
        .payload = request,
        .payload_destroy = NULL,
    };
    if (!actor_send(&relay->handle->actor, &msg)) {
        free(request);
    }
}

// ============================================================================
// STREAM CALLBACKS
// ============================================================================

/**
 * The owner handler for a tunnel stream.
 *
 * It runs on the stream entry's actor: the receive buffer is the actor's,
 * and the message is decoded there rather than on msquic's worker.
 */
static void relay_stream_message(void* ctx, meridian_stream_entry_t* entry,
                                 const uint8_t* data, size_t len) {
    meridian_relay_t* relay = (meridian_relay_t*)ctx;
    if (relay == NULL) return;
    if (entry != NULL && entry->is_bidi) {
        relay_process_bidi_message(relay, data, len);
    } else {
        relay_process_uni_message(relay, data, len);
    }
}

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
                ATOMIC_STORE(&relay->local_endpoint_id, addr_resp->endpoint_id);

                /* The relay's view of this node's address: the client
                 * keeps it, the way upstream's relay client keeps its
                 * reflexive_addr and reflexive_port
                 * (liboffs/src/Network/relay_client.h). */
                relay->reflexive_addr = addr_resp->reflexive_addr;
                relay->reflexive_port = addr_resp->reflexive_port;

                relay_post_addr_response(relay,
                                         addr_resp->reflexive_addr,
                                         addr_resp->reflexive_port,
                                         addr_resp->endpoint_id);

                meridian_addr_response_destroy(addr_resp);
            }
        } else if (packet_type == MERIDIAN_PACKET_TYPE_PUNCH_REQUEST ||
                   packet_type == MERIDIAN_PACKET_TYPE_PUNCH_SYNC) {
            // Forward punch messages to datagram callback
            platform_mutex_lock(relay->lock);
            meridian_relay_datagram_cb_t callback = relay->on_datagram;
            void* callback_ctx = relay->on_datagram_ctx;
            platform_mutex_unlock(relay->lock);

            if (callback != NULL) {
                callback(callback_ctx, payload, payload_len, src_endpoint_id);
            }
            cbor_decref(&item);
        } else {
            cbor_decref(&item);
        }
    } else {
        // Forwarded datagram from a peer
        platform_mutex_lock(relay->lock);
        meridian_relay_datagram_cb_t callback = relay->on_datagram;
        void* callback_ctx = relay->on_datagram_ctx;
        platform_mutex_unlock(relay->lock);

        if (callback != NULL) {
            callback(callback_ctx, payload, payload_len, src_endpoint_id);
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
            ATOMIC_STORE(&relay->local_endpoint_id, addr_resp->endpoint_id);

            relay_post_addr_response(relay,
                                     addr_resp->reflexive_addr,
                                     addr_resp->reflexive_port,
                                     addr_resp->endpoint_id);

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

        /* The bytes are copied into the entry's buffer, so all of them
         * are consumed. msquic reads the length on the way out, and a
         * zero would say that nothing was taken: the data would never
         * be acknowledged and the peer's FIN would never arrive. Left
         * unchanged it says "all of it"
         * (deps/msquic/docs/Streams.md). */
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // The entry's actor hands the message to the owner.
        meridian_stream_entry_complete(entry);
        if (Stream != NULL && entry != NULL && entry->user_context != NULL) {
            meridian_relay_t* relay_ctx = (meridian_relay_t*)entry->user_context;
            relay_ctx->msquic->StreamShutdown(
                Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // The send context holds the bytes and the buffer that pointed
        // at them; both go back here.
        meridian_stream_send_complete(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_relay_t* relay_ctx = (entry != NULL && entry->user_context != NULL)
            ? (meridian_relay_t*)entry->user_context : NULL;
        if (entry != NULL) {
            if (relay_ctx != NULL) {
                platform_mutex_lock(relay_ctx->lock);
                relay_remove_stream(relay_ctx, entry);
                platform_mutex_unlock(relay_ctx->lock);
            }
            meridian_stream_entry_destroy(entry);
        }
        if (Stream != NULL && relay_ctx != NULL) {
            relay_ctx->msquic->StreamClose(Stream);
        }
        break;
    }

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

        /* The bytes are copied into the entry's buffer, so all of them
         * are consumed. msquic reads the length on the way out, and a
         * zero would say that nothing was taken: the data would never
         * be acknowledged and the peer's FIN would never arrive. Left
         * unchanged it says "all of it"
         * (deps/msquic/docs/Streams.md). */
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // The entry's actor hands the message to the owner.
        meridian_stream_entry_complete(entry);
        if (Stream != NULL && entry != NULL && entry->user_context != NULL) {
            meridian_relay_t* relay_ctx = (meridian_relay_t*)entry->user_context;
            relay_ctx->msquic->StreamShutdown(
                Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // The send context holds the bytes and the buffer that pointed
        // at them; both go back here.
        meridian_stream_send_complete(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_relay_t* relay_ctx = (entry != NULL && entry->user_context != NULL)
            ? (meridian_relay_t*)entry->user_context : NULL;
        if (entry != NULL) {
            if (relay_ctx != NULL) {
                platform_mutex_lock(relay_ctx->lock);
                relay_remove_stream(relay_ctx, entry);
                platform_mutex_unlock(relay_ctx->lock);
            }
            meridian_stream_entry_destroy(entry);
        }
        if (Stream != NULL && relay_ctx != NULL) {
            relay_ctx->msquic->StreamClose(Stream);
        }
        break;
    }

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
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        ATOMIC_STORE(&relay->connected, 1);
        /* The connection came up: the retry budget starts over. */
        relay->retry_count = 0;

        // Ask the relay server for our reflexive address. The request is
        // handed to the relay's actor so it is not built on msquic's
        // worker thread; the actor's pool owns the work, the way
        // upstream's relay client hands its work to the pool.
        message_t msg = {
            .type = MERIDIAN_RELAY_ADDR_REQUEST,
            .payload = NULL,
            .payload_destroy = NULL,
        };
        actor_send(&relay->actor, &msg);
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        ATOMIC_STORE(&relay->connected, 0);
        /* The relay dropped us. Schedule a retry, unless the application
         * asked for the shutdown or the budget is spent: a bounced relay
         * server orphans the client until the process restarts, and the
         * retry budget bounds the loop for a relay that is gone for good.
         * Upstream counts the same two shutdown events
         * (liboffs/src/Network/relay_client.c). */
        if (!relay->shutdown_pending && relay->retry_count < relay->max_retries) {
            relay->retry_count++;
            log_info("Relay: scheduling retry %u/%u", relay->retry_count,
                     relay->max_retries);
        }
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        ATOMIC_STORE(&relay->connected, 0);
        platform_mutex_lock(relay->lock);
        relay->connection = NULL;
        platform_mutex_unlock(relay->lock);

        /* If a retry is scheduled, arm the backoff. The wheel fires it
         * on its own thread and the reconnect is the actor's work, so no
         * worker sleeps through the delay. */
        if (relay->retry_count > 0 && relay->retry_count <= relay->max_retries &&
            !relay->shutdown_pending && ATOMIC_LOAD(&relay->running) &&
            relay->wheel != NULL) {
            uint32_t delay_ms = relay->retry_delay_ms *
                                (1u << (relay->retry_count - 1));
            log_info("Relay: reconnecting in %u ms (attempt %u/%u)", delay_ms,
                     relay->retry_count, relay->max_retries);
            hierarchical_timing_wheel_set_timer(
                relay->wheel, relay, relay_retry_fire, NULL,
                (timer_duration_t){.milliseconds = delay_ms});
        }
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        // Peer initiated a stream — track it with the appropriate callback
        bool is_bidi = !(Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL);
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;

        meridian_stream_entry_t* entry = meridian_stream_entry_create(
            stream, is_bidi, relay, relay->pool);
        if (entry != NULL) {
            meridian_stream_entry_on_message(entry, relay_stream_message);
        }
        if (entry == NULL) {
            // Reject the stream if we can't allocate tracking state
            return QUIC_STATUS_OUT_OF_MEMORY;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? relay_bidi_stream_callback
            : relay_uni_stream_callback;

        relay->msquic->SetCallbackHandler(stream, callback, entry);

        platform_mutex_lock(relay->lock);
        relay_track_stream(relay, entry);
        platform_mutex_unlock(relay->lock);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
