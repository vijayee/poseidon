//
// Created by victor on 5/16/26.
//

#include "meridian_stream.h"
#include "../../Actor/actor_reply.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// THE ENTRY'S ACTOR
// ============================================================================

/* The bytes that arrived. The request owns them: the caller's buffer is
 * msquic's and is released as soon as the callback returns. */
typedef struct meridian_stream_receive_msg_t {
    uint8_t* data;
    size_t len;
} meridian_stream_receive_msg_t;

/* The peer finished sending: the actor hands the accumulated message to
 * the owner. */
typedef struct meridian_stream_complete_msg_t {
    int unused;
} meridian_stream_complete_msg_t;

/* The entry is going away. The actor answers once it has worked through
 * everything that was queued before it. */
typedef struct meridian_stream_close_msg_t {
    actor_reply_t reply;
} meridian_stream_close_msg_t;

static void meridian_stream_dispatch(void* state, message_t* msg) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)state;
    if (msg == NULL) return;

    switch (msg->type) {
        case MERIDIAN_STREAM_RECEIVE: {
            meridian_stream_receive_msg_t* p =
                (meridian_stream_receive_msg_t*)msg->payload;
            /* The buffer is the actor's: only this thread touches it. */
            if (entry->recv_len + p->len > MERIDIAN_STREAM_MAX_CAP) {
                log_warn("meridian stream: append would exceed max capacity "
                         "(%zu + %zu > %zu)", entry->recv_len, p->len,
                         (size_t)MERIDIAN_STREAM_MAX_CAP);
                break;
            }
            if (entry->recv_len + p->len > entry->recv_cap) {
                size_t new_cap = entry->recv_cap;
                while (new_cap < entry->recv_len + p->len) {
                    new_cap *= 2;
                }
                if (new_cap > MERIDIAN_STREAM_MAX_CAP) {
                    new_cap = MERIDIAN_STREAM_MAX_CAP;
                }
                uint8_t* new_buf = (uint8_t*)realloc(entry->recv_buf, new_cap);
                if (new_buf == NULL) {
                    log_error("meridian stream: failed to grow recv buffer to %zu bytes",
                              new_cap);
                    break;
                }
                memset(new_buf + entry->recv_cap, 0, new_cap - entry->recv_cap);
                entry->recv_buf = new_buf;
                entry->recv_cap = new_cap;
            }
            memcpy(entry->recv_buf + entry->recv_len, p->data, p->len);
            entry->recv_len += p->len;
            break;
        }
        case MERIDIAN_STREAM_COMPLETE: {
            /* The owner gets the message on this thread: the decode does
             * not run on msquic's worker. */
            if (entry->on_message != NULL) {
                entry->on_message(entry->user_context, entry, entry->recv_buf,
                                  entry->recv_len);
            }
            entry->recv_len = 0;
            break;
        }
        case MERIDIAN_STREAM_CLOSE: {
            meridian_stream_close_msg_t* p =
                (meridian_stream_close_msg_t*)msg->payload;
            /* Everything that was queued before this ran first. */
            actor_reply_complete(&p->reply);
            break;
        }
        default:
            break;
    }
}

/* Releases a receive request: the bytes and the request itself. */
static void meridian_stream_receive_destroy(void* payload) {
    meridian_stream_receive_msg_t* p = (meridian_stream_receive_msg_t*)payload;
    if (p == NULL) return;
    free(p->data);
    free(p);
}

// ============================================================================
// STREAM ENTRY LIFECYCLE
// ============================================================================

meridian_stream_entry_t* meridian_stream_entry_create(HQUIC stream, bool is_bidi,
                                                       void* user_context,
                                                       scheduler_pool_t* pool) {
    if (stream == NULL) return NULL;

    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)
        get_clear_memory(sizeof(meridian_stream_entry_t));
    if (entry == NULL) return NULL;

    entry->stream = stream;
    entry->is_bidi = is_bidi;
    entry->user_context = user_context;
    entry->pool = pool;
    entry->recv_cap = MERIDIAN_STREAM_INITIAL_CAP;
    entry->recv_buf = (uint8_t*)get_clear_memory(entry->recv_cap);
    if (entry->recv_buf == NULL) {
        free(entry);
        return NULL;
    }
    entry->recv_len = 0;

    /* The stream's actor owns the receive buffer. */
    actor_init(&entry->actor, entry, meridian_stream_dispatch, pool);

    return entry;
}

void meridian_stream_entry_on_message(meridian_stream_entry_t* entry,
                                      meridian_stream_message_cb_t callback) {
    if (entry == NULL) return;
    entry->on_message = callback;
}

void meridian_stream_entry_destroy(meridian_stream_entry_t* entry) {
    if (entry == NULL) return;

    /* The message that says the peer finished sending must reach the
     * owner before the entry — and the buffer it points at — goes away.
     * The close request goes through the same queue, so by the time it
     * is answered everything before it has been dispatched. */
    meridian_stream_close_msg_t* request =
        (meridian_stream_close_msg_t*)
        get_clear_memory(sizeof(meridian_stream_close_msg_t));
    if (request != NULL && actor_reply_init(&request->reply) == 0) {
        message_t msg = {
            .type = MERIDIAN_STREAM_CLOSE,
            .payload = request,
            .payload_destroy = NULL,
        };
        if (actor_send(&entry->actor, &msg)) {
            /* Wait for the actor to work through the queue. */
            actor_reply_wait(&request->reply, MERIDIAN_STREAM_TIMEOUT_MS);
        }
        actor_reply_destroy(&request->reply);
        free(request);
    }

    actor_destroy(&entry->actor);

    if (entry->recv_buf != NULL) {
        free(entry->recv_buf);
        entry->recv_buf = NULL;
    }
    free(entry);
}

