//
// Created by victor on 4/19/26.
//

#include "meridian_relay_server.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ============================================================================
// RELAY STREAM CONTEXT
// ============================================================================
//
// Wraps a stream entry with the relay server and client slot,
// giving stream callbacks direct access to server state without
// needing to iterate the client array.

typedef struct relay_stream_context_t {
    meridian_stream_entry_t* entry;   /**< Stream entry with receive buffer */
    meridian_relay_server_t* server;   /**< Owning relay server */
    int client_slot;                  /**< Index into server->clients[] */
} relay_stream_context_t;

static relay_stream_context_t* relay_stream_context_create(
    meridian_stream_entry_t* entry,
    meridian_relay_server_t* server,
    int client_slot) {
    relay_stream_context_t* ctx = (relay_stream_context_t*)
        get_clear_memory(sizeof(relay_stream_context_t));
    if (ctx == NULL) return NULL;
    ctx->entry = entry;
    ctx->server = server;
    ctx->client_slot = client_slot;
    return ctx;
}

static void relay_stream_context_destroy(relay_stream_context_t* ctx) {
    if (ctx == NULL) return;
    // The stream entry is destroyed separately in SHUTDOWN_COMPLETE
    free(ctx);
}

// ============================================================================
// CALLBACK FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayServerListenerCallback(HQUIC Listener, void* Context,
                            QUIC_LISTENER_EVENT* Event);

static QUIC_STATUS QUIC_API
RelayServerConnectionCallback(HQUIC Connection, void* Context,
                              QUIC_CONNECTION_EVENT* Event);

static QUIC_STATUS QUIC_API
RelayServerUniStreamCallback(HQUIC Stream, void* Context,
                             QUIC_STREAM_EVENT* Event);

static QUIC_STATUS QUIC_API
RelayServerBidiStreamCallback(HQUIC Stream, void* Context,
                              QUIC_STREAM_EVENT* Event);

// ============================================================================
// RELAY SERVER LIFECYCLE
// ============================================================================

meridian_relay_server_t* meridian_relay_server_create(
    const struct QUIC_API_TABLE* msquic,
    const meridian_relay_server_config_t* config) {

    if (msquic == NULL || config == NULL) return NULL;

    meridian_relay_server_t* server = (meridian_relay_server_t*)
        get_clear_memory(sizeof(meridian_relay_server_t));

    if (server == NULL) return NULL;

    server->msquic = msquic;
    server->listen_port = config->listen_port;
    server->num_clients = 0;
    server->next_endpoint_id = 1;

    // Initialize client slots (stream arrays are initialized on connection)
    for (size_t i = 0; i < MERIDIAN_RELAY_MAX_CLIENTS; i++) {
        server->clients[i].endpoint_id = MERIDIAN_RELAY_ENDPOINT_ID_NONE;
        server->clients[i].connection = NULL;
        server->clients[i].authenticated = false;
        server->clients[i].last_activity_ms = 0;
    }

    platform_lock_init(&server->lock);
    refcounter_init(&server->refcounter);

    // Create registration
    QUIC_REGISTRATION_CONFIG RegConfig = {
        .AppName = "meridian_relay",
        .ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY
    };

    QUIC_STATUS Status = server->msquic->RegistrationOpen(&RegConfig, &server->registration);
    if (QUIC_FAILED(Status)) {
        platform_lock_destroy(&server->lock);
        free(server);
        return NULL;
    }

    // Configure ALPN
    QUIC_BUFFER Alpn = {
        (uint32_t)(config->alpn ? strlen(config->alpn) : sizeof("meridian_relay") - 1),
        (uint8_t*)(config->alpn ? config->alpn : "meridian_relay")
    };

    // Configure settings — stream-based, not datagram
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = config->idle_timeout_ms ? config->idle_timeout_ms : 30000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.KeepAliveIntervalMs = config->keepalive_interval_ms ? config->keepalive_interval_ms : 10000;
    Settings.IsSet.KeepAliveIntervalMs = TRUE;
    Settings.PeerBidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    Settings.PeerUnidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
    Settings.IsSet.PeerUnidiStreamCount = TRUE;

    Status = server->msquic->ConfigurationOpen(
        server->registration,
        &Alpn, 1,
        &Settings, sizeof(Settings),
        NULL,
        &server->configuration);

    if (QUIC_FAILED(Status)) {
        server->msquic->RegistrationClose(server->registration);
        platform_lock_destroy(&server->lock);
        free(server);
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
    } else {
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    }

    Status = server->msquic->ConfigurationLoadCredential(
        server->configuration,
        &CredConfig);

    if (QUIC_FAILED(Status)) {
        server->msquic->ConfigurationClose(server->configuration);
        server->msquic->RegistrationClose(server->registration);
        platform_lock_destroy(&server->lock);
        free(server);
        return NULL;
    }

    return server;
}

