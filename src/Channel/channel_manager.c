//
// Created by victor on 4/20/26.
//

#include "channel_manager.h"
#include "channel_manager_messages.h"
#include "channel_notice.h"
#include "../Platform/platform_thread.h"
#include "../Crypto/key_pair.h"
#include "../Network/Meridian/meridian_packet.h"
#include "../Network/Meridian/meridian_protocol.h"
#include <string.h>
#include <time.h>
#include <cbor.h>

// ============================================================================
// BOOTSTRAP INTERCEPT
// ============================================================================

static bool channel_manager_bootstrap_intercept(void* ctx, const uint8_t* data, size_t data_len) {
    poseidon_channel_manager_t* mgr = (poseidon_channel_manager_t*)ctx;

    struct cbor_load_result result;
    cbor_item_t* item = cbor_load(data, data_len, &result);
    if (item == NULL || !cbor_array_is_definite(item) || cbor_array_size(item) < 1) {
        if (item != NULL) cbor_decref(&item);
        return false;
    }

    cbor_item_t** items = cbor_array_handle(item);
    if (!cbor_isa_uint(items[0])) {
        cbor_decref(&item);
        return false;
    }

    uint8_t pkt_type = cbor_get_uint8(items[0]);
    bool handled = false;

    if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP) {
        char topic_id[64], sender_node_id[64];
        uint64_t ts;
        if (meridian_channel_bootstrap_decode(item, topic_id, sizeof(topic_id),
                                               sender_node_id, sizeof(sender_node_id), &ts) == 0) {
            poseidon_channel_manager_handle_bootstrap_request(mgr, topic_id, sender_node_id);
            handled = true;
        }
    } else if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY) {
        char topic_id[64], responder_node_id[64];
        uint32_t addr; uint16_t port; uint64_t ts;
        uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds;
        if (meridian_channel_bootstrap_reply_decode(item, topic_id, sizeof(topic_id),
                                                      responder_node_id, sizeof(responder_node_id),
                                                      &addr, &port, &ts,
                                                      seed_addrs, seed_ports, &num_seeds, 16) == 0) {
            poseidon_channel_manager_handle_bootstrap_reply(
                mgr, topic_id, addr, port, ts, seed_addrs, seed_ports, num_seeds);
            handled = true;
        }
    } else if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE) {
        meridian_channel_delete_notice_t notice;
        if (meridian_channel_delete_notice_decode(item, &notice) == 0) {
            if (poseidon_channel_verify_delete_notice(&notice) == 0) {
                poseidon_channel_manager_handle_delete_notice(mgr, &notice);
                handled = true;
            }
        }
    } else if (pkt_type == MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE) {
        meridian_channel_modify_notice_t notice;
        if (meridian_channel_modify_notice_decode(item, &notice) == 0) {
            if (poseidon_channel_verify_modify_notice(&notice) == 0) {
                poseidon_channel_manager_handle_modify_notice(mgr, &notice);
                handled = true;
            }
        }
    }

    cbor_decref(&item);
    return handled;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

// Forward decl: actor_init in create() passes this function pointer.
static void channel_manager_dispatch(void* state, message_t* msg);

poseidon_channel_manager_t* poseidon_channel_manager_create(
    poseidon_key_pair_t* dial_key_pair,
    uint16_t dial_port,
    uint16_t port_range_start,
    uint16_t port_range_end,
    work_pool_t* pool,
    hierarchical_timing_wheel_t* wheel,
    scheduler_pool_t* sched) {
    if (dial_key_pair == NULL || pool == NULL || wheel == NULL) return NULL;
    /* The manager's actor needs a scheduler pool: the public calls send a
     * request and wait for the answer, and only a pool worker runs the
     * actor. */
    if (sched == NULL) return NULL;

    poseidon_channel_manager_t* mgr = (poseidon_channel_manager_t*)calloc(1, sizeof(poseidon_channel_manager_t));
    if (mgr == NULL) return NULL;
    refcounter_init(&mgr->refcounter);

    mgr->external_lock = platform_mutex_create();
    if (mgr->external_lock == NULL) {
        free(mgr);
        return NULL;
    }

    mgr->pool = pool;
    mgr->wheel = wheel;
    mgr->port_range_start = port_range_start;
    mgr->port_range_end = port_range_end;
    mgr->next_port = port_range_start;

    /* poseidon_channel_create rejects NULL config — pass a defaults-built
       instance so the dial channel runs with sane ring/gossip/quasar
       settings. */
    poseidon_channel_config_t dial_config = poseidon_channel_config_defaults();
    mgr->dial_channel = poseidon_channel_create(
        dial_key_pair, "dial", dial_port, &dial_config, pool, wheel, sched);
    if (mgr->dial_channel == NULL) {
        platform_mutex_destroy(mgr->external_lock);
        free(mgr);
        return NULL;
    }

    /* poseidon_channel_subscribe_with_intercept does not exist as a public
       API. Wire the intercept via the channel's own fields. The dial
       channel is passive: it does not subscribe to its own topic (the
       intercept catches any notice routed to it regardless of topic). */
    mgr->dial_channel->intercept_cb = channel_manager_bootstrap_intercept;
    mgr->dial_channel->intercept_ctx = mgr;

    actor_init(&mgr->actor, mgr, channel_manager_dispatch, sched);
    return mgr;
}

int poseidon_channel_manager_start(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL || mgr->dial_channel == NULL) return -1;
    /* Triggers meridian_protocol_start which opens the msquic listener.
       Production callers (the daemon) invoke this after _create; tests
       leave the dial channel passive to avoid needing a live msquic env. */
    return poseidon_channel_start(mgr->dial_channel, NULL, 0);
}

