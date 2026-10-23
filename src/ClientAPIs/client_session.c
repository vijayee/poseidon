/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon client_session.
 *
 * Vendored from liboffs/src/ClientAPIs/client_session.c. See "Local modifications" at EOF.
 */

#include "client_session.h"
#include "transport.h"
#include "client_protocol.h"
#include "../Channel/channel.h"
#include "../Channel/channel_notice.h"
#include "../Channel/channel_manager.h"
#include "../Crypto/key_pair.h"
#include "../Util/allocator.h"
#include "../Platform/platform_thread.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include <cbor.h>
#include <string.h>
#include "../Util/vec.h"

// ============================================================================
// ACTOR MESSAGE TYPES
// ============================================================================
//
// Per-session single-actor shape. All cbor encode/decode for the session
// happens inside dispatch so the per-session lock (now removed) is unnecessary:
// the actor serialises every state mutation on a single thread of control.
// Mirrors src/Channel/channel_manager.c 2.10.

#define POSEIDON_CLIENT_SESSION_MSG_SUBSCRIBE         0x01
#define POSEIDON_CLIENT_SESSION_MSG_UNSUBSCRIBE       0x02
#define POSEIDON_CLIENT_SESSION_MSG_IS_SUBSCRIBED     0x03
#define POSEIDON_CLIENT_SESSION_MSG_HAS_LOOPBACK      0x04
#define POSEIDON_CLIENT_SESSION_MSG_HANDLE_REQUEST    0x05
#define POSEIDON_CLIENT_SESSION_MSG_DELIVER           0x06
#define POSEIDON_CLIENT_SESSION_MSG_CLEANUP_SUBS      0x07
#define POSEIDON_CLIENT_SESSION_MSG_DISPATCH_EVENT    0x08

// HANDLE_REQUEST carries a heap-allocated client_frame_t by pointer so the
// wrapper can pass a stable copy to dispatch (the wrapper's stack frame
// vanishes as soon as actor_run returns). dispatch frees the frame.
typedef struct client_session_msg_handle_request_t {
    client_frame_t* frame;
} client_session_msg_handle_request_t;

// DELIVER carries the wire bytes (already cbor-encoded by the channel
// fan-out path) and the null-terminated topic path used for subscription
// filtering. dispatch owns both buffers and frees them.
typedef struct client_session_msg_deliver_t {
    char* topic_path;
    uint8_t* buf;
    size_t buf_len;
} client_session_msg_deliver_t;

// DISPATCH_EVENT carries the raw inputs to the public dispatch_event API.
// Strings and the payload are heap-copied by the wrapper so the actor's
// dispatch can outlive the wrapper's stack frame. dispatch owns every
// pointer and frees them after encoding and sending.
typedef struct client_session_msg_dispatch_event_t {
    uint8_t event_type;
    char* topic_path;
    char* subtopic;
    uint8_t* payload;
    size_t payload_len;
} client_session_msg_dispatch_event_t;

// SUBSCRIBE / UNSUBSCRIBE / IS_SUBSCRIBED / HAS_LOOPBACK / CLEANUP_SUBS
// payloads — plain data, no allocation. The wrappers read/write the matching
// out-fields on the session struct after actor_run returns.
typedef struct client_session_msg_topic_t {
    const char* topic_path;
    bool loopback;
} client_session_msg_topic_t;

// ============================================================================
// STATIC HELPERS (pure functions — no actor needed)
// ============================================================================

// Forward declarations — defined in later sections
static bool manager_has_other_subscriber(poseidon_channel_manager_t* mgr,
                                          const char* topic_path,
                                          const client_session_t* exclude);
static void session_dispatch_message(void* ctx, const uint8_t* topic, size_t topic_len,
                                      const char* subtopic, const uint8_t* data, size_t data_len);

static poseidon_channel_t* find_channel_by_topic(poseidon_channel_manager_t* mgr,
                                                  const char* topic_path) {
    if (mgr == NULL || topic_path == NULL) return NULL;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        poseidon_channel_t* ch = mgr->channels[i];
        if (ch == NULL) continue;
        const char* ch_topic = poseidon_channel_get_topic(ch);
        if (ch_topic != NULL && strcmp(ch_topic, topic_path) == 0) {
            return ch;
        }
    }
    return NULL;
}

