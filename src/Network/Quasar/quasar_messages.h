/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon Quasar overlay actor messages.
 *
 * Per-subsystem message-payload header for the MERIDIAN_QUASAR_* family of
 * message_type_e values declared in src/Actor/message.h.
 *
 * The overlay's state — the routing filter, the local subscriptions, the
 * per-neighbour filters, the seen filter and the application callback —
 * belongs to its actor. Upstream keeps the gossip inside the network
 * actor for the same reason.
 *
 * A request owns everything it carries: the actor may still be holding it
 * after the caller has given up and walked away, so nothing in a request
 * may point at the caller's stack. The topic and the payload are
 * therefore copied into the request.
 */

#ifndef POSEIDON_QUASAR_MESSAGES_H
#define POSEIDON_QUASAR_MESSAGES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../Actor/actor_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the application callback for delivered messages. */
typedef struct meridian_quasar_set_message_cb_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* callback;         /**< The application callback. */
    void* ctx;              /**< Its user context. */
} meridian_quasar_set_message_cb_msg_t;

/* Subscribe to a topic. */
typedef struct meridian_quasar_subscribe_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* topic;         /**< Owned: the request's copy of the topic. */
    size_t topic_len;      /**< The topic's length. */
    uint32_t ttl;          /**< The subscription's time to live. */
} meridian_quasar_subscribe_msg_t;

/* Unsubscribe from a topic. */
typedef struct meridian_quasar_unsubscribe_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* topic;         /**< Owned: the request's copy of the topic. */
    size_t topic_len;      /**< The topic's length. */
} meridian_quasar_unsubscribe_msg_t;

/* Publish a message into the overlay. */
typedef struct meridian_quasar_publish_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* topic;         /**< Owned: the request's copy of the topic. */
    size_t topic_len;      /**< The topic's length. */
    uint8_t* data;          /**< Owned: the request's copy of the payload. */
    size_t data_len;       /**< The payload's length. */
} meridian_quasar_publish_msg_t;

/* Run the propagation walk. */
typedef struct meridian_quasar_propagate_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
} meridian_quasar_propagate_msg_t;

/* Run the periodic tick. */
typedef struct meridian_quasar_tick_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
} meridian_quasar_tick_msg_t;

/* Look up a neighbour's filter. */
typedef struct meridian_quasar_neighbor_filter_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* node;            /**< The neighbour (borrowed). */
    void* out_filter;      /**< The actor writes the filter here; it belongs
                                to the actor. */
} meridian_quasar_neighbor_filter_msg_t;

/* Remove a neighbour's filter. */
typedef struct meridian_quasar_remove_neighbor_filter_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* node;            /**< The neighbour (borrowed). */
    int result;            /**< The actor writes the outcome here. */
} meridian_quasar_remove_neighbor_filter_msg_t;

/* A gossip arrived from a peer. */
typedef struct meridian_quasar_on_gossip_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    uint8_t* data;          /**< Owned: the request's copy of the packet. */
    size_t len;             /**< The packet's length. */
    void* from;             /**< The peer that sent it (borrowed). */
} meridian_quasar_on_gossip_msg_t;

/* A routed message arrived from a peer. The actor takes the message. */
typedef struct meridian_quasar_on_route_message_msg_t {
    actor_reply_t reply;   /**< Must stay first. */
    void* message;          /**< Owned: the route message the actor frees. */
    void* from;             /**< The peer that sent it (borrowed). */
} meridian_quasar_on_route_message_msg_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_QUASAR_MESSAGES_H */
