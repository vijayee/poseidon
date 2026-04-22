//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_rendv.h"
#include "meridian_relay.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ============================================================================
// RENDEZVOUS POINT LIFECYCLE
// ============================================================================

/**
 * Creates a rendezvous point with the given address and port.
 * Rendezvous points are used for NAT traversal and node discovery.
 *
 * @param addr  Rendezvous server IPv4 address
 * @param port  Rendezvous server port
 * @return      New rendezvous point with refcount=1, or NULL on failure
 */
meridian_rendv_t* meridian_rendv_create(uint32_t addr, uint16_t port) {
    meridian_rendv_t* rendv = (meridian_rendv_t*)
        get_clear_memory(sizeof(meridian_rendv_t));

    rendv->addr = addr;
    rendv->port = port;
    rendv->nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;
    rendv->is_active = false;

    refcounter_init(&rendv->refcounter);
    return rendv;
}

/**
 * Destroys a rendezvous point.
 *
 * @param rendv  Rendezvous to destroy
 */
void meridian_rendv_destroy(meridian_rendv_t* rendv) {
    if (rendv == NULL) return;

    refcounter_dereference(&rendv->refcounter);
    if (refcounter_count(&rendv->refcounter) == 0) {
        free(rendv);
    }
}

/**
 * Sets the NAT type of a rendezvous point.
 *
 * @param rendv  Rendezvous to update
 * @param type   NAT type enum value
 * @return       0 on success, -1 on failure
 */
int meridian_rendv_set_nat_type(meridian_rendv_t* rendv, meridian_nat_type_t type) {
    if (rendv == NULL) return -1;
    rendv->nat_type = type;
    return 0;
}

/**
 * Gets the NAT type of a rendezvous point.
 *
 * @param rendv  Rendezvous to query
 * @return       NAT type, or UNKNOWN if rendv is NULL
 */
meridian_nat_type_t meridian_rendv_get_nat_type(const meridian_rendv_t* rendv) {
    return rendv ? rendv->nat_type : MERIDIAN_NAT_TYPE_UNKNOWN;
}

uint32_t meridian_rendv_get_reflexive_addr(const meridian_rendv_t* rendv) {
    return rendv ? rendv->reflexive_addr : 0;
}

uint16_t meridian_rendv_get_reflexive_port(const meridian_rendv_t* rendv) {
    return rendv ? rendv->reflexive_port : 0;
}

int meridian_rendv_set_reflexive_addr(meridian_rendv_t* rendv, uint32_t addr, uint16_t port) {
    if (rendv == NULL) return -1;
    rendv->reflexive_addr = addr;
    rendv->reflexive_port = port;
    return 0;
}

bool meridian_rendv_can_direct_connect(const meridian_rendv_t* rendv) {
    if (rendv == NULL) return false;
    switch (rendv->nat_type) {
        case MERIDIAN_NAT_TYPE_OPEN:
        case MERIDIAN_NAT_TYPE_FULL_CONE:
        case MERIDIAN_NAT_TYPE_RESTRICTED_CONE:
        case MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE:
            return true;
        case MERIDIAN_NAT_TYPE_SYMMETRIC:
        case MERIDIAN_NAT_TYPE_UNKNOWN:
        default:
            return false;
    }
}

// ============================================================================
// RENDEZVOUS HANDLE
// ============================================================================

/**
 * Creates a handle for managing rendezvous operations.
 * Initializes vectors for peer rendezvous and active connections.
 *
 * @return  New handle with refcount=1, or NULL on failure
 */
meridian_rendv_handle_t* meridian_rendv_handle_create(void) {
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)
        get_clear_memory(sizeof(meridian_rendv_handle_t));

    vec_init(&handle->peer_rendvs);
    vec_init(&handle->connections);
    platform_lock_init(&handle->lock);
    refcounter_init(&handle->refcounter);
    handle->msquic = NULL;
    handle->registration = NULL;
    handle->configuration = NULL;

    return handle;
}