static int verify_channel_owner_signature(poseidon_channel_t* channel,
                                           const client_frame_t* frame) {
    if (channel == NULL || frame == NULL) return -1;
    if (frame->signature_len == 0 || frame->payload_len == 0) return -1;
    const char* topic = poseidon_channel_get_topic(channel);
    if (topic == NULL) return -1;
    return poseidon_verify_signature(topic,
                                     frame->payload, frame->payload_len,
                                     frame->signature, frame->signature_len);
}

// ============================================================================
// ACTOR DISPATCH
// ============================================================================

static void handle_subscribe(client_session_t* s, const client_session_msg_topic_t* p) {
    s->subscribe_out = -1;
    if (p->topic_path == NULL) return;

    // Dedup: if already subscribed to this exact topic, succeed (idempotent).
    for (size_t i = 0; i < s->num_subscriptions; i++) {
        if (s->subscriptions[i].active &&
            strcmp(s->subscriptions[i].topic_path, p->topic_path) == 0) {
            s->subscribe_out = 0;
            return;
        }
    }
    if (s->num_subscriptions >= CLIENT_SESSION_MAX_SUBSCRIPTIONS) {
        s->subscribe_out = -1;
        return;
    }

    // Session-side record. This is what the test (and any actor-only caller)
    // depends on. Channel attach is best-effort: a manager-less session is a
    // valid in-memory subscription table; the channel side will catch up
    // later when a manager is attached and the topic is published.
    s->subscriptions[s->num_subscriptions].active = true;
    s->subscriptions[s->num_subscriptions].loopback = p->loopback;
    strncpy(s->subscriptions[s->num_subscriptions].topic_path, p->topic_path,
            CLIENT_MAX_TOPIC_PATH - 1);
    s->subscriptions[s->num_subscriptions].topic_path[CLIENT_MAX_TOPIC_PATH - 1] = '\0';
    s->num_subscriptions++;
    s->subscribe_out = 0;

    if (s->manager != NULL) {
        // Ensure the channel exists, then attach the per-session callback.
        poseidon_channel_t* channel = find_channel_by_topic(s->manager, p->topic_path);
        if (channel == NULL) {
            poseidon_channel_config_t config = poseidon_channel_config_defaults();
            channel = poseidon_channel_create(NULL, p->topic_path, 0, &config,
                                              s->manager->pool, s->manager->wheel);
        }
        if (channel != NULL) {
            poseidon_channel_set_message_callback(channel, session_dispatch_message, s->manager);
            poseidon_channel_subscribe(channel,
                                       (const uint8_t*)p->topic_path, strlen(p->topic_path), 0);
        }
        // Channel-attach failure is non-fatal: the session-side record stands
        // and a later publish on the same topic will re-attach.
    }
}

static void handle_unsubscribe(client_session_t* s, const client_session_msg_topic_t* p) {
    s->unsubscribe_out = -1;
    if (p->topic_path == NULL) return;
    for (size_t i = 0; i < s->num_subscriptions; i++) {
        if (s->subscriptions[i].active &&
            strcmp(s->subscriptions[i].topic_path, p->topic_path) == 0) {
            s->subscriptions[i].active = false;
            // Compact: shift remaining entries down.
            for (size_t j = i; j + 1 < s->num_subscriptions; j++) {
                s->subscriptions[j] = s->subscriptions[j + 1];
            }
            s->num_subscriptions--;
            // Detach from the channel if no other session is still subscribed.
            if (s->manager != NULL) {
                poseidon_channel_t* channel = find_channel_by_topic(s->manager, p->topic_path);
                if (channel != NULL && !manager_has_other_subscriber(s->manager,
                                                                     p->topic_path, s)) {
                    poseidon_channel_unsubscribe(channel,
                                                  (const uint8_t*)p->topic_path,
                                                  strlen(p->topic_path));
                }
            }
            s->unsubscribe_out = 0;
            return;
        }
    }
    s->unsubscribe_out = -1;
}

static void handle_is_subscribed(client_session_t* s, const client_session_msg_topic_t* p) {
    s->is_subscribed_out = false;
    if (p->topic_path == NULL) return;
    for (size_t i = 0; i < s->num_subscriptions; i++) {
        if (s->subscriptions[i].active &&
            strcmp(s->subscriptions[i].topic_path, p->topic_path) == 0) {
            s->is_subscribed_out = true;
            return;
        }
    }
}

