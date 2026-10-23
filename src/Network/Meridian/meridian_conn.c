//
// Created by victor on 4/19/26.
//

#include "meridian_conn.h"
#include "meridian_conn_messages.h"
#include "meridian_relay.h"
#include "meridian_relay_server.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../Actor/actor.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONNECTION LIFECYCLE
// ============================================================================

/** Forward declaration — defined below. */
static void meridian_conn_dispatch(void* state, message_t* msg);
static void meridian_conn_handle_disconnect(meridian_conn_t* conn);
static void meridian_conn_handle_send(meridian_conn_t* conn, const uint8_t* data,
                                      uint32_t len, int* result);
static void meridian_conn_handle_set_peer_nat_type(meridian_conn_t* conn,
                                                   meridian_nat_type_t type);
static void meridian_conn_handle_set_peer_reflexive(meridian_conn_t* conn,
                                                    uint32_t addr, uint16_t port);
static void meridian_conn_handle_upgrade_to_direct(meridian_conn_t* conn,
                                                   int* result);
static void meridian_conn_handle_connect(meridian_conn_t* conn, int* result);
static void meridian_conn_handle_test_force_state(meridian_conn_t* conn,
                                                  meridian_conn_state_t force_state);

meridian_conn_t* meridian_conn_create(meridian_node_t* peer, struct meridian_relay_t* relay,
                                       meridian_nat_type_t local_nat_type,
                                       const struct QUIC_API_TABLE* msquic,
                                       struct scheduler_pool_t* pool) {
    if (peer == NULL) return NULL;

    meridian_conn_t* conn = (meridian_conn_t*)get_clear_memory(sizeof(meridian_conn_t));
    if (conn == NULL) return NULL;

    conn->peer = peer;
    conn->msquic = msquic;
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

    // Wire the actor mailbox. NULL pool = caller-thread dispatch (registry skipped).
    conn->pool = pool;
    actor_init(&conn->actor, conn, meridian_conn_dispatch, pool);

    refcounter_init(&conn->refcounter);
    return conn;
}

/**
 * @brief Actor dispatch entry point.
 *
 * Called serially on the actor's owning thread (worker pool thread when a
 * scheduler pool is wired in, caller thread when pool == NULL). Receives one
 * message at a time off conn->actor's mailbox.
 *
 * 2.4 routes the first mutator (MERIDIAN_CONN_DISCONNECT) through this switch.
 * 2.5 adds MERIDIAN_CONN_SEND (with an out-param result slot). 2.6 adds the
 * two peer-info mutators (MERIDIAN_CONN_SET_PEER_NAT_TYPE and
 * MERIDIAN_CONN_SET_PEER_REFLEXIVE). 2.7 adds MERIDIAN_CONN_UPGRADE_TO_DIRECT
 * (with an out-param result slot). 2.8 adds MERIDIAN_CONN_CONNECT (also with
 * an out-param result slot). Unhandled message types fall through to a
 * no-op; payload_destroy is NULL for all 11 MERIDIAN_CONN_* types so no
 * payload cleanup is required.
 */