void poseidon_channel_manager_destroy(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return;
    if (!refcounter_dereference_is_zero(&mgr->refcounter)) return;

    /* Destroy the actor: that sets DESTROY, waits for a dispatch that is
       already running to finish, drains the mailbox and detaches the actor
       from the pool's registry. The detach matters: the manager is freed
       here, and a pool that still holds the actor in its registry would
       walk the freed block when it is torn down. */
    actor_destroy(&mgr->actor);

    if (mgr->dial_channel != NULL) {
        poseidon_channel_destroy(mgr->dial_channel);
        mgr->dial_channel = NULL;
    }
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (mgr->channels[i] != NULL) {
            poseidon_channel_destroy(mgr->channels[i]);
            mgr->channels[i] = NULL;
        }
    }
    if (mgr->external_lock != NULL) {
        platform_mutex_destroy(mgr->external_lock);
        mgr->external_lock = NULL;
    }
    free(mgr);
}

// ============================================================================
// DISPATCH
// ============================================================================

// Forward declarations for _impl workers dispatched below. Keeping these
// static here avoids reordering the file when signatures evolve.
static void channel_manager_dispatch(void* state, message_t* msg);

static poseidon_channel_t* poseidon_channel_manager_create_channel_impl(
    poseidon_channel_manager_t* mgr,
    const char* key_type,
    const char* name,
    const poseidon_channel_config_t* config);

static poseidon_channel_t* poseidon_channel_manager_join_channel_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_str);

static int poseidon_channel_manager_destroy_channel_impl(
    poseidon_channel_manager_t* mgr,
    const poseidon_node_id_t* node_id);

static int poseidon_channel_manager_handle_bootstrap_reply_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds,
    bool* out_first_reply,
    meridian_protocol_t** out_protocol);

static int poseidon_channel_manager_handle_bootstrap_request_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id,
    char* out_responder_node_id,
    uint32_t* out_responder_addr,
    uint16_t* out_responder_port,
    poseidon_channel_t** out_dial_channel,
    uint64_t* out_timestamp_us,
    unsigned char** out_publish_buf,
    size_t* out_publish_len,
    cbor_item_t** out_tombstone_encoded,
    int* out_tombstone_type,
    int* out_return_value);

static void poseidon_channel_manager_handle_delete_notice_impl(
    poseidon_channel_manager_t* mgr,
    meridian_channel_delete_notice_t* notice,
    bool* out_did_work);

static void poseidon_channel_manager_handle_modify_notice_impl(
    poseidon_channel_manager_t* mgr,
    meridian_channel_modify_notice_t* notice,
    bool* out_did_work);

static int poseidon_channel_manager_add_tombstone_impl(
    poseidon_channel_manager_t* mgr,
    const poseidon_tombstone_t* tombstone);

static void poseidon_channel_manager_expire_tombstones_impl(
    poseidon_channel_manager_t* mgr);

static int poseidon_channel_manager_register_session_impl(
    poseidon_channel_manager_t* mgr,
    void* session);

static int poseidon_channel_manager_unregister_session_impl(
    poseidon_channel_manager_t* mgr,
    void* session);

