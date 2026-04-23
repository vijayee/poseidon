//
// Created by victor on 4/22/26.
//

#include "client_session.h"
#include "transport.h"
#include "../Channel/channel_notice.h"
#include "../Channel/channel_manager.h"
#include "../Crypto/key_pair.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include <cbor.h>
#include <string.h>
#include "../Util/vec.h"

// Forward declarations — defined in later sections
static bool manager_has_other_subscriber(poseidon_channel_manager_t* mgr,
                                          const char* topic_path,
                                          const client_session_t* exclude);
static void session_dispatch_message(void* ctx, const uint8_t* topic, size_t topic_len,
                                      const char* subtopic, const uint8_t* data, size_t data_len);

// ============================================================================
// LIFECYCLE
// ============================================================================

client_session_t* client_session_create(uint32_t session_id,
                                         poseidon_channel_manager_t* manager) {
    client_session_t* session = (client_session_t*)
        get_clear_memory(sizeof(client_session_t));
    if (session == NULL) return NULL;

    session->session_id = session_id;
    session->manager = manager;
    session->client_fd = -1;
    session->num_subscriptions = 0;
    session->next_request_id = 1;

    platform_lock_init(&session->lock);
    refcounter_init(&session->refcounter);
    return session;
}

void client_session_destroy(client_session_t* session) {
    if (session == NULL) return;
    refcounter_dereference(&session->refcounter);
    if (refcounter_count(&session->refcounter) == 0) {
        platform_lock_destroy(&session->lock);
        free(session);
    }
}

// ============================================================================
// CHANNEL LOOKUP
// ============================================================================

static poseidon_channel_t* find_channel_by_topic(poseidon_channel_manager_t* mgr,
                                                  const char* topic) {
    if (mgr == NULL || topic == NULL) return NULL;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        if (mgr->channels[i] != NULL) {
            const char* channel_topic = poseidon_channel_get_topic(mgr->channels[i]);
            if (channel_topic != NULL && strcmp(channel_topic, topic) == 0) {
                return mgr->channels[i];
            }
        }
    }
    return NULL;
}

// ============================================================================
// CLIENT AUTHORIZATION
// ============================================================================

static int verify_channel_owner_signature(const poseidon_channel_t* channel,
                                           const client_frame_t* frame) {
    if (channel == NULL || channel->key_pair == NULL || frame == NULL) return -1;
    if (frame->signature_len == 0) return -1;

    // Reconstruct signed data: method || topic_path
    uint8_t data_to_verify[1 + CLIENT_MAX_TOPIC_PATH];
    size_t pos = 0;
    data_to_verify[pos++] = frame->method;
    size_t topic_len = strlen(frame->topic_path);
    memcpy(data_to_verify + pos, frame->topic_path, topic_len);
    pos += topic_len;

    const char* key_type = poseidon_key_pair_get_key_type(channel->key_pair);
    uint8_t* pub_key = NULL;
    size_t pub_len = 0;
    if (poseidon_key_pair_get_public_key(channel->key_pair, &pub_key, &pub_len) != 0) {
        return -1;
    }

    int rc = poseidon_verify_signature_with_key(key_type, pub_key, pub_len,
                                                  data_to_verify, pos,
                                                  frame->signature, frame->signature_len);
    free(pub_key);
    return rc;
}

// ============================================================================
// REQUEST HANDLER
// ============================================================================

