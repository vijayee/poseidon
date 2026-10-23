//
// Created by victor on 4/19/26.
//

#include "quasar.h"
#include "quasar_messages.h"

/* The dispatch runs the overlay's work on the actor's thread. */
static void meridian_quasar_dispatch(void* state, message_t* msg);

static void quasar_set_message_callback_impl(quasar_t* quasar, quasar_message_cb_t cb, void* ctx);
static int quasar_subscribe_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl);
static int quasar_unsubscribe_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len);
static int quasar_publish_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len,
                               const uint8_t* data, size_t data_len);
static int quasar_on_route_message_impl(quasar_t* quasar, quasar_route_message_t* msg,
                                        const struct meridian_node_t* from);
static int quasar_on_gossip_impl(quasar_t* quasar, const uint8_t* data, size_t len,
                                 const struct meridian_node_t* from);
static int quasar_propagate_impl(quasar_t* quasar);
static int quasar_tick_impl(quasar_t* quasar);

/* The actor's own helpers: they touch the neighbour filters, which the
 * actor owns. */
static attenuated_bloom_filter_t* quasar_get_neighbor_filter_impl(
    quasar_t* quasar, const meridian_node_t* node);
static attenuated_bloom_filter_t* quasar_get_or_create_neighbor_filter_impl(
    quasar_t* quasar, const meridian_node_t* node);
static int quasar_remove_neighbor_filter_impl(
    quasar_t* quasar, const meridian_node_t* node);
#include "quasar_route.h"
#include "../Meridian/meridian_protocol.h"
#include "../../Bloom/elastic_bloom_filter.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ============================================================================
// DEFAULT PARAMETERS
// ============================================================================

/** Number of levels in the attenuated Bloom filter (max routing hops) */
#define QUASAR_DEFAULT_LEVELS 5

/** Number of bits per elastic Bloom filter level */
#define QUASAR_DEFAULT_SIZE 1024

/** Number of hash functions per Bloom filter level */
#define QUASAR_DEFAULT_HASH_COUNT 3

/** Fill ratio threshold that triggers elastic Bloom filter expansion */
#define QUASAR_DEFAULT_OMEGA 0.75f

/** Fingerprint bit width for elastic Bloom filter bucket entries */
#define QUASAR_DEFAULT_FP_BITS EBF_DEFAULT_FP_BITS

/** Default size in bits for the negative (visited) bloom filter */
#define QUASAR_DEFAULT_NEGATIVE_SIZE 2048

/** Default number of hash functions for the negative bloom filter */
#define QUASAR_DEFAULT_NEGATIVE_HASHES 3

/** Default size in bits for the dedup (seen) bloom filter */
#define QUASAR_DEFAULT_SEEN_SIZE 4096

/** Default number of hash functions for the dedup bloom filter */
#define QUASAR_DEFAULT_SEEN_HASHES 3

// ============================================================================
// ROUTE MESSAGE LIFECYCLE
// ============================================================================

quasar_route_message_t* quasar_route_message_create(const uint8_t* topic, size_t topic_len,
                                                      const uint8_t* data, size_t data_len,
                                                      uint32_t max_hops,
                                                      size_t neg_size, uint32_t neg_hashes) {
    if (topic == NULL || data == NULL) return NULL;
    quasar_message_id_init();
    quasar_route_message_t* msg = get_clear_memory(sizeof(quasar_route_message_t));
    msg->id = quasar_message_id_get_next();
    msg->topic = buffer_create_from_pointer_copy((uint8_t*)topic, topic_len);
    msg->data = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    msg->visited = bloom_filter_create(neg_size, neg_hashes);
    msg->hops_remaining = max_hops;
    msg->pub_addrs = NULL;
    msg->pub_ports = NULL;
    msg->pub_count = 0;
    msg->pub_capacity = 0;
    refcounter_init((refcounter_t*)msg);
    return msg;
}

void quasar_route_message_destroy(quasar_route_message_t* msg) {
    if (msg == NULL) return;
    refcounter_dereference((refcounter_t*)msg);
    if (refcounter_count((refcounter_t*)msg) == 0) {
        buffer_destroy(msg->topic);
        buffer_destroy(msg->data);
        bloom_filter_destroy(msg->visited);
        free(msg->pub_addrs);
        free(msg->pub_ports);
        free(msg);
    }
}

int quasar_route_message_add_visited(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return -1;
    // Hash the node's address and port as the bloom filter key
    // This identifies a node uniquely within the network
    uint8_t node_key[6];
    memcpy(node_key, &node->addr, sizeof(node->addr));
    memcpy(node_key + sizeof(node->addr), &node->port, sizeof(node->port));
    int result = bloom_filter_add(msg->visited, node_key, sizeof(node_key));
    return result;
}

bool quasar_route_message_has_visited(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return false;
    uint8_t node_key[6];
    memcpy(node_key, &node->addr, sizeof(node->addr));
    memcpy(node_key + sizeof(node->addr), &node->port, sizeof(node->port));
    bool result = bloom_filter_contains(msg->visited, node_key, sizeof(node_key));
    return result;
}

int quasar_route_message_add_publisher(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return -1;
    if (msg->pub_count >= msg->pub_capacity) {
        uint32_t new_cap = msg->pub_capacity == 0 ? 4 : msg->pub_capacity * 2;
        uint32_t* new_addrs = realloc(msg->pub_addrs, sizeof(uint32_t) * new_cap);
        uint16_t* new_ports = realloc(msg->pub_ports, sizeof(uint16_t) * new_cap);
        if (new_addrs == NULL || new_ports == NULL) {
            free(new_addrs);
            free(new_ports);
            return -1;
        }
        msg->pub_addrs = new_addrs;
        msg->pub_ports = new_ports;
        msg->pub_capacity = new_cap;
    }
    msg->pub_addrs[msg->pub_count] = node->addr;
    msg->pub_ports[msg->pub_count] = node->port;
    msg->pub_count++;
    return 0;
}

