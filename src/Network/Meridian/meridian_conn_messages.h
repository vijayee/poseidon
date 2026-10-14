/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon Meridian connection actor messages.
 *
 * Per-subsystem message-payload header for the MERIDIAN_CONN_* family of
 * message_type_e values declared in src/Actor/message.h. The actor dispatch
 * uses the global type tag to select a payload layout; this header defines
 * those layouts. Keeping the structs in a per-subsystem header — rather
 * than inside the global message.h — preserves message.h's
 * dependency-free contract (no msquic, no Meridian, no transport).
 */

#ifndef POSEIDON_MERIDIAN_CONN_MESSAGES_H
#define POSEIDON_MERIDIAN_CONN_MESSAGES_H

#include <stdint.h>
#include "meridian_rendv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MESSAGE TABLE
 * ----------------------------------------------------------------------------
 * The 10 MERIDIAN_CONN_* values defined in message.h map to the following
 * payload layouts. The two lifetime rules are uniform across the family:
 *
 *   - Borrowed payloads: the sender's buffer must outlive the actor's
 *     processing of this message. The receiver MUST NOT free it. The
 *     sender's message_t has payload_destroy = NULL.
 *
 *   - NULL payloads: the sender's message_t has payload = NULL and
 *     payload_destroy = NULL. The receiver asserts payload == NULL.
 *
 * When commit 2.4 routes mutators through actor_send, the caller is
 * responsible for holding borrowed buffers alive until actor_run drains
 * the actor. The Meridian connection's per-actor pool is the natural
 * drain boundary.
 * ============================================================================
 *
 *   MERIDIAN_CONN_CONNECT            -> NULL
 *   MERIDIAN_CONN_DISCONNECT         -> NULL
 *   MERIDIAN_CONN_SEND               -> meridian_conn_send_msg_t (borrowed)
 *   MERIDIAN_CONN_UPGRADE_TO_DIRECT  -> NULL
 *   MERIDIAN_CONN_DIRECT_CONNECTED   -> NULL (reserved for msquic callback
 *                                            wiring; layout TBD in a later
 *                                            phase)
 *   MERIDIAN_CONN_RELAY_CONNECTED    -> NULL (reserved for msquic callback
 *                                            wiring; layout TBD in a later
 *                                            phase)
 *   MERIDIAN_CONN_PATH_FAILED        -> NULL (reserved for msquic callback
 *                                            wiring; layout TBD in a later
 *                                            phase)
 *   MERIDIAN_CONN_SET_PEER_NAT_TYPE  -> meridian_conn_set_peer_nat_type_msg_t
 *                                       (borrowed: enum value passed by value)
 *   MERIDIAN_CONN_SET_PEER_REFLEXIVE -> meridian_conn_set_peer_reflexive_msg_t
 *                                       (borrowed: integers passed by value)
 *   MERIDIAN_CONN_DESTROY            -> NULL
 *
 * No payload-destroy functions are defined here. Every message in the
 * MERIDIAN_CONN_* family is either NULL-payload or borrowed-by-value,
 * so the actor machinery's NULL/NULL no-op free (message_queue.c) handles
 * the entire family's lifetime.
 * ============================================================================ */

/* MERIDIAN_CONN_SEND payload. Borrowed: the caller's data buffer must
 * outlive the actor's processing of the message. */
typedef struct meridian_conn_send_msg_t {
    const uint8_t* data;   /**< Borrowed: caller-owned send buffer. */
    uint32_t       len;    /**< Buffer length in bytes. */
} meridian_conn_send_msg_t;

/* MERIDIAN_CONN_SET_PEER_NAT_TYPE payload. Borrowed-by-value: the enum
 * is copied into the struct, no heap lifetime to manage. */
typedef struct meridian_conn_set_peer_nat_type_msg_t {
    meridian_nat_type_t type;  /**< New peer NAT classification. */
} meridian_conn_set_peer_nat_type_msg_t;

/* MERIDIAN_CONN_SET_PEER_REFLEXIVE payload. Borrowed-by-value: the
 * address tuple is copied into the struct, no heap lifetime to manage. */
typedef struct meridian_conn_set_peer_reflexive_msg_t {
    uint32_t addr;   /**< Peer reflexive IPv4 address (network byte order). */
    uint16_t port;   /**< Peer reflexive port (host byte order). */
} meridian_conn_set_peer_reflexive_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_CONN_MESSAGES_H */
