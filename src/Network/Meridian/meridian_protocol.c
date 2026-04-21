//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_protocol.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <cbor.h>

// ============================================================================
// CALLBACK FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
ListenerCallback(HQUIC Listener, void* Context,
                 QUIC_LISTENER_EVENT* Event);

static QUIC_STATUS QUIC_API
ConnectionCallback(HQUIC Connection, void* Context,
                   QUIC_CONNECTION_EVENT* Event);

static void gossip_outbound_cb(void* ctx, const uint8_t* data, size_t len,
                                const meridian_node_t* target) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)ctx;
    meridian_protocol_send_packet(protocol, data, len, target);
}

// ============================================================================
// PROTOCOL LIFECYCLE
// ============================================================================

meridian_protocol_t* meridian_protocol_create(const meridian_protocol_config_t* config) {
    if (config == NULL) return NULL;

    meridian_protocol_t* protocol = (meridian_protocol_t*)
        get_clear_memory(sizeof(meridian_protocol_t));

    protocol->config.listen_port = config->listen_port;
    protocol->config.info_port = config->info_port;
    protocol->config.primary_ring_size = config->primary_ring_size;
    protocol->config.secondary_ring_size = config->secondary_ring_size;
    protocol->config.ring_exponent_base = config->ring_exponent_base;
    protocol->config.init_gossip_interval_s = config->init_gossip_interval_s;
    protocol->config.num_init_gossip_intervals = config->num_init_gossip_intervals;
    protocol->config.steady_state_gossip_interval_s = config->steady_state_gossip_interval_s;
    protocol->config.replace_interval_s = config->replace_interval_s;
    protocol->config.gossip_timeout_ms = config->gossip_timeout_ms;
    protocol->config.measure_timeout_ms = config->measure_timeout_ms;
    protocol->config.tls_key_path = config->tls_key_path;
    protocol->config.tls_cert_path = config->tls_cert_path;
    memcpy(&protocol->config.local_node_id, &config->local_node_id, sizeof(poseidon_node_id_t));
    protocol->config.pool = config->pool;
    protocol->config.wheel = config->wheel;

    protocol->state = MERIDIAN_PROTOCOL_STATE_INIT;
    protocol->msquic = NULL;
    protocol->registration = NULL;
    protocol->listener = NULL;
    protocol->configuration = NULL;
    protocol->running = false;

    protocol->ring_set = meridian_ring_set_create(
        config->primary_ring_size,
        config->secondary_ring_size,
        config->ring_exponent_base
    );

    protocol->rendv_handle = meridian_rendv_handle_create();
    protocol->latency_cache = meridian_latency_cache_create(MERIDIAN_PROBE_CACHE_SIZE);

    protocol->num_seed_nodes = 0;
    protocol->num_connected_peers = 0;

    // Initialize NAT traversal fields
    protocol->connections = NULL;
    protocol->num_connections = 0;
    protocol->default_relay = NULL;

    platform_lock_init(&protocol->lock);
    refcounter_init(&protocol->refcounter);

    return protocol;
}

