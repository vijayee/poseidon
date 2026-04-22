# ClientAPIs + Bootstrap Protocol + Transport Layer Design

**Goal:** Build the client-facing layer that lets applications create/join/destroy channels, subscribe to topics, and publish messages through the Poseidon daemon — with multiple transport bindings and a proper bootstrap wire protocol.

**Architecture:** Thread-per-transport model where each transport (Unix socket, TCP, WebSocket, QUIC, Android Binder, iOS XPC) runs in its own thread with a poll-dancer event loop. All transports speak the same CBOR-based client protocol to the daemon. The daemon resolves aliases, routes requests through the channel manager, and pushes delivery events back to clients. Channel bootstrap uses dedicated packet types on the dial channel's Quasar overlay.

**Tech Stack:** C (daemon + transports + C client lib), Kotlin (Android), Swift (iOS), CBOR (wire format), poll-dancer (event I/O), TLS for TCP/WebSocket transports

---

## 1. Topic ID System

Two namespaces serve different purposes:

| Context | ID format | Bit depth | Base58 length | Source |
|---|---|---|---|---|
| Dial channel (nodes & channels) | `poseidon_node_id_t` — BLAKE3(pub_key) → Base58 | 256 | ~43 chars | PKI-derived, cryptographically verifiable |
| In-channel topics | UUID v4 → Base58 | 128 | ~22 chars | Random, for pub/sub labeling |

Rationale: Dial channel IDs must be PKI-derived for authentication. In-channel topics are just labels — 128 bits of entropy is sufficient and the shorter Base58 representation is much easier for humans to type.

### Topic ID struct

```c
typedef struct poseidon_topic_id_t {
    uint8_t bytes[32];   // 256-bit for node IDs, upper 128 bits for UUIDs
    uint8_t bit_depth;   // 128 or 256
    char str[48];        // Base58-encoded representation
} poseidon_topic_id_t;
```

Factory functions:
- `poseidon_topic_id_from_node_id(const poseidon_node_id_t* node_id, poseidon_topic_id_t* out)` — wraps a PKI-derived node ID
- `poseidon_topic_id_generate(poseidon_topic_id_t* out)` — generates a random 128-bit UUID, encodes to Base58
- `poseidon_topic_id_from_string(const char* str, poseidon_topic_id_t* out)` — parses a Base58 string, infers bit depth from length

## 2. Unified Path Resolution

Clients address topics and subtopics using a single path string:

```
"<topic_or_alias>/<subtopic/path>"
```

Examples:
- `"Alice/Feeds/friend-only"` → resolve alias "Alice" → topic ID → subtopic "Feeds/friend-only"
- `"X4jKL.../Feeds/friend-only"` → raw topic ID (detected by length/charset) → subtopic "Feeds/friend-only"
- `"Alice"` → resolve alias, no subtopic (subscribe to all)
- `"X4jKL..."` → raw topic ID, no subtopic

### Resolution algorithm

1. Split path on `/` into components
2. First component: attempt alias resolution. If found, use the mapped topic ID. If not found, treat as a raw topic ID.
3. Remaining components joined with `/` = subtopic. Empty if only one component.
4. **Ambiguity check**: if an alias name is registered more than once (same name, different topic IDs), return an error listing the conflicting IDs. The client must disambiguate by using the raw topic ID.

The existing `topic_alias_registry_t` currently rejects duplicate names. This design extends it: allow duplicate names but flag them as ambiguous. Resolution returns a special "ambiguous" status with the list of candidate topic IDs.

### Alias registry changes