void meridian_relay_server_destroy(meridian_relay_server_t* server) {
    if (server == NULL) return;

    refcounter_dereference(&server->refcounter);
    if (refcounter_count(&server->refcounter) == 0) {
        // Stop server first
        meridian_relay_server_stop(server);

        // Close configuration
        if (server->configuration) {
            server->msquic->ConfigurationClose(server->configuration);
            server->configuration = NULL;
        }

        // Close registration
        if (server->registration) {
            server->msquic->RegistrationClose(server->registration);
            server->registration = NULL;
        }

        platform_lock_destroy(&server->lock);
        free(server);
    }
}

int meridian_relay_server_start(meridian_relay_server_t* server) {
    if (server == NULL) return -1;

    platform_lock(&server->lock);

    // Create listener
    QUIC_STATUS Status = server->msquic->ListenerOpen(
        server->registration,
        RelayServerListenerCallback,
        server,
        &server->listener);

    if (QUIC_FAILED(Status)) {
        platform_unlock(&server->lock);
        return -1;
    }

    // Set callback handler
    server->msquic->SetCallbackHandler(server->listener, RelayServerListenerCallback, server);

    // Build address
    QUIC_ADDR Address = {0};
#ifdef QUIC_ADDRESS_FAMILY_UNSPEC
    Address.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET;
#else
    Address.Ipv4.sin_family = AF_INET;
#endif
    Address.Ipv4.sin_port = htons(server->listen_port);

    // Start listener
    QUIC_BUFFER Alpn = { sizeof("meridian_relay") - 1, (uint8_t*)"meridian_relay" };
    Status = server->msquic->ListenerStart(
        server->listener,
        &Alpn, 1,
        &Address);

    if (QUIC_FAILED(Status)) {
        server->msquic->ListenerClose(server->listener);
        server->listener = NULL;
        platform_unlock(&server->lock);
        return -1;
    }

    log_info("Relay server started on port %u", server->listen_port);

    platform_unlock(&server->lock);
    return 0;
}

