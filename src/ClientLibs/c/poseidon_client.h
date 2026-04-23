//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_H
#define POSEIDON_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct poseidon_key_pair_t poseidon_key_pair_t;
typedef struct poseidon_channel_config_t poseidon_channel_config_t;

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CLIENT TYPES
// ============================================================================

typedef struct poseidon_client_t poseidon_client_t;

typedef void (*poseidon_delivery_cb_t)(void* ctx, const char* topic_id,
                                        const char* subtopic,
                                        const uint8_t* data, size_t len);

typedef void (*poseidon_event_cb_t)(void* ctx, uint8_t event_type,
                                     const uint8_t* data, size_t len);

typedef void (*poseidon_response_cb_t)(void* ctx, uint32_t request_id,
                                        uint8_t error_code,
                                        const char* result_data);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

/**
 * Connects to a Poseidon daemon via transport URL.
 * Supported formats: "unix:///path/to/socket", "tcp://host:port"
 *
 * @param transport_url  Connection URL
 * @return              Connected client, or NULL on failure
 */
poseidon_client_t* poseidon_client_connect(const char* transport_url);

/**
 * Disconnects and destroys a client.
 *
 * @param client  Client to disconnect
 */
void poseidon_client_disconnect(poseidon_client_t* client);

// ============================================================================
// CHANNEL LIFECYCLE
// ============================================================================

int poseidon_client_channel_create(poseidon_client_t* client, const char* name,
                                    char* out_topic_id, size_t buf_size);
int poseidon_client_channel_join(poseidon_client_t* client, const char* topic_or_alias,
                                  char* out_topic_id, size_t buf_size);
int poseidon_client_channel_leave(poseidon_client_t* client, const char* topic_id);
int poseidon_client_channel_destroy(poseidon_client_t* client, const char* topic_id,
                                     const poseidon_key_pair_t* owner_key);
int poseidon_client_channel_modify(poseidon_client_t* client, const char* topic_id,
                                    const poseidon_channel_config_t* config,
                                    const poseidon_key_pair_t* owner_key);

// ============================================================================
// PUB/SUB
// ============================================================================

int poseidon_client_subscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_unsubscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_publish(poseidon_client_t* client, const char* topic_path,
                             const uint8_t* data, size_t len);

// ============================================================================
// ALIASES
// ============================================================================

int poseidon_client_alias_register(poseidon_client_t* client, const char* name,
                                    const char* topic_id);
int poseidon_client_alias_unregister(poseidon_client_t* client, const char* name);

// ============================================================================
// EVENTS
// ============================================================================

void poseidon_client_on_delivery(poseidon_client_t* client,
                                  poseidon_delivery_cb_t cb, void* ctx);
void poseidon_client_on_event(poseidon_client_t* client,
                               poseidon_event_cb_t cb, void* ctx);

void poseidon_client_on_response(poseidon_client_t* client,
                                  poseidon_response_cb_t cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_H