**Breaking change from current implementation:** `topic_alias_register` currently rejects duplicate names (`return -1`). This design changes it to allow multiple entries with the same name mapping to different topic IDs. The rationale: aliases are local to a client session, and two channels might legitimately share a human-readable name (e.g., two people both named "Alice" running their own channels).
- `topic_alias_resolve` returns a new result type:
  ```c
  typedef enum {
      TOPIC_ALIAS_RESOLVE_OK,        // single match
      TOPIC_ALIAS_RESOLVE_AMBIGUOUS, // multiple matches
      TOPIC_ALIAS_RESOLVE_NOT_FOUND  // no match
  } topic_alias_resolve_result_t;

  typedef struct {
      topic_alias_resolve_result_t status;
      const char* topic;          // valid if OK
      const char** candidates;     // valid if AMBIGUOUS
      size_t num_candidates;
  } topic_alias_resolve_out_t;
  ```
- `topic_alias_resolve(reg, name, &out)` populates the result struct

## 3. Bootstrap Wire Protocol

### Problem

`poseidon_channel_manager_join_channel` currently just subscribes to the topic on the dial channel and creates a local channel — it never actually discovers existing channel members or connects to them.

### Design

Two packet types already defined in `meridian_packet.h`:
- `MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP` (40)
- `MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY` (41)

### CHANNEL_BOOTSTRAP packet (type 40)

Published via Quasar on the dial channel by a node wanting to join:

```
CBOR array: [
    uint8(40),                           // packet type
    string(topic_id),                     // channel topic ID to join
    string(sender_node_id),              // dial node ID of the requester
    uint64(timestamp_us)                  // request timestamp for matching
]
```

The joining node:
1. Subscribes to the channel's topic on the dial channel
2. Publishes a CHANNEL_BOOTSTRAP via `quasar_publish` targeting that topic
3. Sets a timeout (default 5s, configurable via channel config)

### CHANNEL_BOOTSTRAP_REPLY packet (type 41)

Sent directly (not via Quasar) back to the requester by existing channel members:

```
CBOR array: [
    uint8(41),                           // packet type
    string(topic_id),                     // channel topic ID
    string(responder_node_id),            // responder's node ID in the channel
    uint32(responder_addr_be),            // responder's Meridian listen address
    uint16(responder_port_be),            // responder's Meridian listen port
    uint64(timestamp_us),                 // echoes requester's timestamp
    array([                               // seed nodes for faster bootstrap
        [uint32(addr_be), uint16(port_be)],
        ...
    ])
]
```

The joining node:
1. Collects replies until timeout
2. Connects to the first responder's Meridian address via `meridian_protocol_connect`
3. Adds seed nodes from the reply to accelerate ring population
4. Starts the channel with `poseidon_channel_start(channel, seeds, num_seeds)`

### Encode/decode functions

New in `meridian_packet.h`:

```c
cbor_item_t* meridian_channel_bootstrap_encode(const char* topic_id,
                                                const char* sender_node_id,
                                                uint64_t timestamp_us);

cbor_item_t* meridian_channel_bootstrap_reply_encode(const char* topic_id,
                                                      const char* responder_node_id,
                                                      uint32_t responder_addr,
                                                      uint16_t responder_port,
                                                      uint64_t timestamp_us,
                                                      const uint32_t* seed_addrs,
                                                      const uint16_t* seed_ports,
                                                      size_t num_seeds);

int meridian_channel_bootstrap_decode(const cbor_item_t* item,
                                       char* topic_id, size_t topic_buf_size,
                                       char* sender_node_id, size_t node_id_buf_size,
                                       uint64_t* timestamp_us);

int meridian_channel_bootstrap_reply_decode(const cbor_item_t* item,
                                             char* topic_id, size_t topic_buf_size,
                                             char* responder_node_id, size_t node_id_buf_size,
                                             uint32_t* responder_addr,
                                             uint16_t* responder_port,
                                             uint64_t* timestamp_us,
                                             uint32_t* seed_addrs,
                                             uint16_t* seed_ports,
                                             size_t* num_seeds,
                                             size_t max_seeds);
```

### Handler integration