// ============================================================================
// STREAM ENTRY BUFFER OPS
// ============================================================================

int meridian_stream_entry_append(meridian_stream_entry_t* entry,
                                  const uint8_t* data, size_t len) {
    if (entry == NULL || data == NULL) return -1;
    if (len == 0) return 0;

    /* The bytes are copied into the request: the caller's buffer is
     * msquic's and is released as soon as the callback returns. */
    meridian_stream_receive_msg_t* request =
        (meridian_stream_receive_msg_t*)
        get_clear_memory(sizeof(meridian_stream_receive_msg_t));
    if (request == NULL) return -1;
    request->data = (uint8_t*)get_memory(len);
    if (request->data == NULL) {
        free(request);
        return -1;
    }
    memcpy(request->data, data, len);
    request->len = len;

    message_t msg = {
        .type = MERIDIAN_STREAM_RECEIVE,
        .payload = request,
        .payload_destroy = meridian_stream_receive_destroy,
    };
    if (!actor_send(&entry->actor, &msg)) {
        /* The actor is gone: it released the request and its bytes. */
        return -1;
    }
    return 0;
}

void meridian_stream_entry_complete(meridian_stream_entry_t* entry) {
    if (entry == NULL) return;

    message_t msg = {
        .type = MERIDIAN_STREAM_COMPLETE,
        .payload = NULL,
        .payload_destroy = NULL,
    };
    if (!actor_send(&entry->actor, &msg)) {
        return;
    }
}

void meridian_stream_entry_reset(meridian_stream_entry_t* entry) {
    if (entry == NULL) return;
    entry->recv_len = 0;
}

// ============================================================================
// UNIDIRECTIONAL STREAM SEND
// ============================================================================

int meridian_stream_send_uni(const struct QUIC_API_TABLE* msquic, HQUIC connection,
                              const uint8_t* data, size_t len, void* stream_context) {
    if (msquic == NULL || connection == NULL || data == NULL || len == 0) return -1;

    HQUIC stream = NULL;
    QUIC_STATUS status = msquic->StreamOpen(connection,
                                              QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                              meridian_uni_stream_callback,
                                              stream_context,
                                              &stream);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamOpen (uni) failed: 0x%x", status);
        return -1;
    }

    status = msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamStart (uni) failed: 0x%x", status);
        msquic->StreamClose(stream);
        return -1;
    }

    // Copy data into a send buffer that we free in SEND_COMPLETE
    uint8_t* send_buf = (uint8_t*)get_memory(len);
    if (send_buf == NULL) {
        log_error("meridian stream: failed to allocate send buffer (%zu bytes)", len);
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        return -1;
    }
    memcpy(send_buf, data, len);

    QUIC_BUFFER buf;
    buf.Buffer = send_buf;
    buf.Length = (uint32_t)len;

    status = msquic->StreamSend(stream, &buf, 1, QUIC_SEND_FLAG_FIN, send_buf);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamSend (uni) failed: 0x%x", status);
        free(send_buf);
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        return -1;
    }

    return 0;
}

// ============================================================================
// BIDIRECTIONAL STREAM OPEN
// ============================================================================

HQUIC meridian_stream_open_bidi(const struct QUIC_API_TABLE* msquic, HQUIC connection,
                                 QUIC_STREAM_CALLBACK_HANDLER callback, void* context) {
    if (msquic == NULL || connection == NULL || callback == NULL) return NULL;

    HQUIC stream = NULL;
    QUIC_STATUS status = msquic->StreamOpen(connection,
                                              QUIC_STREAM_OPEN_FLAG_NONE,
                                              callback,
                                              context,
                                              &stream);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamOpen (bidi) failed: 0x%x", status);
        return NULL;
    }

    status = msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamStart (bidi) failed: 0x%x", status);
        msquic->StreamClose(stream);
        return NULL;
    }

    return stream;
}

// ============================================================================
// UNIDIRECTIONAL STREAM CALLBACK
// ============================================================================

QUIC_STATUS QUIC_API meridian_uni_stream_callback(HQUIC stream, void* context,
                                                    QUIC_STREAM_EVENT* event) {
    (void)stream;
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Outbound uni streams (sender side) have context=NULL — no data to receive
        if (entry == NULL) {
            break;
        }

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("meridian stream: uni receive buffer overflow, dropping data");
                break;
            }
        }

        // Acknowledge all received bytes
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // For inbound streams, the peer has finished sending.
        // Protocol-specific handlers inspect recv_buf via user_context.
        if (entry != NULL && entry->user_context != NULL) {
            // The component that owns this stream will process the accumulated data
            // via its own callback set through SetCallbackHandler.
        }
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // Free the send buffer that was passed as ClientContext to StreamSend
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// BIDIRECTIONAL STREAM CALLBACK
// ============================================================================

QUIC_STATUS QUIC_API meridian_bidi_stream_callback(HQUIC stream, void* context,
                                                     QUIC_STREAM_EVENT* event) {
    (void)stream;
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        // Stream is ready for sending. No action needed — the caller
        // will send data on the stream handle when ready.
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) break;

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer,
                                              buffers[i].Length) != 0) {
                log_warn("meridian stream: bidi receive buffer overflow, dropping data");
                break;
            }
        }

        // Acknowledge all received bytes
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Peer finished sending. The bidi stream remains open for our response.
        if (entry != NULL && entry->user_context != NULL) {
            // Protocol-specific handlers process recv_buf and send a response
            // via their own callback set through SetCallbackHandler.
        }
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