int meridian_relay_server_stop(meridian_relay_server_t* server) {
    if (server == NULL) return -1;

    platform_lock(&server->lock);

    // Disconnect all clients
    for (size_t i = 0; i < MERIDIAN_RELAY_MAX_CLIENTS; i++) {
        if (server->clients[i].connection != NULL) {
            server->msquic->ConnectionShutdown(
                server->clients[i].connection,
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
            server->msquic->ConnectionClose(server->clients[i].connection);
            server->clients[i].connection = NULL;
            server->clients[i].endpoint_id = MERIDIAN_RELAY_ENDPOINT_ID_NONE;
            server->clients[i].authenticated = false;

            // Clean up stream entries
            for (size_t j = 0; j < server->clients[i].num_active_streams; j++) {
                meridian_stream_entry_destroy(server->clients[i].active_streams[j]);
            }
            free(server->clients[i].active_streams);
            server->clients[i].active_streams = NULL;
            server->clients[i].num_active_streams = 0;
            server->clients[i].active_streams_cap = 0;
        }
    }
    server->num_clients = 0;

    // Stop and close listener
    if (server->listener) {
        server->msquic->ListenerStop(server->listener);
        server->msquic->ListenerClose(server->listener);
        server->listener = NULL;
    }

    platform_unlock(&server->lock);
    return 0;
}

// ============================================================================
// STATS
// ============================================================================

int meridian_relay_server_get_stats(meridian_relay_server_t* server,
                                     meridian_relay_server_stats_t* stats) {
    if (server == NULL || stats == NULL) return -1;

    memset(stats, 0, sizeof(*stats));

    platform_lock(&server->lock);
    stats->num_clients = server->num_clients;
    stats->datagrams_forwarded = server->stats.datagrams_forwarded;
    stats->datagrams_dropped = server->stats.datagrams_dropped;
    stats->addr_requests_served = server->stats.addr_requests_served;
    platform_unlock(&server->lock);

    return 0;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static int relay_server_find_client_slot(meridian_relay_server_t* server) {
    for (size_t i = 0; i < MERIDIAN_RELAY_MAX_CLIENTS; i++) {
        if (server->clients[i].endpoint_id == MERIDIAN_RELAY_ENDPOINT_ID_NONE) {
            return (int)i;
        }
    }
    return -1;
}

static int relay_server_find_client_by_connection(meridian_relay_server_t* server,
                                                   HQUIC connection) {
    for (size_t i = 0; i < MERIDIAN_RELAY_MAX_CLIENTS; i++) {
        if (server->clients[i].connection == connection) {
            return (int)i;
        }
    }
    return -1;
}

static int relay_server_find_client_by_endpoint(meridian_relay_server_t* server,
                                                 uint32_t endpoint_id) {
    for (size_t i = 0; i < MERIDIAN_RELAY_MAX_CLIENTS; i++) {
        if (server->clients[i].endpoint_id == endpoint_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Add a stream entry to a client's active_streams list.
 * Returns 0 on success, -1 on failure.
 */
static int relay_server_add_stream(meridian_relay_client_t* client,
                                    meridian_stream_entry_t* entry) {
    if (client->num_active_streams >= client->active_streams_cap) {
        size_t new_cap = client->active_streams_cap * 2;
        meridian_stream_entry_t** new_arr = (meridian_stream_entry_t**)
            realloc(client->active_streams, new_cap * sizeof(meridian_stream_entry_t*));
        if (new_arr == NULL) return -1;
        // Zero the newly allocated region
        memset(new_arr + client->active_streams_cap, 0,
               (new_cap - client->active_streams_cap) * sizeof(meridian_stream_entry_t*));
        client->active_streams = new_arr;
        client->active_streams_cap = new_cap;
    }
    client->active_streams[client->num_active_streams++] = entry;
    return 0;
}

/**
 * Remove a stream entry from a client's active_streams list by pointer.
 * Returns 0 on success, -1 if not found.
 */
static int relay_server_remove_stream(meridian_relay_client_t* client,
                                       meridian_stream_entry_t* entry) {
    for (size_t i = 0; i < client->num_active_streams; i++) {
        if (client->active_streams[i] == entry) {
            client->active_streams[i] = client->active_streams[client->num_active_streams - 1];
            client->active_streams[client->num_active_streams - 1] = NULL;
            client->num_active_streams--;
            return 0;
        }
    }
    return -1;
}

// ============================================================================
// STREAM CALLBACKS
// ============================================================================

/**
 * Unidirectional stream callback for relay server.
 *
 * Accumulates data until PEER_SEND_SHUTDOWN, then:
 * - Reads 4-byte dest_endpoint_id + 4-byte src_endpoint_id header
 * - Looks up destination client by dest_endpoint_id
 * - Opens a uni stream to the destination and forwards the full message
 *   (header + payload)
 */
static QUIC_STATUS QUIC_API
RelayServerUniStreamCallback(HQUIC Stream, void* Context,
                             QUIC_STREAM_EVENT* Event) {
    relay_stream_context_t* ctx = (relay_stream_context_t*)Context;
    meridian_stream_entry_t* entry = (ctx != NULL) ? ctx->entry : NULL;
    meridian_relay_server_t* server = (ctx != NULL) ? ctx->server : NULL;
    int client_slot = (ctx != NULL) ? ctx->client_slot : -1;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("Relay: uni stream buffer overflow, dropping stream");
                break;
            }
        }

        // Acknowledge all received bytes
        Event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        if (entry == NULL || server == NULL) break;

        // Need at least 8 bytes: 4 dest + 4 src
        if (entry->recv_len < 8) {
            log_warn("Relay: uni stream too short (%zu bytes), dropping",
                     entry->recv_len);
            server->stats.datagrams_dropped++;
            break;
        }

        // Parse header — network byte order to host
        uint32_t dest_endpoint_id;
        uint32_t src_endpoint_id;
        memcpy(&dest_endpoint_id, entry->recv_buf, sizeof(uint32_t));
        memcpy(&src_endpoint_id, entry->recv_buf + sizeof(uint32_t), sizeof(uint32_t));
        dest_endpoint_id = ntohl(dest_endpoint_id);
        src_endpoint_id = ntohl(src_endpoint_id);

        // Look up destination client
        platform_lock(&server->lock);
        int dest_slot = relay_server_find_client_by_endpoint(server, dest_endpoint_id);

        if (dest_slot < 0 || server->clients[dest_slot].connection == NULL) {
            log_debug("Relay: no client for endpoint %u, dropping forwarded message",
                      dest_endpoint_id);
            server->stats.datagrams_dropped++;
            platform_unlock(&server->lock);
            break;
        }

        HQUIC dest_connection = server->clients[dest_slot].connection;

        // Forward the full message (header + payload) via uni stream
        int result = meridian_stream_send_uni(server->msquic, dest_connection,
                                               entry->recv_buf, entry->recv_len,
                                               NULL);
        if (result == 0) {
            server->stats.datagrams_forwarded++;
            log_debug("Relay: forwarded uni stream to endpoint %u (from %u, %zu bytes)",
                      dest_endpoint_id, src_endpoint_id, entry->recv_len);
        } else {
            server->stats.datagrams_dropped++;
            log_warn("Relay: failed to forward uni stream to endpoint %u",
                     dest_endpoint_id);
        }

        platform_unlock(&server->lock);
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        // Remove from client's active stream list and free resources
        if (ctx != NULL && server != NULL && client_slot >= 0) {
            platform_lock(&server->lock);
            if ((size_t)client_slot < MERIDIAN_RELAY_MAX_CLIENTS &&
                server->clients[client_slot].active_streams != NULL) {
                relay_server_remove_stream(&server->clients[client_slot], entry);
            }
            platform_unlock(&server->lock);
        }

        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        if (ctx != NULL) {
            ctx->entry = NULL;  // Entry already freed above
            relay_stream_context_destroy(ctx);
        }

        // Close the stream handle
        if (Stream != NULL && server != NULL) {
            server->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

/**
 * Bidirectional stream callback for relay server.
 *
 * Accumulates data until PEER_SEND_SHUTDOWN, then:
 * - Reads 4-byte dest header
 * - If dest==0: ADDR_REQUEST — respond on the same stream with the
 *   source client's reflexive address
 * - If dest!=0: Forward to destination client via uni stream
 */
static QUIC_STATUS QUIC_API
RelayServerBidiStreamCallback(HQUIC Stream, void* Context,
                              QUIC_STREAM_EVENT* Event) {
    relay_stream_context_t* ctx = (relay_stream_context_t*)Context;
    meridian_stream_entry_t* entry = (ctx != NULL) ? ctx->entry : NULL;
    meridian_relay_server_t* server = (ctx != NULL) ? ctx->server : NULL;
    int client_slot = (ctx != NULL) ? ctx->client_slot : -1;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        // Stream is ready for sending, no action needed
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("Relay: bidi stream buffer overflow, dropping data");
                break;
            }
        }

        // Acknowledge all received bytes
        Event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        if (entry == NULL || server == NULL || client_slot < 0) break;

        // Need at least 4 bytes for dest_endpoint_id header
        if (entry->recv_len < sizeof(uint32_t)) {
            log_warn("Relay: bidi stream too short (%zu bytes), dropping",
                     entry->recv_len);
            server->stats.datagrams_dropped++;
            break;
        }

        // Parse dest_endpoint_id from header
        uint32_t dest_endpoint_id;
        memcpy(&dest_endpoint_id, entry->recv_buf, sizeof(uint32_t));
        dest_endpoint_id = ntohl(dest_endpoint_id);

        if (dest_endpoint_id == 0) {
            // ADDR_REQUEST: Client wants to discover its reflexive address.
            platform_lock(&server->lock);

            if ((size_t)client_slot >= MERIDIAN_RELAY_MAX_CLIENTS ||
                server->clients[client_slot].connection == NULL) {
                log_warn("Relay: ADDR_REQUEST from invalid client slot %d", client_slot);
                server->stats.datagrams_dropped++;
                platform_unlock(&server->lock);
                break;
            }

            // Get the source client's reflexive address from their connection
            QUIC_ADDR remote_addr;
            uint32_t addr_size = sizeof(remote_addr);
            QUIC_STATUS addr_status = server->msquic->GetParam(
                server->clients[client_slot].connection,
                QUIC_PARAM_CONN_REMOTE_ADDRESS,
                &addr_size,
                &remote_addr);

            if (QUIC_FAILED(addr_status)) {
                log_warn("Relay: failed to get remote address for ADDR_RESPONSE");
                server->stats.datagrams_dropped++;
                platform_unlock(&server->lock);
                break;
            }

            // Build ADDR_RESPONSE: [4 bytes: dest=0 big-endian] + [CBOR address payload]
            // Encode reflexive address as: family(1) + port(2) + ip(4) = 7 bytes
            // Then wrap in a simple CBOR byte string: 0x47 followed by 7 bytes
            uint32_t dest_zero = htonl(0);

            uint8_t addr_bytes[7];
            addr_bytes[0] = (uint8_t)remote_addr.Ipv4.sin_family;
            memcpy(addr_bytes + 1, &remote_addr.Ipv4.sin_port, 2);
            memcpy(addr_bytes + 3, &remote_addr.Ipv4.sin_addr, 4);

            uint8_t response_buf[4 + 1 + 7];  // dest header + CBOR tag + addr
            memcpy(response_buf, &dest_zero, sizeof(uint32_t));
            response_buf[4] = 0x47;  // CBOR byte string, length 7
            memcpy(response_buf + 5, addr_bytes, 7);

            size_t response_len = sizeof(response_buf);

            // Send response on the same bidi stream
            uint8_t* send_buf = (uint8_t*)get_memory(response_len);
            if (send_buf == NULL) {
                log_warn("Relay: failed to allocate ADDR_RESPONSE send buffer");
                server->stats.datagrams_dropped++;
                platform_unlock(&server->lock);
                break;
            }
            memcpy(send_buf, response_buf, response_len);

            QUIC_BUFFER send_buffer;
            send_buffer.Buffer = send_buf;
            send_buffer.Length = (uint32_t)response_len;

            QUIC_STATUS send_status = server->msquic->StreamSend(
                entry->stream, &send_buffer, 1,
                QUIC_SEND_FLAG_FIN, send_buf);

            if (QUIC_FAILED(send_status)) {
                log_warn("Relay: failed to send ADDR_RESPONSE");
                free(send_buf);
                server->stats.datagrams_dropped++;
            } else {
                server->stats.addr_requests_served++;
                log_debug("Relay: sent ADDR_RESPONSE to endpoint %u",
                          server->clients[client_slot].endpoint_id);
            }

            platform_unlock(&server->lock);
        } else {
            // Forward to destination client via uni stream
            platform_lock(&server->lock);
            int dest_slot = relay_server_find_client_by_endpoint(server, dest_endpoint_id);

            if (dest_slot < 0 || server->clients[dest_slot].connection == NULL) {
                log_debug("Relay: no client for endpoint %u, dropping bidi forward",
                          dest_endpoint_id);
                server->stats.datagrams_dropped++;
                platform_unlock(&server->lock);
                break;
            }

            HQUIC dest_connection = server->clients[dest_slot].connection;

            // Forward the full message (header + payload) via uni stream
            int result = meridian_stream_send_uni(server->msquic, dest_connection,
                                                   entry->recv_buf, entry->recv_len,
                                                   NULL);
            if (result == 0) {
                server->stats.datagrams_forwarded++;
                log_debug("Relay: forwarded bidi stream to endpoint %u (%zu bytes)",
                          dest_endpoint_id, entry->recv_len);
            } else {
                server->stats.datagrams_dropped++;
                log_warn("Relay: failed to forward bidi stream to endpoint %u",
                         dest_endpoint_id);
            }

            platform_unlock(&server->lock);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        // Remove from client's active stream list and free resources
        if (ctx != NULL && server != NULL && client_slot >= 0) {
            platform_lock(&server->lock);
            if ((size_t)client_slot < MERIDIAN_RELAY_MAX_CLIENTS &&
                server->clients[client_slot].active_streams != NULL) {
                relay_server_remove_stream(&server->clients[client_slot], entry);
            }
            platform_unlock(&server->lock);
        }

        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        if (ctx != NULL) {
            ctx->entry = NULL;  // Entry already freed above
            relay_stream_context_destroy(ctx);
        }

        // Close the stream handle
        if (Stream != NULL && server != NULL) {
            server->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// QUIC CALLBACKS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayServerListenerCallback(HQUIC Listener, void* Context,
                            QUIC_LISTENER_EVENT* Event) {
    meridian_relay_server_t* server = (meridian_relay_server_t*)Context;

    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
        HQUIC Connection = Event->NEW_CONNECTION.Connection;

        // Set connection callback
        server->msquic->SetCallbackHandler(Connection, RelayServerConnectionCallback, server);

        // Accept connection
        QUIC_STATUS Status = server->msquic->ConnectionSetConfiguration(
            Connection, server->configuration);

        if (QUIC_FAILED(Status)) {
            return Status;
        }

        platform_lock(&server->lock);

        // Find empty client slot
        int slot = relay_server_find_client_slot(server);
        if (slot < 0) {
            platform_unlock(&server->lock);
            return QUIC_STATUS_CONNECTION_REFUSED;  // Too many connections
        }

        // Initialize client with stream tracking
        server->clients[slot].connection = Connection;
        server->clients[slot].endpoint_id = server->next_endpoint_id++;
        server->clients[slot].authenticated = false;
        server->clients[slot].last_activity_ms = 0;
        server->clients[slot].active_streams_cap = 16;
        server->clients[slot].active_streams = (meridian_stream_entry_t**)
            get_clear_memory(server->clients[slot].active_streams_cap *
                              sizeof(meridian_stream_entry_t*));
        server->clients[slot].num_active_streams = 0;
        server->num_clients++;

        platform_unlock(&server->lock);

        log_info("Relay: New connection, assigned endpoint_id=%u",
                 server->clients[slot].endpoint_id);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }
}

static QUIC_STATUS QUIC_API
RelayServerConnectionCallback(HQUIC Connection, void* Context,
                              QUIC_CONNECTION_EVENT* Event) {
    meridian_relay_server_t* server = (meridian_relay_server_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        log_info("Relay: Client connected");
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        log_info("Relay: Client connection shutting down");
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        // Find and clean up client slot
        platform_lock(&server->lock);
        int slot = relay_server_find_client_by_connection(server, Connection);
        if (slot >= 0) {
            log_info("Relay: Client disconnected, endpoint_id=%u",
                     server->clients[slot].endpoint_id);

            // Clean up active streams
            for (size_t i = 0; i < server->clients[slot].num_active_streams; i++) {
                meridian_stream_entry_destroy(server->clients[slot].active_streams[i]);
            }
            free(server->clients[slot].active_streams);
            server->clients[slot].active_streams = NULL;
            server->clients[slot].num_active_streams = 0;
            server->clients[slot].active_streams_cap = 0;

            server->clients[slot].connection = NULL;
            server->clients[slot].endpoint_id = MERIDIAN_RELAY_ENDPOINT_ID_NONE;
            server->clients[slot].authenticated = false;
            server->num_clients--;
        }
        platform_unlock(&server->lock);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC Stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = (Event->PEER_STREAM_STARTED.Flags &
                        QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) == 0;

        platform_lock(&server->lock);
        int slot = relay_server_find_client_by_connection(server, Connection);
        if (slot < 0) {
            platform_unlock(&server->lock);
            log_warn("Relay: stream started on unknown connection, rejecting");
            server->msquic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return QUIC_STATUS_SUCCESS;
        }

        // Create stream entry to accumulate received data
        meridian_stream_entry_t* entry = meridian_stream_entry_create(
            Stream, is_bidi, NULL);
        if (entry == NULL) {
            platform_unlock(&server->lock);
            log_warn("Relay: failed to create stream entry, rejecting stream");
            server->msquic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return QUIC_STATUS_SUCCESS;
        }

        // Create relay stream context with server pointer and client slot
        relay_stream_context_t* stream_ctx = relay_stream_context_create(
            entry, server, slot);
        if (stream_ctx == NULL) {
            meridian_stream_entry_destroy(entry);
            platform_unlock(&server->lock);
            log_warn("Relay: failed to create stream context, rejecting stream");
            server->msquic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return QUIC_STATUS_SUCCESS;
        }

        // Set appropriate callback with stream context
        if (is_bidi) {
            server->msquic->SetCallbackHandler(Stream, RelayServerBidiStreamCallback,
                                                stream_ctx);
        } else {
            server->msquic->SetCallbackHandler(Stream, RelayServerUniStreamCallback,
                                                stream_ctx);
        }

        // Add to client's active streams
        relay_server_add_stream(&server->clients[slot], entry);
        platform_unlock(&server->lock);

        log_debug("Relay: %s stream started on connection (endpoint %u)",
                  is_bidi ? "bidi" : "uni",
                  server->clients[slot].endpoint_id);
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}