void meridian_protocol_destroy(meridian_protocol_t* protocol) {
    if (protocol == NULL) return;

    refcounter_dereference(&protocol->refcounter);
    if (refcounter_count(&protocol->refcounter) == 0) {
        if (protocol->running) {
            meridian_protocol_stop(protocol);
        }

        if (protocol->configuration) {
            protocol->msquic->ConfigurationClose(protocol->configuration);
            protocol->configuration = NULL;
        }

        if (protocol->registration) {
            protocol->msquic->RegistrationClose(protocol->registration);
            protocol->registration = NULL;
        }

        if (protocol->msquic) {
            MsQuicClose(protocol->msquic);
            protocol->msquic = NULL;
        }

        if (protocol->ring_set) {
            meridian_ring_set_destroy(protocol->ring_set);
        }

        if (protocol->rendv_handle) {
            meridian_rendv_handle_destroy(protocol->rendv_handle);
        }

        if (protocol->latency_cache) {
            meridian_latency_cache_destroy(protocol->latency_cache);
        }

        for (size_t i = 0; i < protocol->num_seed_nodes; i++) {
            if (protocol->seed_nodes[i]) {
                refcounter_dereference(&protocol->seed_nodes[i]->refcounter);
                if (refcounter_count(&protocol->seed_nodes[i]->refcounter) == 0) {
                    free(protocol->seed_nodes[i]);
                }
            }
        }

        for (size_t i = 0; i < protocol->num_connected_peers; i++) {
            if (protocol->peer_nodes[i]) {
                refcounter_dereference(&protocol->peer_nodes[i]->refcounter);
                if (refcounter_count(&protocol->peer_nodes[i]->refcounter) == 0) {
                    free(protocol->peer_nodes[i]);
                }
            }
            if (protocol->connected_peers[i]) {
                protocol->msquic->ConnectionClose(protocol->connected_peers[i]);
            }
        }

        if (protocol->gossip_handle) {
            meridian_gossip_handle_destroy(protocol->gossip_handle);
            protocol->gossip_handle = NULL;
        }

        platform_lock_destroy(&protocol->lock);
        free(protocol);
    }
}

