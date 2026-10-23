/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon Meridian relay client actor messages.
 *
 * Per-subsystem message-payload header for the MERIDIAN_RELAY_* family of
 * message_type_e values declared in src/Actor/message.h. The relay actor's
 * dispatch uses the global type tag to select a payload layout; this header
 * defines those layouts. Keeping the structs in a per-subsystem header —
 * rather than inside the global message.h — preserves message.h's
 * dependency-free contract (no msquic, no Meridian, no transport).
 *
 * The relay client is a foreign-thread actor: msquic delivers connection
 * and stream events on its own worker threads, and the application calls
 * the public API from its own thread. The actor is the single owner of
 * the relay's mutable state; every path that touches it — public API call
 * or msquic callback — hands work over through actor_send.
 */

#ifndef POSEIDON_MERIDIAN_RELAY_MESSAGES_H
#define POSEIDON_MERIDIAN_RELAY_MESSAGES_H

#include <stdint.h>
#include <stddef.h>

#include "meridian_relay.h"
#include "meridian_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MESSAGE TABLE
 * ----------------------------------------------------------------------------
 * The 13 MERIDIAN_RELAY_* values defined in message.h map to the following
 * payload layouts. Three lifetime rules are used across the family:
 *
 *   - Borrowed payloads: the sender's buffer must outlive the actor's
 *     processing of this message. The receiver MUST NOT free it. The
 *     sender's message_t has payload_destroy = NULL.
 *
 *   - Borrowed out-params: the wrapper's stack frame owns the int* the
 *     handler writes. The wrapper's frame outlives the synchronous drain
 *     (actor_run_inline), so the handler may write it before the drain
 *     returns.
 *
 *   - Owned payloads: the sender heap-allocates the payload (and, for the
 *     datagram, the copied bytes it points at) and hands ownership to the
 *     message. payload_destroy releases it if the message is dropped
 *     without being dispatched.
 *
 * The application-facing callbacks (on_datagram, on_addr_response) are
 * invoked from inside the actor's dispatch. A callback that calls back
 * into the relay API re-enters the actor; the wrappers detect that case
 * and run the handler directly on the calling thread instead of queueing.
 * ============================================================================
 *
 *   MERIDIAN_RELAY_CONNECT            -> meridian_relay_connect_msg_t
 *                                       (borrowed: caller-owned int* result
 *                                       out-param; handler writes 0 on
 *                                       success, -1 on failure)
 *   MERIDIAN_RELAY_DISCONNECT         -> meridian_relay_disconnect_msg_t
 *                                       (borrowed: caller-owned int* result
 *                                       out-param; handler writes 0 on
 *                                       success, -1 on failure)
 *   MERIDIAN_RELAY_SEND_DATAGRAM      -> meridian_relay_send_datagram_msg_t
 *                                       (borrowed: caller-owned data buffer
 *                                       AND a caller-owned int* result
 *                                       out-param)
 *   MERIDIAN_RELAY_SEND_ADDR_REQUEST  -> meridian_relay_addr_request_msg_t
 *                                       (borrowed: caller-owned int* result
 *                                       out-param)
 *   MERIDIAN_RELAY_SET_DATAGRAM_CB    -> meridian_relay_set_datagram_cb_msg_t
 *                                       (by value: callback + user context)
 *   MERIDIAN_RELAY_SET_ADDR_CB        -> meridian_relay_set_addr_cb_msg_t
 *                                       (by value: callback + user context)
 *   MERIDIAN_RELAY_CONNECTED          -> NULL (msquic connection event)
 *   MERIDIAN_RELAY_SHUTDOWN           -> NULL (msquic connection event)
 *   MERIDIAN_RELAY_SHUTDOWN_COMPLETE  -> NULL (msquic connection event)
 *   MERIDIAN_RELAY_PEER_STREAM_STARTED-> meridian_relay_peer_stream_msg_t
 *                                       (by value: the stream entry the
 *                                       actor must track)
 *   MERIDIAN_RELAY_STREAM_CLOSED      -> meridian_relay_peer_stream_msg_t
 *                                       (by value: the stream entry the
 *                                       actor must drop)
 *   MERIDIAN_RELAY_ADDR_RESPONSE      -> meridian_relay_addr_response_msg_t
 *                                       (by value: endpoint id + reflexive
 *                                       address)
 *   MERIDIAN_RELAY_DATAGRAM           -> meridian_relay_datagram_msg_t
 *                                       (owned: the message owns a copy of
 *                                       the forwarded bytes)
 */

/* Open the QUIC connection to the relay server. */
typedef struct meridian_relay_connect_msg_t {
    int* result;  /* borrowed caller-owned out-param */
} meridian_relay_connect_msg_t;

/* Close the QUIC connection to the relay server. */
typedef struct meridian_relay_disconnect_msg_t {
    int* result;  /* borrowed caller-owned out-param */
} meridian_relay_disconnect_msg_t;

/* Forward a datagram to a peer through the relay. */
typedef struct meridian_relay_send_datagram_msg_t {
    const uint8_t* data;   /* borrowed caller-owned buffer */
    size_t len;
    uint32_t dest_endpoint_id;
    int* result;           /* borrowed caller-owned out-param */
} meridian_relay_send_datagram_msg_t;

/* Ask the relay server for this client's reflexive address. */
typedef struct meridian_relay_addr_request_msg_t {
    int* result;  /* borrowed caller-owned out-param */
} meridian_relay_addr_request_msg_t;

/* Register the datagram callback. */
typedef struct meridian_relay_set_datagram_cb_msg_t {
    meridian_relay_datagram_cb_t callback;
    void* ctx;
} meridian_relay_set_datagram_cb_msg_t;

/* Register the address-response callback. */
typedef struct meridian_relay_set_addr_cb_msg_t {
    meridian_relay_addr_cb_t callback;
    void* ctx;
} meridian_relay_set_addr_cb_msg_t;

/* A peer opened a stream. The entry is created by the msquic callback
 * thread (it decides whether the stream is accepted) and tracked by the
 * actor. */
typedef struct meridian_relay_peer_stream_msg_t {
    meridian_stream_entry_t* entry;  /* borrowed; the callback owns the entry */
} meridian_relay_peer_stream_msg_t;

/* The relay answered the address request. */
typedef struct meridian_relay_addr_response_msg_t {
    uint32_t endpoint_id;
    uint32_t reflexive_addr;
    uint16_t reflexive_port;
} meridian_relay_addr_response_msg_t;

/* A peer datagram arrived through the relay. The message owns the copy of
 * the bytes: the msquic callback's stream buffer is released as soon as
 * the callback returns. */
typedef struct meridian_relay_datagram_msg_t {
    uint32_t src_endpoint_id;
    uint8_t* data;  /* owned by the message */
    size_t len;
} meridian_relay_datagram_msg_t;

/* Releases an owned datagram payload. Registered as the message's
 * payload_destroy, so a message dropped without being dispatched (actor
 * destroyed) still releases the copied bytes. */
void meridian_relay_datagram_msg_destroy(void* payload);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_RELAY_MESSAGES_H */