static void channel_manager_dispatch(void* state, message_t* msg) {
    poseidon_channel_manager_t* mgr = (poseidon_channel_manager_t*)state;
    if (msg == NULL) return;

    switch (msg->type) {
        case POSEIDON_CHANNEL_MANAGER_MSG_CREATE_CHANNEL: {
            poseidon_channel_manager_msg_create_channel_t* p =
                (poseidon_channel_manager_msg_create_channel_t*)msg->payload;
            p->out_channel = poseidon_channel_manager_create_channel_impl(
                mgr, p->key_type, p->name, &p->config);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_JOIN_CHANNEL: {
            poseidon_channel_manager_msg_join_channel_t* p =
                (poseidon_channel_manager_msg_join_channel_t*)msg->payload;
            p->out_channel = poseidon_channel_manager_join_channel_impl(
                mgr, p->topic_id);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_DESTROY_CHANNEL: {
            poseidon_channel_manager_msg_destroy_channel_t* p =
                (poseidon_channel_manager_msg_destroy_channel_t*)msg->payload;
            p->result = poseidon_channel_manager_destroy_channel_impl(
                mgr, &p->node_id);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REPLY: {
            poseidon_channel_manager_msg_handle_bootstrap_reply_t* p =
                (poseidon_channel_manager_msg_handle_bootstrap_reply_t*)msg->payload;
            p->result = poseidon_channel_manager_handle_bootstrap_reply_impl(
                mgr, p->topic_id, p->responder_addr, p->responder_port,
                p->timestamp_us, p->seed_addrs, p->seed_ports, p->num_seeds,
                &p->out_first_reply, &p->out_protocol);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REQUEST: {
            poseidon_channel_manager_msg_handle_bootstrap_request_t* p =
                (poseidon_channel_manager_msg_handle_bootstrap_request_t*)msg->payload;
            poseidon_channel_manager_handle_bootstrap_request_impl(
                mgr, p->topic_id, p->sender_node_id,
                p->responder_node_id, &p->responder_addr,
                &p->responder_port, &p->dial_channel,
                &p->timestamp_us, &p->publish_buf,
                &p->publish_len, &p->tombstone_encoded,
                &p->tombstone_type, &p->result);
            if (actor_reply_complete(&p->reply)) {
                /* The caller gave up: the actor disposes of the answer. */
                if (p->publish_buf != NULL) free(p->publish_buf);
                if (p->tombstone_encoded != NULL) cbor_decref(&p->tombstone_encoded);
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_DELETE_NOTICE: {
            poseidon_channel_manager_msg_handle_delete_notice_t* p =
                (poseidon_channel_manager_msg_handle_delete_notice_t*)msg->payload;
            poseidon_channel_manager_handle_delete_notice_impl(
                mgr, p->notice, &p->did_work);
            if (p->did_work && mgr->dial_channel != NULL) {
                /* The tombstone is redistributed on the dial channel from
                 * the actor's thread — the same seam the bootstrap-request
                 * path uses. */
                poseidon_tombstone_t t;
                if (poseidon_tombstone_from_delete_notice(p->notice, &t) == 0 &&
                    t.type == POSEIDON_TOMBSTONE_DELETE) {
                    meridian_channel_delete_notice_t n;
                    memset(&n, 0, sizeof(n));
                    n.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
                    snprintf(n.topic_id, sizeof(n.topic_id), "%s", t.topic_id);
                    snprintf(n.node_id, sizeof(n.node_id), "%s", t.node_id);
                    snprintf(n.key_type, sizeof(n.key_type), "%s", t.key_type);
                    if (t.public_key_len <= sizeof(n.public_key)) {
                        memcpy(n.public_key, t.public_key, t.public_key_len);
                        n.public_key_len = t.public_key_len;
                    }
                    n.timestamp_us = t.timestamp_us;
                    if (t.signature_len <= sizeof(n.signature)) {
                        memcpy(n.signature, t.signature, t.signature_len);
                        n.signature_len = t.signature_len;
                    }
                    cbor_item_t* encoded = meridian_channel_delete_notice_encode(&n);
                    if (encoded != NULL) {
                        unsigned char* buf = NULL;
                        size_t buf_len = 0;
                        size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
                        cbor_decref(&encoded);
                        if (buf != NULL && written > 0) {
                            (void)poseidon_channel_publish(mgr->dial_channel,
                                (const uint8_t*)t.topic_id, strlen(t.topic_id),
                                buf, written);
                            free(buf);
                        }
                    }
                }
            }
            free(p->notice);
            p->notice = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_MODIFY_NOTICE: {
            poseidon_channel_manager_msg_handle_modify_notice_t* p =
                (poseidon_channel_manager_msg_handle_modify_notice_t*)msg->payload;
            poseidon_channel_manager_handle_modify_notice_impl(
                mgr, p->notice, &p->did_work);
            if (p->did_work && mgr->dial_channel != NULL) {
                poseidon_tombstone_t t;
                if (poseidon_tombstone_from_modify_notice(p->notice, &t) == 0 &&
                    t.type == POSEIDON_TOMBSTONE_MODIFY) {
                    meridian_channel_modify_notice_t n;
                    memset(&n, 0, sizeof(n));
                    n.type = MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE;
                    snprintf(n.topic_id, sizeof(n.topic_id), "%s", t.topic_id);
                    snprintf(n.node_id, sizeof(n.node_id), "%s", t.node_id);
                    snprintf(n.key_type, sizeof(n.key_type), "%s", t.key_type);
                    if (t.public_key_len <= sizeof(n.public_key)) {
                        memcpy(n.public_key, t.public_key, t.public_key_len);
                        n.public_key_len = t.public_key_len;
                    }
                    n.timestamp_us = t.timestamp_us;
                    if (t.signature_len <= sizeof(n.signature)) {
                        memcpy(n.signature, t.signature, t.signature_len);
                        n.signature_len = t.signature_len;
                    }
                    n.config = t.config;
                    cbor_item_t* encoded = meridian_channel_modify_notice_encode(&n);
                    if (encoded != NULL) {
                        unsigned char* buf = NULL;
                        size_t buf_len = 0;
                        size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
                        cbor_decref(&encoded);
                        if (buf != NULL && written > 0) {
                            (void)poseidon_channel_publish(mgr->dial_channel,
                                (const uint8_t*)t.topic_id, strlen(t.topic_id),
                                buf, written);
                            free(buf);
                        }
                    }
                }
            }
            free(p->notice);
            p->notice = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_ADD_TOMBSTONE: {
            poseidon_channel_manager_msg_add_tombstone_t* p =
                (poseidon_channel_manager_msg_add_tombstone_t*)msg->payload;
            p->result = poseidon_channel_manager_add_tombstone_impl(
                mgr, &p->tombstone);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_EXPIRE_TOMBSTONES: {
            poseidon_channel_manager_msg_expire_tombstones_t* p =
                (poseidon_channel_manager_msg_expire_tombstones_t*)msg->payload;
            poseidon_channel_manager_expire_tombstones_impl(mgr);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_REGISTER_SESSION: {
            poseidon_channel_manager_msg_register_session_t* p =
                (poseidon_channel_manager_msg_register_session_t*)msg->payload;
            p->result = poseidon_channel_manager_register_session_impl(
                mgr, p->session);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case POSEIDON_CHANNEL_MANAGER_MSG_UNREGISTER_SESSION: {
            poseidon_channel_manager_msg_unregister_session_t* p =
                (poseidon_channel_manager_msg_unregister_session_t*)msg->payload;
            p->result = poseidon_channel_manager_unregister_session_impl(
                mgr, p->session);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        default:
            /* Unknown message type: drop. Per-actor dispatch; no central table. */
            break;
    }
}

// ============================================================================
// HELPERS
// ============================================================================

static uint16_t allocate_port(poseidon_channel_manager_t* mgr) {
    if (mgr->next_port > mgr->port_range_end) return 0;
    return mgr->next_port++;
}

static poseidon_channel_t* find_channel(const poseidon_channel_manager_t* mgr,
                                          const poseidon_node_id_t* node_id) {
    if (mgr == NULL || node_id == NULL) return NULL;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (poseidon_node_id_compare(&mgr->channels[i]->node_id, node_id) == 0) {
            return mgr->channels[i];
        }
    }
    return NULL;
}

static poseidon_channel_t* get_dial(const poseidon_channel_manager_t* mgr) {
    return mgr ? mgr->dial_channel : NULL;
}

// ============================================================================
// CHANNEL MANAGEMENT — public send_* wrappers + _impl static bodies
// ============================================================================

poseidon_channel_t* poseidon_channel_manager_create_channel(
    poseidon_channel_manager_t* mgr,
    const char* key_type,
    const char* name,
    const poseidon_channel_config_t* config) {
    if (mgr == NULL || key_type == NULL || name == NULL || config == NULL) return NULL;

    poseidon_channel_manager_msg_create_channel_t* request =
        (poseidon_channel_manager_msg_create_channel_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_create_channel_t));
    if (request == NULL) return NULL;
    snprintf(request->key_type, sizeof(request->key_type), "%s", key_type);
    snprintf(request->name, sizeof(request->name), "%s", name);
    request->config = *config;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_CREATE_CHANNEL,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return NULL;
    }
    if (trip != 0) return NULL;  /* the actor owns the request now */

    poseidon_channel_t* channel = request->out_channel;
    actor_reply_destroy(&request->reply);
    free(request);
    return channel;
}

poseidon_channel_t* poseidon_channel_manager_create_channel_impl(
    poseidon_channel_manager_t* mgr,
    const char* key_type,
    const char* name,
    const poseidon_channel_config_t* config) {
    /* NO I/O seam: poseidon_key_pair_create + poseidon_channel_create do not
       call back into the manager. Safe to run serialized under actor
       dispatch. */
    if (mgr->num_channels >= POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS) return NULL;

    uint16_t port = allocate_port(mgr);
    if (port == 0) return NULL;

    poseidon_key_pair_t* kp = poseidon_key_pair_create(key_type);
    if (kp == NULL) return NULL;

    poseidon_channel_t* channel = poseidon_channel_create(kp, name, port, config,
                                                          mgr->pool, mgr->wheel,
                                                          mgr->actor.pool);
    poseidon_key_pair_destroy(kp);
    if (channel == NULL) return NULL;

    mgr->channels[mgr->num_channels++] = channel;
    return channel;
}

poseidon_channel_t* poseidon_channel_manager_join_channel(
    poseidon_channel_manager_t* mgr,
    const char* topic_str) {
    if (mgr == NULL || topic_str == NULL) return NULL;

    poseidon_channel_manager_msg_join_channel_t* request =
        (poseidon_channel_manager_msg_join_channel_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_join_channel_t));
    if (request == NULL) return NULL;
    snprintf(request->topic_id, sizeof(request->topic_id), "%s", topic_str);

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_JOIN_CHANNEL,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return NULL;
    }
    if (trip != 0) return NULL;  /* the actor owns the request now */

    poseidon_channel_t* channel = request->out_channel;
    actor_reply_destroy(&request->reply);
    free(request);

    /* I/O seam: subscribe to the topic on the dial channel, encode and
       publish a bootstrap request. */
    if (channel != NULL) {
        (void)poseidon_channel_subscribe(mgr->dial_channel,
                                         (const uint8_t*)topic_str,
                                         strlen(topic_str), 300);

        const char* sender_node_id = poseidon_channel_get_topic(mgr->dial_channel);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                (uint64_t)ts.tv_nsec / 1000ULL;

        cbor_item_t* encoded = meridian_channel_bootstrap_encode(
            topic_str, sender_node_id, timestamp_us);
        if (encoded != NULL) {
            unsigned char* buf = NULL;
            size_t buf_len = 0;
            size_t written = cbor_serialize_alloc(encoded, &buf, &buf_len);
            cbor_decref(&encoded);
            if (buf != NULL && written > 0) {
                (void)poseidon_channel_publish(mgr->dial_channel,
                    (const uint8_t*)topic_str, strlen(topic_str),
                    buf, written);
                free(buf);
            }
        }
    }
    return channel;
}

poseidon_channel_t* poseidon_channel_manager_join_channel_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_str) {
    if (mgr->num_channels >= POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS) return NULL;
    if (mgr->num_pending_bootstraps >= POSEIDON_MAX_PENDING_BOOTSTRAPS) return NULL;

    uint16_t port = allocate_port(mgr);
    if (port == 0) return NULL;

    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(NULL, topic_str, port,
                                                          &config, mgr->pool,
                                                          mgr->wheel, mgr->actor.pool);
    if (channel == NULL) return NULL;
    channel->state = POSEIDON_CHANNEL_STATE_BOOTSTRAPPING;

    pending_bootstrap_t* pending = &mgr->pending_bootstraps[mgr->num_pending_bootstraps];
    memset(pending, 0, sizeof(*pending));
    snprintf(pending->topic_id, sizeof(pending->topic_id), "%s", topic_str);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pending->timestamp_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    pending->channel = channel;
    pending->num_replies = 0;
    mgr->num_pending_bootstraps++;

    mgr->channels[mgr->num_channels++] = channel;
    return channel;
}

int poseidon_channel_manager_destroy_channel(poseidon_channel_manager_t* mgr,
                                               const poseidon_node_id_t* node_id) {
    if (mgr == NULL || node_id == NULL) return -1;

    poseidon_channel_manager_msg_destroy_channel_t* request =
        (poseidon_channel_manager_msg_destroy_channel_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_destroy_channel_t));
    if (request == NULL) return -1;
    request->node_id = *node_id;
    request->result = -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_DESTROY_CHANNEL,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);
    return result;
}

int poseidon_channel_manager_destroy_channel_impl(poseidon_channel_manager_t* mgr,
                                                    const poseidon_node_id_t* node_id) {
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (poseidon_node_id_compare(&mgr->channels[i]->node_id, node_id) == 0) {
            poseidon_channel_destroy(mgr->channels[i]);
            mgr->channels[i] = mgr->channels[mgr->num_channels - 1];
            mgr->channels[mgr->num_channels - 1] = NULL;
            mgr->num_channels--;
            return 0;
        }
    }
    return -1;
}

poseidon_channel_t* poseidon_channel_manager_find_channel(
    const poseidon_channel_manager_t* mgr,
    const poseidon_node_id_t* node_id) {
    return find_channel(mgr, node_id);
}

poseidon_channel_t* poseidon_channel_manager_get_dial(const poseidon_channel_manager_t* mgr) {
    return get_dial(mgr);
}

// ============================================================================
// BOOTSTRAP REPLY / REQUEST
// ============================================================================

int poseidon_channel_manager_handle_bootstrap_reply(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds) {
    if (mgr == NULL || topic_id == NULL) return -1;

    poseidon_channel_manager_msg_handle_bootstrap_reply_t* request =
        (poseidon_channel_manager_msg_handle_bootstrap_reply_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_handle_bootstrap_reply_t));
    if (request == NULL) return -1;
    snprintf(request->topic_id, sizeof(request->topic_id), "%s", topic_id);
    request->responder_addr = responder_addr;
    request->responder_port = responder_port;
    request->timestamp_us = timestamp_us;
    size_t seeds_to_copy = num_seeds;
    if (seeds_to_copy > POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
        seeds_to_copy = POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX;
    }
    for (size_t i = 0; i < seeds_to_copy; i++) {
        request->seed_addrs[i] = seed_addrs[i];
        request->seed_ports[i] = seed_ports[i];
    }
    request->num_seeds = seeds_to_copy;
    request->out_first_reply = false;
    request->out_protocol = NULL;
    request->result = -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REPLY,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    bool first_reply = request->out_first_reply;
    meridian_protocol_t* protocol = request->out_protocol;
    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);

    /* I/O seam: connect on first reply, then add seeds. */
    if (first_reply && protocol != NULL) {
        meridian_protocol_connect(protocol, responder_addr, responder_port);
        size_t seeds_to_add = num_seeds;
        if (seeds_to_add > POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
            seeds_to_add = POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX;
        }
        for (size_t i = 0; i < seeds_to_add; i++) {
            meridian_protocol_add_seed_node(protocol, seed_addrs[i], seed_ports[i]);
        }
    }
    return result;
}

int poseidon_channel_manager_handle_bootstrap_reply_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds,
    bool* out_first_reply,
    meridian_protocol_t** out_protocol) {
    (void)seed_addrs;
    (void)seed_ports;
    (void)num_seeds;
    *out_first_reply = false;
    *out_protocol = NULL;

    for (size_t i = 0; i < mgr->num_pending_bootstraps; i++) {
        pending_bootstrap_t* pending = &mgr->pending_bootstraps[i];
        if (strcmp(pending->topic_id, topic_id) == 0 &&
            pending->timestamp_us == timestamp_us) {
            size_t reply_index = pending->num_replies;
            if (reply_index >= POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
                reply_index = POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX - 1;
            }
            pending->reply_addrs[reply_index] = responder_addr;
            pending->reply_ports[reply_index] = responder_port;
            pending->num_replies++;
            if (pending->num_replies > POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
                pending->num_replies = POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX;
            }

            if (pending->num_replies == 1) {
                *out_first_reply = true;
                *out_protocol = pending->channel->protocol;
                pending->channel->state = POSEIDON_CHANNEL_STATE_RUNNING;
            }

            /* remove pending bootstrap entry */
            for (size_t j = i; j + 1 < mgr->num_pending_bootstraps; j++) {
                mgr->pending_bootstraps[j] = mgr->pending_bootstraps[j + 1];
            }
            mgr->num_pending_bootstraps--;
            return 0;
        }
    }
    return -1;
}

