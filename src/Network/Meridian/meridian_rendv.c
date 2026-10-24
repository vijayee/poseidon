//
// Created by victor on 4/19/26.
//

#include "../../Platform/platform_thread.h"
#include "meridian_rendv.h"
#include "meridian_rendv_messages.h"

/* The dispatch is defined further down. */
static void meridian_rendv_dispatch(void* state, message_t* msg);
#include "meridian_relay.h"
#include "meridian_stream.h"
#include "../../Util/allocator.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ============================================================================
// RENDEZVOUS POINT LIFECYCLE
// ============================================================================

/**
 * Creates a rendezvous point with the given address and port.
 * Rendezvous points are used for NAT traversal and node discovery.
 *
 * @param addr  Rendezvous server IPv4 address
 * @param port  Rendezvous server port
 * @return      New rendezvous point with refcount=1, or NULL on failure
 */
meridian_rendv_t* meridian_rendv_create(uint32_t addr, uint16_t port) {
    meridian_rendv_t* rendv = (meridian_rendv_t*)
        get_clear_memory(sizeof(meridian_rendv_t));

    rendv->addr = addr;
    rendv->port = port;
    rendv->nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;
    rendv->is_active = false;

    refcounter_init(&rendv->refcounter);
    return rendv;
}

/**
 * Destroys a rendezvous point.
 *
 * @param rendv  Rendezvous to destroy
 */
void meridian_rendv_destroy(meridian_rendv_t* rendv) {
    if (rendv == NULL) return;

    refcounter_dereference(&rendv->refcounter);
    if (refcounter_count(&rendv->refcounter) == 0) {
        free(rendv);
    }
}

/**
 * Sets the NAT type of a rendezvous point.
 *
 * @param rendv  Rendezvous to update
 * @param type   NAT type enum value
 * @return       0 on success, -1 on failure
 */
int meridian_rendv_set_nat_type(meridian_rendv_t* rendv, meridian_nat_type_t type) {
    if (rendv == NULL) return -1;
    rendv->nat_type = type;
    return 0;
}

/**
 * Gets the NAT type of a rendezvous point.
 *
 * @param rendv  Rendezvous to query
 * @return       NAT type, or UNKNOWN if rendv is NULL
 */
meridian_nat_type_t meridian_rendv_get_nat_type(const meridian_rendv_t* rendv) {
    return rendv ? rendv->nat_type : MERIDIAN_NAT_TYPE_UNKNOWN;
}

uint32_t meridian_rendv_get_reflexive_addr(const meridian_rendv_t* rendv) {
    return rendv ? rendv->reflexive_addr : 0;
}

uint16_t meridian_rendv_get_reflexive_port(const meridian_rendv_t* rendv) {
    return rendv ? rendv->reflexive_port : 0;
}

int meridian_rendv_set_reflexive_addr(meridian_rendv_t* rendv, uint32_t addr, uint16_t port) {
    if (rendv == NULL) return -1;
    rendv->reflexive_addr = addr;
    rendv->reflexive_port = port;
    return 0;
}

bool meridian_rendv_can_direct_connect(const meridian_rendv_t* rendv) {
    if (rendv == NULL) return false;
    switch (rendv->nat_type) {
        case MERIDIAN_NAT_TYPE_OPEN:
        case MERIDIAN_NAT_TYPE_FULL_CONE:
        case MERIDIAN_NAT_TYPE_RESTRICTED_CONE:
        case MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE:
            return true;
        case MERIDIAN_NAT_TYPE_SYMMETRIC:
        case MERIDIAN_NAT_TYPE_UNKNOWN:
        default:
            return false;
    }
}

// ============================================================================
// RENDEZVOUS HANDLE
// ============================================================================

/**
 * Creates a handle for managing rendezvous operations.
 * Initializes vectors for peer rendezvous and active connections.
 *
 * @return  New handle with refcount=1, or NULL on failure
 */
meridian_rendv_handle_t* meridian_rendv_handle_create(scheduler_pool_t* pool) {
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)
        get_clear_memory(sizeof(meridian_rendv_handle_t));

    vec_init(&handle->peer_rendvs);
    vec_init(&handle->connections);
    handle->pool = pool;
    actor_init(&handle->actor, handle, meridian_rendv_dispatch, pool);
    refcounter_init(&handle->refcounter);
    handle->msquic = NULL;
    handle->registration = NULL;
    handle->configuration = NULL;

    return handle;
}

