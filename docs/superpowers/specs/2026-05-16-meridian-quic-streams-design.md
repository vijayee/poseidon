# Meridian QUIC Streams Refactoring

**Goal:** Replace all QUIC datagram usage in the Meridian P2P layer with QUIC streams. Every message gets its own short-lived stream; FIN acts as the message delimiter. No length-prefix framing.

**Scope:** Meridian P2P layer only. The client transport (`transport_quic.c`, `poseidon_client.c`) is not changed.

---

## 1. Stream Model & Message Flow

### Stream types by message pattern

| Pattern | Stream type | Lifecycle |
|---------|-------------|-----------|
| One-way broadcast (gossip, QUASAR_GOSSIP, channel notices) | Unidirectional (sender→receiver) | Open → write CBOR → FIN → close |
| Request/response (PING/PONG, ADDR, BOOTSTRAP) | Bidirectional | Client opens, sends request, server responds on same stream, both close |

### No framing

The stream's FIN signal acts as the message delimiter. When the receiver gets `QUIC_STREAM_EVENT_RECEIVE` data followed by `QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN`, it has the complete message. No 4-byte length prefix, no reassembly buffer.

### No datagrams

`DatagramReceiveEnabled` is removed from all QUIC settings. `DatagramSend` and `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` code paths are removed entirely from all Meridian components.

### Message flow examples

**PING/RET_PING (bidi stream):**
1. Node A opens a bidi stream to Node B
2. Node A writes CBOR-encoded PING packet, shuts down send side (FIN)
3. Node B receives stream data, decodes CBOR → handles PING
4. Node B writes CBOR-encoded RET_PING on the same stream, shuts down send side
5. Node A receives RET_PING, computes RTT
6. Stream closes on both sides

**GOSSIP (uni stream):**
1. Node A opens a uni stream to Node B
2. Node A writes CBOR-encoded GOSSIP packet, shuts down send side (FIN)
3. Node B receives stream data, decodes CBOR → handles gossip
4. Stream closes

---

## 2. Data Structure Changes

### Per-stream state

```c
typedef struct meridian_stream_entry_t {
    HQUIC stream;
    uint8_t* recv_buf;      // accumulate data until FIN (initial cap = MERIDIAN_MAX_PAYLOAD_SIZE)
    size_t recv_len;
    size_t recv_cap;
    bool is_bidi;           // true = request/response, false = one-way
    void* user_context;     // for matching requests to responses
} meridian_stream_entry_t;
```

### Per-connection changes

The connection context struct gains:
- A list/vector of active `meridian_stream_entry_t` for tracking in-flight streams
- A lock for thread-safe stream list access (msquic callbacks run on worker threads)

### Stream entry lifecycle

- Created in `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED` (incoming) or when the local side opens a stream (outgoing)
- Accumulates received data in `recv_buf` until FIN
- Destroyed in `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` — free recv_buf, remove from list

No reassembly buffer for partial frames. Since FIN delimits the message, bytes are appended until the stream is fully received, then CBOR is decoded once.

---

## 3. Component Changes

### meridian_protocol.c

- Remove `DatagramSend` in `meridian_protocol_send_packet()` → open uni stream to target peer, write CBOR, send FIN
- Remove `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` handler → handle incoming streams via `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED`
- Add stream callback: on RECEIVE → append to `recv_buf`; on PEER_SEND_SHUTDOWN → decode CBOR, dispatch to `meridian_protocol_on_packet()`, close stream
- `meridian_protocol_send_measure()`: open bidi stream instead of sending a datagram. Response comes back on the same stream.
- `meridian_protocol_broadcast()`: iterate peers, open uni stream to each (same loop structure, stream instead of datagram)
- `QUIC_SETTINGS`: remove `DatagramReceiveEnabled`

### meridian_relay.c (relay client)

- `meridian_relay_send_datagram()` → open uni stream to relay server, write `[4 bytes dest_endpoint_id + payload]`, send FIN
- `meridian_relay_send_addr_request()` → open bidi stream to relay server, write ADDR_REQUEST. ADDR_RESPONSE comes back on the same stream.
- Remove `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` handler → handle incoming streams via `PEER_STREAM_STARTED`
- Stream callback: on RECEIVE → append to recv_buf; on PEER_SEND_SHUTDOWN → decode and dispatch
- Remove `DatagramReceiveEnabled` from settings

### meridian_relay_server.c (relay server)

- Remove `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` handler → handle incoming streams via `PEER_STREAM_STARTED`
- Stream callback on receive+FIN: read 4-byte dest_endpoint_id from stream data, look up destination client, open uni stream to that client, write payload, send FIN
- For ADDR_REQUEST on a bidi stream: read request (dest_endpoint_id == 0), write ADDR_RESPONSE on the same stream, close
- Remove `DatagramReceiveEnabled` from settings