int poseidon_channel_manager_handle_bootstrap_request(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id) {
    if (mgr == NULL || topic_id == NULL || sender_node_id == NULL) return -1;

    poseidon_channel_manager_msg_handle_bootstrap_request_t* request =
        (poseidon_channel_manager_msg_handle_bootstrap_request_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_handle_bootstrap_request_t));
    if (request == NULL) return -1;
    snprintf(request->topic_id, sizeof(request->topic_id), "%s", topic_id);
    snprintf(request->sender_node_id, sizeof(request->sender_node_id), "%s", sender_node_id);

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REQUEST,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    cbor_item_t* tombstone_encoded = request->tombstone_encoded;
    unsigned char* publish_buf = request->publish_buf;
    size_t publish_len = request->publish_len;
    poseidon_channel_t* dial_channel = request->dial_channel;
    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);

    /* I/O seam: the reply goes out before this returns — the requester is
       waiting for it. */
    if (tombstone_encoded != NULL && dial_channel != NULL) {
        unsigned char* buf = NULL;
        size_t buf_len = 0;
        size_t written = cbor_serialize_alloc(tombstone_encoded, &buf, &buf_len);
        cbor_decref(&tombstone_encoded);
        if (buf != NULL && written > 0) {
            (void)poseidon_channel_publish(dial_channel,
                (const uint8_t*)topic_id, strlen(topic_id), buf, written);
            free(buf);
        }
        return 0;
    }

    if (publish_buf != NULL && dial_channel != NULL) {
        (void)poseidon_channel_publish(dial_channel,
            (const uint8_t*)topic_id, strlen(topic_id),
            publish_buf, publish_len);
        free(publish_buf);
        return 0;
    }

    return result;
}