int client_session_handle_request(client_session_t* session,
                                    const client_frame_t* frame,
                                    cbor_item_t** out) {
    if (session == NULL || frame == NULL || out == NULL) return -1;

    *out = NULL;

    switch (frame->method) {
        case CLIENT_METHOD_CHANNEL_CREATE: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_config_t config = poseidon_channel_config_defaults();
            poseidon_channel_t* channel = poseidon_channel_manager_create_channel(
                session->manager, "ED25519", frame->topic_path, &config);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_TOO_MANY_CHANNELS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "created");
            return 0;
        }

        case CLIENT_METHOD_CHANNEL_JOIN: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = poseidon_channel_manager_join_channel(
                session->manager, frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "joined");
            return 0;
        }

        case CLIENT_METHOD_SUBSCRIBE: {
            bool loopback = (frame->payload_len > 0 && frame->payload[0] != 0);
            int rc = client_session_subscribe(session, frame->topic_path, loopback);
            if (rc != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_INVALID_PARAMS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "subscribed");
            return 0;
        }

        case CLIENT_METHOD_UNSUBSCRIBE: {
            int rc = client_session_unsubscribe(session, frame->topic_path);
            if (rc != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_INVALID_PARAMS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "unsubscribed");
            return 0;
        }

        case CLIENT_METHOD_PUBLISH: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            // Set publisher session for self-delivery filtering
            channel->publisher_session = (void*)session;
            int rc = poseidon_channel_publish(channel,
                                               (const uint8_t*)frame->topic_path,
                                               strlen(frame->topic_path),
                                               frame->payload, frame->payload_len);
            channel->publisher_session = NULL;
            if (rc != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_TRANSPORT, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "published");
            return 0;
        }

        case CLIENT_METHOD_ALIAS_REGISTER: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            char topic[CLIENT_MAX_TOPIC_PATH] = {0};
            if (frame->payload_len > 0 && frame->payload_len < CLIENT_MAX_TOPIC_PATH) {
                memcpy(topic, frame->payload, frame->payload_len);
                topic[frame->payload_len] = '\0';
            }
            int rc = poseidon_channel_register_alias(channel, frame->topic_path, topic);
            if (rc != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_ALIAS_AMBIGUOUS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "registered");
            return 0;
        }

        case CLIENT_METHOD_ALIAS_UNREGISTER: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            int rc = poseidon_channel_unregister_alias(channel, frame->topic_path);
            if (rc != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_ALIAS_AMBIGUOUS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "unregistered");
            return 0;
        }

        case CLIENT_METHOD_ALIAS_RESOLVE: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            const char* resolved = poseidon_channel_resolve_alias(channel, frame->topic_path);
            if (resolved == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_ALIAS_AMBIGUOUS, "");
                return -1;
            }
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, resolved);
            return 0;
        }

        case CLIENT_METHOD_CHANNEL_DESTROY: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            if (verify_channel_owner_signature(channel, frame) != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_NOT_AUTHORIZED, "");
                return -1;
            }
            meridian_channel_delete_notice_t* notice =
                poseidon_channel_create_delete_notice(channel);
            if (notice == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_TRANSPORT, "");
                return -1;
            }
            // Verify the notice signature
            if (poseidon_channel_verify_delete_notice(notice) != 0) {
                free(notice);
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_NOT_AUTHORIZED, "");
                return -1;
            }
            // Publish delete notice on channel's Quasar overlay
            cbor_item_t* encoded = meridian_channel_delete_notice_encode(notice);
            if (encoded != NULL) {
                unsigned char* buf = NULL;
                size_t len = 0;
                size_t written = cbor_serialize_alloc(encoded, &buf, &len);
                cbor_decref(&encoded);
                if (written > 0 && buf != NULL) {
                    const char* topic = poseidon_channel_get_topic(channel);
                    poseidon_channel_publish(channel,
                                             (const uint8_t*)topic, strlen(topic),
                                             buf, written);
                    // Also publish on dial channel for redistribution
                    poseidon_channel_t* dial = poseidon_channel_manager_get_dial(session->manager);
                    if (dial != NULL) {
                        poseidon_channel_publish(dial,
                                                 (const uint8_t*)notice->topic_id,
                                                 strlen(notice->topic_id),
                                                 buf, written);
                    }
                }
                free(buf);
            }
            // Store tombstone locally
            poseidon_tombstone_t tombstone;
            poseidon_tombstone_from_delete_notice(notice, &tombstone);
            poseidon_channel_manager_add_tombstone(session->manager, &tombstone);
            // Delete the channel locally
            poseidon_channel_delete_signed(channel);
            // Remove from manager and free resources
            const poseidon_node_id_t* nid = poseidon_channel_get_node_id(channel);
            poseidon_channel_manager_destroy_channel(session->manager, nid);
            free(notice);
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "deleted");
            return 0;
        }

        case CLIENT_METHOD_CHANNEL_MODIFY: {
            if (session->manager == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                 frame->topic_path);
            if (channel == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_CHANNEL_NOT_FOUND, "");
                return -1;
            }
            if (verify_channel_owner_signature(channel, frame) != 0) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_NOT_AUTHORIZED, "");
                return -1;
            }
            poseidon_channel_config_t new_config = channel->config;
            // Use client-provided config if present in payload
            if (frame->payload_len == sizeof(poseidon_channel_config_t)) {
                memcpy(&new_config, frame->payload, sizeof(poseidon_channel_config_t));
            }
            meridian_channel_modify_notice_t* notice =
                poseidon_channel_create_modify_notice(channel, &new_config);
            if (notice == NULL) {
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_TRANSPORT, "");
                return -1;
            }
            if (poseidon_channel_verify_modify_notice(notice) != 0) {
                free(notice);
                *out = client_protocol_encode_response(frame->request_id,
                                                        CLIENT_ERROR_NOT_AUTHORIZED, "");
                return -1;
            }
            // Publish modify notice on channel's Quasar overlay
            cbor_item_t* encoded = meridian_channel_modify_notice_encode(notice);
            if (encoded != NULL) {
                unsigned char* buf = NULL;
                size_t len = 0;
                size_t written = cbor_serialize_alloc(encoded, &buf, &len);
                cbor_decref(&encoded);
                if (written > 0 && buf != NULL) {
                    const char* topic = poseidon_channel_get_topic(channel);
                    poseidon_channel_publish(channel,
                                             (const uint8_t*)topic, strlen(topic),
                                             buf, written);
                    poseidon_channel_t* dial = poseidon_channel_manager_get_dial(session->manager);
                    if (dial != NULL) {
                        poseidon_channel_publish(dial,
                                                 (const uint8_t*)notice->topic_id,
                                                 strlen(notice->topic_id),
                                                 buf, written);
                    }
                }
                free(buf);
            }
            // Store tombstone locally
            poseidon_tombstone_t tombstone;
            poseidon_tombstone_from_modify_notice(notice, &tombstone);
            poseidon_channel_manager_add_tombstone(session->manager, &tombstone);
            // Trigger rejoin with new config
            poseidon_channel_rejoin_with_config(channel, &new_config, session->manager);
            free(notice);
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_OK, "modified");
            return 0;
        }

        default:
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_UNKNOWN_METHOD, "");
            return -1;
    }
}

