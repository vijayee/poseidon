//
// Created by victor on 4/19/26.
//

#include "meridian_relay.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cbor.h>

// ============================================================================
// CALLBACK FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayConnectionCallback(HQUIC Connection, void* Context,
                        QUIC_CONNECTION_EVENT* Event);

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
        Settings.DatagramReceiveEnabled = TRUE;
        Settings.IsSet.DatagramReceiveEnabled = TRUE;

        QUIC_STATUS Status = relay->msquic->ConfigurationOpen(
            relay->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &relay->configuration);

        if (QUIC_FAILED(Status)) {
            platform_lock_destroy(&relay->lock);
            free(relay);
            return NULL;
        }

        // Load credentials (insecure mode for testing)
        QUIC_CREDENTIAL_CONFIG CredConfig = {0};
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;

        Status = relay->msquic->ConfigurationLoadCredential(
            relay->configuration,
            &CredConfig);

        if (QUIC_FAILED(Status)) {
            relay->msquic->ConfigurationClose(relay->configuration);
            relay->configuration = NULL;
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
// DATAGRAM TRANSMISSION
// ============================================================================

int meridian_relay_send_datagram(meridian_relay_t* relay, const uint8_t* data,
                                  size_t len, uint32_t dest_endpoint_id) {
    if (relay == NULL || data == NULL || !relay->connected) return -1;

    // Build RELAY_DATAGRAM packet: [type, src, dest, payload]
    cbor_item_t* array = cbor_new_definite_array(4);
    if (array == NULL) return -1;

    if (!cbor_array_push(array, cbor_build_uint8(MERIDIAN_PACKET_TYPE_RELAY_DATAGRAM)) ||
        !cbor_array_push(array, cbor_build_uint32(relay->local_endpoint_id)) ||
        !cbor_array_push(array, cbor_build_uint32(dest_endpoint_id)) ||
        !cbor_array_push(array, cbor_build_byte_string(data, len))) {
        cbor_decref(&array);
        return -1;
    }

    // Serialize CBOR
    uint8_t* encoded = NULL;
    size_t encoded_len = cbor_encode_bytes(array, &encoded);
    cbor_decref(&array);

    if (encoded == NULL || encoded_len == 0) {
        return -1;
    }

    // Send via QUIC datagram
    QUIC_BUFFER Buffer = {
        .Length = (uint32_t)encoded_len,
        .Buffer = encoded
    };

    QUIC_STATUS Status = relay->msquic->DatagramSend(
        relay->connection,
        &Buffer, 1,
        QUIC_SEND_FLAG_NONE,
        NULL);

    free(encoded);
    return QUIC_SUCCEEDED(Status) ? 0 : -1;
}

// ============================================================================
// ADDRESS DISCOVERY
// ============================================================================

int meridian_relay_send_addr_request(meridian_relay_t* relay) {
    if (relay == NULL || !relay->connected) return -1;

    // Generate a query ID
    uint64_t query_id = (uint64_t)relay->local_endpoint_id << 32;

    // Build ADDR_REQUEST packet: [type, query_id]
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

    // Serialize CBOR
    uint8_t* encoded = NULL;
    size_t encoded_len = cbor_encode_bytes(array, &encoded);
    cbor_decref(&array);

    if (encoded == NULL || encoded_len == 0) {
        return -1;
    }

    // Send via QUIC datagram
    QUIC_BUFFER Buffer = {
        .Length = (uint32_t)encoded_len,
        .Buffer = encoded
    };

    QUIC_STATUS Status = relay->msquic->DatagramSend(
        relay->connection,
        &Buffer, 1,
        QUIC_SEND_FLAG_NONE,
        NULL);

    free(encoded);
    return QUIC_SUCCEEDED(Status) ? 0 : -1;
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
// QUIC CALLBACKS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayConnectionCallback(HQUIC Connection, void* Context,
                        QUIC_CONNECTION_EVENT* Event) {
    meridian_relay_t* relay = (meridian_relay_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        platform_lock(&relay->lock);
        relay->connected = true;
        platform_unlock(&relay->lock);

        // Send initial address request
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

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        const QUIC_BUFFER* Buffer = Event->DATAGRAM_RECEIVED.Buffer;

        if (Buffer == NULL || Buffer->Length == 0) {
            return QUIC_STATUS_SUCCESS;
        }

        // Decode CBOR packet
        struct cbor_load_result result;
        cbor_item_t* item = cbor_load(Buffer->Buffer, Buffer->Length, &result);
        if (item == NULL || !cbor_array_is_definite(item)) {
            if (item) cbor_decref(&item);
            return QUIC_STATUS_SUCCESS;
        }

        if (cbor_array_size(item) < 1) {
            cbor_decref(&item);
            return QUIC_STATUS_SUCCESS;
        }

        cbor_item_t** items = cbor_array_handle(item);
        uint8_t packet_type = cbor_get_uint8(items[0]);

        switch (packet_type) {
        case MERIDIAN_PACKET_TYPE_ADDR_RESPONSE: {
            cbor_decref(&item);  // Free before potential return

            // Decode address response
            struct cbor_load_result decode_result;
            cbor_item_t* resp_item = cbor_load(Buffer->Buffer, Buffer->Length, &decode_result);
            if (resp_item == NULL) {
                return QUIC_STATUS_SUCCESS;
            }

            meridian_addr_response_t* addr_resp = meridian_addr_response_decode(resp_item);
            cbor_decref(&resp_item);

            if (addr_resp != NULL) {
                platform_lock(&relay->lock);
                relay->local_endpoint_id = addr_resp->endpoint_id;
                platform_unlock(&relay->lock);

                // Invoke callback
                if (relay->on_addr_response != NULL) {
                    relay->on_addr_response(
                        relay->on_addr_response_ctx,
                        addr_resp->reflexive_addr,
                        addr_resp->reflexive_port,
                        addr_resp->endpoint_id);
                }

                meridian_addr_response_destroy(addr_resp);
            }
            return QUIC_STATUS_SUCCESS;
        }

        case MERIDIAN_PACKET_TYPE_RELAY_DATAGRAM: {
            // Format: [type, src, dest, payload]
            if (cbor_array_size(item) >= 4) {
                uint32_t src_endpoint = cbor_get_uint32(items[1]);

                // Get payload (byte string)
                cbor_item_t* payload = items[3];
                uint8_t* payload_data = (uint8_t*)cbor_build_byte_string_data(payload);
                size_t payload_len = cbor_byte_string_length(payload);

                // Invoke datagram callback
                if (relay->on_datagram != NULL && payload_data != NULL) {
                    relay->on_datagram(
                        relay->on_datagram_ctx,
                        payload_data,
                        payload_len,
                        src_endpoint);
                }
            }
            cbor_decref(&item);
            return QUIC_STATUS_SUCCESS;
        }

        case MERIDIAN_PACKET_TYPE_PUNCH_REQUEST:
        case MERIDIAN_PACKET_TYPE_PUNCH_SYNC: {
            // Forward to datagram callback for connection manager to handle
            if (relay->on_datagram != NULL) {
                relay->on_datagram(
                    relay->on_datagram_ctx,
                    Buffer->Buffer,
                    Buffer->Length,
                    0);  // No source endpoint for punch messages
            }
            cbor_decref(&item);
            return QUIC_STATUS_SUCCESS;
        }

        default:
            cbor_decref(&item);
            return QUIC_STATUS_SUCCESS;
        }
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
