//
// Created by victor on 4/22/26.
//

#include "client_session.h"
#include "../Util/allocator.h"
#include "../Util/threadding.h"
#include <string.h>

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
            int rc = client_session_subscribe(session, frame->topic_path);
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
            int rc = poseidon_channel_publish(channel,
                                               (const uint8_t*)frame->topic_path,
                                               strlen(frame->topic_path),
                                               frame->payload, frame->payload_len);
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

        default:
            *out = client_protocol_encode_response(frame->request_id,
                                                    CLIENT_ERROR_UNKNOWN_METHOD, "");
            return -1;
    }
}

// ============================================================================
// SUBSCRIPTION MANAGEMENT
// ============================================================================

int client_session_subscribe(client_session_t* session, const char* topic_path) {
    if (session == NULL || topic_path == NULL) return -1;

    platform_lock(&session->lock);

    for (size_t i = 0; i < session->num_subscriptions; i++) {
        if (session->subscriptions[i].active &&
            strcmp(session->subscriptions[i].topic_path, topic_path) == 0) {
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
    session->num_subscriptions++;

    platform_unlock(&session->lock);
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
            return 0;
        }
    }

    platform_unlock(&session->lock);
    return -1;
}