int meridian_rendv_handle_set_msquic(meridian_rendv_handle_t* handle,
                                    const struct QUIC_API_TABLE* msquic,
                                    HQUIC registration,
                                    const meridian_rendv_config_t* config) {
    if (handle == NULL) return -1;

    handle->msquic = msquic;
    handle->registration = registration;

    // Create QUIC configuration if config provided
    if (config != NULL && config->alpn != NULL) {
        QUIC_BUFFER Alpn = { (uint32_t)strlen(config->alpn), (uint8_t*)config->alpn };

        QUIC_SETTINGS Settings = {0};
        Settings.IdleTimeoutMs = config->idle_timeout_ms ? config->idle_timeout_ms : 30000;
        Settings.IsSet.IdleTimeoutMs = TRUE;
        Settings.PeerBidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
        Settings.IsSet.PeerBidiStreamCount = TRUE;
        Settings.PeerUnidiStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
        Settings.IsSet.PeerUnidiStreamCount = TRUE;

        QUIC_STATUS Status = handle->msquic->ConfigurationOpen(
            handle->registration,
            &Alpn, 1,
            &Settings, sizeof(Settings),
            NULL,
            &handle->configuration);

        if (QUIC_FAILED(Status)) {
            handle->configuration = NULL;
            return -1;
        }

        // Load credentials if provided
        if (config->cred_config != NULL) {
            Status = handle->msquic->ConfigurationLoadCredential(
                handle->configuration,
                config->cred_config);

            if (QUIC_FAILED(Status)) {
                handle->msquic->ConfigurationClose(handle->configuration);
                handle->configuration = NULL;
                return -1;
            }
        }
    }

    return 0;
}

/**
 * Destroys a rendezvous handle, releasing all resources.
 * Closes any open connection sockets and frees all rendezvous points.
 *
 * @param handle  Handle to destroy
 */
void meridian_rendv_handle_destroy(meridian_rendv_handle_t* handle) {
    if (handle == NULL) return;

    refcounter_dereference(&handle->refcounter);
    if (refcounter_count(&handle->refcounter) == 0) {
        // Destroy local rendezvous
        if (handle->local_rendv) {
            meridian_rendv_destroy(handle->local_rendv);
        }

        // Destroy all peer rendezvous
        for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
            if (handle->peer_rendvs.data[i]) {
                meridian_rendv_destroy(handle->peer_rendvs.data[i]);
            }
        }
        vec_deinit(&handle->peer_rendvs);

        // Close and free all connections
        for (size_t i = 0; i < handle->connections.length; i++) {
            if (handle->connections.data[i]) {
                if (handle->connections.data[i]->connection && handle->msquic) {
                    handle->msquic->ConnectionShutdown(
                        handle->connections.data[i]->connection,
                        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
                    handle->msquic->ConnectionClose(handle->connections.data[i]->connection);
                }
                free(handle->connections.data[i]);
            }
        }
        vec_deinit(&handle->connections);

        // Do NOT close configuration/registration here — they are borrowed from
        // the protocol. The protocol owns and closes them in stop/destroy.

        actor_destroy(&handle->actor);
        free(handle);
    }
}

void meridian_rendv_handle_on_stream_data(meridian_rendv_handle_t* handle,
                                           meridian_rendv_stream_data_cb_t callback,
                                           void* ctx) {
    if (handle == NULL) return;

    /* The callback belongs to the actor: the tunnel data is delivered
     * from there. */
    meridian_rendv_set_stream_cb_msg_t* request =
        (meridian_rendv_set_stream_cb_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_set_stream_cb_msg_t));
    if (request == NULL) return;
    request->callback = (void*)callback;
    request->ctx = ctx;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_SET_STREAM_CB,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return;
    }
    if (trip != 0) return;  /* the actor owns the request now */

    actor_reply_destroy(&request->reply);
    free(request);
}

