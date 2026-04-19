//
// Created by victor on 4/19/26.
//

#include "../../Util/threadding.h"
#include "meridian_rendv.h"
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

    return handle;
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
                if (handle->connections.data[i]->socket_fd >= 0) {
                    close(handle->connections.data[i]->socket_fd);
                }
                free(handle->connections.data[i]);
            }
        }
        vec_deinit(&handle->connections);

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
 * Creates a UDP tunnel to a peer rendezvous point.
 * Used for hole-punching and direct communication.
 *
 * @param handle  Handle to create tunnel from
 * @param peer    Peer to connect to
 * @param out_fd  Output: file descriptor for the tunnel
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_create_tunnel(meridian_rendv_handle_t* handle,
                                         meridian_rendv_t* peer,
                                         int* out_fd) {
    if (handle == NULL || peer == NULL || out_fd == NULL) return -1;

    *out_fd = -1;

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    // Bind to any address/port (let kernel assign ephemeral port)
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0;

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        close(sock);
        return -1;
    }

    // Connect to peer
    struct sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = peer->addr;
    peer_addr.sin_port = peer->port;

    if (connect(sock, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        close(sock);
        return -1;
    }

    *out_fd = sock;
    return 0;
}

/**
 * Closes a tunnel file descriptor.
 *
 * @param handle  Handle (unused but part of API)
 * @param fd      File descriptor to close
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_close_tunnel(meridian_rendv_handle_t* handle, int fd) {
    (void)handle;
    if (handle == NULL || fd < 0) return -1;
    close(fd);
    return 0;
}

// ============================================================================
// NAT DETECTION
// ============================================================================

/**
 * Detects NAT type for the local rendezvous.
 * Invokes callback with the detected NAT information.
 *
 * @param handle    Handle to detect NAT for
 * @param callback  Function to receive NAT info
 * @param ctx      User context for callback
 * @return         0 on success, -1 on failure
 */
int meridian_rendv_handle_detect_nat(meridian_rendv_handle_t* handle,
                                      meridian_nat_callback_t callback, void* ctx) {
    if (handle == NULL || callback == NULL) return -1;

    callback(ctx,
             handle->local_rendv ? handle->local_rendv->addr : 0,
             handle->local_rendv ? handle->local_rendv->port : 0,
             handle->local_rendv ? handle->local_rendv->nat_type : MERIDIAN_NAT_TYPE_UNKNOWN);

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
 * @param handle     Handle to receive on
 * @param src_addr   Output: source address of punching peer
 * @param src_port   Output: source port of punching peer
 * @return           0 on success, -1 on failure
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