bool quasar_route_message_has_publisher(quasar_route_message_t* msg, const meridian_node_t* node) {
    if (msg == NULL || node == NULL) return false;
    for (uint32_t i = 0; i < msg->pub_count; i++) {
        if (msg->pub_addrs[i] == node->addr && msg->pub_ports[i] == node->port) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// ACTOR DISPATCH
// ============================================================================

/* Releases the overlay's state. The state belongs to the protocol's
 * actor, so the actor releases it. */
static void quasar_release_state(quasar_t* quasar);

/* Rebuilds the overlay's state with a new configuration. */
static int quasar_reconfigure_impl(quasar_t* quasar, uint32_t max_hops,
                                   uint32_t alpha, size_t seen_size,
                                   uint32_t seen_hashes);

/**
 * The overlay's dispatch.
 *
 * The overlay's state — the routing filter, the local subscriptions, the
 * per-neighbour filters, the seen filter and the application callback —
 * belongs to the protocol's actor: the network actor, the way upstream
 * keeps the gossip inside it. The protocol's actor hands the overlay's
 * messages here.
 *
 * A request that arrives from the application waits for its answer; a
 * request that arrives from a peer's thread — a gossip, a routed message
 * — is handed over and the run in progress picks it up.
 */
void quasar_dispatch(void* state, message_t* msg) {
    quasar_t* quasar = (quasar_t*)state;
    if (msg == NULL) return;

    switch (msg->type) {
        case MERIDIAN_QUASAR_DESTROY: {
            meridian_quasar_destroy_msg_t* p =
                (meridian_quasar_destroy_msg_t*)msg->payload;
            /* The overlay's state is this actor's: it goes here, not on
             * the caller's thread. The request names the overlay, so a
             * destroy that is still in flight lands on the right one. */
            quasar_t* victim = (p->quasar != NULL) ? p->quasar : quasar;
            quasar_release_state(victim);
            if (victim->protocol != NULL && victim->protocol->quasar == victim) {
                victim->protocol->quasar = NULL;
            }
            /* The shell belongs to the actor from the moment the request
             * was sent. */
            free(victim);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_RECONFIGURE: {
            meridian_quasar_reconfigure_msg_t* p =
                (meridian_quasar_reconfigure_msg_t*)msg->payload;
            p->reply.result = quasar_reconfigure_impl(quasar, p->max_hops,
                                                      p->alpha, p->seen_size,
                                                      p->seen_hashes);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_SET_MESSAGE_CB: {
            meridian_quasar_set_message_cb_msg_t* p =
                (meridian_quasar_set_message_cb_msg_t*)msg->payload;
            quasar_set_message_callback_impl(quasar,
                (quasar_message_cb_t)p->callback, p->ctx);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_SUBSCRIBE: {
            meridian_quasar_subscribe_msg_t* p =
                (meridian_quasar_subscribe_msg_t*)msg->payload;
            p->reply.result = quasar_subscribe_impl(quasar, p->topic,
                                                    p->topic_len, p->ttl);
            free(p->topic);
            p->topic = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_UNSUBSCRIBE: {
            meridian_quasar_unsubscribe_msg_t* p =
                (meridian_quasar_unsubscribe_msg_t*)msg->payload;
            p->reply.result = quasar_unsubscribe_impl(quasar, p->topic,
                                                      p->topic_len);
            free(p->topic);
            p->topic = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_PUBLISH: {
            meridian_quasar_publish_msg_t* p =
                (meridian_quasar_publish_msg_t*)msg->payload;
            p->reply.result = quasar_publish_impl(quasar, p->topic,
                                                  p->topic_len, p->data,
                                                  p->data_len);
            free(p->topic);
            free(p->data);
            p->topic = NULL;
            p->data = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_PROPAGATE: {
            meridian_quasar_propagate_msg_t* p =
                (meridian_quasar_propagate_msg_t*)msg->payload;
            p->reply.result = quasar_propagate_impl(quasar);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_TICK: {
            meridian_quasar_tick_msg_t* p = (meridian_quasar_tick_msg_t*)msg->payload;
            p->reply.result = quasar_tick_impl(quasar);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_GET_NEIGHBOR_FILTER: {
            meridian_quasar_neighbor_filter_msg_t* p =
                (meridian_quasar_neighbor_filter_msg_t*)msg->payload;
            p->out_filter = quasar_get_neighbor_filter_impl(
                quasar, (const meridian_node_t*)p->node);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_GET_OR_CREATE_NEIGHBOR_FILTER: {
            meridian_quasar_neighbor_filter_msg_t* p =
                (meridian_quasar_neighbor_filter_msg_t*)msg->payload;
            p->out_filter = quasar_get_or_create_neighbor_filter_impl(
                quasar, (const meridian_node_t*)p->node);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_REMOVE_NEIGHBOR_FILTER: {
            meridian_quasar_remove_neighbor_filter_msg_t* p =
                (meridian_quasar_remove_neighbor_filter_msg_t*)msg->payload;
            p->result = quasar_remove_neighbor_filter_impl(
                quasar, (const meridian_node_t*)p->node);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_ON_GOSSIP: {
            meridian_quasar_on_gossip_msg_t* p =
                (meridian_quasar_on_gossip_msg_t*)msg->payload;
            p->reply.result = quasar_on_gossip_impl(quasar, p->data, p->len,
                                                    (const struct meridian_node_t*)p->from);
            free(p->data);
            p->data = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_QUASAR_ON_ROUTE_MESSAGE: {
            meridian_quasar_on_route_message_msg_t* p =
                (meridian_quasar_on_route_message_msg_t*)msg->payload;
            quasar_route_message_t* message = (quasar_route_message_t*)p->message;
            p->reply.result = quasar_on_route_message_impl(quasar, message,
                                                          (const struct meridian_node_t*)p->from);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// PUBLIC API — round trips to the overlay's actor
// ============================================================================

/* Sends a request to the overlay's actor and waits for the answer.
 *
 * Returns 0 when the request was answered, 1 when it was never queued
 * (the caller still owns it), -1 when the caller gave up (the actor owns
 * the request now, and the caller must not free it), and
 * ACTOR_REQUEST_DETACHED when the caller is inside a dispatch: the
 * request goes out detached, the actor owns it, and nobody waits for the
 * answer. A dispatch runs on a pool worker, and a worker that waits for
 * another actor is a worker that runs no actor. */
static int quasar_round_trip(quasar_t* quasar, uint32_t type, actor_reply_t* reply) {
    if (quasar == NULL || reply == NULL) return -1;

    int trip = actor_request(quasar->actor, type, reply,
                             MERIDIAN_QUASAR_TIMEOUT_MS);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request; the answer is dropped. */
        return ACTOR_REQUEST_DETACHED;
    }
    if (trip == 1) {
        /* The actor is gone: nobody will ever look at the request. */
        actor_reply_destroy(reply);
        return 1;
    }
    if (trip != 0) {
        /* The caller gave up: the actor owns the request now. */
        return -1;
    }
    actor_reply_destroy(reply);
    return 0;
}

void quasar_set_message_callback(quasar_t* quasar, quasar_message_cb_t cb, void* ctx) {
    if (quasar == NULL) return;

    meridian_quasar_set_message_cb_msg_t* request =
        (meridian_quasar_set_message_cb_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_set_message_cb_msg_t));
    if (request == NULL) return;
    request->callback = (void*)cb;
    request->ctx = ctx;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_SET_MESSAGE_CB, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the callback is registered when the
         * actor gets to it. */
        return;
    }
    if (trip < 0) return;  /* the actor owns the request now */
    free(request);
}

int quasar_subscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl) {
    if (quasar == NULL || topic == NULL) return -1;

    meridian_quasar_subscribe_msg_t* request =
        (meridian_quasar_subscribe_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_subscribe_msg_t));
    if (request == NULL) return -1;
    request->topic = (uint8_t*)get_memory(topic_len > 0 ? topic_len : 1);
    if (request->topic == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->topic, topic, topic_len);
    request->topic_len = topic_len;
    request->ttl = ttl;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->topic);
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_SUBSCRIBE, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    if (trip == 1) free(request->topic);  /* never queued: the actor freed nothing */
    free(request);
    return rc;
}

int quasar_unsubscribe(quasar_t* quasar, const uint8_t* topic, size_t topic_len) {
    if (quasar == NULL || topic == NULL) return -1;

    meridian_quasar_unsubscribe_msg_t* request =
        (meridian_quasar_unsubscribe_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_unsubscribe_msg_t));
    if (request == NULL) return -1;
    request->topic = (uint8_t*)get_memory(topic_len > 0 ? topic_len : 1);
    if (request->topic == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->topic, topic, topic_len);
    request->topic_len = topic_len;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->topic);
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_UNSUBSCRIBE, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    if (trip == 1) free(request->topic);  /* never queued: the actor freed nothing */
    free(request);
    return rc;
}

int quasar_publish(quasar_t* quasar, const uint8_t* topic, size_t topic_len,
                    const uint8_t* data, size_t data_len) {
    if (quasar == NULL || topic == NULL) return -1;

    meridian_quasar_publish_msg_t* request =
        (meridian_quasar_publish_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_publish_msg_t));
    if (request == NULL) return -1;
    request->topic = (uint8_t*)get_memory(topic_len > 0 ? topic_len : 1);
    request->data = (uint8_t*)get_memory(data_len > 0 ? data_len : 1);
    if (request->topic == NULL || request->data == NULL) {
        free(request->topic);
        free(request->data);
        free(request);
        return -1;
    }
    memcpy(request->topic, topic, topic_len);
    memcpy(request->data, data, data_len);
    request->topic_len = topic_len;
    request->data_len = data_len;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->topic);
        free(request->data);
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_PUBLISH, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    if (trip == 1) {
        /* Never queued: the actor freed nothing. */
        free(request->topic);
        free(request->data);
    }
    free(request);
    return rc;
}

int quasar_reconfigure(quasar_t* quasar, uint32_t max_hops, uint32_t alpha,
                      size_t seen_size, uint32_t seen_hashes) {
    if (quasar == NULL) return -1;

    meridian_quasar_reconfigure_msg_t* request =
        (meridian_quasar_reconfigure_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_reconfigure_msg_t));
    if (request == NULL) return -1;
    request->max_hops = max_hops;
    request->alpha = alpha;
    request->seen_size = seen_size;
    request->seen_hashes = seen_hashes;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_RECONFIGURE, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    free(request);
    return rc;
}

int quasar_propagate(quasar_t* quasar) {
    if (quasar == NULL) return -1;

    meridian_quasar_propagate_msg_t* request =
        (meridian_quasar_propagate_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_propagate_msg_t));
    if (request == NULL) return -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_PROPAGATE, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    free(request);
    return rc;
}

int quasar_tick(quasar_t* quasar) {
    if (quasar == NULL) return -1;

    meridian_quasar_tick_msg_t* request =
        (meridian_quasar_tick_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_tick_msg_t));
    if (request == NULL) return -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_TICK, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    free(request);
    return rc;
}

attenuated_bloom_filter_t* quasar_get_neighbor_filter(quasar_t* quasar,
                                                     const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;

    meridian_quasar_neighbor_filter_msg_t* request =
        (meridian_quasar_neighbor_filter_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_neighbor_filter_msg_t));
    if (request == NULL) return NULL;
    request->node = (void*)node;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_GET_NEIGHBOR_FILTER,
                                &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The answer is a pointer into the actor's state, so it cannot be
         * dropped the way a number can. The caller is inside a dispatch:
         * it gets nothing. */
        return NULL;
    }
    if (trip < 0) return NULL;  /* the actor owns the request now */
    attenuated_bloom_filter_t* filter =
        (attenuated_bloom_filter_t*)request->out_filter;
    free(request);
    return trip == 0 ? filter : NULL;
}

attenuated_bloom_filter_t* quasar_get_or_create_neighbor_filter(
    quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;

    meridian_quasar_neighbor_filter_msg_t* request =
        (meridian_quasar_neighbor_filter_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_neighbor_filter_msg_t));
    if (request == NULL) return NULL;
    request->node = (void*)node;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_GET_OR_CREATE_NEIGHBOR_FILTER,
                                &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The answer is a pointer into the actor's state, so it cannot be
         * dropped the way a number can. The caller is inside a dispatch:
         * it gets nothing. */
        return NULL;
    }
    if (trip < 0) return NULL;  /* the actor owns the request now */
    attenuated_bloom_filter_t* filter =
        (attenuated_bloom_filter_t*)request->out_filter;
    free(request);
    return trip == 0 ? filter : NULL;
}

