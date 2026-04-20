//
// Created by victor on 4/19/26.
//

#include "meridian_conn.h"
#include "meridian_relay.h"
#include "meridian_relay_server.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONNECTION LIFECYCLE
// ============================================================================

meridian_conn_t* meridian_conn_create(meridian_node_t* peer, struct meridian_relay_t* relay,
                                       meridian_nat_type_t local_nat_type) {
    if (peer == NULL) return NULL;

    meridian_conn_t* conn = (meridian_conn_t*)get_clear_memory(sizeof(meridian_conn_t));
    if (conn == NULL) return NULL;

    conn->peer = peer;
    conn->direct_connection = NULL;
    conn->relay = relay;
    conn->relay_endpoint_id = MERIDIAN_RELAY_ENDPOINT_ID_NONE;
    conn->state = MERIDIAN_CONN_STATE_RELAY;
    conn->local_nat_type = local_nat_type;
    conn->peer_nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;
    conn->direct_attempts = 0;
    conn->last_direct_attempt_ms = 0;

    // Initialize direct path from peer address
    conn->direct_path.addr = peer->addr;
    conn->direct_path.port = peer->port;
    conn->direct_path.reflexive_addr = 0;
    conn->direct_path.reflexive_port = 0;
    conn->direct_path.rtt_ms = 0;
    conn->direct_path.last_activity_ms = 0;
    conn->direct_path.active = false;

    // Relay path starts inactive until relay connection is established
    conn->relay_path.active = false;

    // Determine initial state based on local NAT type
    if (local_nat_type == MERIDIAN_NAT_TYPE_SYMMETRIC) {
        conn->state = MERIDIAN_CONN_STATE_RELAY_ONLY;
    } else if (peer->addr != 0 && peer->port != 0) {
        // We know the peer's address — try direct first
        conn->state = MERIDIAN_CONN_STATE_TRYING_DIRECT;
    } else {
        conn->state = MERIDIAN_CONN_STATE_RELAY;
    }

    platform_lock_init(&conn->lock);
    refcounter_init(&conn->refcounter);
    return conn;
}

void meridian_conn_destroy(meridian_conn_t* conn) {
    if (conn == NULL) return;

    refcounter_dereference(&conn->refcounter);
    if (refcounter_count(&conn->refcounter) == 0) {
        if (conn->direct_connection != NULL) {
            // Note: direct_connection lifecycle is managed by msquic
            conn->direct_connection = NULL;
        }
        // relay is borrowed — don't destroy it here
        platform_lock_destroy(&conn->lock);
        free(conn);
    }
}

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

int meridian_conn_connect(meridian_conn_t* conn) {
    if (conn == NULL) return -1;

    platform_lock(&conn->lock);

    switch (conn->state) {
        case MERIDIAN_CONN_STATE_RELAY_ONLY:
            // Symmetric NAT — only use relay
            if (conn->relay != NULL) {
                conn->relay_path.active = true;
            }
            break;

        case MERIDIAN_CONN_STATE_TRYING_DIRECT:
            // Try direct connection first, relay as backup
            if (conn->direct_path.addr != 0 && conn->direct_path.port != 0) {
                // Direct attempt would go here — open QUIC connection to peer
                // For now, mark as attempting
                conn->direct_attempts++;
                conn->last_direct_attempt_ms = 0; // Would use timestamp
                // If relay is available, start relay connection as backup
                if (conn->relay != NULL) {
                    conn->relay_path.active = true;
                }
            } else {
                // No address — fall back to relay
                conn->state = MERIDIAN_CONN_STATE_RELAY;
                if (conn->relay != NULL) {
                    conn->relay_path.active = true;
                }
            }
            break;

        case MERIDIAN_CONN_STATE_RELAY:
            // Use relay connection
            if (conn->relay != NULL) {
                conn->relay_path.active = true;
            }
            break;

        case MERIDIAN_CONN_STATE_DIRECT:
            // Already connected directly — nothing to do
            break;
    }

    platform_unlock(&conn->lock);
    return 0;
}