static void meridian_conn_dispatch(void* state, message_t* msg) {
    meridian_conn_t* conn = (meridian_conn_t*)state;
    switch (msg->type) {
        case MERIDIAN_CONN_DISCONNECT:
            meridian_conn_handle_disconnect(conn);
            break;
        case MERIDIAN_CONN_SEND: {
            assert(msg->payload != NULL);
            const meridian_conn_send_msg_t* p = (const meridian_conn_send_msg_t*)msg->payload;
            meridian_conn_handle_send(conn, p->data, p->len, p->result);
            break;
        }
        case MERIDIAN_CONN_SET_PEER_NAT_TYPE: {
            assert(msg->payload != NULL);
            const meridian_conn_set_peer_nat_type_msg_t* p =
                (const meridian_conn_set_peer_nat_type_msg_t*)msg->payload;
            meridian_conn_handle_set_peer_nat_type(conn, p->type);
            break;
        }
        case MERIDIAN_CONN_SET_PEER_REFLEXIVE: {
            assert(msg->payload != NULL);
            const meridian_conn_set_peer_reflexive_msg_t* p =
                (const meridian_conn_set_peer_reflexive_msg_t*)msg->payload;
            meridian_conn_handle_set_peer_reflexive(conn, p->addr, p->port);
            break;
        }
        case MERIDIAN_CONN_UPGRADE_TO_DIRECT: {
            assert(msg->payload != NULL);
            const meridian_conn_upgrade_to_direct_msg_t* p =
                (const meridian_conn_upgrade_to_direct_msg_t*)msg->payload;
            meridian_conn_handle_upgrade_to_direct(conn, p->result);
            break;
        }
        case MERIDIAN_CONN_CONNECT: {
            assert(msg->payload != NULL);
            const meridian_conn_connect_msg_t* p =
                (const meridian_conn_connect_msg_t*)msg->payload;
            meridian_conn_handle_connect(conn, p->result);
            break;
        }
        case MERIDIAN_CONN_TEST_FORCE_STATE: {
            assert(msg->payload != NULL);
            const meridian_conn_test_force_state_msg_t* p =
                (const meridian_conn_test_force_state_msg_t*)msg->payload;
            meridian_conn_handle_test_force_state(conn, p->force_state);
            break;
        }
        default:
            /* Unhandled message type: no-op. The message_node_t wrapper is
             * freed by the drain after this function returns. */
            break;
    }
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
        actor_destroy(&conn->actor);
        free(conn);
    }
}

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

int meridian_conn_connect(meridian_conn_t* conn) {
    if (conn == NULL) return -1;

    int result = 0;
    meridian_conn_connect_msg_t payload = { .result = &result };
    message_t msg = { .type = MERIDIAN_CONN_CONNECT,
                      .payload = &payload,
                      .payload_destroy = NULL };
    if (!actor_send(&conn->actor, &msg)) {
        return -1;
    }
    actor_run_inline(&conn->actor, 32);
    return result;
}

/**
 * @brief Actor-thread handler for MERIDIAN_CONN_DISCONNECT.
 *
 * Runs serially on the actor's owning thread. Actor dispatch already
 * guarantees exclusive access to conn's mutable fields. Shutdown/close,
 * path flag reset, and state transition to RELAY are performed in the
 * same order as the pre-2.4 implementation.
 */
static void meridian_conn_handle_disconnect(meridian_conn_t* conn) {
    // Shut down and close the direct QUIC connection if active
    if (conn->direct_connection != NULL && conn->msquic != NULL) {
        conn->msquic->ConnectionShutdown(conn->direct_connection,
                                          QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        conn->msquic->ConnectionClose(conn->direct_connection);
        conn->direct_connection = NULL;
    }

    conn->direct_path.active = false;
    conn->relay_path.active = false;
    conn->state = MERIDIAN_CONN_STATE_RELAY;
}

void meridian_conn_disconnect(meridian_conn_t* conn) {
    if (conn == NULL) return;

    /* Post a disconnect message to the actor and drain synchronously. The
     * drain claims the actor for this thread: actor_send injects the
     * actor into the scheduler pool when the mailbox was empty, so a
     * pool worker could otherwise pick the very same actor up and run it
     * while this thread runs it. Two threads popping one mailbox corrupt
     * it. With pool == NULL the claim is uncontended and the drain
     * happens on the caller's thread. */
    message_t msg = {
        .type = MERIDIAN_CONN_DISCONNECT,
        .payload = NULL,
        .payload_destroy = NULL,
    };
    if (!actor_send(&conn->actor, &msg)) {
        /* DESTROY flag already set or queue rejected the push — nothing to
         * do; the connection is on its way out. */
        return;
    }
    actor_run_inline(&conn->actor, 32);
}

/**
 * @brief Actor-thread handler for MERIDIAN_CONN_SEND.
 *
 * Runs serially on the actor's owning thread. Actor dispatch already
 * guarantees exclusive access to conn's mutable fields. Writes the send
 * outcome into the caller-owned *result slot: (int)len on success, leaves
 * the initial -1 in place on no-path or send failure.
 * The caller's data buffer must outlive this function — actor_send copies
 * the message_t by value into its own message_node_t, so by the time we
 * run, the wrapper's stack frame is irrelevant to the data pointer.
 */
static void meridian_conn_handle_send(meridian_conn_t* conn, const uint8_t* data,
                                      uint32_t len, int* result) {
    /* result is initialized to -1 by the wrapper before actor_send. We
     * only overwrite on a successful send; on the no-path / failure
     * branches we leave -1 as the final value. */
    if (conn->direct_path.active && conn->direct_connection != NULL && conn->msquic != NULL) {
        int rc = meridian_stream_send_uni(conn->msquic, conn->direct_connection,
                                          data, (size_t)len, NULL);
        if (rc == 0) *result = (int)len;
    } else if (conn->relay_path.active && conn->relay != NULL) {
        int rc = meridian_relay_send_datagram(conn->relay, data, (size_t)len,
                                              conn->relay_endpoint_id);
        if (rc == 0) *result = (int)len;
    }
}

int meridian_conn_send(meridian_conn_t* conn, const uint8_t* data, uint32_t len) {
    if (conn == NULL || data == NULL || len == 0) return -1;

    /* Result lives on our stack; the actor is drained synchronously below
     * so it stays live across the dispatch. */
    int result = -1;
    meridian_conn_send_msg_t payload = {
        .data = data,
        .len = len,
        .result = &result,
    };
    message_t msg = {
        .type = MERIDIAN_CONN_SEND,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&conn->actor, &msg)) {
        /* DESTROY flag already set or queue rejected the push — connection
         * is on its way out. Return -1 to mirror the pre-2.5 contract. */
        return -1;
    }
    actor_run_inline(&conn->actor, 32);
    return result;
}