int quasar_remove_neighbor_filter(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return -1;

    meridian_quasar_remove_neighbor_filter_msg_t* request =
        (meridian_quasar_remove_neighbor_filter_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_remove_neighbor_filter_msg_t));
    if (request == NULL) return -1;
    request->node = (void*)node;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_REMOVE_NEIGHBOR_FILTER,
                                &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int result = request->result;
    free(request);
    return trip == 0 ? result : -1;
}

int quasar_on_gossip(quasar_t* quasar, const uint8_t* data, size_t len,
                     const struct meridian_node_t* from) {
    if (quasar == NULL || data == NULL) return -1;

    meridian_quasar_on_gossip_msg_t* request =
        (meridian_quasar_on_gossip_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_on_gossip_msg_t));
    if (request == NULL) return -1;
    request->data = (uint8_t*)get_memory(len > 0 ? len : 1);
    if (request->data == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->data, data, len);
    request->len = len;
    request->from = (void*)from;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->data);
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_ON_GOSSIP, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    if (trip == 1) free(request->data);  /* never queued: the actor freed nothing */
    free(request);
    return rc;
}

int quasar_on_route_message(quasar_t* quasar, quasar_route_message_t* msg,
                            const struct meridian_node_t* from) {
    if (quasar == NULL || msg == NULL) return -1;

    meridian_quasar_on_route_message_msg_t* request =
        (meridian_quasar_on_route_message_msg_t*)
        get_clear_memory(sizeof(meridian_quasar_on_route_message_msg_t));
    if (request == NULL) return -1;
    request->message = msg;
    request->from = (void*)from;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }
    int trip = quasar_round_trip(quasar, MERIDIAN_QUASAR_ON_ROUTE_MESSAGE, &request->reply);
    if (trip == ACTOR_REQUEST_DETACHED) {
        /* The actor owns the request: the work is queued and the answer
         * is dropped. A dispatch must not wait for another actor. */
        return 0;
    }
    if (trip < 0) return -1;  /* the actor owns the request now */
    int rc = request->reply.result;
    free(request);
    return rc;
}

