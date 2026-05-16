//
// Created by victor on 5/16/26.
//

#include "meridian_stream.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// STREAM ENTRY LIFECYCLE
// ============================================================================

meridian_stream_entry_t* meridian_stream_entry_create(HQUIC stream, bool is_bidi,
                                                       void* user_context) {
    if (stream == NULL) return NULL;

    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)
        get_clear_memory(sizeof(meridian_stream_entry_t));
    if (entry == NULL) return NULL;

    entry->stream = stream;
    entry->is_bidi = is_bidi;
    entry->user_context = user_context;
    entry->recv_cap = MERIDIAN_STREAM_INITIAL_CAP;
    entry->recv_buf = (uint8_t*)get_clear_memory(entry->recv_cap);
    if (entry->recv_buf == NULL) {
        free(entry);
        return NULL;
    }
    entry->recv_len = 0;

    return entry;
}

void meridian_stream_entry_destroy(meridian_stream_entry_t* entry) {
    if (entry == NULL) return;

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

    // Reject outright if it exceeds max capacity
    if (entry->recv_len + len > MERIDIAN_STREAM_MAX_CAP) {
        log_warn("meridian stream: append would exceed max capacity (%zu + %zu > %zu)",
                 entry->recv_len, len, (size_t)MERIDIAN_STREAM_MAX_CAP);
        return -1;
    }

    // Grow buffer by doubling until it fits
    if (entry->recv_len + len > entry->recv_cap) {
        size_t new_cap = entry->recv_cap;
        while (new_cap < entry->recv_len + len) {
            new_cap *= 2;
        }
        if (new_cap > MERIDIAN_STREAM_MAX_CAP) {
            new_cap = MERIDIAN_STREAM_MAX_CAP;
        }

        uint8_t* new_buf = (uint8_t*)realloc(entry->recv_buf, new_cap);
        if (new_buf == NULL) {
            log_error("meridian stream: failed to grow recv buffer to %zu bytes", new_cap);
            return -1;
        }
        // Zero the newly allocated region
        memset(new_buf + entry->recv_cap, 0, new_cap - entry->recv_cap);
        entry->recv_buf = new_buf;
        entry->recv_cap = new_cap;
    }

    memcpy(entry->recv_buf + entry->recv_len, data, len);
    entry->recv_len += len;
    return 0;
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
