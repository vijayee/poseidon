//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_SESSION_H
#define POSEIDON_CLIENT_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "client_protocol.h"
#include "../Channel/channel_manager.h"
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_SESSION_MAX_SUBSCRIPTIONS 256

typedef struct client_subscription_t {
    char topic_path[CLIENT_MAX_TOPIC_PATH];
    bool active;
} client_subscription_t;

typedef struct client_session_t {
    refcounter_t refcounter;
    uint32_t session_id;
    int client_fd;
    poseidon_channel_manager_t* manager;
    client_subscription_t subscriptions[CLIENT_SESSION_MAX_SUBSCRIPTIONS];
    size_t num_subscriptions;
    uint32_t next_request_id;
    PLATFORMLOCKTYPE(lock);
} client_session_t;

client_session_t* client_session_create(uint32_t session_id,
                                         poseidon_channel_manager_t* manager);
void client_session_destroy(client_session_t* session);

int client_session_handle_request(client_session_t* session,
                                    const client_frame_t* frame,
                                    cbor_item_t** out);

int client_session_subscribe(client_session_t* session, const char* topic_path);
int client_session_unsubscribe(client_session_t* session, const char* topic_path);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_SESSION_H
