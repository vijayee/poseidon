//
// Created by victor on 4/19/26.
//

#include "../../Platform/platform_thread.h"
#include "meridian_protocol.h"
#include "meridian_protocol_messages.h"
#include "Platform/poseidon_msquic.h"
#include "../Quasar/quasar_route.h"
#include "../Quasar/quasar.h"
#include "../../Channel/channel_manager.h"
#include "../../Util/allocator.h"
#include "../../Util/vec.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <cbor.h>

// ============================================================================
// CALLBACK FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API
ListenerCallback(HQUIC Listener, void* Context,
                 QUIC_LISTENER_EVENT* Event);

static QUIC_STATUS QUIC_API
ConnectionCallback(HQUIC Connection, void* Context,
                   QUIC_CONNECTION_EVENT* Event);

static QUIC_STATUS QUIC_API
protocol_uni_stream_callback(HQUIC Stream, void* Context,
                              QUIC_STREAM_EVENT* Event);

static QUIC_STATUS QUIC_API
protocol_bidi_stream_callback(HQUIC Stream, void* Context,
                               QUIC_STREAM_EVENT* Event);

// ============================================================================
// ACTOR DISPATCH
// ============================================================================

/* The actor-thread body of the send path. */
static int meridian_protocol_send_packet_impl(meridian_protocol_t* protocol,
                                               const uint8_t* data, size_t len,
                                               uint32_t target_addr,
                                               uint16_t target_port);

/* The stream table helpers live below; the dispatch calls them. */
static int protocol_add_stream(meridian_protocol_t* protocol,
                                meridian_stream_entry_t* entry);
static void protocol_remove_stream(meridian_protocol_t* protocol,
                                   meridian_stream_entry_t* entry);

/**
 * The peer table, read on the actor's own thread.
 *
 * The overlay's work runs on this actor, so it reads the peer table
 * directly. There is no second actor to ask and no worker to block.
 */
meridian_node_t** meridian_protocol_peers_on_actor(meridian_protocol_t* protocol,
                                                   size_t* num_peers) {
    if (protocol == NULL || num_peers == NULL) return NULL;

    size_t count = protocol->num_connected_peers;
    /* The snapshot is always an array: an empty table yields an empty
     * array, as the raw table did before. */
    void** peers = get_clear_memory((count > 0 ? count : 1) * sizeof(void*));
    if (peers == NULL) {
        count = 0;
    } else {
        for (size_t i = 0; i < count; i++) {
            peers[i] = protocol->peer_nodes[i];
        }
    }
    *num_peers = count;
    return (meridian_node_t**)peers;
}

int meridian_protocol_send_on_actor(meridian_protocol_t* protocol,
                                    const uint8_t* data, size_t len,
                                    const meridian_node_t* target) {
    if (protocol == NULL || data == NULL || target == NULL) return -1;
    return meridian_protocol_send_packet_impl(protocol, data, len,
                                              target->addr, target->port);
}

int meridian_protocol_broadcast_on_actor(meridian_protocol_t* protocol,
                                         const uint8_t* data, size_t len) {
    if (protocol == NULL || data == NULL) return -1;
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        HQUIC connection = protocol->connected_peers[i];
        if (connection != NULL) {
            meridian_stream_send_uni(protocol->msquic, connection, data, len, NULL);
        }
    }
    return 0;
}

/**
 * The protocol's actor owns the peer and stream tables.
 *
 * msquic delivers the connection and the stream events on its own worker
 * threads. Those events are handed to the actor, which mutates the tables
 * on its own thread — the way upstream's network actor owns the peer
 * list. The callbacks themselves do no state work: they build the event
 * and move on.
 */
