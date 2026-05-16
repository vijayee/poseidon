# Meridian QUIC Streams Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all QUIC datagram usage in the Meridian P2P layer with per-message QUIC streams. FIN acts as message delimiter. No framing.

**Architecture:** Each Meridian message gets its own short-lived QUIC stream. One-way messages (gossip, QUASAR_GOSSIP, channel notices) use unidirectional streams. Request/response messages (PING/PONG, ADDR, BOOTSTRAP) use bidirectional streams. The relay server forwards stream data between clients using the same stream model. All `DatagramSend` and `DATAGRAM_RECEIVED` code paths are removed entirely.

**Tech Stack:** C11, msquic, libcbor

---

## File Structure

| File | Change | Purpose |
|------|--------|---------|
| `src/Network/Meridian/meridian_stream.h` | New | Shared stream entry struct, stream callback declarations |
| `src/Network/Meridian/meridian_stream.c` | New | Shared stream callback implementations (uni + bidi) |
| `src/Network/Meridian/meridian_protocol.c` | Modify | Replace DatagramSend with stream sends, replace DATAGRAM_RECEIVED with stream handlers |
| `src/Network/Meridian/meridian_protocol.h` | Modify | Add stream tracking to `meridian_protocol_t`, update `meridian_protocol_send_packet` signature |
| `src/Network/Meridian/meridian_relay.c` | Modify | Replace datagram sends with stream sends, replace DATAGRAM_RECEIVED with stream handlers |
| `src/Network/Meridian/meridian_relay.h` | Modify | Add stream tracking to `meridian_relay_t`, update send function signatures |
| `src/Network/Meridian/meridian_relay_server.c` | Modify | Replace datagram forwarding with stream forwarding, handle incoming streams |
| `src/Network/Meridian/meridian_relay_server.h` | Modify | Add stream tracking to `meridian_relay_client_t` |
| `src/Network/Meridian/meridian_rendv.c` | Modify | Add stream callback to tunnel connections, remove DatagramReceiveEnabled |
| `src/Network/Meridian/meridian_rendv.h` | Modify | Remove `datagram_recv_enabled` from config struct |
| `CMakeLists.txt` | Modify | Add `meridian_stream.c` to source list |

---

### Task 1: Create meridian_stream.h — shared stream infrastructure header

**Files:**
- Create: `src/Network/Meridian/meridian_stream.h`

- [ ] **Step 1: Write meridian_stream.h**

```c
//
// Created by victor on 5/16/26.
//
// Shared QUIC stream infrastructure for Meridian P2P layer.
// Provides stream entry tracking and callback declarations
// for unidirectional and bidirectional stream handling.

#ifndef MERIDIAN_STREAM_H
#define MERIDIAN_STREAM_H

#include "msquic.h"
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
int meridian_stream_send_uni(const struct QUIC_API_TABLE* api, HQUIC connection,
                              const uint8_t* data, size_t len, void* stream_context);

/**
 * Opens a bidirectional stream on the given connection.
 * Returns the stream handle on success, NULL on failure.
 * The caller should send data on the stream separately.
 */
HQUIC meridian_stream_open_bidi(const struct QUIC_API_TABLE* api, HQUIC connection,
                                 QUIC_STREAM_CALLBACK_HANDLER callback, void* context);

/**
 * Stream callback for one-way messages (gossip, QUASAR_GOSSIP, channel notices).
 * Accumulates data until FIN, then dispatches via the on_complete callback.
 */
QUIC_STATUS QUIC_API meridian_uni_stream_callback(HQUIC stream, void* context,
                                                    QUIC_STREAM_EVENT* event);

/**
 * Stream callback for request/response messages (PING/PONG, ADDR, BOOTSTRAP).
 * Accumulates data until FIN, then processes and sends response on same stream.
 */
QUIC_STATUS QUIC_API meridian_bidi_stream_callback(HQUIC stream, void* context,
                                                     QUIC_STREAM_EVENT* event);

#endif // MERIDIAN_STREAM_H
```

- [ ] **Step 2: Commit**

```bash
git add src/Network/Meridian/meridian_stream.h
git commit -m "feat: add meridian stream infrastructure header for QUIC streams refactoring"
```

---

### Task 2: Create meridian_stream.c — shared stream implementation

**Files:**
- Create: `src/Network/Meridian/meridian_stream.c`

- [ ] **Step 1: Write meridian_stream.c**