In `meridian_protocol_on_packet`, add a case for type 40/41:
- Type 40 (BOOTSTRAP): if this node is a member of the requested channel, send a BOOTSTRAP_REPLY directly to the sender
- Type 41 (BOOTSTRAP_REPLY): if this node is waiting for bootstrap replies, store the reply

The channel manager tracks pending bootstrap requests:

```c
typedef struct pending_bootstrap_t {
    char topic_id[48];
    uint64_t timestamp_us;
    poseidon_channel_t* channel;
    uint32_t reply_addrs[16];
    uint16_t reply_ports[16];
    size_t num_replies;
} pending_bootstrap_t;
```

## 4. ClientAPI Protocol (CBOR Binary)

All client-daemon communication uses CBOR-framed messages over the transport connection. Each message is a CBOR array:

### Request frame

```
[
    uint8(0x01),          // frame_type = REQUEST
    uint32(request_id),   // client-assigned ID for matching responses
    uint8(method),        // method code
    ...params             // method-specific parameters
]
```

### Response frame

```
[
    uint8(0x02),          // frame_type = RESPONSE
    uint32(request_id),   // matches the request
    uint8(status),        // 0 = success, nonzero = error code
    ...result             // result data (if success)
]
```

### Event frame (server-pushed)

```
[
    uint8(0x03),          // frame_type = EVENT
    uint8(event_type),    // event type code
    ...event_data         // event-specific data
]
```

### Error codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Unknown method |
| 2 | Invalid parameters |
| 3 | Channel not found |
| 4 | Alias ambiguous |
| 5 | Not authorized (signature verification failed) |
| 6 | Channel already exists |
| 7 | Too many channels |
| 8 | Transport error |

### Method codes

| Code | Method | Params | Result |
|------|--------|--------|--------|
| 1 | CHANNEL_CREATE | [string(name), map(config_overrides)] | [string(topic_id)] |
| 2 | CHANNEL_JOIN | [string(topic_or_alias)] | [string(topic_id)] |
| 3 | CHANNEL_LEAVE | [string(topic_id)] | [] |
| 4 | CHANNEL_DESTROY | [string(topic_id), bytes(signature)] | [] |
| 5 | CHANNEL_MODIFY | [string(topic_id), map(config_changes), bytes(signature)] | [] |
| 6 | SUBSCRIBE | [string(topic_path)] | [] |
| 7 | UNSUBSCRIBE | [string(topic_path)] | [] |
| 8 | PUBLISH | [string(topic_path), bytes(data)] | [] |
| 9 | ALIAS_REGISTER | [string(name), string(topic_id)] | [] |
| 10 | ALIAS_UNREGISTER | [string(name)] | [] |
| 11 | ALIAS_RESOLVE | [string(name)] | [string(topic_id)] or [array(candidates)] |

### Event types

| Code | Event | Data |
|------|-------|------|
| 1 | DELIVERY | [string(topic_id), string(subtopic), bytes(data)] |
| 2 | CHANNEL_JOINED | [string(topic_id), string(node_id)] |
| 3 | CHANNEL_LEFT | [string(topic_id)] |
| 4 | PEER_EVENT | [string(topic_id), uint8(event_sub_type), string(peer_node_id)] |

### Admin signature format

For CHANNEL_DESTROY and CHANNEL_MODIFY, the client must prove ownership of the channel's private key. The signature is computed over:

```
sign_data = method_code || topic_id || timestamp_us
```

The client library signs this with the channel's private key using ED25519. The daemon verifies using the channel's public key (derived from the topic ID).

## 5. Transport Architecture

### Transport interface