int poseidon_channel_manager_handle_bootstrap_request_impl(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id,
    char* out_responder_node_id,
    uint32_t* out_responder_addr,
    uint16_t* out_responder_port,
    poseidon_channel_t** out_dial_channel,
    uint64_t* out_timestamp_us,
    unsigned char** out_publish_buf,
    size_t* out_publish_len,
    cbor_item_t** out_tombstone_encoded,
    int* out_tombstone_type,
    int* out_return_value) {
    (void)sender_node_id;
    out_responder_node_id[0] = '\0';
    *out_responder_addr = 0;
    *out_responder_port = 0;
    *out_dial_channel = NULL;
    *out_timestamp_us = 0;
    *out_publish_buf = NULL;
    *out_publish_len = 0;
    *out_tombstone_encoded = NULL;
    *out_tombstone_type = 0;
    *out_return_value = -1;

    /* Branch 1: tombstone found. */
    const poseidon_tombstone_t* tombstone = NULL;
    for (size_t i = 0; i < mgr->num_tombstones; i++) {
        if (strcmp(mgr->tombstones[i].topic_id, topic_id) == 0) {
            tombstone = &mgr->tombstones[i];
            break;
        }
    }
    if (tombstone != NULL) {
        if (tombstone->type == POSEIDON_TOMBSTONE_DELETE) {
            meridian_channel_delete_notice_t notice;
            memset(&notice, 0, sizeof(notice));
            notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_DELETE_NOTICE;
            snprintf(notice.topic_id, sizeof(notice.topic_id), "%s", tombstone->topic_id);
            snprintf(notice.node_id, sizeof(notice.node_id), "%s", tombstone->node_id);
            snprintf(notice.key_type, sizeof(notice.key_type), "%s", tombstone->key_type);
            if (tombstone->public_key_len <= sizeof(notice.public_key)) {
                memcpy(notice.public_key, tombstone->public_key, tombstone->public_key_len);
                notice.public_key_len = tombstone->public_key_len;
            }
            notice.timestamp_us = tombstone->timestamp_us;
            if (tombstone->signature_len <= sizeof(notice.signature)) {
                memcpy(notice.signature, tombstone->signature, tombstone->signature_len);
                notice.signature_len = tombstone->signature_len;
            }
            cbor_item_t* encoded = meridian_channel_delete_notice_encode(&notice);
            if (encoded != NULL) {
                *out_tombstone_encoded = encoded;
                *out_tombstone_type = POSEIDON_TOMBSTONE_DELETE;
                *out_dial_channel = mgr->dial_channel;
                *out_return_value = 0;
                return 0;
            }
        } else if (tombstone->type == POSEIDON_TOMBSTONE_MODIFY) {
            meridian_channel_modify_notice_t notice;
            memset(&notice, 0, sizeof(notice));
            notice.type = MERIDIAN_PACKET_TYPE_CHANNEL_MODIFY_NOTICE;
            snprintf(notice.topic_id, sizeof(notice.topic_id), "%s", tombstone->topic_id);
            snprintf(notice.node_id, sizeof(notice.node_id), "%s", tombstone->node_id);
            snprintf(notice.key_type, sizeof(notice.key_type), "%s", tombstone->key_type);
            if (tombstone->public_key_len <= sizeof(notice.public_key)) {
                memcpy(notice.public_key, tombstone->public_key, tombstone->public_key_len);
                notice.public_key_len = tombstone->public_key_len;
            }
            notice.timestamp_us = tombstone->timestamp_us;
            if (tombstone->signature_len <= sizeof(notice.signature)) {
                memcpy(notice.signature, tombstone->signature, tombstone->signature_len);
                notice.signature_len = tombstone->signature_len;
            }
            notice.config = tombstone->config;
            cbor_item_t* encoded = meridian_channel_modify_notice_encode(&notice);
            if (encoded != NULL) {
                *out_tombstone_encoded = encoded;
                *out_tombstone_type = POSEIDON_TOMBSTONE_MODIFY;
                *out_dial_channel = mgr->dial_channel;
                *out_return_value = 0;
                return 0;
            }
        }
        /* tombstone found but encoding failed: fall through, treat as miss. */
    }

    /* Branch 2: not a tombstone. Check membership, build reply. */
    bool is_member = false;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        const char* ch_topic = poseidon_channel_get_topic(mgr->channels[i]);
        if (ch_topic != NULL && strcmp(ch_topic, topic_id) == 0) {
            is_member = true;
            break;
        }
    }
    if (!is_member) {
        *out_return_value = -1;
        return -1;
    }

    /* collect responder info from the dial channel */
    const char* responder_nid = poseidon_channel_get_topic(mgr->dial_channel);
    if (responder_nid != NULL) {
        snprintf(out_responder_node_id, 64, "%s", responder_nid);
    }
    uint32_t addr = 0;
    uint16_t port = 0;
    if (mgr->dial_channel != NULL && mgr->dial_channel->protocol != NULL) {
        meridian_protocol_get_local_node(mgr->dial_channel->protocol, &addr, &port);
    }
    *out_responder_addr = addr;
    *out_responder_port = port;
    *out_dial_channel = mgr->dial_channel;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *out_timestamp_us = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;

    cbor_item_t* reply = meridian_channel_bootstrap_reply_encode(
        topic_id, out_responder_node_id, addr, port, *out_timestamp_us,
        NULL, NULL, 0);
    if (reply == NULL) {
        *out_return_value = -1;
        return -1;
    }
    unsigned char* buf = NULL;
    size_t buf_len = 0;
    size_t written = cbor_serialize_alloc(reply, &buf, &buf_len);
    cbor_decref(&reply);
    if (buf == NULL || written == 0) {
        *out_return_value = -1;
        return -1;
    }
    *out_publish_buf = buf;
    *out_publish_len = written;
    *out_return_value = 0;
    return 0;
}