int meridian_conn_upgrade_to_direct(meridian_conn_t* conn) {
    if (conn == NULL) return -1;

    /* Result lives on our stack; the actor is drained synchronously below
     * so it stays live across the dispatch. */
    int result = -1;
    meridian_conn_upgrade_to_direct_msg_t payload = {
        .result = &result,
    };
    message_t msg = {
        .type = MERIDIAN_CONN_UPGRADE_TO_DIRECT,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&conn->actor, &msg)) {
        /* DESTROY flag already set or queue rejected the push — connection
         * is on its way out. Return -1 to mirror the pre-2.7 contract. */
        return -1;
    }
    actor_run_inline(&conn->actor, 32);
    return result;
}

// ============================================================================
// PEER INFORMATION UPDATES
// ============================================================================

/**
 * @brief Actor-thread body for MERIDIAN_CONN_SET_PEER_NAT_TYPE.
 *
 * Runs serially on the actor's owning thread. Updates peer_nat_type and,
 * for symmetric peers, downgrades the connection state to RELAY_ONLY by
 * clearing the direct path. Actor dispatch is the serialization point.
 */
static void meridian_conn_handle_set_peer_nat_type(meridian_conn_t* conn,
                                                   meridian_nat_type_t type) {
    conn->peer_nat_type = type;

    // If peer has symmetric NAT, downgrade to relay-only
    if (type == MERIDIAN_NAT_TYPE_SYMMETRIC) {
        conn->direct_path.active = false;
        conn->state = MERIDIAN_CONN_STATE_RELAY_ONLY;
    }
}

void meridian_conn_set_peer_nat_type(meridian_conn_t* conn, meridian_nat_type_t type) {
    if (conn == NULL) return;

    meridian_conn_set_peer_nat_type_msg_t payload = {.type = type};
    message_t msg = {
        .type = MERIDIAN_CONN_SET_PEER_NAT_TYPE,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&conn->actor, &msg)) return;
    actor_run_inline(&conn->actor, 32);
}

/**
 * @brief Actor-thread body for MERIDIAN_CONN_SET_PEER_REFLEXIVE.
 *
 * Runs serially on the actor's owning thread. Records the reflexive
 * address tuple on the direct path. Actor dispatch is the serialization
 * point.
 */
static void meridian_conn_handle_set_peer_reflexive(meridian_conn_t* conn,
                                                    uint32_t addr, uint16_t port) {
    conn->direct_path.reflexive_addr = addr;
    conn->direct_path.reflexive_port = port;
}

void meridian_conn_set_peer_reflexive(meridian_conn_t* conn, uint32_t addr, uint16_t port) {
    if (conn == NULL) return;

    meridian_conn_set_peer_reflexive_msg_t payload = {.addr = addr, .port = port};
    message_t msg = {
        .type = MERIDIAN_CONN_SET_PEER_REFLEXIVE,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&conn->actor, &msg)) return;
    actor_run_inline(&conn->actor, 32);
}