```c
typedef enum {
    POSEIDON_TRANSPORT_UNIX_SOCKET,
    POSEIDON_TRANSPORT_TCP,
    POSEIDON_TRANSPORT_WEBSOCKET,
    POSEIDON_TRANSPORT_QUIC,
    POSEIDON_TRANSPORT_BINDER,
    POSEIDON_TRANSPORT_XPC
} poseidon_transport_type_t;

typedef struct poseidon_transport_t poseidon_transport_t;

typedef void (*poseidon_transport_on_client_cb)(poseidon_transport_t* transport,
                                                  int client_id,
                                                  const uint8_t* data,
                                                  size_t len);

struct poseidon_transport_t {
    const char* name;
    poseidon_transport_type_t type;
    PLATFORMTHREADTYPE thread;
    void* loop;                              // poll-dancer event loop (or platform-specific)
    poseidon_channel_manager_t* manager;      // shared, thread-safe
    poseidon_transport_on_client_cb on_message;
    
    int (*start)(poseidon_transport_t* self);
    int (*stop)(poseidon_transport_t* self);
    int (*send)(poseidon_transport_t* self, int client_id,
                const uint8_t* data, size_t len);
};
```

### Thread model

Each transport runs in its own thread:
1. Transport thread accepts connections
2. Reads CBOR frames from clients
3. Calls into `poseidon_channel_manager_t` (thread-safe via its lock)
4. Encodes responses/events as CBOR frames
5. Sends frames back to the client

The channel manager lock serializes access across all transport threads. For high-contention scenarios, a message queue could replace the lock later.

### Transport encryption

| Transport | Encryption |
|---|---|
| Unix socket | None — kernel enforces local isolation (filesystem permissions on socket path) |
| TCP | TLS — server presents certificate, optional client certificate |
| WebSocket | wss:// — TLS mandated |
| QUIC | Built-in TLS — QUIC mandates encryption |
| Android Binder | Kernel-enforced — Binder IPC isolated by UID/PID |
| iOS XPC | Kernel-enforced — XPC sandboxed by entitlements |