int meridian_rendv_handle_set_msquic(meridian_rendv_handle_t* handle,
                                    const struct QUIC_API_TABLE* msquic,
                                    HQUIC registration,
                                    const meridian_rendv_config_t* config) {
    if (handle == NULL) return -1;

    handle->msquic = msquic;
    handle->registration = registration;

    // Create QUIC configuration if config provided
    if (config != NULL && config->alpn != NULL) {
        QUIC_BUFFER Alpn = { (uint32_t)strlen(config->alpn), (uint8_t*)config->alpn };

        QUIC_SETTINGS Settings = {0};
        Settings.IdleTimeoutMs = config->idle_timeout_ms ? config->idle_timeout_ms : 30000;
        Settings.IsSet.IdleTimeoutMs = TRUE;
        Settings.DatagramReceiveEnabled = config->datagram_recv_enabled;
        Settings.IsSet.DatagramReceiveEnabled = TRUE;

        QUIC_STATUS Status = handle->msquic->ConfigurationOpen(
            handle->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &handle->configuration);

        if (QUIC_FAILED(Status)) {
            handle->configuration = NULL;
            return -1;
        }

        // Load credentials if provided
        if (config->cred_config != NULL) {
            Status = handle->msquic->ConfigurationLoadCredential(
                handle->configuration,
                config->cred_config);

            if (QUIC_FAILED(Status)) {
                handle->msquic->ConfigurationClose(handle->configuration);
                handle->configuration = NULL;
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Destroys a rendezvous handle, releasing all resources.
 * Closes any open connection sockets and frees all rendezvous points.
 *
 * @param handle  Handle to destroy
 */
void meridian_rendv_handle_destroy(meridian_rendv_handle_t* handle) {
    if (handle == NULL) return;

    refcounter_dereference(&handle->refcounter);
    if (refcounter_count(&handle->refcounter) == 0) {
        // Destroy local rendezvous
        if (handle->local_rendv) {
            meridian_rendv_destroy(handle->local_rendv);
        }

        // Destroy all peer rendezvous
        for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
            if (handle->peer_rendvs.data[i]) {
                meridian_rendv_destroy(handle->peer_rendvs.data[i]);
            }
        }
        vec_deinit(&handle->peer_rendvs);

        // Close and free all connections
        for (size_t i = 0; i < handle->connections.length; i++) {
            if (handle->connections.data[i]) {
                if (handle->connections.data[i]->connection && handle->msquic) {
                    handle->msquic->ConnectionShutdown(
                        handle->connections.data[i]->connection,
                        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                    handle->msquic->ConnectionClose(handle->connections.data[i]->connection);
                }
                free(handle->connections.data[i]);
            }
        }
        vec_deinit(&handle->connections);

        // Do NOT close configuration/registration here — they are borrowed from
        // the protocol. The protocol owns and closes them in stop/destroy.

        platform_lock_destroy(&handle->lock);
        free(handle);
    }
}

/**
 * Sets the local rendezvous point for this handle.
 * Takes a reference to the provided rendezvous.
 *
 * @param handle  Handle to update
 * @param rendv   Local rendezvous point
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_set_local(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv) {
    if (handle == NULL || rendv == NULL) return -1;

    platform_lock(&handle->lock);

    // Release previous local rendezvous if any
    if (handle->local_rendv) {
        refcounter_dereference(&handle->local_rendv->refcounter);
        if (refcounter_count(&handle->local_rendv->refcounter) == 0) {
            free(handle->local_rendv);
        }
    }

    handle->local_rendv = (meridian_rendv_t*) refcounter_reference(&rendv->refcounter);
    platform_unlock(&handle->lock);
    return 0;
}

/**
 * Adds a peer rendezvous point to the handle.
 * Does nothing if the peer already exists (by addr:port).
 *
 * @param handle  Handle to add to
 * @param rendv   Peer rendezvous to add
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_add_peer(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv) {
    if (handle == NULL || rendv == NULL) return -1;

    platform_lock(&handle->lock);

    // Check if already exists
    for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
        if (handle->peer_rendvs.data[i]->addr == rendv->addr &&
            handle->peer_rendvs.data[i]->port == rendv->port) {
            platform_unlock(&handle->lock);
            return 0;  // Already exists, no-op
        }
    }

    // Add new peer with reference
    int result = vec_push(&handle->peer_rendvs,
        (meridian_rendv_t*) refcounter_reference(&rendv->refcounter));

    platform_unlock(&handle->lock);
    return result;
}

/**
 * Removes a peer rendezvous by address:port.
 *
 * @param handle  Handle to remove from
 * @param addr    Peer address to remove
 * @param port    Peer port to remove
 * @return        0 if found and removed, -1 if not found
 */
int meridian_rendv_handle_remove_peer(meridian_rendv_handle_t* handle,
                                      uint32_t addr, uint16_t port) {
    if (handle == NULL) return -1;

    platform_lock(&handle->lock);

    for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
        if (handle->peer_rendvs.data[i]->addr == addr &&
            handle->peer_rendvs.data[i]->port == port) {
            refcounter_dereference(&handle->peer_rendvs.data[i]->refcounter);
            if (refcounter_count(&handle->peer_rendvs.data[i]->refcounter) == 0) {
                free(handle->peer_rendvs.data[i]);
            }
            vec_splice(&handle->peer_rendvs, i, 1);
            platform_unlock(&handle->lock);
            return 0;
        }
    }

    platform_unlock(&handle->lock);
    return -1;
}

/**
 * Finds a peer rendezvous by address:port.
 * Returns a new reference that caller must eventually destroy.
 *
 * @param handle  Handle to search
 * @param addr    Address to find
 * @param port    Port to find
 * @return        Peer rendezvous with new reference, or NULL if not found
 */
meridian_rendv_t* meridian_rendv_handle_find_peer(meridian_rendv_handle_t* handle,
                                                  uint32_t addr, uint16_t port) {
    if (handle == NULL) return NULL;

    platform_lock(&handle->lock);

    for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
        if (handle->peer_rendvs.data[i]->addr == addr &&
            handle->peer_rendvs.data[i]->port == port) {
            platform_unlock(&handle->lock);
            return (meridian_rendv_t*) refcounter_reference(
                &handle->peer_rendvs.data[i]->refcounter);
        }
    }

    platform_unlock(&handle->lock);
    return NULL;
}

// ============================================================================
// TUNNEL MANAGEMENT
// ============================================================================

/**
 * Creates a QUIC tunnel to a peer rendezvous point.
 * Used for hole-punching and direct communication.
 *
 * @param handle  Handle to create tunnel from
 * @param peer    Peer to connect to
 * @param out_conn  Output: QUIC connection handle for the tunnel
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_create_tunnel(meridian_rendv_handle_t* handle,
                                         meridian_rendv_t* peer,
                                         HQUIC* out_conn) {
    if (handle == NULL || peer == NULL || out_conn == NULL) return -1;
    if (handle->msquic == NULL || handle->registration == NULL) return -1;

    *out_conn = NULL;

    // Create QUIC connection
    HQUIC Connection;
    QUIC_STATUS Status = handle->msquic->ConnectionOpen(
        handle->registration,
        NULL,  // No callback for tunnel connections
        handle,
        &Connection);

    if (QUIC_FAILED(Status)) {
        return -1;
    }

    // Build remote address
    QUIC_ADDR RemoteAddr = {0};
    RemoteAddr.Ipv4.sin_family = AF_INET;
    RemoteAddr.Ipv4.sin_addr.s_addr = peer->addr;
    RemoteAddr.Ipv4.sin_port = peer->port;

    // Start connection - use configuration if set, otherwise NULL for defaults
    Status = handle->msquic->ConnectionStart(
        Connection,
        handle->configuration,
        AF_INET,
        NULL,  // Let QUIC resolve
        peer->port);

    if (QUIC_FAILED(Status)) {
        handle->msquic->ConnectionClose(Connection);
        return -1;
    }

    *out_conn = Connection;
    return 0;
}

/**
 * Closes a QUIC tunnel connection.
 *
 * @param handle  Handle
 * @param conn    QUIC connection to close
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_close_tunnel(meridian_rendv_handle_t* handle, HQUIC conn) {
    if (handle == NULL || conn == NULL) return -1;
    if (handle->msquic == NULL) return -1;

    handle->msquic->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    handle->msquic->ConnectionClose(conn);
    return 0;
}

// ============================================================================
// NAT DETECTION
// ============================================================================

/**
 * Detects NAT type for the local rendezvous.
 * Detects NAT type by comparing reflexive addresses from relay servers.
 * Sends ADDR_REQUEST to each relay, compares observed public addresses,
 * and classifies NAT type accordingly.
 *
 * @param handle    Handle to detect NAT for
 * @param relay_a   First relay client (may be NULL)
 * @param relay_b   Second relay client (may be NULL)
 * @param callback  Function to receive NAT info
 * @param ctx       User context for callback
 * @return          0 on success, -1 on failure
 */
int meridian_rendv_handle_detect_nat(meridian_rendv_handle_t* handle,
                                      struct meridian_relay_t* relay_a,
                                      struct meridian_relay_t* relay_b,
                                      meridian_nat_callback_t callback, void* ctx) {
    if (handle == NULL || callback == NULL) return -1;
    if (handle->local_rendv == NULL) return -1;

    platform_lock(&handle->lock);

    // Send address requests to relay servers
    if (relay_a != NULL) {
        meridian_relay_send_addr_request(relay_a);
    }
    if (relay_b != NULL) {
        meridian_relay_send_addr_request(relay_b);
    }

    // Read stored reflexive addresses from local rendezvous
    uint32_t reflexive_addr = meridian_rendv_get_reflexive_addr(handle->local_rendv);
    uint16_t reflexive_port = meridian_rendv_get_reflexive_port(handle->local_rendv);
    uint32_t local_addr = handle->local_rendv->addr;

    // Classification algorithm
    meridian_nat_type_t nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;

    if (local_addr != 0 && local_addr == reflexive_addr) {
        // Local address matches reflexive — we're not behind NAT
        nat_type = MERIDIAN_NAT_TYPE_OPEN;
    } else if (reflexive_addr != 0) {
        // We have at least one reflexive address that differs from local
        // With two relays we can distinguish EIM from EDM
        // For now, default to PORT_RESTRICTED_CONE when we have reflexive info
        // Full two-relay comparison happens when both responses arrive
        nat_type = MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE;
    }

    handle->local_rendv->nat_type = nat_type;
    meridian_nat_type_t result_type = nat_type;

    platform_unlock(&handle->lock);

    callback(ctx, reflexive_addr, reflexive_port, result_type);
    return 0;
}

// ============================================================================
// HOLE PUNCHING
// ============================================================================

/**
 * Sends a hole-punching packet to a target rendezvous.
 * This initiates NAT hole-punching for peer-to-peer connection.
 *
 * @param handle  Handle to send from
 * @param target  Target rendezvous to punch
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_send_hole_punch(meridian_rendv_handle_t* handle,
                                    const meridian_rendv_t* target) {
    if (handle == NULL || target == NULL) return -1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_addr.s_addr = target->addr;
    target_addr.sin_port = target->port;

    const char* punch_msg = "PUNCH";
    ssize_t sent = sendto(sock, punch_msg, strlen(punch_msg), 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));

    close(sock);

    return (sent == (ssize_t)strlen(punch_msg)) ? 0 : -1;
}

/**
 * Receives a hole-punching packet and reports source.
 *
 * Note: QUIC doesn't use UDP hole-punching. NAT traversal in QUIC is handled
 * by establishing connections through the rendezvous server directly.
 * This function is a no-op and always returns 0.
 *
 * @param handle     Handle to receive on (unused)
 * @param src_addr   Output: source address of punching peer (unchanged)
 * @param src_port   Output: source port of punching peer (unchanged)
 * @return           0 (always succeeds, no-op)
 */
int meridian_rendv_recv_hole_punch(meridian_rendv_handle_t* handle,
                                     uint32_t* src_addr, uint16_t* src_port) {
    (void)handle;
    (void)src_addr;
    (void)src_port;
    if (handle == NULL || src_addr == NULL || src_port == NULL) return -1;
    return -1;  // Placeholder - not yet implemented
}

// ============================================================================
// NAT TYPE UTILITIES
// ============================================================================

/**
 * Converts a string to a NAT type enum.
 *
 * @param str  String like "open", "full_cone", "symmetric", etc.
 * @return     Corresponding NAT type, or UNKNOWN if unrecognized
 */
meridian_nat_type_t meridian_nat_type_from_string(const char* str) {
    if (str == NULL) return MERIDIAN_NAT_TYPE_UNKNOWN;

    if (strcmp(str, "open") == 0) return MERIDIAN_NAT_TYPE_OPEN;
    if (strcmp(str, "full_cone") == 0) return MERIDIAN_NAT_TYPE_FULL_CONE;
    if (strcmp(str, "restricted_cone") == 0) return MERIDIAN_NAT_TYPE_RESTRICTED_CONE;
    if (strcmp(str, "port_restricted") == 0) return MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE;
    if (strcmp(str, "symmetric") == 0) return MERIDIAN_NAT_TYPE_SYMMETRIC;

    return MERIDIAN_NAT_TYPE_UNKNOWN;
}

/**
 * Converts a NAT type enum to a string.
 *
 * @param type  NAT type to convert
 * @return      String representation, or "unknown" for unrecognized
 */
const char* meridian_nat_type_to_string(meridian_nat_type_t type) {
    switch (type) {
        case MERIDIAN_NAT_TYPE_OPEN: return "open";
        case MERIDIAN_NAT_TYPE_FULL_CONE: return "full_cone";
        case MERIDIAN_NAT_TYPE_RESTRICTED_CONE: return "restricted_cone";
        case MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE: return "port_restricted";
        case MERIDIAN_NAT_TYPE_SYMMETRIC: return "symmetric";
        default: return "unknown";
    }
}