// ============================================================================
// PATH UPGRADE
// ============================================================================

/**
 * @brief Actor-thread body of MERIDIAN_CONN_UPGRADE_TO_DIRECT.
 *
 * Runs on the conn's actor thread (worker thread under a real pool, caller
 * thread under the NULL-pool synchronous-drain path used in tests). The
 * pre-actor body held an internal lock around this exact sequence; the
 * actor model already serializes the body on a single thread, so no
 * additional synchronization is required.
 *
 * Four rejection branches (state, peer symmetric NAT, local symmetric
 * NAT, missing reflexive address) all write -1 into *result and return.
 * The success branch mutates three fields atomically from the actor's
 * point of view and writes 0 into *result.
 */
static void meridian_conn_handle_upgrade_to_direct(meridian_conn_t* conn,
                                                   int* result) {
    assert(conn != NULL);
    assert(result != NULL);

    /* Only upgrade from RELAY state. */
    if (conn->state != MERIDIAN_CONN_STATE_RELAY) {
        *result = (conn->state == MERIDIAN_CONN_STATE_DIRECT) ? 0 : -1;
        return;
    }

    /* Don't upgrade if peer or local has symmetric NAT. */
    if (conn->peer_nat_type == MERIDIAN_NAT_TYPE_SYMMETRIC ||
        conn->local_nat_type == MERIDIAN_NAT_TYPE_SYMMETRIC) {
        *result = -1;
        return;
    }

    /* Need a reflexive address to attempt direct. */
    if (conn->direct_path.reflexive_addr == 0) {
        *result = -1;
        return;
    }

    conn->state = MERIDIAN_CONN_STATE_TRYING_DIRECT;
    conn->direct_attempts++;
    conn->last_direct_attempt_ms = 0;
    *result = 0;
}

/**
 * @brief Actor-thread body of MERIDIAN_CONN_CONNECT.
 *
 * Runs on the conn's actor thread (worker thread under a real pool, caller
 * thread under the NULL-pool synchronous-drain path used in tests). The
 * pre-actor body held an internal lock around this exact 4-state switch;
 * the actor model already serializes the body on a single thread, so no
 * additional synchronization is required.
 *
 * Replicates the original wrapper's switch on conn->state: RELAY_ONLY and
 * RELAY both activate the relay path (no-op if relay is NULL); TRYING_DIRECT
 * increments direct_attempts and may fall back to RELAY when no direct
 * address is known; DIRECT is a no-op (already connected). Writes 0 into
 * *result on every branch (the contract: the only non-zero return is the
 * NULL-conn fast-path in the public wrapper, which never reaches the
 * actor).
 */
static void meridian_conn_handle_connect(meridian_conn_t* conn, int* result) {
    assert(conn != NULL);
    assert(result != NULL);

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

    *result = 0;
}

/**
 * @brief Actor-thread body for MERIDIAN_CONN_TEST_FORCE_STATE.
 *
 * Runs on the conn's actor thread. Test-only override: directly assigns
 * the requested state. There is no validation against the connection's
 * path activity flags — callers in tests are expected to drive conn state
 * through stages in a coherent order.
 */
static void meridian_conn_handle_test_force_state(meridian_conn_t* conn,
                                                  meridian_conn_state_t force_state) {
    assert(conn != NULL);
    conn->state = force_state;
}

// ============================================================================
// TEST HELPERS
// ============================================================================

/**
 * @brief Test-only: force conn->state through the actor dispatch path.
 *
 * Useful in tests that need to set up a specific connection state before
 * exercising a handler. Unlike direct field assignment, this dispatch goes
 * through the same actor mailbox as production sends, so the state
 * transition is observable to any in-flight messages on the same actor.
 *
 * @param conn   The connection (NULL is a no-op).
 * @param state  The state to assign.
 */
void meridian_conn_test_force_state(meridian_conn_t* conn, meridian_conn_state_t state) {
    if (conn == NULL) return;

    meridian_conn_test_force_state_msg_t payload = { .force_state = state };
    message_t msg = { .type = MERIDIAN_CONN_TEST_FORCE_STATE,
                      .payload = &payload,
                      .payload_destroy = NULL };
    if (!actor_send(&conn->actor, &msg)) return;
    actor_run_inline(&conn->actor, 32);
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