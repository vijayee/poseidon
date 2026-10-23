/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon Meridian protocol actor messages.
 *
 * Per-subsystem message-payload header for the MERIDIAN_PROTOCOL_* family of
 * message_type_e values declared in src/Actor/message.h.
 *
 * The protocol is a foreign-thread subsystem: msquic delivers the
 * connection and the stream events on its own worker threads, and the
 * application calls the protocol from its own thread. The actor owns the
 * protocol's tables; the msquic callbacks hand their events over and
 * move on, the way upstream's network actor does.
 */

#ifndef POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H
#define POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The protocol's actor carries the events that arrive on msquic's worker
 * threads. Each one is a request the actor answers by mutating the
 * protocol's tables; the payload owns everything it carries, because the
 * actor may still be holding it after the sender has moved on.
 */

/* A peer connected. The actor adds it to the connected-peer table. */
typedef struct meridian_protocol_peer_added_msg_t {
    void* connection;  /**< The msquic connection handle (borrowed). */
} meridian_protocol_peer_added_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H */