void meridian_conn_disconnect(meridian_conn_t* conn) {
    if (conn == NULL) return;

    platform_lock(&conn->lock);

    conn->direct_path.active = false;
    conn->relay_path.active = false;
    conn->direct_connection = NULL;
    conn->state = MERIDIAN_CONN_STATE_RELAY;

    platform_unlock(&conn->lock);
}

int meridian_conn_send(meridian_conn_t* conn, const uint8_t* data, uint32_t len) {
    if (conn == NULL || data == NULL || len == 0) return -1;

    platform_lock(&conn->lock);

    int result = -1;

    // Prefer direct path if active
    if (conn->direct_path.active && conn->direct_connection != NULL) {
        // Would send via msquic DatagramSend on direct connection
        // For now, indicate success
        result = (int)len;
    } else if (conn->relay_path.active && conn->relay != NULL) {
        // Fall back to relay
        result = meridian_relay_send_datagram(conn->relay, data, (size_t)len,
                                               conn->relay_endpoint_id);
    }

    platform_unlock(&conn->lock);
    return result;
}

int meridian_conn_upgrade_to_direct(meridian_conn_t* conn) {
    if (conn == NULL) return -1;

    platform_lock(&conn->lock);

    // Only upgrade from RELAY state
    if (conn->state != MERIDIAN_CONN_STATE_RELAY) {
        platform_unlock(&conn->lock);
        return (conn->state == MERIDIAN_CONN_STATE_DIRECT) ? 0 : -1;
    }

    // Don't upgrade if peer has symmetric NAT
    if (conn->peer_nat_type == MERIDIAN_NAT_TYPE_SYMMETRIC ||
        conn->local_nat_type == MERIDIAN_NAT_TYPE_SYMMETRIC) {
        platform_unlock(&conn->lock);
        return -1;
    }

    // Need reflexive address to attempt direct
    if (conn->direct_path.reflexive_addr == 0) {
        platform_unlock(&conn->lock);
        return -1;
    }

    conn->state = MERIDIAN_CONN_STATE_TRYING_DIRECT;
    conn->direct_attempts++;
    conn->last_direct_attempt_ms = 0;

    platform_unlock(&conn->lock);
    return 0;
}

// ============================================================================
// PEER INFORMATION UPDATES
// ============================================================================

void meridian_conn_set_peer_nat_type(meridian_conn_t* conn, meridian_nat_type_t type) {
    if (conn == NULL) return;

    platform_lock(&conn->lock);
    conn->peer_nat_type = type;

    // If peer has symmetric NAT, downgrade to relay-only
    if (type == MERIDIAN_NAT_TYPE_SYMMETRIC) {
        conn->direct_path.active = false;
        conn->state = MERIDIAN_CONN_STATE_RELAY_ONLY;
    }

    platform_unlock(&conn->lock);
}

void meridian_conn_set_peer_reflexive(meridian_conn_t* conn, uint32_t addr, uint16_t port) {
    if (conn == NULL) return;

    platform_lock(&conn->lock);
    conn->direct_path.reflexive_addr = addr;
    conn->direct_path.reflexive_port = port;
    platform_unlock(&conn->lock);
}

// ============================================================================
// STATE QUERIES
// ============================================================================

meridian_conn_state_t meridian_conn_get_state(const meridian_conn_t* conn) {
    if (conn == NULL) return MERIDIAN_CONN_STATE_RELAY;
    return conn->state;
}

bool meridian_conn_is_direct(const meridian_conn_t* conn) {
    if (conn == NULL) return false;
    return conn->state == MERIDIAN_CONN_STATE_DIRECT;
}

bool meridian_conn_is_relay(const meridian_conn_t* conn) {
    if (conn == NULL) return false;
    return conn->state == MERIDIAN_CONN_STATE_RELAY ||
           conn->state == MERIDIAN_CONN_STATE_RELAY_ONLY;
}