// ============================================================================
// QUASAR LIFECYCLE
// ============================================================================

quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha,
                          uint32_t seen_size, uint32_t seen_hashes,
                          scheduler_pool_t* pool) {
    quasar_t* quasar = get_clear_memory(sizeof(quasar_t));
    quasar->protocol = protocol;

    // Build the attenuated Bloom filter: max_hops levels, each an elastic filter
    // Level 0 = local subscriptions, level N = topics N hops away
    quasar->routing = attenuated_bloom_filter_create(
        max_hops,
        QUASAR_DEFAULT_SIZE,
        QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA,
        QUASAR_DEFAULT_FP_BITS
    );
    vec_init(&quasar->local_subs);
    vec_init(&quasar->neighbor_filters);
    quasar->max_hops = max_hops;
    quasar->alpha = alpha;
    quasar->seen = bloom_filter_create(seen_size, seen_hashes);
    quasar->seen_size = seen_size;
    quasar->seen_hashes = seen_hashes;
    quasar->on_message = NULL;
    quasar->message_ctx = NULL;
    quasar->pool = pool;
    quasar->owns_actor = false;
    if (protocol != NULL) {
        /* The overlay has no actor of its own. Its state belongs to the
         * protocol's actor — the network actor — the way upstream keeps
         * the gossip inside the network actor. One actor for the layer
         * means no worker waiting on another one. */
        quasar->actor = &protocol->actor;
        protocol->quasar = quasar;
    } else {
        /* No network under it: the overlay runs on an actor of its own.
         * A degenerate case — an overlay with no peers to route to — but
         * the overlay's own logic still has to run somewhere. */
        quasar->actor = &quasar->own_actor;
        quasar->owns_actor = true;
        actor_init(&quasar->own_actor, quasar, quasar_dispatch, pool);
    }
    refcounter_init((refcounter_t*)quasar);
    return quasar;
}

/**
 * Releases the overlay's state.
 *
 * The state belongs to the protocol's actor, so the actor releases it —
 * a destroy on the caller's thread would free filters the actor may
 * still be walking.
 */
static void quasar_release_state(quasar_t* quasar) {
    if (quasar == NULL) return;

    attenuated_bloom_filter_destroy(quasar->routing);
    bloom_filter_destroy(quasar->seen);

    // Free all local subscription topic buffers
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL) {
            buffer_destroy(quasar->local_subs.data[i].topic);
        }
    }
    vec_deinit(&quasar->local_subs);
    for (int i = 0; i < quasar->neighbor_filters.length; i++) {
        attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
    }
    vec_deinit(&quasar->neighbor_filters);
    quasar->routing = NULL;
    quasar->seen = NULL;
}