int meridian_protocol_start(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_lock(&protocol->lock);

    if (protocol->running) {
        platform_unlock(&protocol->lock);
        return 0;
    }

    // Open msquic library
    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuicOpen2(&protocol->msquic))) {
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Create registration
    QUIC_REGISTRATION_CONFIG RegConfig = {
        .AppName = "meridian",
        .ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY
    };

    if (QUIC_FAILED(Status = protocol->msquic->RegistrationOpen(&RegConfig, &protocol->registration))) {
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Configure ALPN
    QUIC_BUFFER Alpn = { sizeof("meridian") - 1, (uint8_t*)"meridian" };

    // Configure settings with datagram support
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = 30000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;

    if (QUIC_FAILED(Status = protocol->msquic->ConfigurationOpen(
            protocol->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &protocol->configuration))) {
        protocol->msquic->RegistrationClose(protocol->registration);
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        protocol->registration = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Load credentials
    QUIC_CREDENTIAL_CONFIG CredConfig = {0};
    QUIC_CERTIFICATE_FILE CertFile = {0};
    if (protocol->config.tls_key_path != NULL && protocol->config.tls_cert_path != NULL) {
        CertFile.PrivateKeyFile = protocol->config.tls_key_path;
        CertFile.CertificateFile = protocol->config.tls_cert_path;
        CredConfig.CertificateFile = &CertFile;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    } else {
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    }

    if (QUIC_FAILED(Status = protocol->msquic->ConfigurationLoadCredential(
            protocol->configuration,
            &CredConfig))) {
        protocol->msquic->ConfigurationClose(protocol->configuration);
        protocol->msquic->RegistrationClose(protocol->registration);
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Create listener
    if (QUIC_FAILED(Status = protocol->msquic->ListenerOpen(
            protocol->registration,
            ListenerCallback,
            protocol,
            &protocol->listener))) {
        protocol->msquic->ConfigurationClose(protocol->configuration);
        protocol->msquic->RegistrationClose(protocol->registration);
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Set listener callback
    protocol->msquic->SetCallbackHandler(protocol->listener, ListenerCallback, protocol);

    // Start listener on configured port
    QUIC_ADDR Address = {0};
#ifdef QUIC_ADDRESS_FAMILY_UNSPEC
    Address.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET;
#else
    Address.Ipv4.sin_family = AF_INET;
#endif
    Address.Ipv4.sin_port = htons(protocol->config.listen_port);

    if (QUIC_FAILED(Status = protocol->msquic->ListenerStart(
            protocol->listener,
            &Alpn, 1,
            &Address))) {
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->msquic->ConfigurationClose(protocol->configuration);
        protocol->msquic->RegistrationClose(protocol->registration);
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        protocol->listener = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    // Initialize local address for queries
    memset(&protocol->local_addr, 0, sizeof(protocol->local_addr));
    protocol->local_addr.sin_family = AF_INET;
    protocol->local_addr.sin_addr.s_addr = INADDR_ANY;
    protocol->local_addr.sin_port = htons(protocol->config.listen_port);

    // Initialize gossip
    meridian_gossip_config_t gossip_config = {
        .user_ctx = protocol,
        .outbound_cb = gossip_outbound_cb,
        .completed_cb = NULL,
        .init_interval_s = protocol->config.init_gossip_interval_s,
        .num_init_intervals = protocol->config.num_init_gossip_intervals,
        .steady_state_interval_s = protocol->config.steady_state_gossip_interval_s,
        .timeout_ms = protocol->config.gossip_timeout_ms
    };
    protocol->gossip_handle = meridian_gossip_handle_create(&gossip_config);

    // Set msquic context on rendezvous handle for QUIC tunnel support
    // Pass NULL config - tunnels use registration defaults
    meridian_rendv_handle_set_msquic(protocol->rendv_handle,
                                     protocol->msquic,
                                     protocol->registration,
                                     NULL);

    if (protocol->gossip_handle == NULL) {
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->msquic->ConfigurationClose(protocol->configuration);
        protocol->msquic->RegistrationClose(protocol->registration);
        MsQuicClose(protocol->msquic);
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        protocol->listener = NULL;
        platform_unlock(&protocol->lock);
        return -1;
    }

    protocol->state = MERIDIAN_PROTOCOL_STATE_BOOTSTRAPPING;
    protocol->running = true;

    platform_unlock(&protocol->lock);
    return 0;
}

int meridian_protocol_stop(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_lock(&protocol->lock);

    if (!protocol->running) {
        platform_unlock(&protocol->lock);
        return 0;
    }

    protocol->running = false;
    protocol->state = MERIDIAN_PROTOCOL_STATE_SHUTTING_DOWN;

    if (protocol->gossip_handle) {
        meridian_gossip_handle_stop(protocol->gossip_handle);
    }

    // Shutdown all peer connections
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->connected_peers[i]) {
            protocol->msquic->ConnectionShutdown(
                protocol->connected_peers[i],
                QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                0);
        }
    }

    // Stop and close listener
    if (protocol->listener) {
        protocol->msquic->ListenerStop(protocol->listener);
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->listener = NULL;
    }

    platform_unlock(&protocol->lock);
    return 0;
}

// ============================================================================
// PEER MANAGEMENT
// ============================================================================

int meridian_protocol_add_seed_node(meridian_protocol_t* protocol,
                                     uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (protocol->num_seed_nodes >= 16) return -1;

    meridian_node_t* node = meridian_node_create(addr, port, NULL);
    if (node == NULL) return -1;

    protocol->seed_nodes[protocol->num_seed_nodes++] = node;
    return 0;
}

int meridian_protocol_connect(meridian_protocol_t* protocol,
                               uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (protocol->num_connected_peers >= 64) return -1;

    // Check if already connected
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == addr &&
            protocol->peer_nodes[i]->port == port) {
            return 0;
        }
    }

    // Create peer node
    meridian_node_t* node = meridian_node_create(addr, port, NULL);
    if (node == NULL) return -1;

    // Create QUIC connection
    HQUIC Connection;
    QUIC_STATUS Status = protocol->msquic->ConnectionOpen(
        protocol->registration,
        ConnectionCallback,
        protocol,
        &Connection);

    if (QUIC_FAILED(Status)) {
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }

    // Set connection callback
    protocol->msquic->SetCallbackHandler(Connection, ConnectionCallback, protocol);

    // Build remote address
    QUIC_ADDR RemoteAddr = {0};
    RemoteAddr.Ipv4.sin_family = AF_INET;
    RemoteAddr.Ipv4.sin_addr.s_addr = addr;
    RemoteAddr.Ipv4.sin_port = port;

    // Start connection
    Status = protocol->msquic->ConnectionStart(
        Connection,
        protocol->configuration,
        AF_INET,
        NULL,  // Let QUIC resolve
        port);

    if (QUIC_FAILED(Status)) {
        protocol->msquic->ConnectionClose(Connection);
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }

    // Add to connected peers
    platform_lock(&protocol->lock);
    protocol->connected_peers[protocol->num_connected_peers] = Connection;
    protocol->peer_nodes[protocol->num_connected_peers] = node;
    protocol->num_connected_peers++;
    platform_unlock(&protocol->lock);

    return 0;
}

int meridian_protocol_disconnect(meridian_protocol_t* protocol,
                                  uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;

    platform_lock(&protocol->lock);
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == addr &&
            protocol->peer_nodes[i]->port == port) {

            // Shutdown and close connection
            if (protocol->connected_peers[i]) {
                protocol->msquic->ConnectionShutdown(
                    protocol->connected_peers[i],
                    QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                    0);
                protocol->msquic->ConnectionClose(protocol->connected_peers[i]);
            }

            // Free peer node
            refcounter_dereference(&protocol->peer_nodes[i]->refcounter);
            if (refcounter_count(&protocol->peer_nodes[i]->refcounter) == 0) {
                free(protocol->peer_nodes[i]);
            }

            // Remove from array
            for (size_t j = i; j < protocol->num_connected_peers - 1; j++) {
                protocol->connected_peers[j] = protocol->connected_peers[j + 1];
                protocol->peer_nodes[j] = protocol->peer_nodes[j + 1];
            }
            protocol->num_connected_peers--;
            platform_unlock(&protocol->lock);
            return 0;
        }
    }
    platform_unlock(&protocol->lock);
    return -1;
}