// ============================================================================
// SUBSCRIPTION MANAGEMENT
// ============================================================================

int client_session_subscribe(client_session_t* session, const char* topic_path, bool loopback) {
    if (session == NULL || topic_path == NULL) return -1;

    platform_lock(&session->lock);

    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active &&
            strcmp(session->subscriptions[i].topic_path, topic_path) == 0) {
            session->subscriptions[i].loopback = loopback;
            platform_unlock(&session->lock);
            return 0;
        }
    }

    if (session->num_subscriptions >= CLIENT_SESSION_MAX_SUBSCRIPTIONS) {
        platform_unlock(&session->lock);
        return -1;
    }

    strncpy(session->subscriptions[session->num_subscriptions].topic_path,
            topic_path, CLIENT_MAX_TOPIC_PATH - 1);
    session->subscriptions[session->num_subscriptions].topic_path[CLIENT_MAX_TOPIC_PATH - 1] = '\0';
    session->subscriptions[session->num_subscriptions].active = true;
    session->subscriptions[session->num_subscriptions].loopback = loopback;
    session->num_subscriptions++;

    platform_unlock(&session->lock);

    // Wire subscription to the Quasar overlay
    if (session->manager != NULL) {
        poseidon_channel_t* channel = find_channel_by_topic(session->manager, topic_path);
        if (channel != NULL) {
            poseidon_channel_subscribe(channel,
                                       (const uint8_t*)topic_path,
                                       strlen(topic_path), 0);
            // Set message callback if not already set
            if (channel->message_cb == NULL) {
                poseidon_channel_set_message_callback(channel,
                    session_dispatch_message, session->manager);
            }
        }
    }

    return 0;
}

int client_session_unsubscribe(client_session_t* session, const char* topic_path) {
    if (session == NULL || topic_path == NULL) return -1;

    platform_lock(&session->lock);

    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active &&
            strcmp(session->subscriptions[i].topic_path, topic_path) == 0) {
            session->subscriptions[i].active = false;
            for (size_t j = i; j + 1 < session->num_subscriptions; j++) {
                session->subscriptions[j] = session->subscriptions[j + 1];
            }
            session->num_subscriptions--;
            platform_unlock(&session->lock);

            // Unsubscribe from Quasar if this was the last local subscriber
            if (session->manager != NULL &&
                !manager_has_other_subscriber(session->manager, topic_path, session)) {
                poseidon_channel_t* channel = find_channel_by_topic(session->manager,
                                                                     topic_path);
                if (channel != NULL) {
                    poseidon_channel_unsubscribe(channel,
                        (const uint8_t*)topic_path, strlen(topic_path));
                }
            }
            return 0;
        }
    }

    platform_unlock(&session->lock);
    return -1;
}

bool client_session_is_subscribed(client_session_t* session, const char* topic_path) {
    if (session == NULL || topic_path == NULL) return false;
    platform_lock(&session->lock);
    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active &&
            strcmp(session->subscriptions[i].topic_path, topic_path) == 0) {
            platform_unlock(&session->lock);
            return true;
        }
    }
    platform_unlock(&session->lock);
    return false;
}

bool client_session_has_loopback(client_session_t* session, const char* topic_path) {
    if (session == NULL || topic_path == NULL) return false;
    platform_lock(&session->lock);
    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active &&
            strcmp(session->subscriptions[i].topic_path, topic_path) == 0) {
            bool lb = session->subscriptions[i].loopback;
            platform_unlock(&session->lock);
            return lb;
        }
    }
    platform_unlock(&session->lock);
    return false;
}