void quasar_destroy(quasar_t* quasar) {
    if (quasar == NULL) return;
    refcounter_dereference((refcounter_t*)quasar);
    if (refcounter_count((refcounter_t*)quasar) == 0) {
        if (quasar->owns_actor) {
            /* The overlay's own actor: actor_destroy waits for it to be
             * idle, so the state is ours to release. */
            quasar_release_state(quasar);
            actor_destroy(&quasar->own_actor);
            free(quasar);
            return;
        }
        /* The state is the network actor's, and so is the shell: the
         * actor releases the state and frees the shell. The request goes
         * out detached — a dispatch must not wait for another actor, and
         * a destroy that waits is a destroy that can deadlock the layer
         * it is tearing down. */
        meridian_quasar_destroy_msg_t* request =
            (meridian_quasar_destroy_msg_t*)
            get_clear_memory(sizeof(meridian_quasar_destroy_msg_t));
        if (request == NULL) {
            /* The request could not be built, so the actor will not be
             * told. The state and the shell leak: bounded, teardown-only,
             * leak-not-UAF. */
            return;
        }
        if (actor_reply_init(&request->reply) != 0) {
            free(request);
            return;
        }
        request->quasar = quasar;
        int trip = actor_request(quasar->actor, MERIDIAN_QUASAR_DESTROY,
                                 &request->reply, MERIDIAN_QUASAR_TIMEOUT_MS);
        if (trip == ACTOR_REQUEST_DETACHED) {
            /* The actor owns the request and the shell. It releases both
             * when it gets to the request. */
            return;
        }
        if (trip == 0) {
            /* The actor released the state and the shell. */
            actor_reply_destroy(&request->reply);
            free(request);
            return;
        }
        if (trip == 1) {
            /* Never queued: the actor is gone and it never saw the
             * request, so the state and the shell are ours to release. */
            actor_reply_destroy(&request->reply);
            free(request);
            quasar_release_state(quasar);
            free(quasar);
            return;
        }
        /* The caller gave up: the actor owns the request, the state and
         * the shell, and it may still be walking the state. */
    }
}

/**
 * Rebuilds the overlay's state with a new configuration.
 *
 * The overlay keeps its identity: the messages already queued for it
 * stay with it. A fresh overlay would have new filters and no
 * subscriptions, and so does this one.
 */
static int quasar_reconfigure_impl(quasar_t* quasar, uint32_t max_hops,
                                   uint32_t alpha, size_t seen_size,
                                   uint32_t seen_hashes) {
    if (quasar == NULL) return -1;

    quasar_release_state(quasar);

    quasar->max_hops = max_hops;
    quasar->alpha = alpha;
    quasar->routing = attenuated_bloom_filter_create(
        max_hops,
        QUASAR_DEFAULT_SIZE,
        QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA,
        QUASAR_DEFAULT_FP_BITS
    );
    quasar->seen = bloom_filter_create(seen_size, seen_hashes);
    quasar->seen_size = seen_size;
    quasar->seen_hashes = seen_hashes;
    vec_init(&quasar->local_subs);
    vec_init(&quasar->neighbor_filters);

    if (quasar->routing == NULL || quasar->seen == NULL) return -1;
    return 0;
}

static void quasar_set_message_callback_impl(quasar_t* quasar, quasar_message_cb_t cb, void* ctx) {
    if (quasar == NULL) return;
    quasar->on_message = cb;
    quasar->message_ctx = ctx;
}

static attenuated_bloom_filter_t* quasar_get_neighbor_filter_impl(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;
    for (int i = 0; i < quasar->neighbor_filters.length; i++) {
        if (quasar->neighbor_filters.data[i].addr == node->addr &&
            quasar->neighbor_filters.data[i].port == node->port) {
            return quasar->neighbor_filters.data[i].filter;
        }
    }
    return NULL;
}

static attenuated_bloom_filter_t* quasar_get_or_create_neighbor_filter_impl(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return NULL;
    attenuated_bloom_filter_t* existing = quasar_get_neighbor_filter_impl(quasar, node);
    if (existing != NULL) {
        return existing;
    }
    quasar_neighbor_filter_t nf;
    nf.addr = node->addr;
    nf.port = node->port;
    nf.filter = attenuated_bloom_filter_create(
        quasar->max_hops,
        QUASAR_DEFAULT_SIZE,
        QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA,
        QUASAR_DEFAULT_FP_BITS
    );
    if (nf.filter == NULL) {
        return NULL;
    }
    vec_push(&quasar->neighbor_filters, nf);
    return nf.filter;
}

static int quasar_remove_neighbor_filter_impl(quasar_t* quasar, const meridian_node_t* node) {
    if (quasar == NULL || node == NULL) return -1;
    for (int i = 0; i < quasar->neighbor_filters.length; i++) {
        if (quasar->neighbor_filters.data[i].addr == node->addr &&
            quasar->neighbor_filters.data[i].port == node->port) {
            attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
            vec_splice(&quasar->neighbor_filters, i, 1);
            return 0;
        }
    }
    return -1;
}

// ============================================================================
// SUBSCRIPTION MANAGEMENT
// ============================================================================

static int quasar_subscribe_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len, uint32_t ttl) {
    if (quasar == NULL || topic == NULL) return -1;

    // Add topic to level 0 of the routing filter (local subscription)
    int result = attenuated_bloom_filter_subscribe(quasar->routing, topic, topic_len);
    if (result == 0) {
        // Record the subscription locally with its TTL
        quasar_subscription_t sub;
        sub.topic = buffer_create_from_pointer_copy((uint8_t*)topic, topic_len);
        sub.ttl = ttl;
        vec_push(&quasar->local_subs, sub);

        // Algorithm 1, line 4: Insert own node ID at level 0 for negative information
        if (quasar->protocol != NULL) {
            uint32_t addr = 0;
            uint16_t port = 0;
            meridian_protocol_get_local_node(quasar->protocol, &addr, &port);
            if (addr != 0 || port != 0) {
                uint8_t node_key[6];
                memcpy(node_key, &addr, sizeof(addr));
                memcpy(node_key + sizeof(addr), &port, sizeof(port));
                elastic_bloom_filter_t* level0 = attenuated_bloom_filter_get_level(quasar->routing, 0);
                if (level0 != NULL) {
                    elastic_bloom_filter_add(level0, node_key, sizeof(node_key));
                }
            }
        }
    }

    return result;
}

static int quasar_unsubscribe_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len) {
    if (quasar == NULL || topic == NULL) return -1;

    // Remove topic from level 0 of the routing filter
    int result = attenuated_bloom_filter_unsubscribe(quasar->routing, topic, topic_len);

    // Find and remove the matching local subscription entry
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == topic_len &&
            memcmp(quasar->local_subs.data[i].topic->data, topic, topic_len) == 0) {
            buffer_destroy(quasar->local_subs.data[i].topic);
            quasar->local_subs.data[i].topic = NULL;
            vec_splice(&quasar->local_subs, i, 1);
            break;
        }
    }

    return result;
}