### meridian_rendv.c (rendezvous/tunnels)

- Tunnel connections currently pass NULL as the connection callback. They need a proper callback that handles `PEER_STREAM_STARTED` and stream events.
- Remove `DatagramReceiveEnabled` from tunnel settings
- `meridian_rendv_handle_detect_nat()` uses bidi streams through the relay for ADDR requests instead of datagrams

---

## 4. Stream Callbacks & Error Handling

### Two stream callbacks

1. **`meridian_uni_stream_callback`** — for one-way messages (gossip, QUASAR_GOSSIP, channel notices, relay datagrams)
   - RECEIVE: append to `recv_buf`
   - PEER_SEND_SHUTDOWN: decode CBOR, dispatch to handler, call `StreamShutdown(GRACEFUL)`
   - SHUTDOWN_COMPLETE: free `recv_buf`, remove from connection's stream list, call `StreamClose`
   - SEND_COMPLETE: free send buffer

2. **`meridian_bidi_stream_callback`** — for request/response (PING/PONG, ADDR, BOOTSTRAP)
   - RECEIVE: append to `recv_buf`
   - PEER_SEND_SHUTDOWN: decode request from `recv_buf`, process it, write response on same stream, call `StreamShutdown(GRACEFUL)` on our send side
   - SHUTDOWN_COMPLETE: free `recv_buf`, remove from stream list, call `StreamClose`
   - SEND_COMPLETE: free send buffer

### Error handling

| Scenario | Behavior |
|----------|----------|
| Stream open fails | Log error, return failure to caller |
| Stream send fails | `SEND_COMPLETE` with failure status — log and discard. No retry at transport level. |
| Receive buffer overflow (message > max size) | Close stream with error, log warning, discard partial data |
| Invalid CBOR on stream completion | Log warning, close stream, discard |
| Peer closes stream abnormally (no FIN) | Discard partial `recv_buf`, clean up stream entry |
| Connection drops mid-stream | `SHUTDOWN_COMPLETE` fires, all stream entries for that connection are cleaned up |

### Send buffer ownership

`StreamSend` requires the buffer to remain valid until `SEND_COMPLETE`. Allocate buffer, pass as `ClientContext`, free in `SEND_COMPLETE`. Same pattern as current code.

---

## 5. Relay Wire Format

### Current problem

The relay client sends CBOR arrays `[type, src, dest, payload]` but the relay server reads raw 4-byte dest IDs. This mismatch needs fixing.

### New format over streams

For one-way relay messages (RELAY_DATAGRAM equivalent):
- Client opens uni stream to relay
- Stream data: `[4 bytes: dest_endpoint_id (big-endian)] + [payload bytes]`
- Relay reads dest_endpoint_id, opens uni stream to destination, writes payload, FIN
- No CBOR wrapping for the relay envelope

For request/response relay messages (ADDR_REQUEST/RESPONSE):
- Client opens bidi stream to relay
- Stream data: `[4 bytes: dest_endpoint_id] + [CBOR payload]`
- ADDR_REQUEST uses dest_endpoint_id = 0 (addressed to relay server itself)
- Relay server handles dest_endpoint_id == 0 itself, forwards everything else

ADDR_RESPONSE: when a client sends an ADDR_REQUEST (bidi stream, dest=0), the relay writes the ADDR_RESPONSE on the same stream.

PUNCH_REQUEST / PUNCH_SYNC: flow through the relay via uni streams with the dest_endpoint_id prefix, same as RELAY_DATAGRAM.

### ALPN

ALPNs stay as `"meridian"` and `"meridian_relay"`. No change.

---

## 6. Testing & Migration

### Build verification

The refactoring touches 4 source files and their headers. After changes, the full build must pass and the daemon must start with `--enable-quic` flag.

### Testing

| Test | How |
|------|-----|
| Two nodes connect and gossip | Start two poseidond instances, verify gossip exchange over streams |
| PING/latency measurement | Measure RTT between two nodes, verify bidi stream PING/PONG works |
| Channel bootstrap | Join a channel, verify bootstrap request/response on bidi stream |
| Relay forwarding | Connect through relay server, verify uni stream forwarding between clients |
| ADDR discovery through relay | Verify bidi stream ADDR_REQUEST/RESPONSE |
| Connection loss | Kill one node, verify streams clean up without leaks |
| Invalid CBOR | Send garbage on a stream, verify stream closes cleanly |

### Stream resource limits

Set concurrent stream limits to 100 per connection via QUIC transport parameters (`PeerStreamCount` / `StreamMultiReceiveCount`) to prevent resource exhaustion from a misbehaving peer.