// ============================================================================
// SUBSCRIPTION CLEANUP — called before unregistering session from manager
// ============================================================================

static bool manager_has_other_subscriber(poseidon_channel_manager_t* mgr,
                                          const char* topic_path,
                                          const client_session_t* exclude) {
    // Snapshot session pointers to avoid holding manager->lock while
    // taking session->lock (prevents deadlock with dispatch path)
    vec_t(client_session_t*) snap;
    vec_init(&snap);
    platform_lock(&mgr->lock);
    for (size_t i = 0; i < mgr->num_sessions; i++) {
        client_session_t* s = (client_session_t*)mgr->sessions[i];
        if (s != NULL && s != exclude) vec_push(&snap, s);
    }
    platform_unlock(&mgr->lock);

    bool found = false;
    for (int i = 0; i < snap.length; i++) {
        if (client_session_is_subscribed(snap.data[i], topic_path)) {
            found = true;
            break;
        }
    }
    vec_deinit(&snap);
    return found;
}

void client_session_cleanup_subscriptions(client_session_t* session) {
    if (session == NULL || session->manager == NULL) return;

    vec_t(char*) topics;
    vec_init(&topics);

    platform_lock(&session->lock);
    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active) {
            char* topic = strdup(session->subscriptions[i].topic_path);
            if (topic != NULL) vec_push(&topics, topic);
        }
    }
    platform_unlock(&session->lock);

    for (int i = 0; i < topics.length; i++) {
        if (!manager_has_other_subscriber(session->manager, topics.data[i], session)) {
            poseidon_channel_t* channel = find_channel_by_topic(session->manager, topics.data[i]);
            if (channel != NULL) {
                poseidon_channel_unsubscribe(channel,
                    (const uint8_t*)topics.data[i], strlen(topics.data[i]));
            }
        }
        free(topics.data[i]);
    }
    vec_deinit(&topics);
}

// ============================================================================
// EVENT DISPATCH
// ============================================================================

static void session_dispatch_message(void* ctx, const uint8_t* topic, size_t topic_len,
                                      const char* subtopic, const uint8_t* data, size_t data_len) {
    poseidon_channel_manager_t* manager = (poseidon_channel_manager_t*)ctx;

    // Build null-terminated topic string
    char topic_str[CLIENT_MAX_TOPIC_PATH];
    size_t copy_len = topic_len < CLIENT_MAX_TOPIC_PATH - 1 ? topic_len : CLIENT_MAX_TOPIC_PATH - 1;
    memcpy(topic_str, topic, copy_len);
    topic_str[copy_len] = '\0';

    // Find the channel to get publisher_session for self-delivery filtering
    poseidon_channel_t* channel = find_channel_by_topic(manager, topic_str);
    void* publisher_session = (channel != NULL) ? channel->publisher_session : NULL;

    // Encode the event frame
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

    // Snapshot session list under manager lock to avoid holding
    // manager->lock while taking session->lock (prevents deadlock)
    vec_t(client_session_t*) snap;
    vec_init(&snap);
    platform_lock(&manager->lock);
    for (size_t i = 0; i < manager->num_sessions; i++) {
        client_session_t* s = (client_session_t*)manager->sessions[i];
        if (s != NULL) vec_push(&snap, s);
    }
    platform_unlock(&manager->lock);

    // Deliver to each subscribed session (no manager lock held)
    for (int i = 0; i < snap.length; i++) {
        client_session_t* s = snap.data[i];
        if (!client_session_is_subscribed(s, topic_str)) continue;
        if (s == publisher_session && !client_session_has_loopback(s, topic_str)) continue;
        if (s->transport != NULL && s->client_fd >= 0) {
            s->transport->send(s->transport, s->client_fd, buf, buf_len);
        }
    }
    vec_deinit(&snap);

    free(buf);
}

void client_session_dispatch_event(client_session_t* session,
                                    uint8_t event_type,
                                    const char* topic_id,
                                    const char* subtopic,
                                    const uint8_t* data, size_t data_len) {
    if (session == NULL || session->transport == NULL || session->client_fd < 0) return;

    cbor_item_t* event = client_protocol_encode_event(event_type, topic_id,
                                                        subtopic ? subtopic : "",
                                                        data, data_len);
    if (event == NULL) return;

    uint8_t* buf = NULL;
    size_t buf_len = 0;
    if (client_protocol_serialize(event, &buf, &buf_len) == 0) {
        session->transport->send(session->transport, session->client_fd, buf, buf_len);
        free(buf);
    }
    cbor_decref(&event);
}