TLS configuration: the daemon loads a certificate and key (same as the dial channel's PKI). TCP and WebSocket transports use these for their TLS contexts.

### Transport implementations

Each transport file implements the `poseidon_transport_t` interface:

```
src/ClientAPIs/
    transport.h              # Interface definition
    transport_unix.c         # Unix domain socket (Linux, macOS)
    transport_tcp.c           # TCP + TLS (all platforms)
    transport_ws.c           # WebSocket over TLS (all platforms)
    transport_quic.c         # QUIC transport using msquic singleton
    transport_binder.c       # Android Binder IPC (#ifdef __ANDROID__)
    transport_xpc.c          # iOS XPC (#ifdef __APPLE__ && TARGET_OS_IPHONE)
```

Binder and XPC don't use poll-dancer — they have their own callback-driven APIs. The transport wrapper bridges those callbacks to the `on_message` callback.

## 6. Daemon Entry Point

New `src/poseidond.c`:

1. Parse command-line config (port range, transport enable flags, TLS paths)
2. Initialize msquic singleton
3. Create work pool and timing wheel
4. Create channel manager with dial channel
5. Start each enabled transport (each in its own thread)
6. Run periodic tick/gossip on a timer thread
7. Signal handling (SIGINT/SIGTERM) → stop all transports, destroy manager, close msquic singleton

## 7. Client Libraries

Each library wraps the CBOR protocol into a native API. The library handles:
- Connection management (connect, reconnect, disconnect)
- CBOR frame encoding/decoding
- Request ID tracking (match responses to requests)
- Event dispatch (register callbacks for delivery, peer events, etc.)
- Admin signing (holds private key, signs modify/destroy requests)

### C client library (reference)

```
src/client_libs/c/
    poseidon_client.h
    poseidon_client.c
```

```c
typedef struct poseidon_client_t poseidon_client_t;

typedef void (*poseidon_delivery_cb_t)(void* ctx, const char* topic_id,
                                        const char* subtopic,
                                        const uint8_t* data, size_t len);

typedef void (*poseidon_event_cb_t)(void* ctx, uint8_t event_type,
                                     const uint8_t* data, size_t len);

poseidon_client_t* poseidon_client_connect(const char* transport_url);
void poseidon_client_disconnect(poseidon_client_t* client);

// Channel lifecycle
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

// Pub/sub
int poseidon_client_subscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_unsubscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_publish(poseidon_client_t* client, const char* topic_path,
                             const uint8_t* data, size_t len);

// Aliases
int poseidon_client_alias_register(poseidon_client_t* client, const char* name,
                                    const char* topic_id);
int poseidon_client_alias_unregister(poseidon_client_t* client, const char* name);

// Events
void poseidon_client_on_delivery(poseidon_client_t* client,
                                  poseidon_delivery_cb_t cb, void* ctx);
void poseidon_client_on_event(poseidon_client_t* client,
                               poseidon_event_cb_t cb, void* ctx);
```

Transport URL format: `"unix:///var/run/poseidond.sock"`, `"tcp://127.0.0.1:9090"`, `"ws://127.0.0.1:9091"`, `"quic://127.0.0.1:9092"`, `"binder:poseidon"`, `"xpc:poseidon"`.

### Android client library (Kotlin + Java interop)

```
src/client_libs/android/
    PoseidonClient.kt
    PoseidonConnection.kt
```

Uses Android Binder for IPC. Connects to the daemon's Binder service. Provides two API styles:

**Kotlin-native API** — coroutine-based, idiomatic for Kotlin apps:

```kotlin
class PoseidonClient(private val connection: PoseidonConnection) {
    suspend fun createChannel(name: String): String
    suspend fun joinChannel(topicOrAlias: String): String
    suspend fun leaveChannel(topicId: String)
    suspend fun destroyChannel(topicId: String, ownerKey: PrivateKey)
    suspend fun modifyChannel(topicId: String, config: ChannelConfig, ownerKey: PrivateKey)
    suspend fun subscribe(topicPath: String)
    suspend fun unsubscribe(topicPath: String)
    suspend fun publish(topicPath: String, data: ByteArray)
    suspend fun registerAlias(name: String, topicId: String)
    suspend fun unregisterAlias(name: String)

    fun onDelivery(callback: (topicId: String, subtopic: String, data: ByteArray) -> Unit)
}
```

**Java-compatible API** — blocking wrappers that Java code can call without coroutines. These are generated alongside the Kotlin API and delegate to the coroutine versions via `runBlocking`:

```kotlin
// Java-friendly blocking wrappers (also callable from Kotlin)
class PoseidonClientJava(private val client: PoseidonClient) {
    fun createChannelBlocking(name: String): String
    fun joinChannelBlocking(topicOrAlias: String): String
    fun leaveChannelBlocking(topicId: String)
    fun destroyChannelBlocking(topicId: String, ownerKey: PrivateKey)
    fun modifyChannelBlocking(topicId: String, config: ChannelConfig, ownerKey: PrivateKey)
    fun subscribeBlocking(topicPath: String)
    fun unsubscribeBlocking(topicPath: String)
    fun publishBlocking(topicPath: String, data: ByteArray)
    fun registerAliasBlocking(name: String, topicId: String)
    fun unregisterAliasBlocking(name: String)
}
```

Both APIs share the same underlying `PoseidonConnection` and CBOR protocol. The `PoseidonClientJava` class is a thin wrapper — it simply calls the suspend functions inside `runBlocking { }`. Java developers use `PoseidonClientJava` directly; Kotlin developers use `PoseidonClient`.

### iOS client library (Swift + Objective-C interop)

```
src/client_libs/swift/
    PoseidonClient.swift
    PoseidonConnection.swift
```

Uses XPC for IPC. Provides two API styles:

**Swift-native API** — async/await, idiomatic for Swift apps:

```swift
class PoseidonClient {
    func createChannel(name: String) async throws -> String
    func joinChannel(topicOrAlias: String) async throws -> String
    func leaveChannel(topicId: String) async throws
    func destroyChannel(topicId: String, ownerKey: PrivateKey) async throws
    func modifyChannel(topicId: String, config: ChannelConfig, ownerKey: PrivateKey) async throws
    func subscribe(topicPath: String) async throws
    func unsubscribe(topicPath: String) async throws
    func publish(topicPath: String, data: Data) async throws
    func registerAlias(name: String, topicId: String) async throws
    func unregisterAlias(name: String) async throws

    func onDelivery(_ handler: @escaping (String, String, Data) -> Void)
}
```

**Objective-C-compatible API** — completion-handler wrappers marked `@objc` for Objective-C callers. Swift's `async throws` functions cannot cross the ObjC bridge directly, so these wrappers translate to/from completion handlers:

```swift
@objcMembers
class PoseidonClientObjC: NSObject {
    private let client: PoseidonClient

    func createChannel(name: String, completion: @escaping (String?, Error?) -> Void)
    func joinChannel(topicOrAlias: String, completion: @escaping (String?, Error?) -> Void)
    func leaveChannel(topicId: String, completion: @escaping (Error?) -> Void)
    func destroyChannel(topicId: String, ownerKey: PrivateKey, completion: @escaping (Error?) -> Void)
    func modifyChannel(topicId: String, config: ChannelConfig, ownerKey: PrivateKey, completion: @escaping (Error?) -> Void)
    func subscribe(topicPath: String, completion: @escaping (Error?) -> Void)
    func unsubscribe(topicPath: String, completion: @escaping (Error?) -> Void)
    func publish(topicPath: String, data: Data, completion: @escaping (Error?) -> Void)
    func registerAlias(name: String, topicId: String, completion: @escaping (Error?) -> Void)
    func unregisterAlias(name: String, completion: @escaping (Error?) -> Void)
}
```

Both APIs share the same `PoseidonConnection` and XPC protocol. The `PoseidonClientObjC` class wraps each async call with `Task { }` and dispatches results to the completion handler on the main queue. Objective-C developers use `PoseidonClientObjC`; Swift developers use `PoseidonClient`.

## 8. Directory Structure

```
src/
  ClientAPIs/
    transport.h              # poseidon_transport_t interface
    transport_unix.c         # Unix domain socket transport
    transport_tcp.c          # TCP + TLS transport
    transport_ws.c           # WebSocket over TLS transport
    transport_quic.c         # QUIC transport (via msquic singleton)
    transport_binder.c       # Android Binder transport
    transport_xpc.c          # iOS XPC transport
    client_protocol.h        # CBOR frame types, method/event codes, encode/decode
    client_protocol.c        # CBOR frame serialization/deserialization
    client_session.h         # Per-client session state (subscriptions, ID tracking)
    client_session.c         # Session management
  client_libs/
    c/
      poseidon_client.h
      poseidon_client.c
    android/
      PoseidonClient.kt         # Kotlin coroutine API
      PoseidonClientJava.kt      # Java-compatible blocking wrappers
      PoseidonConnection.kt
    swift/
      PoseidonClient.swift       # Swift async/await API
      PoseidonClientObjC.swift   # Objective-C completion-handler wrappers
      PoseidonConnection.swift
  poseidond.c                # Daemon main
```

## 9. Implementation Phases

This spec is large enough to require phased implementation:

**Phase 1:** Bootstrap wire protocol — packet encode/decode, handler integration, channel_manager join_channel rewrite
**Phase 2:** ClientAPI protocol — CBOR frame types, client protocol encode/decode, session management
**Phase 3:** Daemon + Unix transport — poseidond.c, transport_unix.c, end-to-end local testing
**Phase 4:** TCP + WebSocket + QUIC transports — TLS configuration, transport_tcp.c, transport_ws.c, transport_quic.c
**Phase 5:** C client library — poseidon_client.h/c with all methods and signing
**Phase 6:** Android Binder transport + Kotlin client library
**Phase 7:** iOS XPC transport + Swift client library
**Phase 8:** Topic ID system + alias ambiguity + unified path resolution