// ============================================================================
// DELETE / MODIFY NOTICE
// ============================================================================

void poseidon_channel_manager_handle_delete_notice(
    poseidon_channel_manager_t* mgr,
    const meridian_channel_delete_notice_t* notice) {
    if (mgr == NULL || notice == NULL) return;

    /* The request owns its own copy of the notice: the actor may still be
       holding it after this call returns. */
    poseidon_channel_manager_msg_handle_delete_notice_t* request =
        (poseidon_channel_manager_msg_handle_delete_notice_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_handle_delete_notice_t));
    if (request == NULL) return;
    request->notice = (meridian_channel_delete_notice_t*)
        malloc(sizeof(meridian_channel_delete_notice_t));
    if (request->notice == NULL) {
        free(request);
        return;
    }
    memcpy(request->notice, notice, sizeof(*notice));

    if (actor_reply_init(&request->reply) != 0) {
        free(request->notice);
        free(request);
        return;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_DELETE_NOTICE,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request->notice);
        free(request);
        return;
    }
    if (trip != 0) return;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
}

void poseidon_channel_manager_handle_delete_notice_impl(
    poseidon_channel_manager_t* mgr,
    meridian_channel_delete_notice_t* notice,
    bool* out_did_work) {
    *out_did_work = false;

    poseidon_tombstone_t tombstone;
    if (poseidon_tombstone_from_delete_notice(notice, &tombstone) != 0) return;
    /* This runs on the actor's own thread: the impl is called directly. A
       message to this same actor would wait for an answer the actor cannot
       give while it is inside this dispatch. */
    if (poseidon_channel_manager_add_tombstone_impl(mgr, &tombstone) == 0) {
        *out_did_work = true;
    }

    poseidon_node_id_t nid;
    if (poseidon_node_id_from_string(notice->node_id, &nid) == 0) {
        poseidon_channel_t* found = find_channel(mgr, &nid);
        if (found != NULL) {
            poseidon_channel_delete_signed(found);
            /* Same thread, same actor: the impl is called directly. */
            poseidon_channel_manager_destroy_channel_impl(mgr, &nid);
        }
    }
}