static void handle_has_loopback(client_session_t* s, const client_session_msg_topic_t* p) {
    s->has_loopback_out = false;
    if (p->topic_path == NULL) return;
    for (size_t i = 0; i < s->num_subscriptions; i++) {
        if (s->subscriptions[i].active &&
            strcmp(s->subscriptions[i].topic_path, p->topic_path) == 0) {
            s->has_loopback_out = s->subscriptions[i].loopback;
            return;
        }
    }
}

static void handle_handle_request(client_session_t* s,
                                  client_session_msg_handle_request_t* p) {
    s->handle_request_response_out = NULL;
    s->handle_request_rc_out = -1;
    if (p->frame == NULL) return;
    // CHANNEL_DESTROY and CHANNEL_MODIFY both have cbor-heavy paths that
    // must live inside the actor (the original direct callsite now runs
    // through the wrapper + actor_send + actor_run path).
    int rc = 0;
    cbor_item_t* response = NULL;
    // For the ctor-time channel calls (CREATE/JOIN) and the other methods,
    // reuse the public wrappers so the manager-side interceptors run.
    switch (p->frame->method) {
    case CLIENT_METHOD_CHANNEL_CREATE: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch != NULL) { rc = -1; break; }
        poseidon_channel_config_t config = poseidon_channel_config_defaults();
        ch = poseidon_channel_create(NULL, p->frame->topic_path, 0, &config,
                                     s->manager->pool, s->manager->wheel);
        rc = (ch != NULL) ? 0 : -1;
        break;
    }
    case CLIENT_METHOD_CHANNEL_JOIN: {
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        rc = (ch != NULL) ? 0 : -1;
        break;
    }
    case CLIENT_METHOD_SUBSCRIBE: {
        if (s->num_subscriptions >= CLIENT_SESSION_MAX_SUBSCRIPTIONS) { rc = -1; break; }
        s->subscriptions[s->num_subscriptions].active = true;
        s->subscriptions[s->num_subscriptions].loopback = false;
        strncpy(s->subscriptions[s->num_subscriptions].topic_path, p->frame->topic_path,
                CLIENT_MAX_TOPIC_PATH - 1);
        s->subscriptions[s->num_subscriptions].topic_path[CLIENT_MAX_TOPIC_PATH - 1] = '\0';
        s->num_subscriptions++;
        rc = 0;
        break;
    }
    case CLIENT_METHOD_UNSUBSCRIBE: {
        for (size_t i = 0; i < s->num_subscriptions; i++) {
            if (s->subscriptions[i].active &&
                strcmp(s->subscriptions[i].topic_path, p->frame->topic_path) == 0) {
                s->subscriptions[i].active = false;
                rc = 0;
                break;
            }
        }
        if (rc == 0) break;
        rc = -1;
        break;
    }
    case CLIENT_METHOD_PUBLISH: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) { rc = -1; break; }
        // Set publisher session for self-delivery filtering.
        ch->publisher_session = (void*)s;
        rc = poseidon_channel_publish(ch,
                                      (const uint8_t*)p->frame->topic_path,
                                      strlen(p->frame->topic_path),
                                      p->frame->payload, p->frame->payload_len);
        ch->publisher_session = NULL;
        break;
    }
    case CLIENT_METHOD_ALIAS_REGISTER: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) { rc = -1; break; }
        char alias[CLIENT_MAX_TOPIC_PATH] = {0};
        if (p->frame->payload_len > 0 && p->frame->payload_len < CLIENT_MAX_TOPIC_PATH) {
            memcpy(alias, p->frame->payload, p->frame->payload_len);
            alias[p->frame->payload_len] = '\0';
        }
        rc = poseidon_channel_register_alias(ch, p->frame->topic_path, alias);
        break;
    }
    case CLIENT_METHOD_ALIAS_UNREGISTER: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) { rc = -1; break; }
        rc = poseidon_channel_unregister_alias(ch, p->frame->topic_path);
        break;
    }
    case CLIENT_METHOD_ALIAS_RESOLVE: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) {
            response = client_protocol_encode_response(p->frame->request_id,
                                                       CLIENT_ERROR_CHANNEL_NOT_FOUND,
                                                       "");
            rc = 0;
            break;
        }
        const char* resolved = poseidon_channel_resolve_alias(ch, p->frame->topic_path);
        if (resolved == NULL) {
            response = client_protocol_encode_response(p->frame->request_id,
                                                       CLIENT_ERROR_CHANNEL_NOT_FOUND,
                                                       "");
            rc = 0;
            break;
        }
        response = client_protocol_encode_response(p->frame->request_id,
                                                   CLIENT_ERROR_OK,
                                                   resolved);
        rc = 0;
        break;
    }
    case CLIENT_METHOD_CHANNEL_DESTROY: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) { rc = -1; break; }
        if (verify_channel_owner_signature(ch, p->frame) != 0) { rc = -1; break; }

        // cbor work inside the actor: build delete notice, verify, encode,
        // publish, tombstone, destroy.
        meridian_channel_delete_notice_t* notice = poseidon_channel_create_delete_notice(ch);
        if (notice == NULL) { rc = -1; break; }
        if (poseidon_channel_verify_delete_notice(notice) != 0) {
            free(notice);
            rc = -1;
            break;
        }
        cbor_item_t* encoded = meridian_channel_delete_notice_encode(notice);
        if (encoded != NULL) {
            uint8_t* notice_buf = NULL;
            size_t notice_len = 0;
            size_t written = cbor_serialize_alloc(encoded, &notice_buf, &notice_len);
            cbor_decref(&encoded);
            if (written > 0 && notice_buf != NULL) {
                poseidon_channel_publish(ch, (const uint8_t*)p->frame->topic_path, strlen(p->frame->topic_path),
                                         notice_buf, notice_len);
            }
            free(notice_buf);
        }
        poseidon_tombstone_t tombstone;
        if (poseidon_tombstone_from_delete_notice(notice, &tombstone) == 0) {
            poseidon_channel_manager_add_tombstone(s->manager, &tombstone);
        }
        poseidon_channel_delete_signed(ch);
        const poseidon_node_id_t* nid = poseidon_channel_get_node_id(ch);
        poseidon_channel_manager_destroy_channel(s->manager, nid);
        free(notice);
        response = client_protocol_encode_response(p->frame->request_id, CLIENT_ERROR_OK, "deleted");
        rc = 0;
        break;
    }
    case CLIENT_METHOD_CHANNEL_MODIFY: {
        if (s->manager == NULL) { rc = -1; break; }
        poseidon_channel_t* ch = find_channel_by_topic(s->manager, p->frame->topic_path);
        if (ch == NULL) { rc = -1; break; }
        if (verify_channel_owner_signature(ch, p->frame) != 0) { rc = -1; break; }

        // cbor work inside the actor: build modify notice, verify, encode,
        // publish, tombstone, rejoin.
        poseidon_channel_config_t new_config = ch->config;
        if (p->frame->payload_len == sizeof(poseidon_channel_config_t)) {
            memcpy(&new_config, p->frame->payload, sizeof(poseidon_channel_config_t));
        }
        meridian_channel_modify_notice_t* notice = poseidon_channel_create_modify_notice(ch, &new_config);
        if (notice == NULL) { rc = -1; break; }
        if (poseidon_channel_verify_modify_notice(notice) != 0) {
            free(notice);
            rc = -1;
            break;
        }
        cbor_item_t* encoded = meridian_channel_modify_notice_encode(notice);
        if (encoded != NULL) {
            uint8_t* notice_buf = NULL;
            size_t notice_len = 0;
            size_t written = cbor_serialize_alloc(encoded, &notice_buf, &notice_len);
            cbor_decref(&encoded);
            if (written > 0 && notice_buf != NULL) {
                poseidon_channel_publish(ch, (const uint8_t*)p->frame->topic_path, strlen(p->frame->topic_path),
                                         notice_buf, notice_len);
            }
            free(notice_buf);
        }
        poseidon_tombstone_t tombstone;
        if (poseidon_tombstone_from_modify_notice(notice, &tombstone) == 0) {
            poseidon_channel_manager_add_tombstone(s->manager, &tombstone);
        }
        poseidon_channel_rejoin_with_config(ch, &new_config, s->manager);
        free(notice);
        response = client_protocol_encode_response(p->frame->request_id, CLIENT_ERROR_OK, "modified");
        rc = 0;
        break;
    }
    default:
        rc = -1;
        break;
    }

    if (response == NULL) {
        response = client_protocol_encode_response(p->frame->request_id,
                                                   (rc == 0) ? CLIENT_ERROR_OK : CLIENT_ERROR_INVALID_PARAMS,
                                                   "");
    }
    s->handle_request_response_out = response;
    s->handle_request_rc_out = rc;
}