/**
 * Sets the local rendezvous point for this handle.
 * Takes a reference to the provided rendezvous.
 *
 * @param handle  Handle to update
 * @param rendv   Local rendezvous point
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_set_local(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv) {
    if (handle == NULL || rendv == NULL) return -1;

    meridian_rendv_set_local_msg_t* request =
        (meridian_rendv_set_local_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_set_local_msg_t));
    if (request == NULL) return -1;
    request->rendv = rendv;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_SET_LOCAL,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
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

/**
 * Adds a peer rendezvous point to the handle.
 * Does nothing if the peer already exists (by addr:port).
 *
 * @param handle  Handle to add to
 * @param rendv   Peer rendezvous to add
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_add_peer(meridian_rendv_handle_t* handle, meridian_rendv_t* rendv) {
    if (handle == NULL || rendv == NULL) return -1;

    meridian_rendv_add_peer_msg_t* request =
        (meridian_rendv_add_peer_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_add_peer_msg_t));
    if (request == NULL) return -1;
    request->rendv = rendv;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_ADD_PEER,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
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

/**
 * Removes a peer rendezvous by address:port.
 *
 * @param handle  Handle to remove from
 * @param addr    Peer address to remove
 * @param port    Peer port to remove
 * @return        0 if found and removed, -1 if not found
 */
int meridian_rendv_handle_remove_peer(meridian_rendv_handle_t* handle,
                                      uint32_t addr, uint16_t port) {
    if (handle == NULL) return -1;

    meridian_rendv_remove_peer_msg_t* request =
        (meridian_rendv_remove_peer_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_remove_peer_msg_t));
    if (request == NULL) return -1;
    request->addr = addr;
    request->port = port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_REMOVE_PEER,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
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

/**
 * Finds a peer rendezvous by address:port.
 * Returns a new reference that caller must eventually destroy.
 *
 * @param handle  Handle to search
 * @param addr    Address to find
 * @param port    Port to find
 * @return        Peer rendezvous with new reference, or NULL if not found
 */
meridian_rendv_t* meridian_rendv_handle_find_peer(meridian_rendv_handle_t* handle,
                                                  uint32_t addr, uint16_t port) {
    if (handle == NULL) return NULL;

    meridian_rendv_find_peer_msg_t* request =
        (meridian_rendv_find_peer_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_find_peer_msg_t));
    if (request == NULL) return NULL;
    request->addr = addr;
    request->port = port;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return NULL;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_FIND_PEER,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return NULL;
    }
    if (trip != 0) return NULL;  /* the actor owns the request now */

    meridian_rendv_t* peer = (meridian_rendv_t*)request->out_peer;
    actor_reply_destroy(&request->reply);
    free(request);
    return peer;
}

// ============================================================================
// ACTOR DISPATCH
// ============================================================================

/* Classifies the local NAT type from the reflexive address the relay
 * reported. Defined below the dispatch that uses it. */
static meridian_nat_type_t rendv_classify_nat(meridian_rendv_handle_t* handle);

/**
 * The rendezvous handle's actor owns the handle's state.
 *
 * Upstream's nat_detect has the same shape: an actor, a scheduler pool,
 * the state in the struct and no mutex. The tunnel data arrives on
 * msquic's worker threads and is handed to the actor, which delivers it
 * to the application callback.
 */