void poseidon_channel_manager_handle_modify_notice(
    poseidon_channel_manager_t* mgr,
    const meridian_channel_modify_notice_t* notice) {
    if (mgr == NULL || notice == NULL) return;

    poseidon_channel_manager_msg_handle_modify_notice_t* request =
        (poseidon_channel_manager_msg_handle_modify_notice_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_handle_modify_notice_t));
    if (request == NULL) return;
    request->notice = (meridian_channel_modify_notice_t*)
        malloc(sizeof(meridian_channel_modify_notice_t));
    if (request->notice == NULL) {
        free(request);
        return;
    }
    memcpy(request->notice, notice, sizeof(*notice));

    if (actor_reply_init(&request->reply) != 0) {
        free(request->notice);
        free(request);
        return;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_MODIFY_NOTICE,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request->notice);
        free(request);
        return;
    }
    if (trip != 0) return;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
}

void poseidon_channel_manager_handle_modify_notice_impl(
    poseidon_channel_manager_t* mgr,
    meridian_channel_modify_notice_t* notice,
    bool* out_did_work) {
    *out_did_work = false;

    poseidon_tombstone_t tombstone;
    if (poseidon_tombstone_from_modify_notice(notice, &tombstone) != 0) return;
    /* This runs on the actor's own thread: the impl is called directly. */
    if (poseidon_channel_manager_add_tombstone_impl(mgr, &tombstone) == 0) {
        *out_did_work = true;
    }

    poseidon_node_id_t nid;
    if (poseidon_node_id_from_string(notice->node_id, &nid) == 0) {
        poseidon_channel_t* found = find_channel(mgr, &nid);
        if (found != NULL) {
            poseidon_channel_rejoin_with_config(found, &notice->config, mgr);
        }
    }
}

// ============================================================================
// TICK / GOSSIP (non-mutators, but tick_all forwards to expire_tombstones)
// ============================================================================

int poseidon_channel_manager_tick_all(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return -1;

    int rc = 0;
    if (mgr->dial_channel != NULL) {
        rc |= poseidon_channel_tick(mgr->dial_channel);
    }
    for (size_t i = 0; i < mgr->num_channels; i++) {
        rc |= poseidon_channel_tick(mgr->channels[i]);
    }

    /* expire_tombstones is a pure state mutator — it goes to the actor and
       the tick waits for the answer. */
    poseidon_channel_manager_msg_expire_tombstones_t* request =
        (poseidon_channel_manager_msg_expire_tombstones_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_expire_tombstones_t));
    if (request == NULL) return rc;
    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return rc;
    }
    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_EXPIRE_TOMBSTONES,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return rc;
    }
    if (trip == 0) {
        actor_reply_destroy(&request->reply);
        free(request);
    }
    return rc;
}

int poseidon_channel_manager_gossip_all(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return -1;
    int rc = 0;
    if (mgr->dial_channel != NULL) {
        rc |= poseidon_channel_gossip(mgr->dial_channel);
    }
    for (size_t i = 0; i < mgr->num_channels; i++) {
        rc |= poseidon_channel_gossip(mgr->channels[i]);
    }
    return rc;
}

// ============================================================================
// TOMBSTONES
// ============================================================================

int poseidon_channel_manager_add_tombstone(poseidon_channel_manager_t* mgr,
                                             const poseidon_tombstone_t* tombstone) {
    if (mgr == NULL || tombstone == NULL) return -1;

    /* The request carries its own copy of the tombstone: the actor may
       still be holding it after this call returns. */
    poseidon_channel_manager_msg_add_tombstone_t* request =
        (poseidon_channel_manager_msg_add_tombstone_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_add_tombstone_t));
    if (request == NULL) return -1;
    request->tombstone = *tombstone;
    request->result = -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_ADD_TOMBSTONE,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);
    return result;
}

int poseidon_channel_manager_add_tombstone_impl(poseidon_channel_manager_t* mgr,
                                                   const poseidon_tombstone_t* tombstone) {
    for (size_t i = 0; i < mgr->num_tombstones; i++) {
        if (strcmp(mgr->tombstones[i].topic_id, tombstone->topic_id) == 0) {
            if (tombstone->timestamp_us > mgr->tombstones[i].timestamp_us) {
                mgr->tombstones[i] = *tombstone;
            }
            return 0;
        }
    }
    if (mgr->num_tombstones >= POSEIDON_TOMBSTONE_MAX) return -1;
    mgr->tombstones[mgr->num_tombstones++] = *tombstone;
    return 0;
}