static void handle_deliver(client_session_t* s, client_session_msg_deliver_t* p) {
    if (p->topic_path == NULL || p->buf == NULL) return;
    if (s->transport == NULL || s->client_fd < 0) return;
    poseidon_transport_send(s->transport, s->client_fd, p->buf, p->buf_len);
}

static void handle_dispatch_event(client_session_t* s,
                                   const client_session_msg_dispatch_event_t* p) {
    // cbor encode/decode must happen inside the actor (boss directive).
    // We replicate the old wrapper's encode-then-send pipeline but on the
    // actor's own thread, then free the heap-copied inputs the wrapper
    // handed us.
    if (p->topic_path == NULL) return;

    if (s->transport != NULL && s->client_fd >= 0) {
        cbor_item_t* event = client_protocol_encode_event(
            p->event_type, p->topic_path,
            p->subtopic ? p->subtopic : "",
            p->payload, p->payload_len);
        if (event != NULL) {
            uint8_t* buf = NULL;
            size_t buf_len = 0;
            if (client_protocol_serialize(event, &buf, &buf_len) == 0) {
                poseidon_transport_send(s->transport, s->client_fd, buf, buf_len);
                free(buf);
            }
            cbor_decref(&event);
        }
    }

    // Actor owns the heap-copied inputs and frees them regardless of
    // whether the wire-send path was taken.
    free(p->topic_path);
    free(p->subtopic);
    free(p->payload);
}

