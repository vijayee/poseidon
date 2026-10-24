//
// Created by victor on 5/16/26.
//
// Shared QUIC stream infrastructure for Meridian P2P layer.
// Provides stream entry tracking and callback declarations
// for unidirectional and bidirectional stream handling.

#ifndef MERIDIAN_STREAM_H
#define MERIDIAN_STREAM_H

#include "msquic.h"
#include "meridian.h"
#include "../../Actor/actor.h"
#include "../../Scheduler/scheduler.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * How long the entry's close waits for the actor to work through the
 * queue: the message that says the peer finished sending must reach the
 * owner before the entry — and the buffer it points at — goes away.
 */
#define MERIDIAN_STREAM_TIMEOUT_MS 5000

#define MERIDIAN_STREAM_INITIAL_CAP MERIDIAN_MAX_PAYLOAD_SIZE
#define MERIDIAN_STREAM_MAX_CAP 65536
#define MERIDIAN_MAX_CONCURRENT_STREAMS 100

struct meridian_stream_entry_t;

/**
 * The handler that receives a completed message.
 *
 * It runs on the stream entry's actor, never on the msquic thread: the
 * receive buffer is the actor's, and the message is handed over rather
 * than decoded in place. The entry is passed along so the owner can
 * reach the stream handle.
 */
typedef void (*meridian_stream_message_cb_t)(void* user_context,
                                              struct meridian_stream_entry_t* entry,
                                              const uint8_t* data, size_t len);

/**
 * One Meridian stream: the msquic handle, the receive buffer and the
 * actor that owns them.
 *
 * The shape follows upstream's stream: the stream has an actor, and the
 * public entry points post messages to it rather than touching the
 * stream's state in place. The receive buffer is therefore touched by
 * exactly one thread — the entry's actor — and the decode runs there
 * too, not on msquic's worker.
 */
typedef struct meridian_stream_entry_t {
    HQUIC stream;
    actor_t actor;              /**< Owns the receive buffer */
    scheduler_pool_t* pool;     /**< Scheduler pool that runs the actor */
    uint8_t* recv_buf;          /**< Owned by the actor */
    size_t recv_len;            /**< Owned by the actor */
    size_t recv_cap;            /**< Owned by the actor */
    bool is_bidi;
    void* user_context;         /**< The component that owns this stream */
    meridian_stream_message_cb_t on_message;  /**< The owner's handler */
} meridian_stream_entry_t;

/**
 * Creates a stream entry with initial buffer capacity.
 *
 * The entry's actor needs a scheduler pool: the receive bytes and the
 * completed message are handed to it, and only a pool worker runs it.
 * Returns NULL on allocation failure.
 */
meridian_stream_entry_t* meridian_stream_entry_create(HQUIC stream, bool is_bidi,
                                                       void* user_context,
                                                       scheduler_pool_t* pool);

/**
 * Registers the handler that receives the completed message.
 *
 * The handler runs on the entry's actor.
 */
void meridian_stream_entry_on_message(meridian_stream_entry_t* entry,
                                      meridian_stream_message_cb_t callback);

/**
 * Destroys a stream entry, freeing its receive buffer.
 * Does NOT close the stream handle — call StreamClose separately.
 */
void meridian_stream_entry_destroy(meridian_stream_entry_t* entry);

/**
 * Hands received data to the stream entry's actor.
 *
 * The bytes are copied into the request: the caller's buffer is
 * msquic's, and it is released as soon as the callback returns. The
 * entry's actor appends them to the receive buffer.
 * Returns 0 on success, -1 if the request could not be made.
 */
int meridian_stream_entry_append(meridian_stream_entry_t* entry, const uint8_t* data, size_t len);

/**
 * Tells the stream entry's actor that the peer finished sending.
 *
 * The actor then hands the accumulated message to the owner's handler.
 */
void meridian_stream_entry_complete(meridian_stream_entry_t* entry);

/**
 * Resets the receive buffer after processing, keeping allocated capacity.
 */
void meridian_stream_entry_reset(meridian_stream_entry_t* entry);

/**
 * The context of one send.
 *
 * The QUIC_BUFFER travels with the bytes it points at: msquic stores a
 * pointer to the buffer and reads from it asynchronously, so both have
 * to stay alive until the SEND_COMPLETE event. Upstream packs them into
 * one context for the same reason
 * (liboffs/src/Network/quic_peer_send.c).
 */
typedef struct meridian_stream_send_context_t {
    uint8_t* data;      /**< The bytes being sent */
    QUIC_BUFFER buffer; /**< The buffer that points at them */
} meridian_stream_send_context_t;

/**
 * Sends data on a stream.
 *
 * The data is copied into a heap buffer that travels with the
 * QUIC_BUFFER until the SEND_COMPLETE event, so nothing the caller
 * passes in has to outlive the call.
 *
 * Returns 0 on success, -1 on failure.
 */
int meridian_stream_send(const struct QUIC_API_TABLE* msquic, HQUIC stream,
                         const uint8_t* data, size_t len, QUIC_SEND_FLAGS flags);

/**
 * Releases the context of a completed send.
 *
 * The stream's SEND_COMPLETE event hands the context back; this is what
 * frees the bytes and the buffer.
 */
void meridian_stream_send_complete(void* client_context);

/**
 * Opens a unidirectional stream on the given connection and sends data.
 * Sends the data with QUIC_SEND_FLAG_FIN to signal end-of-message.
 * Returns 0 on success, -1 on failure.
 */
int meridian_stream_send_uni(const struct QUIC_API_TABLE* msquic, HQUIC connection,
                              const uint8_t* data, size_t len, void* stream_context);

/**
 * Opens a bidirectional stream on the given connection.
 * Returns the stream handle on success, NULL on failure.
 * The caller should send data on the stream separately.
 */
HQUIC meridian_stream_open_bidi(const struct QUIC_API_TABLE* msquic, HQUIC connection,
                                 QUIC_STREAM_CALLBACK_HANDLER callback, void* context);

/**
 * Stream callback for one-way messages (gossip, QUASAR_GOSSIP, channel notices).
 * Accumulates data until FIN, then the connection-specific handler dispatches.
 */
QUIC_STATUS QUIC_API meridian_uni_stream_callback(HQUIC stream, void* context,
                                                    QUIC_STREAM_EVENT* event);

/**
 * Stream callback for request/response messages (PING/PONG, ADDR, BOOTSTRAP).
 * Accumulates data until FIN, then the connection-specific handler processes and responds.
 */
QUIC_STATUS QUIC_API meridian_bidi_stream_callback(HQUIC stream, void* context,
                                                     QUIC_STREAM_EVENT* event);

#endif // MERIDIAN_STREAM_H