// ============================================================================
// PUBLISHING
// ============================================================================

static int quasar_publish_impl(quasar_t* quasar, const uint8_t* topic, size_t topic_len,
                   const uint8_t* data, size_t data_len) {
    if (quasar == NULL || topic == NULL) return -1;

    // Check if locally subscribed
    bool locally_subscribed = false;
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == topic_len &&
            memcmp(quasar->local_subs.data[i].topic->data, topic, topic_len) == 0) {
            locally_subscribed = true;
            break;
        }
    }

    if (locally_subscribed) {
        // Algorithm 2, lines 4-8: deliver locally, add self to PUB, re-publish to all neighbors

        if (quasar->on_message != NULL) {
            quasar_message_cb_t cb = quasar->on_message;
            void* ctx = quasar->message_ctx;
            cb(ctx, topic, topic_len, data, data_len);
        }

        // Re-publish to all overlay neighbors
        if (quasar->protocol != NULL) {
            uint32_t local_addr = 0;
            uint16_t local_port = 0;
            meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);

            size_t num_peers = 0;
            meridian_node_t** peers = meridian_protocol_peers_on_actor(quasar->protocol, &num_peers);
            for (size_t i = 0; peers != NULL && i < num_peers; i++) {
                quasar_route_message_t* republish_msg = quasar_route_message_create(
                    topic, topic_len, data, data_len,
                    quasar->max_hops, QUASAR_DEFAULT_NEGATIVE_SIZE, QUASAR_DEFAULT_NEGATIVE_HASHES
                );
                if (republish_msg != NULL) {
                    bloom_filter_add(quasar->seen, (const uint8_t*)&republish_msg->id, sizeof(republish_msg->id));
                    meridian_node_t* local = meridian_node_create_unidentified(local_addr, local_port);
                    if (local != NULL) {
                        quasar_route_message_add_publisher(republish_msg, local);
                        quasar_route_message_add_visited(republish_msg, local);
                        meridian_node_destroy(local);
                    }
                    uint8_t* route_buf = NULL;
                    size_t route_buf_len = 0;
                    if (quasar_route_message_serialize(republish_msg, &route_buf, &route_buf_len) == 0) {
                        meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, peers[i]);
                        free(route_buf);
                    }
                    quasar_route_message_destroy(republish_msg);
                }
            }
            if (peers != NULL) free(peers);
        }
        return 0;
    }

    // Algorithm 2, lines 10-19: Directed walk
    if (quasar->protocol != NULL) {

        uint32_t local_addr = 0;
        uint16_t local_port = 0;
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
        uint8_t local_key[6];
        memcpy(local_key, &local_addr, sizeof(uint32_t));
        memcpy(local_key + sizeof(uint32_t), &local_port, sizeof(uint16_t));

        uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);
        for (uint32_t level = 0; level < level_count; level++) {
            for (int ni = 0; ni < quasar->neighbor_filters.length; ni++) {
                quasar_neighbor_filter_t* nf = &quasar->neighbor_filters.data[ni];
                if (nf->filter == NULL) continue;
                elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(nf->filter, level);
                if (ebf == NULL) continue;

                if (!elastic_bloom_filter_contains(ebf, topic, topic_len)) continue;

                // Topic found at level L in neighbor O's filter — check negative info
                bool negated = false;
                if (local_addr != 0 || local_port != 0) {
                    if (elastic_bloom_filter_contains(ebf, local_key, sizeof(local_key))) {
                        negated = true;
                    }
                }

                if (!negated) {
                    // Directed walk: forward to this neighbor
                    meridian_node_t* target = meridian_node_create_unidentified(nf->addr, nf->port);
                    quasar_route_message_t* fwd = quasar_route_message_create(
                        topic, topic_len, data, data_len,
                        quasar->max_hops, QUASAR_DEFAULT_NEGATIVE_SIZE, QUASAR_DEFAULT_NEGATIVE_HASHES
                    );
                    if (fwd != NULL) {
                        bloom_filter_add(quasar->seen, (const uint8_t*)&fwd->id, sizeof(fwd->id));
                        meridian_node_t* local = meridian_node_create_unidentified(local_addr, local_port);
                        if (local != NULL) {
                            quasar_route_message_add_publisher(fwd, local);
                            quasar_route_message_add_visited(fwd, local);
                            meridian_node_destroy(local);
                        }
                        uint8_t* route_buf = NULL;
                        size_t route_buf_len = 0;
                        if (quasar_route_message_serialize(fwd, &route_buf, &route_buf_len) == 0) {
                            meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, target);
                            free(route_buf);
                        }
                        quasar_route_message_destroy(fwd);
                    }
                    meridian_node_destroy(target);
                    return 0;
                }
            }
        }
    }

    // Algorithm 2, lines 20-21: Random walk
    quasar_route_message_t* msg = quasar_route_message_create(
        topic, topic_len, data, data_len,
        quasar->max_hops,
        QUASAR_DEFAULT_NEGATIVE_SIZE,
        QUASAR_DEFAULT_NEGATIVE_HASHES
    );
    if (msg == NULL) return -1;

    bloom_filter_add(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id));

    // Add local node as publisher and to visited filter
    if (quasar->protocol != NULL) {
        uint32_t local_addr = 0;
        uint16_t local_port = 0;
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
        meridian_node_t* local_node = meridian_node_create_unidentified(local_addr, local_port);
        if (local_node != NULL) {
            quasar_route_message_add_visited(msg, local_node);
            quasar_route_message_add_publisher(msg, local_node);
            meridian_node_destroy(local_node);
        }
    }

    // Forward to alpha random unvisited neighbors
    int result = 0;
    if (quasar->protocol != NULL) {
        size_t num_peers = 0;
        meridian_node_t** peers = meridian_protocol_peers_on_actor(quasar->protocol, &num_peers);
        uint32_t sent = 0;
        for (size_t i = 0; peers != NULL && i < num_peers && sent < quasar->alpha; i++) {
            if (!quasar_route_message_has_visited(msg, peers[i])) {
                uint8_t* route_buf = NULL;
                size_t route_buf_len = 0;
                if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                    meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, peers[i]);
                    free(route_buf);
                }
                quasar_route_message_add_visited(msg, peers[i]);
                sent++;
            }
        }
        if (peers != NULL) free(peers);
    }

    quasar_route_message_destroy(msg);
    return result;
}

