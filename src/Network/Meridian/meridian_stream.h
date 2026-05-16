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
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MERIDIAN_STREAM_INITIAL_CAP MERIDIAN_MAX_PAYLOAD_SIZE
#define MERIDIAN_STREAM_MAX_CAP 65536
#define MERIDIAN_MAX_CONCURRENT_STREAMS 100

typedef struct meridian_stream_entry_t {
    HQUIC stream;
    uint8_t* recv_buf;
    size_t recv_len;
    size_t recv_cap;
    bool is_bidi;
    void* user_context;
} meridian_stream_entry_t;

/**
 * Creates a stream entry with initial buffer capacity.
 * Returns NULL on allocation failure.
 */
meridian_stream_entry_t* meridian_stream_entry_create(HQUIC stream, bool is_bidi, void* user_context);

/**
 * Destroys a stream entry, freeing its receive buffer.
 * Does NOT close the stream handle — call StreamClose separately.
 */
void meridian_stream_entry_destroy(meridian_stream_entry_t* entry);

/**
 * Appends received data to the stream entry's buffer.
 * Grows the buffer if needed (up to MERIDIAN_STREAM_MAX_CAP).
 * Returns 0 on success, -1 on overflow or allocation failure.
 */
int meridian_stream_entry_append(meridian_stream_entry_t* entry, const uint8_t* data, size_t len);

/**
 * Resets the receive buffer after processing, keeping allocated capacity.
 */
void meridian_stream_entry_reset(meridian_stream_entry_t* entry);

/**
 * Opens a unidirectional stream on the given connection and sends data.
 * Sends the data with QUIC_SEND_FLAG_FIN to signal end-of-message.
 * Returns 0 on success, -1 on failure.
 * The send_buf is freed in the SEND_COMPLETE callback.
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