static void meridian_rendv_dispatch(void* state, message_t* msg) {
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)state;
    if (msg == NULL) return;

    switch (msg->type) {
        case MERIDIAN_RENDV_SET_LOCAL: {
            meridian_rendv_set_local_msg_t* p =
                (meridian_rendv_set_local_msg_t*)msg->payload;
            if (handle->local_rendv) {
                refcounter_dereference(&handle->local_rendv->refcounter);
                if (refcounter_count(&handle->local_rendv->refcounter) == 0) {
                    free(handle->local_rendv);
                }
            }
            handle->local_rendv = (meridian_rendv_t*)
                refcounter_reference(&((meridian_rendv_t*)p->rendv)->refcounter);
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_ADD_PEER: {
            meridian_rendv_add_peer_msg_t* p =
                (meridian_rendv_add_peer_msg_t*)msg->payload;
            meridian_rendv_t* rendv = (meridian_rendv_t*)p->rendv;
            bool known = false;
            for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
                if (handle->peer_rendvs.data[i]->addr == rendv->addr &&
                    handle->peer_rendvs.data[i]->port == rendv->port) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                vec_push(&handle->peer_rendvs,
                         (meridian_rendv_t*)refcounter_reference(&rendv->refcounter));
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_REMOVE_PEER: {
            meridian_rendv_remove_peer_msg_t* p =
                (meridian_rendv_remove_peer_msg_t*)msg->payload;
            for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
                if (handle->peer_rendvs.data[i]->addr == p->addr &&
                    handle->peer_rendvs.data[i]->port == p->port) {
                    refcounter_dereference(&handle->peer_rendvs.data[i]->refcounter);
                    if (refcounter_count(&handle->peer_rendvs.data[i]->refcounter) == 0) {
                        free(handle->peer_rendvs.data[i]);
                    }
                    vec_splice(&handle->peer_rendvs, i, 1);
                    break;
                }
            }
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_FIND_PEER: {
            meridian_rendv_find_peer_msg_t* p =
                (meridian_rendv_find_peer_msg_t*)msg->payload;
            p->out_peer = NULL;
            for (size_t i = 0; i < handle->peer_rendvs.length; i++) {
                if (handle->peer_rendvs.data[i]->addr == p->addr &&
                    handle->peer_rendvs.data[i]->port == p->port) {
                    p->out_peer = (meridian_rendv_t*)refcounter_reference(
                        &handle->peer_rendvs.data[i]->refcounter);
                    break;
                }
            }
            if (actor_reply_complete(&p->reply)) {
                if (p->out_peer != NULL) {
                    refcounter_dereference(&((meridian_rendv_t*)p->out_peer)->refcounter);
                }
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_SET_STREAM_CB: {
            meridian_rendv_set_stream_cb_msg_t* p =
                (meridian_rendv_set_stream_cb_msg_t*)msg->payload;
            handle->on_stream_data =
                (meridian_rendv_stream_data_cb_t*)p->callback;
            handle->on_stream_data_ctx = p->ctx;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_STREAM_DATA: {
            meridian_rendv_stream_data_msg_t* p =
                (meridian_rendv_stream_data_msg_t*)msg->payload;
            if (handle->on_stream_data != NULL) {
                handle->on_stream_data(handle->on_stream_data_ctx, p->data,
                                       p->len, p->is_bidi);
            }
            free(p->data);
            p->data = NULL;
            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_DETECT_NAT: {
            meridian_rendv_detect_nat_msg_t* p =
                (meridian_rendv_detect_nat_msg_t*)msg->payload;
            p->reflexive_addr = 0;
            p->reflexive_port = 0;
            p->nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;

            if (handle->local_rendv != NULL) {
                if (p->relay_a != NULL) {
                    meridian_relay_send_addr_request(
                        (struct meridian_relay_t*)p->relay_a);
                }
                if (p->relay_b != NULL) {
                    meridian_relay_send_addr_request(
                        (struct meridian_relay_t*)p->relay_b);
                }

                uint32_t reflexive_addr =
                    meridian_rendv_get_reflexive_addr(handle->local_rendv);
                uint16_t reflexive_port =
                    meridian_rendv_get_reflexive_port(handle->local_rendv);

                p->reflexive_addr = reflexive_addr;
                p->reflexive_port = reflexive_port;
                p->nat_type = (int)rendv_classify_nat(handle);
            }

            if (actor_reply_complete(&p->reply)) {
                actor_reply_destroy(&p->reply);
                free(p);
            }
            break;
        }
        case MERIDIAN_RENDV_ADDR_RESPONSE: {
            meridian_rendv_addr_response_msg_t* p =
                (meridian_rendv_addr_response_msg_t*)msg->payload;
            /* A relay saw this node's address from the outside. That is
             * the only view of it the classification can use, and it is
             * the reason the relay hands the response over as an event
             * rather than a callback: this runs on the actor that owns
             * the state. */
            if (handle->local_rendv != NULL) {
                meridian_rendv_set_reflexive_addr(handle->local_rendv,
                                                  p->reflexive_addr,
                                                  p->reflexive_port);
                (void)rendv_classify_nat(handle);
            }
            free(p);
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// TUNNEL STREAM CALLBACKS
// ============================================================================

/**
 * Classifies the local NAT type from the reflexive address the relay
 * reported, and records it on the local rendezvous point.
 *
 * The classification runs on the handle's actor: the reflexive address
 * is the handle's state.
 */
static meridian_nat_type_t rendv_classify_nat(meridian_rendv_handle_t* handle) {
    if (handle == NULL || handle->local_rendv == NULL) {
        return MERIDIAN_NAT_TYPE_UNKNOWN;
    }

    uint32_t reflexive_addr =
        meridian_rendv_get_reflexive_addr(handle->local_rendv);
    uint32_t local_addr = handle->local_rendv->addr;

    meridian_nat_type_t nat_type = MERIDIAN_NAT_TYPE_UNKNOWN;
    if (local_addr != 0 && local_addr == reflexive_addr) {
        nat_type = MERIDIAN_NAT_TYPE_OPEN;
    } else if (reflexive_addr != 0) {
        nat_type = MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE;
    }
    handle->local_rendv->nat_type = nat_type;
    return nat_type;
}

/**
 * The owner handler for a tunnel stream.
 *
 * It runs on the stream entry's actor and hands the data to the
 * application callback the handle was given.
 */
static void rendv_stream_message(void* ctx, meridian_stream_entry_t* entry,
                                 const uint8_t* data, size_t len) {
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)ctx;
    if (handle == NULL) return;

    /* The registered callback is the handle actor's state, so the data
     * is handed over to that actor rather than called from here. */
    meridian_rendv_stream_data_msg_t* request =
        (meridian_rendv_stream_data_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_stream_data_msg_t));
    if (request == NULL) return;

    request->len = len;
    request->is_bidi = (entry != NULL && entry->is_bidi);
    request->data = (uint8_t*)get_memory(len > 0 ? len : 1);
    if (request->data == NULL) {
        free(request);
        return;
    }
    if (len > 0) {
        memcpy(request->data, data, len);
    }

    if (actor_reply_init(&request->reply) != 0) {
        free(request->data);
        free(request);
        return;
    }
    /* The caller does not wait: the request is the actor's to free. */
    request->reply.orphaned = 1;
    message_t msg = {
        .type = MERIDIAN_RENDV_STREAM_DATA,
        .payload = request,
        .payload_destroy = NULL,
    };
    /* The handle's actor owns the request and its bytes: it frees them
     * when it has handed the data on. A destroyed actor is the only one
     * that drops the message — actor_send's return value says whether
     * the mailbox was empty, not whether the message was taken
     * (src/Actor/actor_reply.c). */
    if (atomic_load(&handle->actor.flags) & ACTOR_FLAG_DESTROY) {
        actor_reply_destroy(&request->reply);
        free(request->data);
        free(request);
        return;
    }
    actor_send(&handle->actor, &msg);
}

/**
 * Unidirectional stream callback for rendezvous tunnel connections.
 * Accumulates data until PEER_SEND_SHUTDOWN, then dispatches to
 * the registered on_stream_data callback.
 */
static QUIC_STATUS QUIC_API
rendv_uni_stream_callback(HQUIC Stream, void* Context,
                           QUIC_STREAM_EVENT* Event) {
    (void)Stream;
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)Context;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = Event->RECEIVE.Buffers;
        uint32_t buffer_count = Event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("meridian rendv: uni stream buffer overflow, dropping data");
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
        // The entry's actor hands the message to the owner.
        meridian_stream_entry_complete(entry);
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_rendv_handle_t* handle = (entry != NULL && entry->user_context != NULL)
            ? (meridian_rendv_handle_t*)entry->user_context : NULL;
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        if (Stream != NULL && handle != NULL && handle->msquic != NULL) {
            handle->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

/**
 * Bidirectional stream callback for rendezvous tunnel connections.
 * Accumulates data until PEER_SEND_SHUTDOWN, then dispatches to
 * the registered on_stream_data callback and shuts down our send side.
 */
static QUIC_STATUS QUIC_API
rendv_bidi_stream_callback(HQUIC Stream, void* Context,
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
                log_warn("meridian rendv: bidi stream buffer overflow, dropping data");
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
        // The entry's actor hands the message to the owner.
        meridian_stream_entry_complete(entry);
        if (Stream != NULL && entry != NULL && entry->user_context != NULL) {
            meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)entry->user_context;
            handle->msquic->StreamShutdown(
                Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (Event->SEND_COMPLETE.ClientContext != NULL) {
            free(Event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        meridian_rendv_handle_t* handle = (entry != NULL && entry->user_context != NULL)
            ? (meridian_rendv_handle_t*)entry->user_context : NULL;
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        if (Stream != NULL && handle != NULL && handle->msquic != NULL) {
            handle->msquic->StreamClose(Stream);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// TUNNEL CONNECTION CALLBACK
// ============================================================================

/**
 * Connection callback for rendezvous tunnel connections.
 * Handles stream events from remote peers, routing incoming streams
 * to the appropriate stream callback (bidi or uni).
 */
static QUIC_STATUS QUIC_API rendv_tunnel_connection_callback(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    (void)Connection;
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        log_info("meridian rendv: tunnel connected");
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC Stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = !(Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL);

        meridian_stream_entry_t* entry = meridian_stream_entry_create(
            Stream, is_bidi, handle, handle->pool);
        if (entry != NULL) {
            meridian_stream_entry_on_message(entry, rendv_stream_message);
        }
        if (entry == NULL) return QUIC_STATUS_SUCCESS;

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? rendv_bidi_stream_callback
            : rendv_uni_stream_callback;
        handle->msquic->SetCallbackHandler(Stream, (void*)callback, entry);
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        log_info("meridian rendv: tunnel connection closed");
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// TUNNEL MANAGEMENT
// ============================================================================

/**
 * Creates a QUIC tunnel to a peer rendezvous point.
 * Used for hole-punching and direct communication.
 *
 * @param handle  Handle to create tunnel from
 * @param peer    Peer to connect to
 * @param out_conn  Output: QUIC connection handle for the tunnel
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_create_tunnel(meridian_rendv_handle_t* handle,
                                         meridian_rendv_t* peer,
                                         HQUIC* out_conn) {
    if (handle == NULL || peer == NULL || out_conn == NULL) return -1;
    if (handle->msquic == NULL || handle->registration == NULL) return -1;

    *out_conn = NULL;

    // Create QUIC connection
    HQUIC Connection;
    QUIC_STATUS Status = handle->msquic->ConnectionOpen(
        handle->registration,
        (void*)rendv_tunnel_connection_callback,
        handle,
        &Connection);

    if (QUIC_FAILED(Status)) {
        return -1;
    }

    // Build remote address
    QUIC_ADDR RemoteAddr = {0};
    RemoteAddr.Ipv4.sin_family = AF_INET;
    RemoteAddr.Ipv4.sin_addr.s_addr = peer->addr;
    RemoteAddr.Ipv4.sin_port = peer->port;

    // Start connection - use configuration if set, otherwise NULL for defaults
    Status = handle->msquic->ConnectionStart(
        Connection,
        handle->configuration,
        AF_INET,
        NULL,  // Let QUIC resolve
        peer->port);

    if (QUIC_FAILED(Status)) {
        handle->msquic->ConnectionClose(Connection);
        return -1;
    }

    *out_conn = Connection;
    return 0;
}

/**
 * Closes a QUIC tunnel connection.
 *
 * @param handle  Handle
 * @param conn    QUIC connection to close
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_handle_close_tunnel(meridian_rendv_handle_t* handle, HQUIC conn) {
    if (handle == NULL || conn == NULL) return -1;
    if (handle->msquic == NULL) return -1;

    handle->msquic->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    handle->msquic->ConnectionClose(conn);
    return 0;
}

// ============================================================================
// NAT DETECTION
// ============================================================================

/**
 * Detects NAT type for the local rendezvous.
 * Detects NAT type by comparing reflexive addresses from relay servers.
 * Sends ADDR_REQUEST to each relay, compares observed public addresses,
 * and classifies NAT type accordingly.
 *
 * @param handle    Handle to detect NAT for
 * @param relay_a   First relay client (may be NULL)
 * @param relay_b   Second relay client (may be NULL)
 * @param callback  Function to receive NAT info
 * @param ctx       User context for callback
 * @return          0 on success, -1 on failure
 */
int meridian_rendv_handle_detect_nat(meridian_rendv_handle_t* handle,
                                      struct meridian_relay_t* relay_a,
                                      struct meridian_relay_t* relay_b,
                                      meridian_nat_callback_t callback, void* ctx) {
    if (handle == NULL || callback == NULL) return -1;

    /* The local rendezvous belongs to the handle's actor: it sends the
     * address requests, reads the reflexive address and classifies. */
    meridian_rendv_detect_nat_msg_t* request =
        (meridian_rendv_detect_nat_msg_t*)
        get_clear_memory(sizeof(meridian_rendv_detect_nat_msg_t));
    if (request == NULL) return -1;
    request->relay_a = relay_a;
    request->relay_b = relay_b;

    if (actor_reply_init(&request->reply) != 0) {
        free(request);
        return -1;
    }

    int trip = actor_round_trip(&handle->actor, MERIDIAN_RENDV_DETECT_NAT,
                                &request->reply, MERIDIAN_RENDV_TIMEOUT_MS);
    if (trip == 1) {
        actor_reply_destroy(&request->reply);
        free(request);
        return -1;
    }
    if (trip != 0) return -1;  /* the actor owns the request now */

    uint32_t reflexive_addr = request->reflexive_addr;
    uint16_t reflexive_port = request->reflexive_port;
    meridian_nat_type_t result_type = (meridian_nat_type_t)request->nat_type;
    actor_reply_destroy(&request->reply);
    free(request);

    callback(ctx, reflexive_addr, reflexive_port, result_type);
    return 0;
}

// ============================================================================
// HOLE PUNCHING
// ============================================================================

/**
 * Sends a hole-punching packet to a target rendezvous.
 * This initiates NAT hole-punching for peer-to-peer connection.
 *
 * @param handle  Handle to send from
 * @param target  Target rendezvous to punch
 * @return        0 on success, -1 on failure
 */
int meridian_rendv_send_hole_punch(meridian_rendv_handle_t* handle,
                                    const meridian_rendv_t* target) {
    if (handle == NULL || target == NULL) return -1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_addr.s_addr = target->addr;
    target_addr.sin_port = target->port;

    const char* punch_msg = "PUNCH";
    ssize_t sent = sendto(sock, punch_msg, strlen(punch_msg), 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));

    close(sock);

    return (sent == (ssize_t)strlen(punch_msg)) ? 0 : -1;
}

/**
 * Receives a hole-punching packet and reports source.
 *
 * Note: QUIC doesn't use UDP hole-punching. NAT traversal in QUIC is handled
 * by establishing connections through the rendezvous server directly.
 * This function is a no-op and always returns 0.
 *
 * @param handle     Handle to receive on (unused)
 * @param src_addr   Output: source address of punching peer (unchanged)
 * @param src_port   Output: source port of punching peer (unchanged)
 * @return           0 (always succeeds, no-op)
 */
int meridian_rendv_recv_hole_punch(meridian_rendv_handle_t* handle,
                                     uint32_t* src_addr, uint16_t* src_port) {
    (void)handle;
    (void)src_addr;
    (void)src_port;
    if (handle == NULL || src_addr == NULL || src_port == NULL) return -1;
    return -1;  // Placeholder - not yet implemented
}

// ============================================================================
// NAT TYPE UTILITIES
// ============================================================================

/**
 * Converts a string to a NAT type enum.
 *
 * @param str  String like "open", "full_cone", "symmetric", etc.
 * @return     Corresponding NAT type, or UNKNOWN if unrecognized
 */
meridian_nat_type_t meridian_nat_type_from_string(const char* str) {
    if (str == NULL) return MERIDIAN_NAT_TYPE_UNKNOWN;

    if (strcmp(str, "open") == 0) return MERIDIAN_NAT_TYPE_OPEN;
    if (strcmp(str, "full_cone") == 0) return MERIDIAN_NAT_TYPE_FULL_CONE;
    if (strcmp(str, "restricted_cone") == 0) return MERIDIAN_NAT_TYPE_RESTRICTED_CONE;
    if (strcmp(str, "port_restricted") == 0) return MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE;
    if (strcmp(str, "symmetric") == 0) return MERIDIAN_NAT_TYPE_SYMMETRIC;

    return MERIDIAN_NAT_TYPE_UNKNOWN;
}

/**
 * Converts a NAT type enum to a string.
 *
 * @param type  NAT type to convert
 * @return      String representation, or "unknown" for unrecognized
 */
const char* meridian_nat_type_to_string(meridian_nat_type_t type) {
    switch (type) {
        case MERIDIAN_NAT_TYPE_OPEN: return "open";
        case MERIDIAN_NAT_TYPE_FULL_CONE: return "full_cone";
        case MERIDIAN_NAT_TYPE_RESTRICTED_CONE: return "restricted_cone";
        case MERIDIAN_NAT_TYPE_PORT_RESTRICTED_CONE: return "port_restricted";
        case MERIDIAN_NAT_TYPE_SYMMETRIC: return "symmetric";
        default: return "unknown";
    }
}