// ============================================================================
// ROUTED MESSAGE HANDLING
// ============================================================================

static int quasar_on_route_message_impl(quasar_t* quasar, quasar_route_message_t* msg, const struct meridian_node_t* from) {
    if (quasar == NULL || msg == NULL) return -1;

    // Algorithm 2, line 1: Dedup check
    if (bloom_filter_contains(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id))) {
        return 0;
    }
    bloom_filter_add(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id));

    // Get local node info
    uint32_t local_addr = 0;
    uint16_t local_port = 0;
    if (quasar->protocol != NULL) {
        meridian_protocol_get_local_node(quasar->protocol, &local_addr, &local_port);
    }
    meridian_node_t* local_node = meridian_node_create_unidentified(local_addr, local_port);
    if (local_node != NULL) {
        quasar_route_message_add_visited(msg, local_node);
    }

    // Algorithm 2, lines 3-8: Check if locally subscribed
    bool locally_subscribed = false;
    for (int i = 0; i < quasar->local_subs.length; i++) {
        if (quasar->local_subs.data[i].topic != NULL &&
            quasar->local_subs.data[i].topic->size == msg->topic->size &&
            memcmp(quasar->local_subs.data[i].topic->data, msg->topic->data, msg->topic->size) == 0) {
            locally_subscribed = true;
            break;
        }
    }

    if (locally_subscribed) {
        // Algorithm 2, line 8: append own node ID to PUB
        if (local_node != NULL) {
            quasar_route_message_add_publisher(msg, local_node);
        }

        if (quasar->on_message != NULL) {
            quasar_message_cb_t cb = quasar->on_message;
            void* ctx = quasar->message_ctx;
            cb(ctx, msg->topic->data, msg->topic->size, msg->data->data, msg->data->size);
        }

        // Re-publish to all overlay neighbors
        if (quasar->protocol != NULL) {
            size_t num_peers = 0;
            meridian_node_t** peers = meridian_protocol_peers_on_actor(quasar->protocol, &num_peers);
            for (size_t i = 0; peers != NULL && i < num_peers; i++) {
                uint8_t* route_buf = NULL;
                size_t route_buf_len = 0;
                if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                    meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, peers[i]);
                    free(route_buf);
                }
            }
            if (peers != NULL) free(peers);
        }

        if (local_node != NULL) meridian_node_destroy(local_node);
        return 0;
    }


    // Algorithm 2, line 9: Check TTL
    if (msg->hops_remaining == 0) {
        if (local_node != NULL) meridian_node_destroy(local_node);
        return 0;
    }
    msg->hops_remaining--;

    // Algorithm 2, lines 10-19: Directed walk
    if (quasar != NULL) {
        uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);

        for (uint32_t level = 0; level < level_count; level++) {
            for (int ni = 0; ni < quasar->neighbor_filters.length; ni++) {
                quasar_neighbor_filter_t* nf = &quasar->neighbor_filters.data[ni];
                if (nf->filter == NULL) continue;
                elastic_bloom_filter_t* ebf = attenuated_bloom_filter_get_level(nf->filter, level);
                if (ebf == NULL) continue;

                if (!elastic_bloom_filter_contains(ebf, msg->topic->data, msg->topic->size)) continue;

                // Check negative information: is any publisher at this same level?
                bool negated = false;
                for (uint32_t pi = 0; pi < msg->pub_count; pi++) {
                    uint8_t pub_key[6];
                    memcpy(pub_key, &msg->pub_addrs[pi], sizeof(uint32_t));
                    memcpy(pub_key + sizeof(uint32_t), &msg->pub_ports[pi], sizeof(uint16_t));
                    if (elastic_bloom_filter_contains(ebf, pub_key, sizeof(pub_key))) {
                        negated = true;
                        break;
                    }
                }

                if (!negated) {
                    // Directed walk: forward the existing route message to this neighbor
                    meridian_node_t* target = meridian_node_create_unidentified(nf->addr, nf->port);
                    uint8_t* route_buf = NULL;
                    size_t route_buf_len = 0;
                    if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                        meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, target);
                        free(route_buf);
                    }
                    meridian_node_destroy(target);
                    if (local_node != NULL) meridian_node_destroy(local_node);
                    return 0;
                }
            }
        }
    }

    // Algorithm 2, lines 20-21: Random walk
    if (quasar->protocol == NULL) {
        if (local_node != NULL) meridian_node_destroy(local_node);
        return 0;
    }

    size_t num_peers = 0;
    meridian_node_t** peers = meridian_protocol_peers_on_actor(quasar->protocol, &num_peers);
    uint32_t sent = 0;
    for (size_t i = 0; peers != NULL && i < num_peers && sent < quasar->alpha; i++) {
        if (!quasar_route_message_has_visited(msg, peers[i])) {
            uint8_t* route_buf = NULL;
            size_t route_buf_len = 0;
            if (quasar_route_message_serialize(msg, &route_buf, &route_buf_len) == 0) {
                meridian_protocol_send_on_actor(quasar->protocol, route_buf, route_buf_len, peers[i]);
                free(route_buf);
            }
            quasar_route_message_add_visited(msg, peers[i]);
            sent++;
        }
    }
    if (peers != NULL) free(peers);

    if (local_node != NULL) meridian_node_destroy(local_node);
    return 0;
}

// ============================================================================
// GOSSIP AND PROPAGATION
// ============================================================================