const poseidon_tombstone_t* poseidon_channel_manager_find_tombstone(
    const poseidon_channel_manager_t* mgr,
    const char* topic_id) {
    if (mgr == NULL || topic_id == NULL) return NULL;
    for (size_t i = 0; i < mgr->num_tombstones; i++) {
        if (strcmp(mgr->tombstones[i].topic_id, topic_id) == 0) {
            return &mgr->tombstones[i];
        }
    }
    return NULL;
}

void poseidon_channel_manager_expire_tombstones(poseidon_channel_manager_t* mgr) {
    if (mgr == NULL) return;

    poseidon_channel_manager_msg_expire_tombstones_t* request =
        (poseidon_channel_manager_msg_expire_tombstones_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_expire_tombstones_t));
    if (request == NULL) return;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_EXPIRE_TOMBSTONES,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return;
    }
    if (trip != 0) return;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
}

void poseidon_channel_manager_expire_tombstones_impl(poseidon_channel_manager_t* mgr) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;

    size_t write = 0;
    for (size_t read = 0; read < mgr->num_tombstones; read++) {
        if (mgr->tombstones[read].expires_at_us > now_us) {
            if (write != read) mgr->tombstones[write] = mgr->tombstones[read];
            write++;
        }
    }
    mgr->num_tombstones = write;
}

int poseidon_tombstone_from_delete_notice(const meridian_channel_delete_notice_t* notice,
                                            poseidon_tombstone_t* tombstone) {
    if (notice == NULL || tombstone == NULL) return -1;
    memset(tombstone, 0, sizeof(*tombstone));

    tombstone->type = POSEIDON_TOMBSTONE_DELETE;
    strncpy(tombstone->topic_id, notice->topic_id, sizeof(tombstone->topic_id) - 1);
    strncpy(tombstone->node_id, notice->node_id, sizeof(tombstone->node_id) - 1);
    strncpy(tombstone->key_type, notice->key_type, sizeof(tombstone->key_type) - 1);
    if (notice->public_key_len <= sizeof(tombstone->public_key)) {
        memcpy(tombstone->public_key, notice->public_key, notice->public_key_len);
        tombstone->public_key_len = notice->public_key_len;
    }
    tombstone->timestamp_us = notice->timestamp_us;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    tombstone->expires_at_us = now_us + (uint64_t)POSEIDON_TOMBSTONE_DEFAULT_TTL_SECONDS * 1000000;

    if (notice->signature_len <= sizeof(tombstone->signature)) {
        memcpy(tombstone->signature, notice->signature, notice->signature_len);
        tombstone->signature_len = notice->signature_len;
    }
    tombstone->has_config = false;
    return 0;
}

int poseidon_tombstone_from_modify_notice(const meridian_channel_modify_notice_t* notice,
                                            poseidon_tombstone_t* tombstone) {
    if (notice == NULL || tombstone == NULL) return -1;
    memset(tombstone, 0, sizeof(*tombstone));

    tombstone->type = POSEIDON_TOMBSTONE_MODIFY;
    strncpy(tombstone->topic_id, notice->topic_id, sizeof(tombstone->topic_id) - 1);
    strncpy(tombstone->node_id, notice->node_id, sizeof(tombstone->node_id) - 1);
    strncpy(tombstone->key_type, notice->key_type, sizeof(tombstone->key_type) - 1);
    if (notice->public_key_len <= sizeof(tombstone->public_key)) {
        memcpy(tombstone->public_key, notice->public_key, notice->public_key_len);
        tombstone->public_key_len = notice->public_key_len;
    }
    tombstone->timestamp_us = notice->timestamp_us;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    tombstone->expires_at_us = now_us + (uint64_t)POSEIDON_TOMBSTONE_DEFAULT_TTL_SECONDS * 1000000;

    if (notice->signature_len <= sizeof(tombstone->signature)) {
        memcpy(tombstone->signature, notice->signature, notice->signature_len);
        tombstone->signature_len = notice->signature_len;
    }
    tombstone->config = notice->config;
    tombstone->has_config = true;
    return 0;
}

// ============================================================================
// SESSION REGISTRY
// ============================================================================

int poseidon_channel_manager_register_session(poseidon_channel_manager_t* mgr, void* session) {
    if (mgr == NULL) return -1;

    poseidon_channel_manager_msg_register_session_t* request =
        (poseidon_channel_manager_msg_register_session_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_register_session_t));
    if (request == NULL) return -1;
    request->session = session;
    request->result = -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_REGISTER_SESSION,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);
    return result;
}

int poseidon_channel_manager_register_session_impl(poseidon_channel_manager_t* mgr, void* session) {
    if (mgr->num_sessions >= POSEIDON_SESSION_REGISTRY_MAX) return -1;
    mgr->sessions[mgr->num_sessions++] = session;
    return 0;
}

int poseidon_channel_manager_unregister_session(poseidon_channel_manager_t* mgr, void* session) {
    if (mgr == NULL) return -1;

    poseidon_channel_manager_msg_unregister_session_t* request =
        (poseidon_channel_manager_msg_unregister_session_t*)
        get_clear_memory(sizeof(poseidon_channel_manager_msg_unregister_session_t));
    if (request == NULL) return -1;
    request->session = session;
    request->result = -1;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&mgr->actor, POSEIDON_CHANNEL_MANAGER_MSG_UNREGISTER_SESSION,
                                &request->reply, POSEIDON_CHANNEL_MANAGER_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    int result = request->result;
    actor_reply_destroy(&request->reply);
    free(request);
    return result;
}

int poseidon_channel_manager_unregister_session_impl(poseidon_channel_manager_t* mgr, void* session) {
    for (size_t i = 0; i < mgr->num_sessions; i++) {
        if (mgr->sessions[i] == session) {
            mgr->sessions[i] = mgr->sessions[mgr->num_sessions - 1];
            mgr->sessions[mgr->num_sessions - 1] = NULL;
            mgr->num_sessions--;
            return 0;
        }
    }
    return -1;
}
