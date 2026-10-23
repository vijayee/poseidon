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
 * protocol's peer table — the way upstream's network actor owns the peer
 * list — and every access to that table goes through it.
 *
 * A request owns everything it carries: the actor may still be holding
 * it after the caller has given up and walked away, so nothing in a
 * request may point at the caller's stack. The bytes a send carries are
 * therefore copied into the request.
 */

#ifndef POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H
#define POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H

#include <stdint.h>
#include <stddef.h>

#include "../../Actor/actor_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A peer connected. The actor adds it to the peer table.
 *
 * The listener callback hands this over and moves on: it marks the
 * request orphaned up front, which is the actor's signal that the
 * request is its to free. The connect path waits for the answer
 * instead, so the peer is in the table when connect returns. */
typedef struct meridian_protocol_peer_added_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* connection;      /**< The msquic connection handle. */
    void* node;            /**< The peer node (borrowed, its refcount is
                                held by the table), or NULL. */
} meridian_protocol_peer_added_msg_t;

/* Drop the peer with this address: the actor closes its connection,
 * releases the node and takes it out of the table. */
typedef struct meridian_protocol_peer_removed_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint32_t addr;         /**< The peer's address. */
    uint16_t port;         /**< The peer's port. */
} meridian_protocol_peer_removed_msg_t;

/* A snapshot of the peer table. The actor builds the copy; the caller
 * frees the array (not the nodes — the table keeps those). */
typedef struct meridian_protocol_peers_snapshot_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void** peers;          /**< The actor's copy of the peer pointers. */
    size_t num_peers;      /**< The actor writes the count here. */
} meridian_protocol_peers_snapshot_msg_t;

/* Send a packet to one peer. The actor finds the peer's connection and
 * sends the bytes. */
typedef struct meridian_protocol_send_packet_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* data;         /**< Owned: the request's copy of the bytes. */
    size_t len;            /**< The byte count. */
    uint32_t target_addr;  /**< The peer to send to. */
    uint16_t target_port;  /**< The peer's port. */
} meridian_protocol_send_packet_msg_t;

/* Send a measure request to one peer. The actor finds the peer's
 * connection, builds the PING and puts it on a stream. */
typedef struct meridian_protocol_send_measure_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint64_t query_id;     /**< The measure request's id. */
    uint32_t target_addr;  /**< The peer to measure. */
    uint16_t target_port;  /**< The peer's port. */
} meridian_protocol_send_measure_msg_t;

/* Run the gossip tick. The actor ticks the scheduler and, when it is
 * time, sends a gossip to every peer. */
typedef struct meridian_protocol_gossip_tick_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
} meridian_protocol_gossip_tick_msg_t;

/* A gossip reply arrived. The actor matches it against the in-flight
 * gossips and delivers the completion. */
typedef struct meridian_protocol_gossip_response_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* data;         /**< Owned: the request's copy of the packet. */
    size_t len;            /**< The byte count. */
    void* from;            /**< The peer that sent it (borrowed; it belongs
                                to the protocol's peer table). */
} meridian_protocol_gossip_response_msg_t;

/* Send a packet to every peer. The actor does the fan-out. */
typedef struct meridian_protocol_broadcast_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* data;         /**< Owned: the request's copy of the bytes. */
    size_t len;            /**< The byte count. */
} meridian_protocol_broadcast_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MERIDIAN_PROTOCOL_MESSAGES_H */