static int quasar_on_gossip_impl(quasar_t* quasar, const uint8_t* data, size_t len, const struct meridian_node_t* from) {
    if (quasar == NULL || data == NULL || len < 7 * sizeof(uint32_t)) return -1;

    struct cbor_load_result result;
    cbor_item_t* root = cbor_load(data, len, &result);
    if (root == NULL || result.error.code != CBOR_ERR_NONE) {
        if (root != NULL) cbor_decref(&root);
        return -1;
    }

    // Validate: must be a definite array with at least 3 elements
    if (!cbor_isa_array(root) || !cbor_array_is_definite(root) ||
        cbor_array_size(root) < 3) {
        cbor_decref(&root);
        return -1;
    }

    cbor_item_t** items = cbor_array_handle(root);

    // items[0]: packet type (must be QUASAR_GOSSIP)
    if (!cbor_isa_uint(items[0])) {
        cbor_decref(&root);
        return -1;
    }
    uint8_t type = cbor_get_uint8(items[0]);
    if (type != MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP) {
        cbor_decref(&root);
        return -1;
    }

    // items[1]: level_count (validated for structure)
    if (!cbor_isa_uint(items[1])) {
        cbor_decref(&root);
        return -1;
    }
    uint32_t level_count = cbor_get_uint32(items[1]);

    // items[2]: encoded attenuated bloom filter
    attenuated_bloom_filter_t* decoded = attenuated_bloom_filter_decode(items[2]);
    if (decoded == NULL || attenuated_bloom_filter_level_count(decoded) != level_count) {
        if (decoded != NULL) attenuated_bloom_filter_destroy(decoded);
        cbor_decref(&root);
        return -1;
    }

    // Store the incoming filter in the per-neighbor slot for directed routing (Algorithm 2)
    if (from != NULL) {
        // Remove old filter for this neighbor if it exists
        for (int i = 0; i < quasar->neighbor_filters.length; i++) {
            if (quasar->neighbor_filters.data[i].addr == from->addr &&
                quasar->neighbor_filters.data[i].port == from->port) {
                attenuated_bloom_filter_destroy(quasar->neighbor_filters.data[i].filter);
                vec_splice(&quasar->neighbor_filters, i, 1);
                break;
            }
        }
        // Transfer ownership of decoded to the neighbor filter slot
        quasar_neighbor_filter_t nf;
        nf.addr = from->addr;
        nf.port = from->port;
        nf.filter = decoded;
        vec_push(&quasar->neighbor_filters, nf);
        // Also merge into the aggregated routing filter for local checks
        attenuated_bloom_filter_merge(quasar->routing, decoded);
    } else {
        // No sender info — just merge into routing and destroy
        int result = attenuated_bloom_filter_merge(quasar->routing, decoded);
        attenuated_bloom_filter_destroy(decoded);
        cbor_decref(&root);
        return result;
    }

    cbor_decref(&root);
    return 0;
}

static int quasar_propagate_impl(quasar_t* quasar) {
    if (quasar == NULL) return -1;
    if (quasar->protocol == NULL) return -1;


    // Encode the attenuated bloom filter to CBOR
    cbor_item_t* encoded_filter = attenuated_bloom_filter_encode(quasar->routing);
    if (encoded_filter == NULL) {
        return -1;
    }

    // Read level_count under lock since it reads quasar->routing
    uint32_t level_count = attenuated_bloom_filter_level_count(quasar->routing);


    // Build the QUASAR_GOSSIP packet: [type, level_count, encoded_filter]
    cbor_item_t* packet = cbor_new_definite_array(3);
    if (packet == NULL) {
        cbor_decref(&encoded_filter);
        return -1;
    }

    cbor_item_t* type_item = cbor_build_uint8(MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP);
    cbor_item_t* level_item = cbor_build_uint32(level_count);
    if (type_item == NULL || level_item == NULL) {
        if (type_item != NULL) cbor_decref(&type_item);
        if (level_item != NULL) cbor_decref(&level_item);
        cbor_decref(&packet);
        cbor_decref(&encoded_filter);
        return -1;
    }

    bool ok = cbor_array_push(packet, type_item) &&
              cbor_array_push(packet, level_item) &&
              cbor_array_push(packet, encoded_filter);
    if (!ok) {
        cbor_decref(&type_item);
        cbor_decref(&level_item);
        cbor_decref(&packet);
        cbor_decref(&encoded_filter);
        return -1;
    }

    // cbor_array_push increfs each item, so we can decref our local refs now
    cbor_decref(&type_item);
    cbor_decref(&level_item);
    cbor_decref(&encoded_filter);

    // Serialize to bytes
    unsigned char* buf = NULL;
    size_t buf_size = 0;
    cbor_serialize_alloc(packet, &buf, &buf_size);
    if (buf == NULL || buf_size == 0) {
        cbor_decref(&packet);
        return -1;
    }

    // Broadcast to all connected peers
    int rc = meridian_protocol_broadcast_on_actor(quasar->protocol, buf, buf_size);

    free(buf);
    cbor_decref(&packet);
    return rc == 0 ? 0 : -1;
}

// ============================================================================
// PERIODIC MAINTENANCE
// ============================================================================

static int quasar_tick_impl(quasar_t* quasar) {
    if (quasar == NULL) return -1;

    // Walk through all local subscriptions and decrement their TTL.
    // Expired subscriptions are removed from both the local list and the
    // routing filter (level 0), so neighbors will eventually learn via
    // the next filter propagation cycle.
    int i = 0;
    while (i < quasar->local_subs.length) {
        if (quasar->local_subs.data[i].ttl > 0) {
            quasar->local_subs.data[i].ttl--;

            if (quasar->local_subs.data[i].ttl == 0) {
                // Subscription expired — remove from routing filter and local list
                if (quasar->local_subs.data[i].topic != NULL) {
                    attenuated_bloom_filter_unsubscribe(
                        quasar->routing,
                        quasar->local_subs.data[i].topic->data,
                        quasar->local_subs.data[i].topic->size
                    );
                    buffer_destroy(quasar->local_subs.data[i].topic);
                }
                vec_splice(&quasar->local_subs, i, 1);
                continue; // Don't increment i — next element slid into this position
            }
        }
        i++;
    }

    return 0;
}