// ============================================================================
// NETWORK I/O
// ============================================================================

int meridian_protocol_send_packet(meridian_protocol_t* protocol,
                                   const uint8_t* data, size_t len,
                                   const meridian_node_t* target) {
    if (protocol == NULL || data == NULL || target == NULL) return -1;

    // Find connection for target
    HQUIC Connection = NULL;
    platform_lock(&protocol->lock);
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == target->addr &&
            protocol->peer_nodes[i]->port == target->port) {
            Connection = protocol->connected_peers[i];
            break;
        }
    }
    platform_unlock(&protocol->lock);

    if (Connection == NULL) return -1;

    QUIC_BUFFER Buffer = {
        .Length = (uint32_t)len,
        .Buffer = (uint8_t*)data
    };

    QUIC_STATUS Status = protocol->msquic->DatagramSend(
        Connection,
        &Buffer, 1,
        QUIC_SEND_FLAG_NONE,
        NULL);

    return QUIC_SUCCEEDED(Status) ? 0 : -1;
}

int meridian_protocol_broadcast(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len) {
    if (protocol == NULL || data == NULL) return -1;

    int result = 0;
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i]) {
            if (meridian_protocol_send_packet(protocol, data, len,
                                             protocol->peer_nodes[i]) != 0) {
                result = -1;
            }
        }
    }
    return result;
}

// ============================================================================
// NODE DISCOVERY
// ============================================================================

meridian_node_t* meridian_protocol_find_closest(meridian_protocol_t* protocol,
                                                uint32_t target_addr, uint16_t target_port) {
    if (protocol == NULL) return NULL;

    return meridian_ring_set_find_closest(protocol->ring_set, target_addr, target_port);
}

meridian_node_t** meridian_protocol_get_connected_peers(meridian_protocol_t* protocol,
                                                      size_t* num_peers) {
    if (protocol == NULL || num_peers == NULL) return NULL;

    *num_peers = protocol->num_connected_peers;
    return protocol->peer_nodes;
}

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int meridian_protocol_gossip(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    bool should_gossip = false;
    if (protocol->gossip_handle) {
        meridian_gossip_scheduler_t* sched = protocol->gossip_handle->scheduler;
        if (sched) {
            meridian_gossip_scheduler_tick(sched, &should_gossip);
        }
    }

    if (should_gossip && protocol->num_connected_peers > 0) {
        meridian_gossip_handle_send_gossip(protocol->gossip_handle,
                                           protocol->peer_nodes,
                                           protocol->num_connected_peers);
    }

    return 0;
}