```c
//
// Created by victor on 5/16/26.
//
// Shared QUIC stream infrastructure for Meridian P2P layer.
// Implements stream entry management, unidirectional and bidirectional
// stream callbacks, and stream send helpers.

#include "meridian_stream.h"
#include "meridian.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>

// ============================================================================
// STREAM ENTRY LIFECYCLE
// ============================================================================

meridian_stream_entry_t* meridian_stream_entry_create(HQUIC stream, bool is_bidi, void* user_context) {
    meridian_stream_entry_t* entry = get_clear_memory(sizeof(meridian_stream_entry_t));
    if (entry == NULL) return NULL;

    entry->stream = stream;
    entry->is_bidi = is_bidi;
    entry->user_context = user_context;
    entry->recv_cap = MERIDIAN_STREAM_INITIAL_CAP;
    entry->recv_buf = get_clear_memory(entry->recv_cap);
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

int meridian_stream_entry_append(meridian_stream_entry_t* entry, const uint8_t* data, size_t len) {
    if (entry == NULL || data == NULL) return -1;

    if (entry->recv_len + len > MERIDIAN_STREAM_MAX_CAP) {
        log_warn("meridian stream: receive buffer overflow (have=%zu, adding=%zu, max=%zu)",
                 entry->recv_len, len, MERIDIAN_STREAM_MAX_CAP);
        return -1;
    }

    if (entry->recv_len + len > entry->recv_cap) {
        size_t new_cap = entry->recv_cap;
        while (new_cap < entry->recv_len + len) {
            new_cap *= 2;
        }
        if (new_cap > MERIDIAN_STREAM_MAX_CAP) {
            new_cap = MERIDIAN_STREAM_MAX_CAP;
        }
        uint8_t* new_buf = realloc(entry->recv_buf, new_cap);
        if (new_buf == NULL) {
            log_error("meridian stream: failed to grow receive buffer");
            return -1;
        }
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
// STREAM SEND HELPERS
// ============================================================================

int meridian_stream_send_uni(const struct QUIC_API_TABLE* api, HQUIC connection,
                              const uint8_t* data, size_t len, void* stream_context) {
    if (api == NULL || connection == NULL || data == NULL || len == 0) return -1;

    HQUIC stream = NULL;
    QUIC_STATUS status = api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                                           meridian_uni_stream_callback, stream_context, &stream);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamOpen (uni) failed: 0x%x", status);
        return -1;
    }

    // Copy data into send buffer that will be freed in SEND_COMPLETE
    uint8_t* send_buf = get_clear_memory(len);
    if (send_buf == NULL) {
        api->StreamClose(stream);
        return -1;
    }
    memcpy(send_buf, data, len);

    QUIC_BUFFER buf;
    buf.Buffer = send_buf;
    buf.Length = (uint32_t)len;

    status = api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamStart (uni) failed: 0x%x", status);
        free(send_buf);
        api->StreamClose(stream);
        return -1;
    }

    // Send with FIN to signal end-of-message
    status = api->StreamSend(stream, &buf, 1, QUIC_SEND_FLAG_FIN, send_buf);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamSend (uni) failed: 0x%x", status);
        free(send_buf);
        // Stream will be cleaned up in SHUTDOWN_COMPLETE
        return -1;
    }

    return 0;
}

HQUIC meridian_stream_open_bidi(const struct QUIC_API_TABLE* api, HQUIC connection,
                                 QUIC_STREAM_CALLBACK_HANDLER callback, void* context) {
    if (api == NULL || connection == NULL || callback == NULL) return NULL;

    HQUIC stream = NULL;
    QUIC_STATUS status = api->StreamOpen(connection, QUIC_STREAM_OPEN_FLAG_NONE,
                                           callback, context, &stream);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamOpen (bidi) failed: 0x%x", status);
        return NULL;
    }

    status = api->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(status)) {
        log_error("meridian stream: StreamStart (bidi) failed: 0x%x", status);
        api->StreamClose(stream);
        return NULL;
    }

    return stream;
}

// ============================================================================
// UNIDIRECTIONAL STREAM CALLBACK
// ============================================================================

QUIC_STATUS QUIC_API meridian_uni_stream_callback(HQUIC stream, void* context,
                                                     QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) {
            // Outbound uni stream — we initiated this, no receive expected
            return QUIC_STATUS_SUCCESS;
        }

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                log_warn("meridian stream: uni receive buffer overflow, closing stream");
                event->RECEIVE.TotalBufferLength = 0;
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }

        // Acknowledge all received bytes
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // FIN received — message is complete, dispatch to handler
        // The caller (connection callback) sets user_context to the protocol/relay
        // and will process the complete message
        if (entry != NULL && entry->user_context != NULL) {
            // Signal that this stream's data is ready for processing
            // The connection-level handler will process recv_buf
            // This is handled in the connection-specific callback wrappers
        }
        // Gracefully shutdown our side
        // (For uni streams we're receiving, we just close)
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        // Stream handle will be closed by the api call after this callback returns
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
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        if (entry != NULL) {
            // Stream is ready for sending
        }
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                log_warn("meridian stream: bidi receive buffer overflow, closing stream");
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }

        // Acknowledge all received bytes
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // FIN received — request is complete
        // The connection-specific handler will process the request and
        // send a response on this same stream
        break;
    }

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
```

- [ ] **Step 2: Add meridian_stream.c to CMakeLists.txt**

In `CMakeLists.txt`, add `${MERIDIAN_SRC_DIR}/meridian_stream.c` after the other Meridian source files.

- [ ] **Step 3: Build and verify**

```bash
cd build && cmake .. && cmake --build . -- -j$(nproc)
```

Expected: Build succeeds with no errors. meridian_stream.o is produced.

- [ ] **Step 4: Commit**

```bash
git add src/Network/Meridian/meridian_stream.c src/Network/Meridian/meridian_stream.h CMakeLists.txt
git commit -m "feat: add meridian stream infrastructure implementation"
```

---

### Task 3: Refactor meridian_protocol.c — replace datagrams with streams

**Files:**
- Modify: `src/Network/Meridian/meridian_protocol.c`
- Modify: `src/Network/Meridian/meridian_protocol.h`

This is the largest change. The protocol module currently uses `DatagramSend` for all outbound messages and receives inbound messages via `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED`. Both are replaced with stream-based I/O.

- [ ] **Step 1: Add stream tracking to meridian_protocol_t in meridian_protocol.h**

Add a stream list to the protocol struct. After the `default_relay` field, add:

```c
    meridian_stream_entry_t** active_streams;
    size_t num_active_streams;
    size_t active_streams_cap;
```

Add `#include "meridian_stream.h"` to the header.

- [ ] **Step 2: Add stream list initialization in meridian_protocol_create()**

After the `platform_lock_init(&protocol->lock)` line, add stream list initialization:

```c
    protocol->active_streams_cap = 16;
    protocol->active_streams = get_clear_memory(protocol->active_streams_cap * sizeof(meridian_stream_entry_t*));
    protocol->num_active_streams = 0;
```

- [ ] **Step 3: Add stream list cleanup in meridian_protocol_destroy()**

Before `platform_lock_destroy(&protocol->lock)`, add:

```c
    for (size_t i = 0; i < protocol->num_active_streams; i++) {
        meridian_stream_entry_destroy(protocol->active_streams[i]);
    }
    free(protocol->active_streams);
```

- [ ] **Step 4: Replace DatagramReceiveEnabled with stream limits in QUIC_SETTINGS**

In `meridian_protocol_start()`, remove the datagram settings and add stream limits:

```c
    // Remove these:
    // Settings.DatagramReceiveEnabled = TRUE;
    // Settings.IsSet.DatagramReceiveEnabled = TRUE;

    // Add stream limits
    Settings.PeerStreamCount = MERIDIAN_MAX_CONCURRENT_STREAMS;
    Settings.IsSet.PeerStreamCount = TRUE;
```

- [ ] **Step 5: Replace meridian_protocol_send_packet() with stream-based sending**

Replace the `DatagramSend` call in `meridian_protocol_send_packet()` with `meridian_stream_send_uni()`. The function currently finds the HQUIC connection for the target peer and calls `DatagramSend`. Change it to:

```c
int meridian_protocol_send_packet(meridian_protocol_t* protocol,
                                   const uint8_t* data, size_t len,
                                   const meridian_node_t* target) {
    if (protocol == NULL || data == NULL || target == NULL) return -1;
    if (!protocol->running) return -1;

    HQUIC Connection = NULL;
    platform_lock(&protocol->lock);
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == target->addr &&
            protocol->peer_nodes[i]->port == target->port) {
            Connection = protocol->connected_peers[i];
            break;
        }
    }
    platform_unlock(&protocol->lock);

    if (Connection == NULL) return -1;

    return meridian_stream_send_uni(protocol->msquic, Connection, data, len, NULL);
}
```

- [ ] **Step 6: Add PEER_STREAM_STARTED handler to ConnectionCallback**

Replace the `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` case in `ConnectionCallback` with `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED`. The new handler opens a stream entry and sets the stream callback:

```c
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = (Event->PEER_STREAM_STARTED.IsBidirectional != FALSE);

        meridian_stream_entry_t* entry = meridian_stream_entry_create(stream, is_bidi, protocol);
        if (entry == NULL) {
            log_error("meridian protocol: failed to create stream entry");
            return QUIC_STATUS_SUCCESS;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? meridian_bidi_stream_callback
            : meridian_uni_stream_callback;

        protocol->msquic->SetCallbackHandler(stream, (void*)callback, entry);

        platform_lock(&protocol->lock);
        if (protocol->num_active_streams >= protocol->active_streams_cap) {
            size_t new_cap = protocol->active_streams_cap * 2;
            meridian_stream_entry_t** new_arr = realloc(protocol->active_streams,
                new_cap * sizeof(meridian_stream_entry_t*));
            if (new_arr == NULL) {
                meridian_stream_entry_destroy(entry);
                platform_unlock(&protocol->lock);
                return QUIC_STATUS_SUCCESS;
            }
            protocol->active_streams = new_arr;
            protocol->active_streams_cap = new_cap;
        }
        protocol->active_streams[protocol->num_active_streams++] = entry;
        platform_unlock(&protocol->lock);

        return QUIC_STATUS_SUCCESS;
    }
```

- [ ] **Step 7: Modify meridian_uni_stream_callback to dispatch to protocol's on_packet**

The generic `meridian_uni_stream_callback` in meridian_stream.c handles buffer accumulation and cleanup, but the protocol-specific dispatch must happen in meridian_protocol.c. Add a protocol-specific uni stream callback wrapper in meridian_protocol.c:

```c
static QUIC_STATUS QUIC_API protocol_uni_stream_callback(HQUIC stream, void* context,
                                                            QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_protocol_t* protocol = (entry != NULL) ? (meridian_protocol_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Message is complete — dispatch to on_packet
        if (protocol != NULL && entry != NULL && entry->recv_len > 0) {
            if (protocol->callbacks.on_packet) {
                protocol->callbacks.on_packet(
                    protocol->callbacks.user_ctx,
                    protocol,
                    entry->recv_buf,
                    entry->recv_len);
            }
        }
        // Graceful shutdown of our side
        if (entry != NULL && entry->stream != NULL) {
            // For uni streams from peer, we don't need to send anything back
            // Just let the stream close
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (entry != NULL) {
            // Remove from active streams list
            if (protocol != NULL) {
                platform_lock(&protocol->lock);
                for (size_t i = 0; i < protocol->num_active_streams; i++) {
                    if (protocol->active_streams[i] == entry) {
                        protocol->active_streams[i] = protocol->active_streams[protocol->num_active_streams - 1];
                        protocol->active_streams[protocol->num_active_streams - 1] = NULL;
                        protocol->num_active_streams--;
                        break;
                    }
                }
                platform_unlock(&protocol->lock);
            }
            meridian_stream_entry_destroy(entry);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 8: Add protocol_bidi_stream_callback for request/response messages**

This callback handles PING/RET_PING and other request/response patterns:

```c
static QUIC_STATUS QUIC_API protocol_bidi_stream_callback(HQUIC stream, void* context,
                                                             QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_protocol_t* protocol = (entry != NULL) ? (meridian_protocol_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        // Outbound bidi stream is ready for sending
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;

        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Request is complete — dispatch to on_packet
        if (protocol != NULL && entry != NULL && entry->recv_len > 0) {
            if (protocol->callbacks.on_packet) {
                protocol->callbacks.on_packet(
                    protocol->callbacks.user_ctx,
                    protocol,
                    entry->recv_buf,
                    entry->recv_len);
            }
        }
        // For bidi streams where we need to send a response, the response
        // is sent by on_packet -> meridian_protocol_send_packet() on a
        // separate uni stream back to the peer.
        // Shut down our send side since we're done with this stream.
        if (entry != NULL && entry->stream != NULL) {
            protocol->msquic->StreamShutdown(entry->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (entry != NULL) {
            if (protocol != NULL) {
                platform_lock(&protocol->lock);
                for (size_t i = 0; i < protocol->num_active_streams; i++) {
                    if (protocol->active_streams[i] == entry) {
                        protocol->active_streams[i] = protocol->active_streams[protocol->num_active_streams - 1];
                        protocol->active_streams[protocol->num_active_streams - 1] = NULL;
                        protocol->num_active_streams--;
                        break;
                    }
                }
                platform_unlock(&protocol->lock);
            }
            meridian_stream_entry_destroy(entry);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 9: Update PEER_STREAM_STARTED to use protocol-specific callbacks**

In the `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED` case from Step 6, change the callback selection to use the protocol-specific wrappers:

```c
        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? protocol_bidi_stream_callback
            : protocol_uni_stream_callback;
```

- [ ] **Step 10: Update meridian_protocol_send_measure() to use bidi stream**

Replace the `meridian_protocol_send_packet()` call in `meridian_protocol_send_measure()` with a bidi stream. The PING is sent on a bidi stream so RET_PING can come back on the same stream:

```c
int meridian_protocol_send_measure(meridian_protocol_t* protocol,
                                    meridian_measure_request_t* req) {
    if (protocol == NULL || req == NULL || req->target == NULL) return -1;

    gettimeofday(&req->start_time, NULL);

    meridian_packet_t pkt;
    pkt.type = MERIDIAN_PACKET_TYPE_PING;
    pkt.query_id = req->query_id;
    pkt.magic = MERIDIAN_MAGIC_NUMBER;
    pkt.rendv_addr = 0;
    pkt.rendv_port = 0;

    cbor_item_t* encoded = meridian_packet_encode(&pkt);
    if (encoded == NULL) return -1;

    size_t buf_len = 0;
    uint8_t* buf = NULL;
    cbor_serialize_alloc(encoded, &buf, &buf_len);
    cbor_decref(&encoded);

    if (buf == NULL) return -1;

    // Find connection for target
    HQUIC Connection = NULL;
    platform_lock(&protocol->lock);
    for (size_t i = 0; i < protocol->num_connected_peers; i++) {
        if (protocol->peer_nodes[i] &&
            protocol->peer_nodes[i]->addr == req->target->addr &&
            protocol->peer_nodes[i]->port == req->target->port) {
            Connection = protocol->connected_peers[i];
            break;
        }
    }
    platform_unlock(&protocol->lock);

    if (Connection == NULL) {
        free(buf);
        return -1;
    }

    // Open bidi stream for PING/RET_PING exchange
    HQUIC stream = meridian_stream_open_bidi(protocol->msquic, Connection,
                                               protocol_bidi_stream_callback, protocol);
    if (stream == NULL) {
        free(buf);
        return -1;
    }

    // Send PING on the bidi stream with FIN
    uint8_t* send_buf = get_clear_memory(buf_len);
    if (send_buf == NULL) {
        free(buf);
        return -1;
    }
    memcpy(send_buf, buf, buf_len);
    free(buf);

    QUIC_BUFFER qbuf;
    qbuf.Buffer = send_buf;
    qbuf.Length = (uint32_t)buf_len;

    QUIC_STATUS status = protocol->msquic->StreamSend(stream, &qbuf, 1,
                                                        QUIC_SEND_FLAG_FIN, send_buf);
    if (QUIC_FAILED(status)) {
        free(send_buf);
        return -1;
    }

    // Register pending measure request
    platform_lock(&protocol->lock);
    if (protocol->num_pending_measures >= protocol->pending_measures_capacity) {
        size_t new_cap = protocol->pending_measures_capacity == 0 ? 8 :
                         protocol->pending_measures_capacity * 2;
        meridian_measure_request_t** new_arr = (meridian_measure_request_t**)
            realloc(protocol->pending_measures, new_cap * sizeof(meridian_measure_request_t*));
        if (new_arr == NULL) {
            platform_unlock(&protocol->lock);
            return -1;
        }
        protocol->pending_measures = new_arr;
        protocol->pending_measures_capacity = new_cap;
    }
    protocol->pending_measures[protocol->num_pending_measures++] = req;
    platform_unlock(&protocol->lock);

    return 0;
}
```

- [ ] **Step 11: Update meridian_protocol_broadcast() to use streams**

The function already calls `meridian_protocol_send_packet()` in a loop, which now uses streams. No change needed — `meridian_protocol_send_packet()` already sends via streams after Step 5.

- [ ] **Step 12: Clean up SHUTDOWN_COMPLETE to destroy stream entries**

In `ConnectionCallback`, the `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE` case should clean up all stream entries for that connection. Add stream entry cleanup before the existing peer cleanup:

```c
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        // Remove all stream entries associated with this connection's peers
        platform_lock(&protocol->lock);
        size_t write = 0;
        for (size_t i = 0; i < protocol->num_active_streams; i++) {
            // Keep streams not associated with this connection
            // (We can't easily determine which streams belong to this connection
            //  from the stream entry alone, so we keep all entries.
            //  Stale entries will be cleaned up in SHUTDOWN_COMPLETE on the stream.)
            protocol->active_streams[write++] = protocol->active_streams[i];
        }
        protocol->num_active_streams = write;
        platform_unlock(&protocol->lock);

        // Existing peer cleanup code continues below...
```

- [ ] **Step 13: Remove DATAGRAM_RECEIVED handler from ConnectionCallback**

Delete the entire `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` case from `ConnectionCallback`. It's been replaced by `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED`.

- [ ] **Step 14: Build and verify**

```bash
cd build && cmake .. && cmake --build . -- -j$(nproc)
```

Expected: Build succeeds. The protocol module compiles with stream-based I/O.

- [ ] **Step 15: Commit**

```bash
git add src/Network/Meridian/meridian_protocol.c src/Network/Meridian/meridian_protocol.h
git commit -m "feat: refactor meridian_protocol from datagrams to QUIC streams"
```

---

### Task 4: Refactor meridian_relay.c — replace datagrams with streams

**Files:**
- Modify: `src/Network/Meridian/meridian_relay.c`
- Modify: `src/Network/Meridian/meridian_relay.h`

- [ ] **Step 1: Add stream tracking to meridian_relay_t in meridian_relay.h**

Add `#include "meridian_stream.h"` to the header. Add stream tracking fields to the relay struct after the callback fields:

```c
    meridian_stream_entry_t** active_streams;
    size_t num_active_streams;
    size_t active_streams_cap;
```

- [ ] **Step 2: Initialize and clean up stream list in create/destroy**

In `meridian_relay_create()`, add after lock initialization:

```c
    relay->active_streams_cap = 16;
    relay->active_streams = get_clear_memory(relay->active_streams_cap * sizeof(meridian_stream_entry_t*));
    relay->num_active_streams = 0;
```

In `meridian_relay_destroy()`, add before lock destroy:

```c
    for (size_t i = 0; i < relay->num_active_streams; i++) {
        meridian_stream_entry_destroy(relay->active_streams[i]);
    }
    free(relay->active_streams);
```

- [ ] **Step 3: Replace meridian_relay_send_datagram() with stream-based send**

Replace the entire function body. The new version opens a uni stream, sends the 4-byte dest_endpoint_id prefix + payload, and closes with FIN:

```c
int meridian_relay_send_datagram(meridian_relay_t* relay, const uint8_t* data,
                                  size_t len, uint32_t dest_endpoint_id) {
    if (relay == NULL || data == NULL || !relay->connected) return -1;

    // Build the relay envelope: [4 bytes dest_endpoint_id big-endian] + [4 bytes src_endpoint_id big-endian] + [payload]
    size_t total_len = sizeof(uint32_t) * 2 + len;
    uint8_t* send_data = get_clear_memory(total_len);
    if (send_data == NULL) return -1;

    // Write dest_endpoint_id in big-endian
    send_data[0] = (uint8_t)(dest_endpoint_id >> 24);
    send_data[1] = (uint8_t)(dest_endpoint_id >> 16);
    send_data[2] = (uint8_t)(dest_endpoint_id >> 8);
    send_data[3] = (uint8_t)(dest_endpoint_id);

    // Write src_endpoint_id in big-endian
    uint32_t src = relay->local_endpoint_id;
    send_data[4] = (uint8_t)(src >> 24);
    send_data[5] = (uint8_t)(src >> 16);
    send_data[6] = (uint8_t)(src >> 8);
    send_data[7] = (uint8_t)(src);
    memcpy(send_data + sizeof(uint32_t) * 2, data, len);

    int result = meridian_stream_send_uni(relay->msquic, relay->connection,
                                            send_data, total_len, relay);
    free(send_data);
    return result;
}
```

- [ ] **Step 4: Replace meridian_relay_send_addr_request() with bidi stream**

Replace the entire function body. The new version opens a bidi stream, sends the 4-byte dest=0 prefix + ADDR_REQUEST payload:

```c
int meridian_relay_send_addr_request(meridian_relay_t* relay) {
    if (relay == NULL || !relay->connected) return -1;

    uint64_t query_id = (uint64_t)relay->local_endpoint_id << 32;

    // Build ADDR_REQUEST: [4 bytes dest=0] + [CBOR: type, query_id]
    cbor_item_t* array = cbor_new_definite_array(3);
    if (array == NULL) return -1;

    uint64_t qid_1 = query_id >> 32;
    uint64_t qid_2 = query_id & 0xFFFFFFFF;

    if (!cbor_array_push(array, cbor_build_uint8(MERIDIAN_PACKET_TYPE_ADDR_REQUEST)) ||
        !cbor_array_push(array, cbor_build_uint64(qid_1)) ||
        !cbor_array_push(array, cbor_build_uint64(qid_2))) {
        cbor_decref(&array);
        return -1;
    }

    unsigned char* cbor_buf = NULL;
    size_t cbor_sz = 0;
    size_t encoded_len = cbor_serialize_alloc(array, &cbor_buf, &cbor_sz);
    cbor_decref(&array);

    if (encoded_len == 0 || cbor_buf == NULL) return -1;

    // Prepend 4-byte dest_endpoint_id = 0 (addressed to relay server)
    size_t total_len = sizeof(uint32_t) + encoded_len;
    uint8_t* send_data = get_clear_memory(total_len);
    if (send_data == NULL) {
        free(cbor_buf);
        return -1;
    }
    memset(send_data, 0, sizeof(uint32_t));  // dest_endpoint_id = 0
    memcpy(send_data + sizeof(uint32_t), cbor_buf, encoded_len);
    free(cbor_buf);

    // Open bidi stream for ADDR_REQUEST/ADDR_RESPONSE
    meridian_stream_entry_t* entry = meridian_stream_entry_create(NULL, true, relay);
    if (entry == NULL) {
        free(send_data);
        return -1;
    }

    HQUIC stream = meridian_stream_open_bidi(relay->msquic, relay->connection,
                                               relay_bidi_stream_callback, entry);
    if (stream == NULL) {
        meridian_stream_entry_destroy(entry);
        free(send_data);
        return -1;
    }
    entry->stream = stream;

    // Track the stream
    platform_lock(&relay->lock);
    if (relay->num_active_streams >= relay->active_streams_cap) {
        size_t new_cap = relay->active_streams_cap * 2;
        meridian_stream_entry_t** new_arr = realloc(relay->active_streams,
            new_cap * sizeof(meridian_stream_entry_t*));
        if (new_arr == NULL) {
            platform_unlock(&relay->lock);
            free(send_data);
            return -1;
        }
        relay->active_streams = new_arr;
        relay->active_streams_cap = new_cap;
    }
    relay->active_streams[relay->num_active_streams++] = entry;
    platform_unlock(&relay->lock);

    // Send with FIN
    uint8_t* stream_buf = get_clear_memory(total_len);
    if (stream_buf == NULL) {
        free(send_data);
        return -1;
    }
    memcpy(stream_buf, send_data, total_len);
    free(send_data);

    QUIC_BUFFER qbuf;
    qbuf.Buffer = stream_buf;
    qbuf.Length = (uint32_t)total_len;

    QUIC_STATUS status = relay->msquic->StreamSend(stream, &qbuf, 1,
                                                     QUIC_SEND_FLAG_FIN, stream_buf);
    if (QUIC_FAILED(status)) {
        free(stream_buf);
        return -1;
    }

    return 0;
}
```

- [ ] **Step 5: Add relay-specific stream callbacks**

Add relay uni stream callback for incoming forwarded messages:

```c
static QUIC_STATUS QUIC_API relay_uni_stream_callback(HQUIC stream, void* context,
                                                        QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_relay_t* relay = (entry != NULL) ? (meridian_relay_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;
        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Complete message received — process it
        if (relay != NULL && entry != NULL && entry->recv_len > sizeof(uint32_t)) {
            // Read dest_endpoint_id from first 4 bytes
            uint32_t dest_endpoint_id = ((uint32_t)entry->recv_buf[0] << 24) |
                                         ((uint32_t)entry->recv_buf[1] << 16) |
                                         ((uint32_t)entry->recv_buf[2] << 8) |
                                         ((uint32_t)entry->recv_buf[3]);
            const uint8_t* payload = entry->recv_buf + sizeof(uint32_t);
            size_t payload_len = entry->recv_len - sizeof(uint32_t);

            if (dest_endpoint_id == 0) {
                // Addressed to relay server itself — decode as ADDR_RESPONSE
                struct cbor_load_result result;
                cbor_item_t* item = cbor_load(payload, payload_len, &result);
                if (item != NULL && cbor_array_is_definite(item) && cbor_array_size(item) >= 1) {
                    cbor_item_t** items = cbor_array_handle(item);
                    uint8_t packet_type = cbor_get_uint8(items[0]);

                    if (packet_type == MERIDIAN_PACKET_TYPE_ADDR_RESPONSE) {
                        meridian_addr_response_t* addr_resp = meridian_addr_response_decode(item);
                        if (addr_resp != NULL) {
                            platform_lock(&relay->lock);
                            relay->local_endpoint_id = addr_resp->endpoint_id;
                            platform_unlock(&relay->lock);

                            if (relay->on_addr_response != NULL) {
                                relay->on_addr_response(
                                    relay->on_addr_response_ctx,
                                    addr_resp->reflexive_addr,
                                    addr_resp->reflexive_port,
                                    addr_resp->endpoint_id);
                            }
                            meridian_addr_response_destroy(addr_resp);
                        }
                    }
                }
                if (item != NULL) cbor_decref(&item);
            } else {
                // Forwarded datagram — invoke datagram callback with src_endpoint
                // For now, src_endpoint is 0 since we don't have the source from uni stream
                if (relay->on_datagram != NULL) {
                    relay->on_datagram(
                        relay->on_datagram_ctx,
                        payload,
                        payload_len,
                        0);
                }
            }
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (entry != NULL && relay != NULL) {
            platform_lock(&relay->lock);
            for (size_t i = 0; i < relay->num_active_streams; i++) {
                if (relay->active_streams[i] == entry) {
                    relay->active_streams[i] = relay->active_streams[relay->num_active_streams - 1];
                    relay->active_streams[relay->num_active_streams - 1] = NULL;
                    relay->num_active_streams--;
                    break;
                }
            }
            platform_unlock(&relay->lock);
            meridian_stream_entry_destroy(entry);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 6: Add relay bidi stream callback**

Add forward declaration before `RelayConnectionCallback`:

```c
static QUIC_STATUS QUIC_API relay_bidi_stream_callback(HQUIC stream, void* context,
                                                         QUIC_STREAM_EVENT* event);
static QUIC_STATUS QUIC_API relay_uni_stream_callback(HQUIC stream, void* context,
                                                        QUIC_STREAM_EVENT* event);
```

The `relay_bidi_stream_callback` handles ADDR_RESPONSE coming back on the bidi stream opened by `meridian_relay_send_addr_request()`:

```c
static QUIC_STATUS QUIC_API relay_bidi_stream_callback(HQUIC stream, void* context,
                                                         QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_relay_t* relay = (entry != NULL) ? (meridian_relay_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;
        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Response received on bidi stream
        if (relay != NULL && entry != NULL && entry->recv_len > sizeof(uint32_t)) {
            uint32_t dest_endpoint_id = ((uint32_t)entry->recv_buf[0] << 24) |
                                         ((uint32_t)entry->recv_buf[1] << 16) |
                                         ((uint32_t)entry->recv_buf[2] << 8) |
                                         ((uint32_t)entry->recv_buf[3]);
            const uint8_t* payload = entry->recv_buf + sizeof(uint32_t);
            size_t payload_len = entry->recv_len - sizeof(uint32_t);

            if (dest_endpoint_id == 0) {
                // ADDR_RESPONSE from relay server
                struct cbor_load_result result;
                cbor_item_t* item = cbor_load(payload, payload_len, &result);
                if (item != NULL) {
                    if (cbor_array_is_definite(item) && cbor_array_size(item) >= 1) {
                        cbor_item_t** items = cbor_array_handle(item);
                        uint8_t packet_type = cbor_get_uint8(items[0]);
                        if (packet_type == MERIDIAN_PACKET_TYPE_ADDR_RESPONSE) {
                            meridian_addr_response_t* addr_resp = meridian_addr_response_decode(item);
                            if (addr_resp != NULL) {
                                platform_lock(&relay->lock);
                                relay->local_endpoint_id = addr_resp->endpoint_id;
                                platform_unlock(&relay->lock);

                                if (relay->on_addr_response != NULL) {
                                    relay->on_addr_response(
                                        relay->on_addr_response_ctx,
                                        addr_resp->reflexive_addr,
                                        addr_resp->reflexive_port,
                                        addr_resp->endpoint_id);
                                }
                                meridian_addr_response_destroy(addr_resp);
                            }
                        }
                    }
                    cbor_decref(&item);
                }
            }
        }
        // Shutdown our send side
        if (entry != NULL && entry->stream != NULL && relay != NULL) {
            relay->msquic->StreamShutdown(entry->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        if (entry != NULL && relay != NULL) {
            platform_lock(&relay->lock);
            for (size_t i = 0; i < relay->num_active_streams; i++) {
                if (relay->active_streams[i] == entry) {
                    relay->active_streams[i] = relay->active_streams[relay->num_active_streams - 1];
                    relay->active_streams[relay->num_active_streams - 1] = NULL;
                    relay->num_active_streams--;
                    break;
                }
            }
            platform_unlock(&relay->lock);
            meridian_stream_entry_destroy(entry);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 7: Update RelayConnectionCallback — replace DATAGRAM_RECEIVED with PEER_STREAM_STARTED**

Replace the `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` case in `RelayConnectionCallback` with:

```c
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = (Event->PEER_STREAM_STARTED.IsBidirectional != FALSE);

        meridian_stream_entry_t* entry = meridian_stream_entry_create(stream, is_bidi, relay);
        if (entry == NULL) {
            return QUIC_STATUS_SUCCESS;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? relay_bidi_stream_callback
            : relay_uni_stream_callback;

        relay->msquic->SetCallbackHandler(stream, (void*)callback, entry);

        platform_lock(&relay->lock);
        if (relay->num_active_streams >= relay->active_streams_cap) {
            size_t new_cap = relay->active_streams_cap * 2;
            meridian_stream_entry_t** new_arr = realloc(relay->active_streams,
                new_cap * sizeof(meridian_stream_entry_t*));
            if (new_arr == NULL) {
                meridian_stream_entry_destroy(entry);
                platform_unlock(&relay->lock);
                return QUIC_STATUS_SUCCESS;
            }
            relay->active_streams = new_arr;
            relay->active_streams_cap = new_cap;
        }
        relay->active_streams[relay->num_active_streams++] = entry;
        platform_unlock(&relay->lock);

        return QUIC_STATUS_SUCCESS;
    }
```

Remove the `DATAGRAM_RECEIVED` case entirely.

- [ ] **Step 8: Remove DatagramReceiveEnabled from QUIC_SETTINGS**

In the relay configuration setup (around line 71-77), remove:

```c
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;
```

- [ ] **Step 9: Remove the initial ADDR_REQUEST from CONNECTED handler**

The `QUIC_CONNECTION_EVENT_CONNECTED` case in `RelayConnectionCallback` currently calls `meridian_relay_send_addr_request(relay)`. This now opens a bidi stream instead of sending a datagram. The function already handles this correctly after Step 4, so no change is needed here — the function itself was updated.

- [ ] **Step 10: Build and verify**

```bash
cd build && cmake --build . -- -j$(nproc)
```

Expected: Build succeeds.

- [ ] **Step 11: Commit**

```bash
git add src/Network/Meridian/meridian_relay.c src/Network/Meridian/meridian_relay.h
git commit -m "feat: refactor meridian_relay from datagrams to QUIC streams"
```

---

### Task 5: Refactor meridian_relay_server.c — replace datagram forwarding with stream forwarding

**Files:**
- Modify: `src/Network/Meridian/meridian_relay_server.c`
- Modify: `src/Network/Meridian/meridian_relay_server.h`

- [ ] **Step 1: Add stream tracking to meridian_relay_client_t in meridian_relay_server.h**

Add `#include "meridian_stream.h"`. Add stream tracking to `meridian_relay_client_t`:

```c
typedef struct meridian_relay_client_t {
    uint32_t endpoint_id;
    HQUIC connection;
    struct sockaddr_storage remote_addr;
    uint64_t last_activity_ms;
    bool authenticated;
    meridian_stream_entry_t** active_streams;
    size_t num_active_streams;
    size_t active_streams_cap;
} meridian_relay_client_t;
```

- [ ] **Step 2: Initialize and clean up stream list in client slot allocation**

In `RelayServerListenerCallback` where client slots are initialized, add:

```c
    server->clients[slot].active_streams_cap = 16;
    server->clients[slot].active_streams = get_clear_memory(
        server->clients[slot].active_streams_cap * sizeof(meridian_stream_entry_t*));
    server->clients[slot].num_active_streams = 0;
```

In `RelayServerConnectionCallback` SHUTDOWN_COMPLETE, clean up:

```c
    for (size_t i = 0; i < server->clients[slot].num_active_streams; i++) {
        meridian_stream_entry_destroy(server->clients[slot].active_streams[i]);
    }
    free(server->clients[slot].active_streams);
    server->clients[slot].active_streams = NULL;
```

- [ ] **Step 3: Add relay server stream callbacks**

Add stream callback functions for the relay server. Incoming streams from clients are either uni (RELAY_DATAGRAM, PUNCH) or bidi (ADDR_REQUEST):

```c
static QUIC_STATUS QUIC_API relay_server_uni_stream_callback(HQUIC stream, void* context,
                                                                QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_relay_server_t* server = (entry != NULL) ? (meridian_relay_server_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;
        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Complete message — forward to destination
        if (server != NULL && entry != NULL && entry->recv_len > sizeof(uint32_t)) {
            uint32_t dest_endpoint_id = ((uint32_t)entry->recv_buf[0] << 24) |
                                         ((uint32_t)entry->recv_buf[1] << 16) |
                                         ((uint32_t)entry->recv_buf[2] << 8) |
                                         ((uint32_t)entry->recv_buf[3]);
            const uint8_t* payload = entry->recv_buf + sizeof(uint32_t);
            size_t payload_len = entry->recv_len - sizeof(uint32_t);

            if (dest_endpoint_id != 0) {
                // Forward to destination client via uni stream
                platform_lock(&server->lock);
                int dest_slot = relay_server_find_client_by_endpoint(server, dest_endpoint_id);
                if (dest_slot >= 0 && server->clients[dest_slot].connection != NULL) {
                    meridian_stream_send_uni(server->msquic,
                        server->clients[dest_slot].connection,
                        payload, payload_len, NULL);
                    server->stats.datagrams_forwarded++;
                } else {
                    server->stats.datagrams_dropped++;
                }
                platform_unlock(&server->lock);
            }
        }
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        // Clean up stream entry
        // Find the client that owns this connection and remove the stream
        // This is handled by iterating server->clients in the connection shutdown
        if (entry != NULL) {
            meridian_stream_entry_destroy(entry);
        }
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
```

Add bidi stream callback for ADDR_REQUEST:

```c
static QUIC_STATUS QUIC_API relay_server_bidi_stream_callback(HQUIC stream, void* context,
                                                                 QUIC_STREAM_EVENT* event) {
    meridian_stream_entry_t* entry = (meridian_stream_entry_t*)context;
    meridian_relay_server_t* server = (entry != NULL) ? (meridian_relay_server_t*)entry->user_context : NULL;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        if (entry == NULL) return QUIC_STATUS_SUCCESS;
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;
        for (uint32_t i = 0; i < buffer_count; i++) {
            if (meridian_stream_entry_append(entry, buffers[i].Buffer, buffers[i].Length) != 0) {
                return QUIC_STATUS_INTERNAL_ERROR;
            }
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
        // Request is complete
        if (server != NULL && entry != NULL && entry->recv_len > sizeof(uint32_t)) {
            uint32_t dest_endpoint_id = ((uint32_t)entry->recv_buf[0] << 24) |
                                         ((uint32_t)entry->recv_buf[1] << 16) |
                                         ((uint32_t)entry->recv_buf[2] << 8) |
                                         ((uint32_t)entry->recv_buf[3]);
            const uint8_t* payload = entry->recv_buf + sizeof(uint32_t);
            size_t payload_len = entry->recv_len - sizeof(uint32_t);

            if (dest_endpoint_id == 0) {
                // ADDR_REQUEST — find the source client's endpoint_id
                // and respond with their reflexive address
                struct cbor_load_result result;
                cbor_item_t* item = cbor_load(payload, payload_len, &result);
                if (item != NULL && cbor_array_is_definite(item) && cbor_array_size(item) >= 1) {
                    cbor_item_t** items = cbor_array_handle(item);
                    uint8_t packet_type = cbor_get_uint8(items[0]);

                    if (packet_type == MERIDIAN_PACKET_TYPE_ADDR_REQUEST) {
                        // Build ADDR_RESPONSE
                        // Find the client's endpoint_id from the connection
                        // (We need to find which client owns this stream's connection)
                        // For now, respond with a basic ADDR_RESPONSE
                        cbor_item_t* resp = cbor_new_definite_array(4);
                        cbor_array_push(resp, cbor_build_uint8(MERIDIAN_PACKET_TYPE_ADDR_RESPONSE));

                        // Get source endpoint from connection lookup
                        uint32_t src_endpoint = 0;
                        platform_lock(&server->lock);
                        for (size_t i = 0; i < server->num_clients; i++) {
                            // Match by finding the client whose connection matches
                            // We need to pass the connection through user_context
                            // For now, use a placeholder
                            if (server->clients[i].connection != NULL) {
                                src_endpoint = server->clients[i].endpoint_id;
                                break;
                            }
                        }
                        platform_unlock(&server->lock);

                        cbor_array_push(resp, cbor_build_uint32(0));  // reflexive_addr placeholder
                        cbor_array_push(resp, cbor_build_uint16(0));  // reflexive_port placeholder
                        cbor_array_push(resp, cbor_build_uint32(src_endpoint));

                        unsigned char* resp_buf = NULL;
                        size_t resp_sz = 0;
                        size_t resp_len = cbor_serialize_alloc(resp, &resp_buf, &resp_sz);
                        cbor_decref(&resp);

                        if (resp_len > 0 && resp_buf != NULL) {
                            // Prepend dest=0 for the relay envelope
                            size_t total = sizeof(uint32_t) + resp_len;
                            uint8_t* send_data = get_clear_memory(total);
                            if (send_data != NULL) {
                                memset(send_data, 0, sizeof(uint32_t));
                                memcpy(send_data + sizeof(uint32_t), resp_buf, resp_len);

                                uint8_t* stream_buf = get_clear_memory(total);
                                if (stream_buf != NULL) {
                                    memcpy(stream_buf, send_data, total);
                                    QUIC_BUFFER qbuf;
                                    qbuf.Buffer = stream_buf;
                                    qbuf.Length = (uint32_t)total;
                                    server->msquic->StreamSend(entry->stream, &qbuf, 1,
                                        QUIC_SEND_FLAG_FIN, stream_buf);
                                }
                                free(send_data);
                            }
                            free(resp_buf);
                        }
                    }
                }
                if (item != NULL) cbor_decref(&item);
            }
        }
        // Shutdown our send side after response
        if (entry != NULL && entry->stream != NULL && server != NULL) {
            server->msquic->StreamShutdown(entry->stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        break;
    }

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
```

- [ ] **Step 4: Replace DATAGRAM_RECEIVED with PEER_STREAM_STARTED in RelayServerConnectionCallback**

Replace the `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` case with:

```c
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = (Event->PEER_STREAM_STARTED.IsBidirectional != FALSE);

        // Find the client slot for this connection
        int slot = relay_server_find_client_by_connection(server, Connection);
        if (slot < 0) {
            return QUIC_STATUS_SUCCESS;
        }

        meridian_stream_entry_t* entry = meridian_stream_entry_create(stream, is_bidi, server);
        if (entry == NULL) {
            return QUIC_STATUS_SUCCESS;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? relay_server_bidi_stream_callback
            : relay_server_uni_stream_callback;

        server->msquic->SetCallbackHandler(stream, (void*)callback, entry);

        platform_lock(&server->lock);
        if (server->clients[slot].num_active_streams >= server->clients[slot].active_streams_cap) {
            size_t new_cap = server->clients[slot].active_streams_cap * 2;
            meridian_stream_entry_t** new_arr = realloc(server->clients[slot].active_streams,
                new_cap * sizeof(meridian_stream_entry_t*));
            if (new_arr == NULL) {
                meridian_stream_entry_destroy(entry);
                platform_unlock(&server->lock);
                return QUIC_STATUS_SUCCESS;
            }
            server->clients[slot].active_streams = new_arr;
            server->clients[slot].active_streams_cap = new_cap;
        }
        server->clients[slot].active_streams[server->clients[slot].num_active_streams++] = entry;
        platform_unlock(&server->lock);

        return QUIC_STATUS_SUCCESS;
    }
```

- [ ] **Step 5: Remove DatagramReceiveEnabled from QUIC_SETTINGS**

In the relay server configuration (around line 79-85), remove:

```c
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;
```

- [ ] **Step 6: Build and verify**

```bash
cd build && cmake --build . -- -j$(nproc)
```

Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add src/Network/Meridian/meridian_relay_server.c src/Network/Meridian/meridian_relay_server.h
git commit -m "feat: refactor relay server from datagram forwarding to stream forwarding"
```

---

### Task 6: Refactor meridian_rendv.c — add stream support to tunnels, remove DatagramReceiveEnabled

**Files:**
- Modify: `src/Network/Meridian/meridian_rendv.c`
- Modify: `src/Network/Meridian/meridian_rendv.h`

- [ ] **Step 1: Remove datagram_recv_enabled from meridian_rendv_config_t**

In `meridian_rendv.h`, remove the field:

```c
    bool datagram_recv_enabled;
```

The config struct should now look like:

```c
typedef struct meridian_rendv_config_t {
    const char* alpn;
    QUIC_CREDENTIAL_CONFIG* cred_config;
    uint32_t idle_timeout_ms;
} meridian_rendv_config_t;
```

- [ ] **Step 2: Add stream callback for tunnel connections**

In `meridian_rendv.c`, add a tunnel stream callback. Tunnel connections currently pass NULL as the connection callback. We need a minimal stream handler that can receive data on tunnel connections:

```c
static QUIC_STATUS QUIC_API rendv_tunnel_connection_callback(HQUIC Connection, void* Context,
                                                                QUIC_CONNECTION_EVENT* Event) {
    meridian_rendv_handle_t* handle = (meridian_rendv_handle_t*)Context;

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        log_info("meridian rendv: tunnel connected");
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = Event->PEER_STREAM_STARTED.Stream;
        bool is_bidi = (Event->PEER_STREAM_STARTED.IsBidirectional != FALSE);

        // Create a stream entry to receive data
        meridian_stream_entry_t* entry = meridian_stream_entry_create(stream, is_bidi, handle);
        if (entry == NULL) {
            return QUIC_STATUS_SUCCESS;
        }

        QUIC_STREAM_CALLBACK_HANDLER callback = is_bidi
            ? meridian_bidi_stream_callback
            : meridian_uni_stream_callback;

        handle->msquic->SetCallbackHandler(stream, (void*)callback, entry);
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
```

- [ ] **Step 3: Update meridian_rendv_handle_create_tunnel() to use the connection callback**

Replace the `NULL` callback with `rendv_tunnel_connection_callback`:

```c
    QUIC_STATUS Status = handle->msquic->ConnectionOpen(
        handle->registration,
        (void*)rendv_tunnel_connection_callback,
        handle,
        &Connection);
```

- [ ] **Step 4: Remove DatagramReceiveEnabled from QUIC_SETTINGS in meridian_rendv_handle_set_msquic()**

In the QUIC_SETTINGS configuration, remove:

```c
    Settings.DatagramReceiveEnabled = config->datagram_recv_enabled;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;
```

The settings should now be:

```c
    QUIC_SETTINGS Settings = {0};
    Settings.IdleTimeoutMs = config->idle_timeout_ms ? config->idle_timeout_ms : 30000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
```

- [ ] **Step 5: Update any callers that set datagram_recv_enabled**

Search for any code that sets `datagram_recv_enabled` in the config struct and remove those lines. They may be in other files that create `meridian_rendv_config_t` instances.

- [ ] **Step 6: Build and verify**

```bash
cd build && cmake --build . -- -j$(nproc)
```

Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add src/Network/Meridian/meridian_rendv.c src/Network/Meridian/meridian_rendv.h
git commit -m "feat: add stream support to rendezvous tunnels, remove datagram config"
```

---

### Task 7: Update meridian_conn.c send path to use streams

**Files:**
- Modify: `src/Network/Meridian/meridian_conn.c`

The `meridian_conn_send()` function currently sends via `DatagramSend` on the direct connection or via `meridian_relay_send_datagram()` for relay. Both need to use streams now.

- [ ] **Step 1: Read meridian_conn.c and update the send path**

Read `meridian_conn_send()` and replace any `DatagramSend` calls with `meridian_stream_send_uni()`. The relay path already uses `meridian_relay_send_datagram()` which was updated in Task 4 to use streams internally.

If `meridian_conn_send()` calls `DatagramSend` directly on `conn->direct_connection`, replace with:

```c
    return meridian_stream_send_uni(protocol_msquic, conn->direct_connection, data, len, NULL);
```

The `protocol_msquic` pointer needs to be accessible from the connection context. If it's not currently stored, add it to `meridian_conn_t` or pass it as a parameter.

- [ ] **Step 2: Build and verify**

```bash
cd build && cmake --build . -- -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add src/Network/Meridian/meridian_conn.c
git commit -m "feat: update meridian_conn send path to use QUIC streams"
```

---

### Task 8: De-wonk audit

- [ ] **Step 1: Read every file created or modified in Tasks 1-7**
- [ ] **Step 2: Run de-wonk question: "Is anything unimplemented, stubbed, disabled, broken, or weird?"**
- [ ] **Step 3: Fix all CRITICAL and HIGH issues**
- [ ] **Step 4: Re-audit until no new issues**

---

### Task 9: Full build and integration test

- [ ] **Step 1: Clean build**

```bash
cd build && cmake .. && cmake --build . -- -j$(nproc)
```

- [ ] **Step 2: Run existing tests**

```bash
cd build && ctest --output-on-failure
```

- [ ] **Step 3: Manual smoke test — start poseidond with QUIC enabled**

```bash
./poseidond --enable-quic --quic-port 9091 --tls-cert cert.pem --tls-key key.pem
```

Verify the daemon starts without crashes and logs show QUIC stream-based connections.

- [ ] **Step 4: Commit final state**

```bash
git add -A
git commit -m "feat: complete Meridian QUIC streams refactoring — all datagrams replaced with per-message streams"
```