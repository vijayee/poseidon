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
// CALLBACK FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
RelayServerListenerCallback(HQUIC Listener, void* Context,
                            QUIC_LISTENER_EVENT* Event);

static QUIC_STATUS QUIC_API
RelayServerConnectionCallback(HQUIC Connection, void* Context,
                              QUIC_CONNECTION_EVENT* Event);

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

    // Initialize client slots
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

    // Configure settings
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = config->idle_timeout_ms ? config->idle_timeout_ms : 30000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.KeepAliveIntervalMs = config->keepalive_interval_ms ? config->keepalive_interval_ms : 10000;
    Settings.IsSet.KeepAliveIntervalMs = TRUE;
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;

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

        // Initialize client
        server->clients[slot].connection = Connection;
        server->clients[slot].endpoint_id = server->next_endpoint_id++;
        server->clients[slot].authenticated = false;
        server->clients[slot].last_activity_ms = 0;
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

            server->clients[slot].connection = NULL;
            server->clients[slot].endpoint_id = MERIDIAN_RELAY_ENDPOINT_ID_NONE;
            server->clients[slot].authenticated = false;
            server->num_clients--;
        }
        platform_unlock(&server->lock);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        const QUIC_BUFFER* Buffer = Event->DATAGRAM_RECEIVED.Buffer;

        // Process datagram - relay to appropriate client
        if (Buffer != NULL && Buffer->Length > 0) {
            // Parse destination endpoint from first 4 bytes
            if (Buffer->Length >= sizeof(uint32_t)) {
                uint32_t dest_endpoint;
                memcpy(&dest_endpoint, Buffer->Buffer, sizeof(uint32_t));

                platform_lock(&server->lock);
                int slot = relay_server_find_client_by_endpoint(server, dest_endpoint);
                if (slot >= 0 && server->clients[slot].connection != NULL) {
                    // Forward to destination (skip endpoint header)
                    QUIC_BUFFER ForwardBuffer = {
                        .Length = Buffer->Length - sizeof(uint32_t),
                        .Buffer = (uint8_t*)Buffer->Buffer + sizeof(uint32_t)
                    };

                    server->msquic->DatagramSend(
                        server->clients[slot].connection,
                        &ForwardBuffer, 1,
                        QUIC_SEND_FLAG_NONE,
                        NULL);

                    log_debug("Relay: Forwarded datagram to endpoint %u", dest_endpoint);
                }
                platform_unlock(&server->lock);
            }
        }

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
