/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon Meridian rendezvous actor messages.
 *
 * Per-subsystem message-payload header for the MERIDIAN_RENDV_* family of
 * message_type_e values declared in src/Actor/message.h.
 *
 * The rendezvous handle follows the shape upstream's nat_detect uses: an
 * actor, a scheduler pool, and the state embedded in the struct with no
 * mutex. The tunnel data arrives on msquic's worker threads and is handed
 * to the actor, which delivers it to the application callback.
 *
 * A request owns everything it carries: the actor may still be holding it
 * after the caller has given up and walked away, so nothing in a request
 * may point at the caller's stack. The tunnel bytes are therefore copied
 * into the request.
 */

#ifndef POSEIDON_MERIDIAN_RENDV_MESSAGES_H
#define POSEIDON_MERIDIAN_RENDV_MESSAGES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../Actor/actor_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set the local rendezvous point. The actor takes the reference and
 * releases the previous one. */
typedef struct meridian_rendv_set_local_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* rendv;            /**< The local rendezvous (the actor takes a
                                reference). */
} meridian_rendv_set_local_msg_t;

/* Add a peer rendezvous point. The actor takes the reference; a peer
 * that is already known is a no-op. */
typedef struct meridian_rendv_add_peer_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* rendv;            /**< The peer rendezvous (the actor takes a
                                reference). */
} meridian_rendv_add_peer_msg_t;

/* Remove a peer rendezvous by address. */
typedef struct meridian_rendv_remove_peer_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint32_t addr;         /**< The peer's address. */
    uint16_t port;         /**< The peer's port. */
} meridian_rendv_remove_peer_msg_t;

/* Find a peer rendezvous by address. The actor hands back a new
 * reference, which the caller releases. */
typedef struct meridian_rendv_find_peer_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint32_t addr;         /**< The peer's address. */
    uint16_t port;         /**< The peer's port. */
    void* out_peer;         /**< The actor writes the found peer here. */
} meridian_rendv_find_peer_msg_t;

/* Register the application callback for tunnel data. */
typedef struct meridian_rendv_set_stream_cb_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* callback;         /**< The application callback. */
    void* ctx;              /**< Its user context. */
} meridian_rendv_set_stream_cb_msg_t;

/* Tunnel data arrived. The actor hands it to the application callback. */
typedef struct meridian_rendv_stream_data_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* data;         /**< Owned: the request's copy of the bytes. */
    size_t len;            /**< The byte count. */
    bool is_bidi;          /**< The stream's direction. */
} meridian_rendv_stream_data_msg_t;

/* Detect the NAT type. The actor sends the address requests and
 * classifies; the wrapper delivers the answer to the application. */
typedef struct meridian_rendv_detect_nat_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* relay_a;         /**< The first relay client (borrowed). */
    void* relay_b;         /**< The second relay client (borrowed). */
    uint32_t reflexive_addr;  /**< The actor writes it here. */
    uint16_t reflexive_port;  /**< The actor writes it here. */
    int nat_type;             /**< The actor writes the classification
                                   here. */
} meridian_rendv_detect_nat_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_RENDV_MESSAGES_H */