int meridian_protocol_ring_management(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    for (int ring = 0; ring < MERIDIAN_MAX_RINGS; ring++) {
        if (meridian_ring_set_eligible_for_replacement(protocol->ring_set, ring)) {
            // Promote a secondary node to fill the primary ring
            meridian_ring_set_promote_secondary(protocol->ring_set, ring);
        }
    }

    return 0;
}

// ============================================================================
// PACKET HANDLING
// ============================================================================

int meridian_protocol_on_packet(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len,
                                 const meridian_node_t* from) {
    if (protocol == NULL || data == NULL || from == NULL) return -1;

    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, len, &result);
    if (item == NULL) return -1;

    cbor_decref(&item);
    return 0;
}

int meridian_protocol_on_measure_result(meridian_protocol_t* protocol,
                                         const meridian_measure_result_t* result) {
    if (protocol == NULL || result == NULL) return -1;

    if (result->success && result->node) {
        meridian_latency_cache_insert(protocol->latency_cache,
                                      result->node, result->latency_us);
    }

    return 0;
}

// ============================================================================
// CALLBACKS
// ============================================================================

int meridian_protocol_set_callbacks(meridian_protocol_t* protocol,
                                     const meridian_protocol_callbacks_t* callbacks) {
    if (protocol == NULL || callbacks == NULL) return -1;
    protocol->callbacks = *callbacks;
    return 0;
}

// ============================================================================
// LOCAL NODE INFO
// ============================================================================

int meridian_protocol_get_local_node(meridian_protocol_t* protocol,
                                      uint32_t* addr, uint16_t* port) {
    if (protocol == NULL) return -1;

    if (addr) *addr = protocol->local_addr.sin_addr.s_addr;
    if (port) *port = ntohs(protocol->local_addr.sin_port);

    return 0;
}

// ============================================================================
// QUIC CALLBACKS
// ============================================================================

static QUIC_STATUS QUIC_API
ListenerCallback(HQUIC Listener, void* Context,
                QUIC_LISTENER_EVENT* Event) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)Context;

    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
        HQUIC Connection = Event->NEW_CONNECTION.Connection;

        // Set connection callback
        protocol->msquic->SetCallbackHandler(Connection, ConnectionCallback, protocol);

        // Accept connection by setting configuration
        QUIC_STATUS Status = protocol->msquic->ConnectionSetConfiguration(
            Connection, protocol->configuration);

        if (QUIC_FAILED(Status)) {
            return Status;
        }

        // Add to connected peers
        platform_lock(&protocol->lock);
        if (protocol->num_connected_peers < 64) {
            // Get peer address - need to query connection params
            uint32_t addr = 0;
            uint16_t port = 0;

            // For now, store NULL node - address will be updated when we get peer address
            protocol->connected_peers[protocol->num_connected_peers] = Connection;
            protocol->peer_nodes[protocol->num_connected_peers] = NULL;
            protocol->num_connected_peers++;
        }
        platform_unlock(&protocol->lock);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }
}

static QUIC_STATUS QUIC_API
ConnectionCallback(HQUIC Connection, void* Context,
                   QUIC_CONNECTION_EVENT* Event) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        // Handshake complete
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        // Connection shutting down
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // Ready for close
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        // Unreliable datagram received
        const QUIC_BUFFER* Buffer = Event->DATAGRAM_RECEIVED.Buffer;

        // Find peer node for this connection and invoke callback
        // For now, pass NULL as from - caller can use ConnectionGetParam
        if (protocol->callbacks.on_packet) {
            protocol->callbacks.on_packet(
                protocol->callbacks.user_ctx,
                protocol,
                Buffer->Buffer,
                Buffer->Length);
        }
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