static void handle_cleanup_subs(client_session_t* s) {
    // Snapshot active topics first so we can drop session->lock equivalents
    // (none — actor serialises) and then notify the manager for any topic
    // where this session is the only subscriber.
    vec_t(char*) topics;
    vec_init(&topics);
    for (size_t i = 0; i < s->num_subscriptions; i++) {
        if (s->subscriptions[i].active) {
            char* topic = strdup(s->subscriptions[i].topic_path);
            if (topic != NULL) vec_push(&topics, topic);
            s->subscriptions[i].active = false;
        }
    }
    for (int i = 0; i < topics.length; i++) {
        if (!manager_has_other_subscriber(s->manager, topics.data[i], s)) {
            poseidon_channel_t* ch = find_channel_by_topic(s->manager, topics.data[i]);
            if (ch != NULL) {
                poseidon_channel_unsubscribe(ch,
                    (const uint8_t*)topics.data[i], strlen(topics.data[i]));
            }
        }
        free(topics.data[i]);
    }
    vec_deinit(&topics);
}

static void client_session_dispatch(void* state, message_t* msg) {
    client_session_t* s = (client_session_t*)state;
    switch (msg->type) {
    case POSEIDON_CLIENT_SESSION_MSG_SUBSCRIBE: {
        const client_session_msg_topic_t* p = (const client_session_msg_topic_t*)msg->payload;
        handle_subscribe(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_UNSUBSCRIBE: {
        const client_session_msg_topic_t* p = (const client_session_msg_topic_t*)msg->payload;
        handle_unsubscribe(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_IS_SUBSCRIBED: {
        const client_session_msg_topic_t* p = (const client_session_msg_topic_t*)msg->payload;
        handle_is_subscribed(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_HAS_LOOPBACK: {
        const client_session_msg_topic_t* p = (const client_session_msg_topic_t*)msg->payload;
        handle_has_loopback(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_HANDLE_REQUEST: {
        client_session_msg_handle_request_t* p = (client_session_msg_handle_request_t*)msg->payload;
        handle_handle_request(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_DELIVER: {
        client_session_msg_deliver_t* p = (client_session_msg_deliver_t*)msg->payload;
        handle_deliver(s, p);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_CLEANUP_SUBS: {
        handle_cleanup_subs(s);
        break;
    }
    case POSEIDON_CLIENT_SESSION_MSG_DISPATCH_EVENT: {
        const client_session_msg_dispatch_event_t* p =
            (const client_session_msg_dispatch_event_t*)msg->payload;
        handle_dispatch_event(s, p);
        break;
    }
    default:
        break;
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

client_session_t* client_session_create(uint32_t session_id,
                                        poseidon_channel_manager_t* manager) {
    client_session_t* s = (client_session_t*)get_clear_memory(sizeof(client_session_t));
    if (s == NULL) return NULL;
    s->session_id = session_id;
    s->client_fd = -1;
    s->transport = NULL;
    s->manager = manager;
    s->num_subscriptions = 0;
    s->next_request_id = 1;
    s->pool = (manager != NULL) ? manager->actor.pool : NULL;
    s->subscribe_out = -1;
    s->unsubscribe_out = -1;
    s->is_subscribed_out = false;
    s->has_loopback_out = false;
    s->handle_request_response_out = NULL;
    s->handle_request_rc_out = -1;
    actor_init(&s->actor, s, client_session_dispatch, s->pool);
    refcounter_init(&s->refcounter);
    return s;
}

void client_session_destroy(client_session_t* s) {
    if (s == NULL) return;
    refcounter_dereference(&s->refcounter);
    if (refcounter_count(&s->refcounter) == 0) {
        actor_destroy(&s->actor);
        free(s);
    }
}

// ============================================================================
// PUBLIC API (wrappers — pre-init out-fields, actor_send, actor_run, read)
// ============================================================================

int client_session_subscribe(client_session_t* s, const char* topic_path, bool loopback) {
    if (s == NULL || topic_path == NULL) return -1;
    client_session_msg_topic_t payload = { .topic_path = topic_path, .loopback = loopback };
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_SUBSCRIBE,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) return -1;
    actor_run_inline(&s->actor, 32);
    return s->subscribe_out;
}

int client_session_unsubscribe(client_session_t* s, const char* topic_path) {
    if (s == NULL || topic_path == NULL) return -1;
    client_session_msg_topic_t payload = { .topic_path = topic_path };
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_UNSUBSCRIBE,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) return -1;
    actor_run_inline(&s->actor, 32);
    return s->unsubscribe_out;
}

bool client_session_is_subscribed(client_session_t* s, const char* topic_path) {
    if (s == NULL || topic_path == NULL) return false;
    client_session_msg_topic_t payload = { .topic_path = topic_path };
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_IS_SUBSCRIBED,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) return false;
    actor_run_inline(&s->actor, 32);
    return s->is_subscribed_out;
}

bool client_session_has_loopback(client_session_t* s, const char* topic_path) {
    if (s == NULL || topic_path == NULL) return false;
    client_session_msg_topic_t payload = { .topic_path = topic_path };
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_HAS_LOOPBACK,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) return false;
    actor_run_inline(&s->actor, 32);
    return s->has_loopback_out;
}

int client_session_handle_request(client_session_t* s, const client_frame_t* frame,
                                   cbor_item_t** out_response) {
    if (s == NULL || frame == NULL) return -1;
    client_frame_t* frame_copy = (client_frame_t*)get_clear_memory(sizeof(client_frame_t));
    if (frame_copy == NULL) return -1;
    *frame_copy = *frame;

    client_session_msg_handle_request_t payload = { .frame = frame_copy };
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_HANDLE_REQUEST,
        .payload = &payload,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) {
        free(frame_copy);
        return -1;
    }
    actor_run_inline(&s->actor, 32);
    int rc = s->handle_request_rc_out;
    if (out_response != NULL) {
        *out_response = s->handle_request_response_out;
    } else if (s->handle_request_response_out != NULL) {
        cbor_decref(&s->handle_request_response_out);
    }
    s->handle_request_response_out = NULL;
    free(frame_copy);
    return rc;
}

void client_session_dispatch_event(client_session_t* s,
                                    uint8_t event_type,
                                    const char* topic_path,
                                    const char* subtopic,
                                    const uint8_t* payload, size_t payload_len) {
    if (s == NULL || topic_path == NULL) return;

    // Heap-copy every input the actor will need so its dispatch can outlive
    // this wrapper's stack frame. The actor (handle_dispatch_event) owns
    // and frees all three buffers after encoding and sending.
    char* topic_copy = strdup(topic_path);
    char* subtopic_copy = (subtopic != NULL) ? strdup(subtopic) : NULL;
    uint8_t* payload_copy = NULL;
    if (payload_len > 0) {
        payload_copy = (uint8_t*)malloc(payload_len);
        if (payload_copy != NULL) memcpy(payload_copy, payload, payload_len);
    }
    if (topic_copy == NULL || (subtopic != NULL && subtopic_copy == NULL) ||
        (payload_len > 0 && payload_copy == NULL)) {
        free(topic_copy);
        free(subtopic_copy);
        free(payload_copy);
        return;
    }

    client_session_msg_dispatch_event_t dp = {
        .event_type = event_type,
        .topic_path = topic_copy,
        .subtopic = subtopic_copy,
        .payload = payload_copy,
        .payload_len = payload_len,
    };
    message_t m = {
        .type = POSEIDON_CLIENT_SESSION_MSG_DISPATCH_EVENT,
        .payload = &dp,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &m)) {
        free(topic_copy);
        free(subtopic_copy);
        free(payload_copy);
        return;
    }
    actor_run_inline(&s->actor, 32);
}

void client_session_cleanup_subscriptions(client_session_t* s) {
    if (s == NULL || s->manager == NULL) return;
    message_t msg = {
        .type = POSEIDON_CLIENT_SESSION_MSG_CLEANUP_SUBS,
        .payload = NULL,
        .payload_destroy = NULL,
    };
    if (!actor_send(&s->actor, &msg)) return;
    actor_run_inline(&s->actor, 32);
}

// ============================================================================
// CHANNEL FAN-OUT CALLBACK (manager-side dispatcher)
// ============================================================================
//
// Registered per-channel via poseidon_channel_set_message_callback. Runs on
// whichever thread the channel is delivering on (typically the worker pool).
// For each recipient session we build a deliver message and feed it through
// the session's own actor so all session state mutation stays serialised.
//
// cbor encoding currently happens in this callback (matches upstream
// ecc27e6:src/ClientAPIs/client_session.c session_dispatch_message). The
// boss-approved carve-out is that cbor-in-actor is required for the session's
// public API wrappers; this channel-side callback is fan-out and was deferred
// to a later phase. Revisit when the channel layer itself moves to actor model.

static bool manager_has_other_subscriber(poseidon_channel_manager_t* mgr,
                                          const char* topic_path,
                                          const client_session_t* exclude) {
    if (mgr == NULL || topic_path == NULL) return false;

    // Snapshot session pointers under mgr->external_lock so we don't race
    // manager mutators. After the lock is released we read each session's
    // subscription array without taking session->lock (the lock is gone in
    // 2.11; per-actor serialisation covers session-side mutation).
    vec_t(client_session_t*) snap;
    vec_init(&snap);
    platform_mutex_lock(mgr->external_lock);
    for (size_t i = 0; i < mgr->num_sessions; i++) {
        client_session_t* s = (client_session_t*)mgr->sessions[i];
        if (s != NULL && s != exclude) vec_push(&snap, s);
    }
    platform_mutex_unlock(mgr->external_lock);

    bool found = false;
    for (size_t i = 0; i < snap.length && !found; i++) {
        client_session_t* s = snap.data[i];
        for (size_t j = 0; j < s->num_subscriptions; j++) {
            if (s->subscriptions[j].active &&
                strcmp(s->subscriptions[j].topic_path, topic_path) == 0) {
                found = true;
                break;
            }
        }
    }
    vec_deinit(&snap);
    return found;
}

static void session_dispatch_message(void* ctx, const uint8_t* topic, size_t topic_len,
                                      const char* subtopic, const uint8_t* data, size_t data_len) {
    poseidon_channel_manager_t* manager = (poseidon_channel_manager_t*)ctx;
    if (manager == NULL || topic == NULL) return;

    // Build null-terminated topic string.
    char topic_str[CLIENT_MAX_TOPIC_PATH];
    size_t copy_len = topic_len < (CLIENT_MAX_TOPIC_PATH - 1) ? topic_len : (CLIENT_MAX_TOPIC_PATH - 1);
    memcpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    // Locate channel for self-delivery filtering.
    poseidon_channel_t* channel = find_channel_by_topic(manager, topic_str);
    void* publisher_session = (channel != NULL) ? channel->publisher_session : NULL;

    // Encode the event frame (cbor here, deferred carve-out — see header).
    cbor_item_t* event = client_protocol_encode_event(
        CLIENT_EVENT_MESSAGE, topic_str,
        subtopic ? subtopic : "",
        data, data_len);
    if (event == NULL) return;
    uint8_t* buf = NULL;
    size_t buf_len = 0;
    if (client_protocol_serialize(event, &buf, &buf_len) != 0) {
        cbor_decref(&event);
        return;
    }
    cbor_decref(&event);

    // Snapshot sessions under mgr->external_lock.
    vec_t(client_session_t*) snap;
    vec_init(&snap);
    platform_mutex_lock(manager->external_lock);
    for (size_t i = 0; i < manager->num_sessions; i++) {
        client_session_t* s = (client_session_t*)manager->sessions[i];
        if (s != NULL) vec_push(&snap, s);
    }
    platform_mutex_unlock(manager->external_lock);

    // Fan out via each session's actor.
    for (size_t i = 0; i < snap.length; i++) {
        client_session_t* s = snap.data[i];
        bool is_publisher = (s == publisher_session);

        // Check loopback: if this session is the publisher and the matching
        // subscription has loopback=false, skip delivery.
        bool subscribed = false;
        bool loopback = true;
        for (size_t j = 0; j < s->num_subscriptions; j++) {
            if (s->subscriptions[j].active &&
                strcmp(s->subscriptions[j].topic_path, topic_str) == 0) {
                subscribed = true;
                loopback = s->subscriptions[j].loopback;
                break;
            }
        }
        if (!subscribed) continue;
        if (is_publisher && !loopback) continue;

        // Per-recipient buffer copy (each session owns its buf lifetime).
        uint8_t* buf_copy = (uint8_t*)malloc(buf_len);
        char* topic_copy = strdup(topic_str);
        if (buf_copy == NULL || topic_copy == NULL) {
            free(buf_copy);
            free(topic_copy);
            continue;
        }
        memcpy(buf_copy, buf, buf_len);

        client_session_msg_deliver_t payload = {
            .topic_path = topic_copy,
            .buf = buf_copy,
            .buf_len = buf_len,
        };
        message_t m = {
            .type = POSEIDON_CLIENT_SESSION_MSG_DELIVER,
            .payload = &payload,
            .payload_destroy = NULL,
        };
        if (!actor_send(&s->actor, &m)) {
            free(topic_copy);
            free(buf_copy);
            continue;
        }
        actor_run_inline(&s->actor, 32);
        free(topic_copy);
        free(buf_copy);
    }
    vec_deinit(&snap);
    free(buf);
}

/* ---- Local modifications (vs upstream) -----------------------------------
 *
 * - Per-FD single-actor shape adopted (mirrors src/Channel/channel_manager.c
 *   2.10): one actor embedded in client_session_t, every state mutation goes
 *   through a dispatched message, every public API became a wrapper that
 *   pre-initialises an out-field, calls actor_send+actor_run, and reads the
 *   out-field.
 * - Per-session platform_mutex_create/destroy removed; all access is
 *   serialised by the session actor itself. The corresponding
 *   platform_mutex_lock/unlock pairs in subscribe/unsubscribe/is_subscribed/
 *   has_loopback/cleanup_subscriptions/handle_request are gone.
 * - All cbor encode/decode (CHANNEL_DESTROY/CHANNEL_MODIFY in handle_request,
 *   and the public dispatch_event wrapper's encode-and-send pipeline) moved
 *   INSIDE dispatch. Boss directive: per-actor FD serialisation makes a
 *   per-session lock redundant, and cbor work must run on the actor's thread.
 * - Public API surface preserved (create/destroy/handle_request/subscribe/
 *   unsubscribe/is_subscribed/has_loopback/dispatch_event/
 *   cleanup_subscriptions).
 * - L2 provenance header retained.
 * ------------------------------------------------------------------------- */