static void meridian_protocol_dispatch(void* state, message_t* msg) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)state;
    if (msg == NULL) return;

    /* The overlay's messages are this actor's: the overlay has no actor
     * of its own, the way upstream keeps the gossip inside the network
     * actor. */
    if (msg->type >= MERIDIAN_QUASAR_SET_MESSAGE_CB &&
        msg->type <= MERIDIAN_QUASAR_DESTROY) {
        if (protocol->quasar != NULL) {
            quasar_dispatch(protocol->quasar, msg);
        } else {
            /* The overlay is gone. The caller is waiting for an answer
             * that will never come, so give it one now. */
            actor_reply_t* reply = (actor_reply_t*)msg->payload;
            if (reply != NULL) {
                reply->result = -1;
                actor_reply_complete(reply);
            }
        }
        return;
    }

    switch (msg->type) {
        case MERIDIAN_PROTOCOL_PEER_ADDED: {
            meridian_protocol_peer_added_msg_t* p =
                (meridian_protocol_peer_added_msg_t*)msg->payload;
            if (protocol->num_connected_peers < 64) {
                protocol->connected_peers[protocol->num_connected_peers] =
                    (HQUIC)p->connection;
                protocol->peer_nodes[protocol->num_connected_peers] =
                    (meridian_node_t*)p->node;
                protocol->num_connected_peers++;
            }
            if (actor_reply_complete(&p->reply)) {
                /* The caller handed the request over: the actor frees it. */
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_PEER_REMOVED: {
            meridian_protocol_peer_removed_msg_t* p =
                (meridian_protocol_peer_removed_msg_t*)msg->payload;
            for (size_t i = 0; i < protocol->num_connected_peers; i++) {
                meridian_node_t* node = protocol->peer_nodes[i];
                if (node != NULL && node->addr == p->addr &&
                    node->port == p->port) {
                    HQUIC connection = protocol->connected_peers[i];
                    if (connection != NULL && protocol->msquic != NULL) {
                        protocol->msquic->ConnectionShutdown(
                            connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                        protocol->msquic->ConnectionClose(connection);
                    }
                    protocol->connected_peers[i] = NULL;
                    protocol->peer_nodes[i] = NULL;
                    if (node != NULL) {
                        refcounter_dereference(&node->refcounter);
                        if (refcounter_count(&node->refcounter) == 0) {
                            free(node);
                        }
                    }
                    break;
                }
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_PEERS_SNAPSHOT: {
            meridian_protocol_peers_snapshot_msg_t* p =
                (meridian_protocol_peers_snapshot_msg_t*)msg->payload;
            size_t count = 0;
            void** peers = meridian_protocol_peers_on_actor(protocol, &count);
            p->peers = peers;
            p->num_peers = count;
            if (actor_reply_complete(&p->reply)) {
                if (p->peers != NULL) free(p->peers);
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_SEND_PACKET: {
            meridian_protocol_send_packet_msg_t* p =
                (meridian_protocol_send_packet_msg_t*)msg->payload;
            p->reply.result = meridian_protocol_send_packet_impl(
                protocol, p->data, p->len, p->target_addr, p->target_port);
            free(p->data);
            p->data = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_SEND_MEASURE: {
            meridian_protocol_send_measure_msg_t* p =
                (meridian_protocol_send_measure_msg_t*)msg->payload;
            HQUIC connection = NULL;
            for (size_t i = 0; i < protocol->num_connected_peers; i++) {
                meridian_node_t* node = protocol->peer_nodes[i];
                if (node != NULL && node->addr == p->target_addr &&
                    node->port == p->target_port) {
                    connection = protocol->connected_peers[i];
                    break;
                }
            }
            if (connection != NULL) {
                /* Build a PING packet with the request's query_id */
                meridian_packet_t pkt;
                pkt.type = MERIDIAN_PACKET_TYPE_PING;
                pkt.query_id = p->query_id;
                pkt.magic = MERIDIAN_MAGIC_NUMBER;
                pkt.rendv_addr = 0;
                pkt.rendv_port = 0;

                cbor_item_t* encoded = meridian_packet_encode(&pkt);
                if (encoded != NULL) {
                    size_t buf_len = 0;
                    uint8_t* buf = NULL;
                    cbor_serialize_alloc(encoded, &buf, &buf_len);
                    cbor_decref(&encoded);

                    if (buf != NULL) {
                        meridian_stream_entry_t* entry =
                            meridian_stream_entry_create(NULL, true, protocol,
                                                        protocol->sched);
                        if (entry != NULL) {
                            HQUIC stream = NULL;
                            QUIC_STATUS status = protocol->msquic->StreamOpen(
                                connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                protocol_bidi_stream_callback, entry, &stream);
                            if (!QUIC_FAILED(status)) {
                                entry->stream = stream;
                                status = protocol->msquic->StreamStart(
                                    stream, QUIC_STREAM_START_FLAG_NONE);
                                if (!QUIC_FAILED(status)) {
                                    if (meridian_stream_send(
                                            protocol->msquic, stream, buf,
                                            buf_len, QUIC_SEND_FLAG_FIN) == 0) {
                                        free(buf);
                                        protocol_add_stream(protocol, entry);
                                        goto measure_done;
                                    }
                                }
                                protocol->msquic->StreamClose(stream);
                            }
                            meridian_stream_entry_destroy(entry);
                        }
                        free(buf);
                    }
                }
            }
        measure_done:
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_MEASURE_RESPONSE: {
            meridian_protocol_measure_response_msg_t* p =
                (meridian_protocol_measure_response_msg_t*)msg->payload;
            for (size_t i = 0; i < protocol->num_pending_measures; i++) {
                meridian_measure_request_t* req = protocol->pending_measures[i];
                if (req != NULL && req->query_id == p->query_id) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    struct timeval elapsed;
                    timersub(&now, &req->start_time, &elapsed);
                    uint32_t rtt_us = (uint32_t)(elapsed.tv_sec * 1000000 +
                                                 elapsed.tv_usec);

                    meridian_measure_result_t meas_result;
                    meas_result.node = req->target;
                    meas_result.latency_us = rtt_us;
                    meas_result.success = true;

                    meridian_measure_callback_t cb = req->callback;
                    void* ctx = req->ctx;
                    meridian_node_t* node = req->target;
                    if (node) {
                        refcounter_reference(&node->refcounter);
                    }

                    protocol->pending_measures[i] =
                        protocol->pending_measures[protocol->num_pending_measures - 1];
                    protocol->pending_measures[protocol->num_pending_measures - 1] = NULL;
                    protocol->num_pending_measures--;

                    if (node) {
                        meridian_latency_cache_insert(protocol->latency_cache,
                                                      node, rtt_us);
                    }
                    if (cb) {
                        cb(ctx, &meas_result);
                    }

                    meridian_measure_request_destroy(req);
                    if (node) {
                        meridian_node_destroy(node);
                    }
                    break;
                }
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_MEASURE_RESULT: {
            meridian_protocol_measure_result_msg_t* p =
                (meridian_protocol_measure_result_msg_t*)msg->payload;
            if (p->success && p->node != NULL) {
                meridian_latency_cache_insert(protocol->latency_cache,
                                              (meridian_node_t*)p->node,
                                              p->latency_us);
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_FIND_CLOSEST: {
            meridian_protocol_find_closest_msg_t* p =
                (meridian_protocol_find_closest_msg_t*)msg->payload;
            p->out_node = meridian_ring_set_find_closest(protocol->ring_set,
                                                          p->target_addr,
                                                          p->target_port);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_GOSSIP_TICK: {
            meridian_protocol_gossip_tick_msg_t* p =
                (meridian_protocol_gossip_tick_msg_t*)msg->payload;
            bool should_gossip = false;
            if (protocol->gossip_handle != NULL &&
                protocol->gossip_handle->scheduler != NULL) {
                meridian_gossip_scheduler_tick(protocol->gossip_handle->scheduler,
                                               &should_gossip);
            }
            if (should_gossip && protocol->num_connected_peers > 0) {
                meridian_gossip_handle_send_gossip(protocol->gossip_handle,
                                                   protocol->peer_nodes,
                                                   protocol->num_connected_peers);
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_GOSSIP_RESPONSE: {
            meridian_protocol_gossip_response_msg_t* p =
                (meridian_protocol_gossip_response_msg_t*)msg->payload;
            if (protocol->gossip_handle != NULL) {
                meridian_gossip_handle_on_packet(protocol->gossip_handle,
                                                 p->data, p->len, p->from);
            }
            free(p->data);
            p->data = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_PROTOCOL_BROADCAST: {
            meridian_protocol_broadcast_msg_t* p =
                (meridian_protocol_broadcast_msg_t*)msg->payload;
            for (size_t i = 0; i < protocol->num_connected_peers; i++) {
                HQUIC connection = protocol->connected_peers[i];
                if (connection != NULL) {
                    meridian_stream_send_uni(protocol->msquic, connection,
                                            p->data, p->len, NULL);
                }
            }
            free(p->data);
            p->data = NULL;
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

/* Hands an event to the protocol's actor. The payload is heap-allocated:
   the actor may dispatch it after this call returns. */
static bool protocol_post_event(meridian_protocol_t* protocol, uint32_t type,
                                void* payload, void (*payload_destroy)(void*)) {
    message_t msg = {
        .type = type,
        .payload = payload,
        .payload_destroy = payload_destroy,
    };
    if (!actor_send(&protocol->actor, &msg)) {
        /* The actor is gone: actor_send has already released the payload. */
        return false;
    }
    return true;
}

// ============================================================================
// STREAM TRACKING HELPERS
// ============================================================================

static int protocol_add_stream(meridian_protocol_t* protocol,
                                meridian_stream_entry_t* entry) {
    if (protocol == NULL || entry == NULL) return -1;

    platform_mutex_lock(protocol->lock);
    if (protocol->num_active_streams >= protocol->active_streams_cap) {
        size_t new_cap = protocol->active_streams_cap * 2;
        meridian_stream_entry_t** new_arr = (meridian_stream_entry_t**)
            realloc(protocol->active_streams, new_cap * sizeof(meridian_stream_entry_t*));
        if (new_arr == NULL) {
            platform_mutex_unlock(protocol->lock);
            return -1;
        }
        protocol->active_streams = new_arr;
        protocol->active_streams_cap = new_cap;
    }
    protocol->active_streams[protocol->num_active_streams++] = entry;
    platform_mutex_unlock(protocol->lock);
    return 0;
}

static void protocol_remove_stream(meridian_protocol_t* protocol,
                                    meridian_stream_entry_t* entry) {
    if (protocol == NULL || entry == NULL) return;

    platform_mutex_lock(protocol->lock);
    for (size_t i = 0; i < protocol->num_active_streams; i++) {
        if (protocol->active_streams[i] == entry) {
            protocol->active_streams[i] =
                protocol->active_streams[protocol->num_active_streams - 1];
            protocol->num_active_streams--;
            break;
        }
    }
    platform_mutex_unlock(protocol->lock);
}

// ============================================================================
// STREAM CALLBACKS
// ============================================================================

/* The owner handler for a Meridian stream. */
static void protocol_stream_message(void* ctx, meridian_stream_entry_t* entry,
                                    const uint8_t* data, size_t len);

/**
 * The owner handler for a Meridian stream.
 *
 * It runs on the stream entry's actor, not on msquic's worker: the
 * receive buffer is the actor's, and the packet callback belongs with it.
 */
static void protocol_stream_message(void* ctx, meridian_stream_entry_t* entry,
                                    const uint8_t* data, size_t len) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)ctx;
    if (protocol == NULL) return;

    if (protocol->callbacks.on_packet) {
        protocol->callbacks.on_packet(protocol->callbacks.user_ctx, protocol,
                                      data, len);
    }

    /* The bidi stream stays open for our answer; the send side is shut
     * down once the peer's message has been delivered. */
    if (entry != NULL && entry->is_bidi && entry->stream != NULL &&
        protocol->msquic != NULL) {
        protocol->msquic->StreamShutdown(entry->stream,
                                         QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    }
}



static QUIC_STATUS QUIC_API
protocol_uni_stream_callback(HQUIC Stream, void* Context,
                              QUIC_STREAM_EVENT* Event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)Context;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Outbound uni streams (sender side) have context=NULL
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                break;
            }
        }
        /* The bytes are copied into the entry's buffer, so all of them
         * are consumed. msquic reads the length on the way out, and a
         * zero would say that nothing was taken: the data would never
         * be acknowledged and the peer's FIN would never arrive. Left
         * unchanged it says "all of it"
         * (deps/msquic/docs/Streams.md). */
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        if (entry != NULL && entry->user_context != NULL) {
            meridian_protocol_t* proto = (meridian_protocol_t*)entry->user_context;
            if (proto->callbacks.on_packet) {
                proto->callbacks.on_packet(proto->callbacks.user_ctx, proto,
                                            entry->recv_buf, entry->recv_len);
            }
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // The send context holds the bytes and the buffer that pointed
        // at them; both go back here.
        meridian_stream_send_complete(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_protocol_t* proto = (entry != NULL && entry->user_context != NULL)
            ? (meridian_protocol_t*)entry->user_context : NULL;
        if (proto != NULL) {
            protocol_remove_stream(proto, entry);
        }
        meridian_stream_entry_destroy(entry);
        if (Stream != NULL && proto != NULL) {
            proto->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API
protocol_bidi_stream_callback(HQUIC Stream, void* Context,
                               QUIC_STREAM_EVENT* Event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)Context;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                break;
            }
        }
        /* The bytes are copied into the entry's buffer, so all of them
         * are consumed. msquic reads the length on the way out, and a
         * zero would say that nothing was taken: the data would never
         * be acknowledged and the peer's FIN would never arrive. Left
         * unchanged it says "all of it"
         * (deps/msquic/docs/Streams.md). */
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        if (entry != NULL && entry->user_context != NULL) {
            meridian_protocol_t* proto = (meridian_protocol_t*)entry->user_context;
            if (proto->callbacks.on_packet) {
                proto->callbacks.on_packet(proto->callbacks.user_ctx, proto,
                                            entry->recv_buf, entry->recv_len);
            }
            // Gracefully close our send side after receiving peer's complete message
            if (Stream) {
                proto->msquic->StreamShutdown(Stream,
                    QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
            }
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // The send context holds the bytes and the buffer that pointed
        // at them; both go back here.
        meridian_stream_send_complete(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_protocol_t* proto = (entry != NULL && entry->user_context != NULL)
            ? (meridian_protocol_t*)entry->user_context : NULL;
        if (proto != NULL) {
            protocol_remove_stream(proto, entry);
        }
        meridian_stream_entry_destroy(entry);
        if (Stream != NULL && proto != NULL) {
            proto->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

static void gossip_outbound_cb(void* ctx, const uint8_t* data, size_t len,
                                const meridian_node_t* target) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)ctx;
    meridian_protocol_send_packet(protocol, data, len, target);
}

static void gossip_completed_cb(void* ctx, uint64_t query_id,
                                 meridian_node_t** peers, size_t num_peers) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)ctx;
    (void)query_id;
    for (size_t i = 0; i < num_peers; i++) {
        // Latency unknown for gossip-discovered peers; insert into ring 0
        // until actual measurement updates their placement
        meridian_ring_set_insert(protocol->ring_set, peers[i], 0, NULL);
    }
}

// ============================================================================
// PROTOCOL LIFECYCLE
// ============================================================================

meridian_protocol_t* meridian_protocol_create(const meridian_protocol_config_t* config) {
    if (config == NULL) return NULL;
    /* The protocol's actor needs a scheduler pool: the msquic callbacks
     * hand their events over to it, and only a pool worker runs it. */
    if (config->sched == NULL) return NULL;

    meridian_protocol_t* protocol = (meridian_protocol_t*)
        get_clear_memory(sizeof(meridian_protocol_t));

    protocol->config.listen_port = config->listen_port;
    protocol->config.info_port = config->info_port;
    protocol->config.primary_ring_size = config->primary_ring_size;
    protocol->config.secondary_ring_size = config->secondary_ring_size;
    protocol->config.ring_exponent_base = config->ring_exponent_base;
    protocol->config.init_gossip_interval_s = config->init_gossip_interval_s;
    protocol->config.num_init_gossip_intervals = config->num_init_gossip_intervals;
    protocol->config.steady_state_gossip_interval_s = config->steady_state_gossip_interval_s;
    protocol->config.replace_interval_s = config->replace_interval_s;
    protocol->config.gossip_timeout_ms = config->gossip_timeout_ms;
    protocol->config.measure_timeout_ms = config->measure_timeout_ms;
    /* The TLS paths belong to the protocol now: the caller usually hands
     * over stack locals, and the credential is loaded later, in the
     * start. */
    protocol->tls_key_path = NULL;
    protocol->tls_cert_path = NULL;
    if (config->tls_key_path != NULL) {
        protocol->tls_key_path = strdup(config->tls_key_path);
    }
    if (config->tls_cert_path != NULL) {
        protocol->tls_cert_path = strdup(config->tls_cert_path);
    }
    protocol->config.tls_key_path = protocol->tls_key_path;
    protocol->config.tls_cert_path = protocol->tls_cert_path;
    memcpy(&protocol->config.local_node_id, &config->local_node_id, sizeof(poseidon_node_id_t));
    protocol->config.pool = config->pool;
    protocol->config.wheel = config->wheel;
    protocol->config.sched = config->sched;

    protocol->state = MERIDIAN_PROTOCOL_STATE_INIT;
    protocol->sched = config->sched;
    protocol->msquic = NULL;
    protocol->registration = NULL;
    protocol->listener = NULL;
    protocol->configuration = NULL;
    ATOMIC_STORE(&protocol->running, 0);

    protocol->ring_set = meridian_ring_set_create(
        config->primary_ring_size,
        config->secondary_ring_size,
        config->ring_exponent_base
    );

    protocol->rendv_handle = meridian_rendv_handle_create(protocol->sched);
    protocol->latency_cache = meridian_latency_cache_create(MERIDIAN_PROBE_CACHE_SIZE);

    protocol->pending_measures = NULL;
    protocol->num_pending_measures = 0;
    protocol->pending_measures_capacity = 0;

    protocol->num_seed_nodes = 0;
    protocol->num_connected_peers = 0;

    // Initialize NAT traversal fields
    protocol->connections = NULL;
    protocol->num_connections = 0;
    protocol->default_relay = NULL;

    // Stream tracking for per-message QUIC streams
    protocol->active_streams_cap = 16;
    protocol->active_streams = get_clear_memory(
        protocol->active_streams_cap * sizeof(meridian_stream_entry_t*));
    protocol->num_active_streams = 0;

    protocol->lock = platform_mutex_create();;
    refcounter_init(&protocol->refcounter);

    /* The actor owns the peer table. The msquic callbacks hand their
     * events over to it, the way upstream's network actor owns the peer
     * list. */
    actor_init(&protocol->actor, protocol, meridian_protocol_dispatch,
               protocol->sched);

    return protocol;
}

void meridian_protocol_destroy(meridian_protocol_t* protocol) {
    if (protocol == NULL) return;

    refcounter_dereference(&protocol->refcounter);
    if (refcounter_count(&protocol->refcounter) == 0) {
        /* The actor must be gone before the tables are read: its destroy
         * waits for a dispatch in flight. */
        actor_destroy(&protocol->actor);

        if (ATOMIC_LOAD(&protocol->running)) {
            meridian_protocol_stop(protocol);
        }

        // Safety: close any remaining connections not cleaned up by stop
        if (protocol->msquic) {
            for (size_t i = 0; i < protocol->num_connected_peers; i++) {
                if (protocol->connected_peers[i]) {
                    protocol->msquic->ConnectionShutdown(
                        protocol->connected_peers[i],
                        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                    protocol->msquic->ConnectionClose(protocol->connected_peers[i]);
                    protocol->connected_peers[i] = NULL;
                }
                if (protocol->peer_nodes[i]) {
                    refcounter_dereference(&protocol->peer_nodes[i]->refcounter);
                    if (refcounter_count(&protocol->peer_nodes[i]->refcounter) == 0) {
                        free(protocol->peer_nodes[i]);
                    }
                }
            }
            protocol->num_connected_peers = 0;
        }

        if (protocol->configuration) {
            protocol->msquic->ConfigurationClose(protocol->configuration);
            protocol->configuration = NULL;
        }

        if (protocol->registration) {
            poseidon_msquic_close_registration("meridian");
            protocol->registration = NULL;
        }

        if (protocol->msquic) {
            poseidon_msquic_close();
            protocol->msquic = NULL;
        }

        /* The TLS path copies are the protocol's. */
        if (protocol->tls_key_path != NULL) {
            free(protocol->tls_key_path);
            protocol->tls_key_path = NULL;
        }
        if (protocol->tls_cert_path != NULL) {
            free(protocol->tls_cert_path);
            protocol->tls_cert_path = NULL;
        }
        protocol->config.tls_key_path = NULL;
        protocol->config.tls_cert_path = NULL;

        if (protocol->ring_set) {
            meridian_ring_set_destroy(protocol->ring_set);
        }

        if (protocol->rendv_handle) {
            meridian_rendv_handle_destroy(protocol->rendv_handle);
        }

        if (protocol->latency_cache) {
            meridian_latency_cache_destroy(protocol->latency_cache);
        }

        for (size_t i = 0; i < protocol->num_pending_measures; i++) {
            meridian_measure_request_destroy(protocol->pending_measures[i]);
        }
        free(protocol->pending_measures);

        for (size_t i = 0; i < protocol->num_seed_nodes; i++) {
            if (protocol->seed_nodes[i]) {
                refcounter_dereference(&protocol->seed_nodes[i]->refcounter);
                if (refcounter_count(&protocol->seed_nodes[i]->refcounter) == 0) {
                    free(protocol->seed_nodes[i]);
                }
            }
        }

        if (protocol->gossip_handle) {
            meridian_gossip_handle_destroy(protocol->gossip_handle);
            protocol->gossip_handle = NULL;
        }

        for (size_t i = 0; i < protocol->num_active_streams; i++) {
            meridian_stream_entry_destroy(protocol->active_streams[i]);
        }
        free(protocol->active_streams);

        platform_mutex_destroy(protocol->lock);;
        free(protocol);
    }
}

int meridian_protocol_start(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_mutex_lock(protocol->lock);

    if (ATOMIC_LOAD(&protocol->running)) {
        platform_mutex_unlock(protocol->lock);
        return 0;
    }

    // Open msquic library (process-wide singleton, reference-counted)
    QUIC_STATUS Status;
    protocol->msquic = poseidon_msquic_open();
    if (protocol->msquic == NULL) {
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Create registration (keyed refcount shared with other meridian components)
    if (!poseidon_msquic_open_registration("meridian", &protocol->registration)) {
        poseidon_msquic_close();
        protocol->msquic = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Configure ALPN
    QUIC_BUFFER Alpn = { sizeof("meridian") - 1, (uint8_t*)"meridian" };

    // Configure settings for per-message streams
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = 30000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.PeerBidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    Settings.PeerUnidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
    Settings.IsSet.PeerUnidiStreamCount = TRUE;

    if (QUIC_FAILED(Status = protocol->msquic->ConfigurationOpen(
            protocol->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &protocol->configuration))) {
        poseidon_msquic_close_registration("meridian");
        protocol->msquic = NULL;
        protocol->registration = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Load credentials
    QUIC_CREDENTIAL_CONFIG CredConfig = {0};
    QUIC_CERTIFICATE_FILE CertFile = {0};
    if (protocol->tls_key_path != NULL && protocol->tls_cert_path != NULL) {
        CertFile.PrivateKeyFile = protocol->tls_key_path;
        CertFile.CertificateFile = protocol->tls_cert_path;
        CredConfig.CertificateFile = &CertFile;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    } else {
        CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    }

    if (QUIC_FAILED(Status = protocol->msquic->ConfigurationLoadCredential(
            protocol->configuration,
            &CredConfig))) {
        protocol->msquic->ConfigurationClose(protocol->configuration);
        poseidon_msquic_close_registration("meridian");
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Create listener
    if (QUIC_FAILED(Status = protocol->msquic->ListenerOpen(
            protocol->registration,
            ListenerCallback,
            protocol,
            &protocol->listener))) {
        protocol->msquic->ConfigurationClose(protocol->configuration);
        poseidon_msquic_close_registration("meridian");
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Set listener callback
    protocol->msquic->SetCallbackHandler(protocol->listener, ListenerCallback, protocol);

    // Start listener on configured port
    QUIC_ADDR Address = {0};
#ifdef QUIC_ADDRESS_FAMILY_UNSPEC
    Address.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET;
#else
    Address.Ipv4.sin_family = AF_INET;
#endif
    Address.Ipv4.sin_port = htons(protocol->config.listen_port);

    if (QUIC_FAILED(Status = protocol->msquic->ListenerStart(
            protocol->listener,
            &Alpn, 1,
            &Address))) {
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->msquic->ConfigurationClose(protocol->configuration);
        poseidon_msquic_close_registration("meridian");
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        protocol->listener = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    // Initialize local address for queries
    memset(&protocol->local_addr, 0, sizeof(protocol->local_addr));
    protocol->local_addr.sin_family = AF_INET;
    protocol->local_addr.sin_addr.s_addr = INADDR_ANY;
    protocol->local_addr.sin_port = htons(protocol->config.listen_port);

    // Initialize gossip
    meridian_gossip_config_t gossip_config = {
        .user_ctx = protocol,
        .outbound_cb = gossip_outbound_cb,
        .completed_cb = gossip_completed_cb,
        .init_interval_s = protocol->config.init_gossip_interval_s,
        .num_init_intervals = protocol->config.num_init_gossip_intervals,
        .steady_state_interval_s = protocol->config.steady_state_gossip_interval_s,
        .timeout_ms = protocol->config.gossip_timeout_ms
    };
    protocol->gossip_handle = meridian_gossip_handle_create(&gossip_config);

    // Set msquic context on rendezvous handle for QUIC tunnel support
    // Pass NULL config - tunnels use registration defaults
    meridian_rendv_handle_set_msquic(protocol->rendv_handle,
                                     protocol->msquic,
                                     protocol->registration,
                                     NULL);

    if (protocol->gossip_handle == NULL) {
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->msquic->ConfigurationClose(protocol->configuration);
        poseidon_msquic_close_registration("meridian");
        protocol->msquic = NULL;
        protocol->registration = NULL;
        protocol->configuration = NULL;
        protocol->listener = NULL;
        platform_mutex_unlock(protocol->lock);
        return -1;
    }

    protocol->state = MERIDIAN_PROTOCOL_STATE_BOOTSTRAPPING;
    ATOMIC_STORE(&protocol->running, 1);

    platform_mutex_unlock(protocol->lock);
    return 0;
}

int meridian_protocol_stop(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    platform_mutex_lock(protocol->lock);

    if (!ATOMIC_LOAD(&protocol->running)) {
        platform_mutex_unlock(protocol->lock);
        return 0;
    }

    ATOMIC_STORE(&protocol->running, 0);
    protocol->state = MERIDIAN_PROTOCOL_STATE_SHUTTING_DOWN;

    if (protocol->gossip_handle) {
        meridian_gossip_handle_stop(protocol->gossip_handle);
    }

    // Shutdown and close all peer connections
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->connected_peers[i]) {
            protocol->msquic->ConnectionShutdown(
                protocol->connected_peers[i],
                QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
                0);
            protocol->msquic->ConnectionClose(protocol->connected_peers[i]);
            protocol->connected_peers[i] = NULL;
        }
        if (protocol->peer_nodes[i]) {
            refcounter_dereference(&protocol->peer_nodes[i]->refcounter);
            if (refcounter_count(&protocol->peer_nodes[i]->refcounter) == 0) {
                free(protocol->peer_nodes[i]);
            }
            protocol->peer_nodes[i] = NULL;
        }
    }
    protocol->num_connected_peers = 0;

    // Stop and close listener
    if (protocol->listener) {
        protocol->msquic->ListenerStop(protocol->listener);
        protocol->msquic->ListenerClose(protocol->listener);
        protocol->listener = NULL;
    }

    // Close configuration and registration so msquic stops
    // delivering callbacks for this protocol's handles
    if (protocol->configuration) {
        protocol->msquic->ConfigurationClose(protocol->configuration);
        protocol->configuration = NULL;
    }

    if (protocol->registration) {
        poseidon_msquic_close_registration("meridian");
        protocol->registration = NULL;
    }

    platform_mutex_unlock(protocol->lock);
    return 0;
}

// ============================================================================
// PEER MANAGEMENT
// ============================================================================

int meridian_protocol_add_seed_node(meridian_protocol_t* protocol,
                                     uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (protocol->num_seed_nodes >= 16) return -1;

    meridian_node_t* node = meridian_node_create(addr, port, NULL);
    if (node == NULL) return -1;

    protocol->seed_nodes[protocol->num_seed_nodes++] = node;
    return 0;
}

int meridian_protocol_connect(meridian_protocol_t* protocol,
                               uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (!ATOMIC_LOAD(&protocol->running)) return -1;
    if (protocol->num_connected_peers >= 64) return -1;

    // Check if already connected
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == addr &&
            protocol->peer_nodes[i]->port == port) {
            return 0;
        }
    }

    // Create peer node
    meridian_node_t* node = meridian_node_create(addr, port, NULL);
    if (node == NULL) return -1;

    // Create QUIC connection
    HQUIC Connection;
    QUIC_STATUS Status = protocol->msquic->ConnectionOpen(
        protocol->registration,
        ConnectionCallback,
        protocol,
        &Connection);

    if (QUIC_FAILED(Status)) {
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }

    // Set connection callback
    protocol->msquic->SetCallbackHandler(Connection, ConnectionCallback, protocol);

    // Build remote address
    QUIC_ADDR RemoteAddr = {0};
    RemoteAddr.Ipv4.sin_family = AF_INET;
    RemoteAddr.Ipv4.sin_addr.s_addr = addr;
    RemoteAddr.Ipv4.sin_port = port;

    // Start connection
    Status = protocol->msquic->ConnectionStart(
        Connection,
        protocol->configuration,
        AF_INET,
        NULL,  // Let QUIC resolve
        port);

    if (QUIC_FAILED(Status)) {
        protocol->msquic->ConnectionClose(Connection);
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }

    // The peer table belongs to the actor. The caller waits for the
    // answer, so the peer is in the table when this returns.
    meridian_protocol_peer_added_msg_t* request =
        (meridian_protocol_peer_added_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_peer_added_msg_t));
    if (request == NULL) {
        protocol->msquic->ConnectionClose(Connection);
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }
    request->connection = Connection;
    request->node = node;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        protocol->msquic->ConnectionClose(Connection);
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_PEER_ADDED,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        protocol->msquic->ConnectionClose(Connection);
        refcounter_dereference(&node->refcounter);
        if (refcounter_count(&node->refcounter) == 0) {
            free(node);
        }
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
    return 0;
}

int meridian_protocol_disconnect(meridian_protocol_t* protocol,
                                  uint32_t addr, uint16_t port) {
    if (protocol == NULL) return -1;
    if (!ATOMIC_LOAD(&protocol->running)) return -1;

    /* The peer table belongs to the actor: it finds the peer, closes its
     * connection and drops it. */
    meridian_protocol_peer_removed_msg_t* request =
        (meridian_protocol_peer_removed_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_peer_removed_msg_t));
    if (request == NULL) return -1;
    request->addr = addr;
    request->port = port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_PEER_REMOVED,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
    return 0;
}

// ============================================================================
// NETWORK I/O
// ============================================================================

/**
 * @brief The actor-thread body of MERIDIAN_PROTOCOL_SEND_PACKET.
 *
 * Finds the peer's connection and sends the bytes. The gossip's outbound
 * callback runs on this same thread, so the send must not go back through
 * a round trip: the actor cannot answer itself while it is inside its own
 * dispatch.
 */
static int meridian_protocol_send_packet_impl(meridian_protocol_t* protocol,
                                               const uint8_t* data, size_t len,
                                               uint32_t target_addr,
                                               uint16_t target_port) {
    int sent = -1;
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        meridian_node_t* node = protocol->peer_nodes[i];
        if (node != NULL && node->addr == target_addr &&
            node->port == target_port) {
            HQUIC connection = protocol->connected_peers[i];
            if (connection != NULL) {
                sent = meridian_stream_send_uni(protocol->msquic, connection,
                                                data, len, NULL);
            }
            break;
        }
    }
    return sent;
}

int meridian_protocol_send_packet(meridian_protocol_t* protocol,
                                   const uint8_t* data, size_t len,
                                   const meridian_node_t* target) {
    if (protocol == NULL || data == NULL || target == NULL) return -1;
    if (!ATOMIC_LOAD(&protocol->running)) return -1;

    /* The peer table belongs to the actor: it finds the connection and
     * sends. The request carries its own copy of the bytes — the actor
     * may still be holding it after this call returns. */
    meridian_protocol_send_packet_msg_t* request =
        (meridian_protocol_send_packet_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_send_packet_msg_t));
    if (request == NULL) return -1;
    request->data = (uint8_t*)get_memory(len > 0 ? len : 1);
    if (request->data == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->data, data, len);
    request->len = len;
    request->target_addr = target->addr;
    request->target_port = target->port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->data);
        free(request);
        return -1;
    }

    /* A call from inside the protocol's own dispatch — the gossip's
     * outbound callback — must not wait for the actor to answer itself. */
    if (actor_is_running_on_this_thread(&protocol->actor)) {
        int rc = meridian_protocol_send_packet_impl(protocol, data, len,
                                                    target->addr, target->port);
        free(request->data);
        free(request);
        return rc;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_SEND_PACKET,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request->data);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request->data);
    free(request);
    return 0;
}

int meridian_protocol_broadcast(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len) {
    if (protocol == NULL || data == NULL) return -1;

    /* The fan-out is the actor's work: it knows the peers. */
    meridian_protocol_broadcast_msg_t* request =
        (meridian_protocol_broadcast_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_broadcast_msg_t));
    if (request == NULL) return -1;
    request->data = (uint8_t*)get_memory(len > 0 ? len : 1);
    if (request->data == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->data, data, len);
    request->len = len;

    if (actor_reply_init(&request->reply) != 0) {
        free(request->data);
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_BROADCAST,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request->data);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request->data);
    free(request);
    return 0;
}

// ============================================================================
// NODE DISCOVERY
// ============================================================================

meridian_node_t* meridian_protocol_find_closest(meridian_protocol_t* protocol,
                                                uint32_t target_addr, uint16_t target_port) {
    if (protocol == NULL) return NULL;

    /* The ring set belongs to the protocol's actor. */
    meridian_protocol_find_closest_msg_t* request =
        (meridian_protocol_find_closest_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_find_closest_msg_t));
    if (request == NULL) return NULL;
    request->target_addr = target_addr;
    request->target_port = target_port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_FIND_CLOSEST,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return NULL;
    }
    if (trip != 0) return NULL;  /* the actor owns the request now */

    meridian_node_t* node = (meridian_node_t*)request->out_node;
    actor_reply_destroy(&request->reply);
    free(request);
    return node;
}

meridian_node_t** meridian_protocol_get_connected_peers(meridian_protocol_t* protocol,
                                                      size_t* num_peers) {
    if (protocol == NULL || num_peers == NULL) return NULL;

    /* The peer table belongs to the actor, so the snapshot is the actor's
     * copy. The caller owns the returned array and frees it; the nodes
     * themselves stay the table's. */
    meridian_protocol_peers_snapshot_msg_t* request =
        (meridian_protocol_peers_snapshot_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_peers_snapshot_msg_t));
    if (request == NULL) return NULL;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_PEERS_SNAPSHOT,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return NULL;
    }
    if (trip != 0) return NULL;  /* the actor owns the request now */

    void** peers = request->peers;
    size_t count = request->num_peers;
    actor_reply_destroy(&request->reply);
    free(request);

    *num_peers = count;
    return (meridian_node_t**)peers;
}

// ============================================================================
// PERIODIC OPERATIONS
// ============================================================================

int meridian_protocol_gossip(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    /* The gossip belongs to the protocol's actor: it knows the peers, and
     * the scheduler and the in-flight gossips are its state. */
    meridian_protocol_gossip_tick_msg_t* request =
        (meridian_protocol_gossip_tick_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_gossip_tick_msg_t));
    if (request == NULL) return -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_GOSSIP_TICK,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
    return 0;
}

int meridian_protocol_ring_management(meridian_protocol_t* protocol) {
    if (protocol == NULL) return -1;

    for (int ring = 0; ring < MERIDIAN_MAX_RINGS; ring++) {
        if (meridian_ring_set_eligible_for_replacement(protocol->ring_set, ring)) {
            // Promote a secondary node to fill the primary ring
            meridian_ring_set_promote_secondary(protocol->ring_set, ring);
        }
    }

    return 0;
}

// ============================================================================
// PACKET HANDLING
// ============================================================================

int meridian_protocol_on_packet(meridian_protocol_t* protocol,
                                 const uint8_t* data, size_t len,
                                 const meridian_node_t* from) {
    if (protocol == NULL || data == NULL) return -1;

    // Check if this is a Quasar route message (uses its own magic, not CBOR)
    if (len >= 4) {
        uint32_t magic = ntohl(*(const uint32_t*)data);
        if (magic == QUASAR_ROUTE_MAGIC) {
            // Forward to the application callback for Quasar handling
            if (protocol->callbacks.on_packet != NULL) {
                protocol->callbacks.on_packet(protocol->callbacks.user_ctx, protocol, data, len);
            }
            return 0;
        }
    }

    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, len, &result);
    if (item == NULL) return -1;

    // Decode base header to dispatch measurement PONG responses
    meridian_packet_t pkt;
    if (meridian_packet_decode(item, &pkt) == 0) {
        if (pkt.type == MERIDIAN_PACKET_TYPE_RET_PING) {
            // PONG response: the pending measurements belong to the
            // protocol's actor, so the reply is handed over.
            meridian_protocol_measure_response_msg_t* request =
                (meridian_protocol_measure_response_msg_t*)
                get_clear_memory(sizeof(meridian_protocol_measure_response_msg_t));
            if (request != NULL) {
                request->query_id = pkt.query_id;
                if (actor_reply_init(&request->reply) == 0) {
                    /* The caller does not wait: the request is the
                     * actor's to free. */
                    request->reply.orphaned = 1;
                    message_t msg = {
                        .type = MERIDIAN_PROTOCOL_MEASURE_RESPONSE,
                        .payload = request,
                        .payload_destroy = NULL,
                    };
                    if (!actor_send(&protocol->actor, &msg)) {
                        actor_reply_destroy(&request->reply);
                    }
                } else {
                    free(request);
                }
            }
            cbor_decref(&item);
            return 0;
        }
    }

    if (!cbor_array_is_definite(item) || cbor_array_size(item) < 1) {
        cbor_decref(&item);
        return -1;
    }

    cbor_item_t** items = cbor_array_handle(item);
    uint8_t type = cbor_get_uint8(items[0]);

    int rc = 0;
    switch (type) {
    case MERIDIAN_PACKET_TYPE_GOSSIP:
    case MERIDIAN_PACKET_TYPE_GOSSIP_PULL:
        // Gossip requires a known source node; skip for stream-received packets
        if (from != NULL) {
            /* The in-flight gossips belong to the protocol's actor, so
             * the reply is handed over: this callback runs on msquic's
             * worker thread. The request carries its own copy of the
             * packet — the receive buffer dies with the stream. */
            meridian_protocol_gossip_response_msg_t* request =
                (meridian_protocol_gossip_response_msg_t*)
                get_clear_memory(sizeof(meridian_protocol_gossip_response_msg_t));
            if (request != NULL) {
                request->len = len;
                request->from = (void*)from;
                request->data = (uint8_t*)get_memory(len > 0 ? len : 1);
                if (request->data != NULL) {
                    if (len > 0) memcpy(request->data, data, len);
                    if (actor_reply_init(&request->reply) == 0) {
                        /* The caller does not wait: the request is the
                         * actor's to free. */
                        request->reply.orphaned = 1;
                        message_t msg = {
                            .type = MERIDIAN_PROTOCOL_GOSSIP_RESPONSE,
                            .payload = request,
                            .payload_destroy = NULL,
                        };
                        if (!actor_send(&protocol->actor, &msg)) {
                            actor_reply_destroy(&request->reply);
                        }
                    } else {
                        free(request->data);
                        free(request);
                    }
                } else {
                    free(request);
                }
            }
        }
        break;
    case MERIDIAN_PACKET_TYPE_QUASAR_GOSSIP:
        if (protocol->callbacks.on_quasar_gossip != NULL) {
            protocol->callbacks.on_quasar_gossip(protocol->callbacks.quasar_ctx, protocol, data, len);
        }
        break;
    case MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP: {
        if (protocol->channel_manager != NULL) {
            char topic_id[64], sender_node_id[64];
            uint64_t ts;
            if (meridian_channel_bootstrap_decode(item, topic_id, sizeof(topic_id),
                                                   sender_node_id, sizeof(sender_node_id), &ts) == 0) {
                poseidon_channel_manager_handle_bootstrap_request(
                    protocol->channel_manager, topic_id, sender_node_id);
            }
        }
        break;
    }
    case MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY: {
        if (protocol->channel_manager != NULL) {
            char topic_id[64], responder_node_id[64];
            uint32_t addr; uint16_t port; uint64_t ts;
            uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds;
            if (meridian_channel_bootstrap_reply_decode(item, topic_id, sizeof(topic_id),
                                                        responder_node_id, sizeof(responder_node_id),
                                                        &addr, &port, &ts,
                                                        seed_addrs, seed_ports, &num_seeds, 16) == 0) {
                poseidon_channel_manager_handle_bootstrap_reply(
                    protocol->channel_manager, topic_id, addr, port, ts,
                    seed_addrs, seed_ports, num_seeds);
            }
        }
        break;
    }
    default:
        if (protocol->callbacks.on_packet != NULL) {
            protocol->callbacks.on_packet(protocol->callbacks.user_ctx, protocol, data, len);
        }
        break;
    }

    cbor_decref(&item);
    return rc;
}

int meridian_protocol_on_measure_result(meridian_protocol_t* protocol,
                                         const meridian_measure_result_t* result) {
    if (protocol == NULL || result == NULL) return -1;

    /* The latency cache belongs to the protocol's actor. */
    meridian_protocol_measure_result_msg_t* request =
        (meridian_protocol_measure_result_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_measure_result_msg_t));
    if (request == NULL) return -1;
    request->node = (void*)result->node;
    request->latency_us = result->latency_us;
    request->success = result->success;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_MEASURE_RESULT,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
    return 0;
}

int meridian_protocol_send_measure(meridian_protocol_t* protocol,
                                    meridian_measure_request_t* req) {
    if (protocol == NULL || req == NULL || req->target == NULL) return -1;

    // Record send time for RTT computation
    gettimeofday(&req->start_time, NULL);

    /* The peer table belongs to the actor: it finds the connection, builds
     * the PING and puts it on a stream. */
    meridian_protocol_send_measure_msg_t* request =
        (meridian_protocol_send_measure_msg_t*)
        get_clear_memory(sizeof(meridian_protocol_send_measure_msg_t));
    if (request == NULL) return -1;
    request->query_id = req->query_id;
    request->target_addr = req->target->addr;
    request->target_port = req->target->port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&protocol->actor, MERIDIAN_PROTOCOL_SEND_MEASURE,
                                &request->reply, MERIDIAN_PROTOCOL_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
    return 0;
}

// ============================================================================
// CALLBACKS
// ============================================================================

int meridian_protocol_set_callbacks(meridian_protocol_t* protocol,
                                     const meridian_protocol_callbacks_t* callbacks) {
    if (protocol == NULL || callbacks == NULL) return -1;
    protocol->callbacks = *callbacks;
    return 0;
}

// ============================================================================
// LOCAL NODE INFO
// ============================================================================

int meridian_protocol_get_local_node(meridian_protocol_t* protocol,
                                      uint32_t* addr, uint16_t* port) {
    if (protocol == NULL) return -1;
    if (!ATOMIC_LOAD(&protocol->running)) return -1;

    if (addr) *addr = protocol->local_addr.sin_addr.s_addr;
    if (port) *port = ntohs(protocol->local_addr.sin_port);

    return 0;
}

// ============================================================================
// QUIC CALLBACKS
// ============================================================================

static QUIC_STATUS QUIC_API
ListenerCallback(HQUIC Listener, void* Context,
                QUIC_LISTENER_EVENT* Event) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)Context;

    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
        HQUIC Connection = Event->NEW_CONNECTION.Connection;

        // Set connection callback
        protocol->msquic->SetCallbackHandler(Connection, ConnectionCallback, protocol);

        // Accept connection by setting configuration
        QUIC_STATUS Status = protocol->msquic->ConnectionSetConfiguration(
            Connection, protocol->configuration);

        if (QUIC_FAILED(Status)) {
            return Status;
        }

        /* The peer table belongs to the protocol's actor: the event is
         * handed over and the callback moves on. The address is filled in
         * later, when the peer address is known. */
        meridian_protocol_peer_added_msg_t* event =
            get_clear_memory(sizeof(meridian_protocol_peer_added_msg_t));
        if (event != NULL) {
            event->connection = Connection;
            protocol_post_event(protocol, MERIDIAN_PROTOCOL_PEER_ADDED, event, free);
        }

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_NOT_SUPPORTED;
    }
}

static QUIC_STATUS QUIC_API
ConnectionCallback(HQUIC Connection, void* Context,
                   QUIC_CONNECTION_EVENT* Event) {
    meridian_protocol_t* protocol = (meridian_protocol_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        // Handshake complete
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        // Connection shutting down
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // Ready for close
        return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC Stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = !(Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL);

        meridian_stream_entry_t* entry = meridian_stream_entry_create(
            Stream, is_bidi, protocol, protocol->sched);
        if (entry == NULL) return QUIC_STATUS_OUT_OF_MEMORY;
        meridian_stream_entry_on_message(entry, protocol_stream_message);

        QUIC_STREAM_CALLBACK_HANDLER handler = is_bidi
            ? protocol_bidi_stream_callback
            : protocol_uni_stream_callback;

        protocol->msquic->SetCallbackHandler(Stream, handler, entry);
        protocol_add_stream(protocol, entry);

        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        return QUIC_STATUS_SUCCESS;

    default:
        return QUIC_STATUS_SUCCESS;
    }
}
