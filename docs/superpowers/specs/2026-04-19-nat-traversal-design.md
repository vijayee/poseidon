# NAT Traversal for Meridian

## Context

Meridian uses MSQUIC for all P2P connections. Currently, connections go directly between peers with no NAT awareness. The rendezvous subsystem (`meridian_rendv`) has NAT type enums and stub functions, but no actual traversal logic. Nodes behind symmetric NATs cannot connect at all, and nodes behind restrictive NATs may fail intermittently.

We need a per-connection strategy: try direct first when possible, relay through a rendezvous server when necessary, and upgrade/downgrade between the two as conditions change.

## Architecture

Three components, layered on the existing `meridian_rendv` module:

```
Layer 1: NAT Detection (meridian_rendv)
  - Probe relay servers to discover reflexive address
  - Compare addresses across multiple probes to classify NAT type
  - Store result in meridian_rendv_t.nat_type

Layer 2: Relay Protocol (meridian_relay)
  - QUIC-based relay server that forwards datagrams between peers
  - Each peer maintains one QUIC connection to the relay
  - Relay assigns endpoint IDs and routes datagrams by ID
  - Ping/pong keepalive

Layer 3: Connection Manager (meridian_conn)
  - Per-connection state machine: DIRECT / TRYING_DIRECT / RELAY / RELAY_ONLY
  - Attempts direct connection first (if address known)
  - Falls back to relay if direct fails or times out
  - Upgrades from relay to direct when hole-punching succeeds
  - Tracks path state: RTT, last activity, preferred path
```

## File Organization

```
src/Network/Meridian/
  meridian_rendv.h / meridian_rendv.c          (existing — extend with NAT detection)
  meridian_relay.h / meridian_relay.c           (new — relay client protocol)
  meridian_relay_server.h / meridian_relay_server.c  (new — relay server)
  meridian_conn.h / meridian_conn.c             (new — connection manager)
src/
  relay_main.c                                  (new — relay server CLI entry point)
```

## Layer 1: NAT Detection

### Approach

Use QAD (QUIC Address Discovery) via the relay server. The relay observes the source address of incoming QUIC connections and reports it back. By comparing reflexive addresses from multiple relays, we classify the NAT type:

- **OPEN**: Local address matches reflexive address
- **EIM (Endpoint-Independent Mapping)**: Same reflexive address from all relays → full cone, restricted cone, or port-restricted cone
- **EDM (Endpoint-Dependent Mapping)**: Different reflexive addresses from different relays → symmetric NAT

### Extending meridian_rendv_t

```c
typedef struct meridian_rendv_t {
    refcounter_t refcounter;
    uint32_t addr;                    // Configured address
    uint16_t port;                    // Configured port
    uint32_t reflexive_addr;          // Discovered public address (0 if unknown)
    uint16_t reflexive_port;          // Discovered public port (0 if unknown)
    meridian_nat_type_t nat_type;     // Detected NAT type
    bool is_active;
} meridian_rendv_t;
```

### New Functions

| Function | Description |
|----------|-------------|
| `meridian_rendv_detect_nat(handle, callback, ctx)` | **Replace stub.** Connect to relay(s), compare reflexive addresses, classify NAT type. Async — invokes callback when detection completes. |
| `meridian_rendv_get_reflexive_addr(rendv)` | Returns discovered public address (0 if unknown). |
| `meridian_rendv_get_reflexive_port(rendv)` | Returns discovered public port (0 if unknown). |
| `meridian_rendv_can_direct_connect(rendv)` | Returns true if NAT type is OPEN, FULL_CONE, RESTRICTED_CONE, or PORT_RESTRICTED_CONE. |

### Detection Algorithm

```
1. Connect to relay A via QUIC
2. Send ADDR_REQUEST packet
3. Relay responds with observed source (reflexive_addr_A, reflexive_port_A)
4. Connect to relay B via QUIC
5. Send ADDR_REQUEST packet
6. Relay responds with observed source (reflexive_addr_B, reflexive_port_B)
7. Compare:
   - If local_addr == reflexive_addr: NAT_TYPE_OPEN
   - If reflexive_addr_A == reflexive_addr_B: EIM (cone variants)
   - If reflexive_addr_A != reflexive_addr_B: NAT_TYPE_SYMMETRIC
```

For EIM types, further probes distinguish full cone vs restricted cone vs port-restricted cone by attempting inbound from uncontacted sources.

### Packet Types (add to meridian_packet.h)

```c
#define MERIDIAN_PACKET_TYPE_ADDR_REQUEST  30
#define MERIDIAN_PACKET_TYPE_ADDR_RESPONSE  31
#define MERIDIAN_PACKET_TYPE_PUNCH_REQUEST  32
#define MERIDIAN_PACKET_TYPE_PUNCH_SYNC     33
```

## Layer 2: Relay Protocol

### Struct

```c
typedef struct meridian_relay_config_t {
    const char* alpn;                         // ALPN for relay connections
    uint32_t idle_timeout_ms;                 // Connection idle timeout
    uint32_t max_datagram_size;               // Max datagram payload size
    uint32_t keepalive_interval_ms;           // Ping interval
} meridian_relay_config_t;

typedef struct meridian_relay_t {
    refcounter_t refcounter;
    const struct QUIC_API_TABLE* msquic;
    HQUIC registration;
    HQUIC configuration;
    HQUIC connection;                         // QUIC connection to relay server
    meridian_rendv_t* server;                 // Relay server address
    meridian_relay_config_t config;
    uint32_t local_endpoint_id;              // Our assigned endpoint ID
    bool connected;
    PLATFORMLOCKTYPE(lock);
} meridian_relay_t;
```

### API

| Function | Description |
|----------|-------------|
| `meridian_relay_create(msquic, registration, server, config)` | Create relay client |
| `meridian_relay_destroy(relay)` | Destroy relay client |
| `meridian_relay_connect(relay)` | Open QUIC connection to relay server |
| `meridian_relay_disconnect(relay)` | Close connection |
| `meridian_relay_send_datagram(relay, data, len, dest_endpoint_id)` | Send datagram to peer via relay |
| `meridian_relay_send_addr_request(relay)` | Request reflexive address from relay |
| `meridian_relay_on_datagram(relay, callback, ctx)` | Register handler for incoming datagrams |
| `meridian_relay_on_addr_response(relay, callback, ctx)` | Register handler for address discovery response |
| `meridian_relay_get_endpoint_id(relay)` | Get our assigned endpoint ID |
| `meridian_relay_is_connected(relay)` | Check connection status |

### Relay Wire Format

All relay frames use CBOR encoding (consistent with Meridian packet format):

```
ADDR_REQUEST:   [type=30, query_id]
ADDR_RESPONSE:  [type=31, query_id, reflexive_addr, reflexive_port, assigned_endpoint_id]
DATAGRAM:       [type=32, src_endpoint_id, dest_endpoint_id, payload]
PUNCH_REQUEST:  [type=33, from_endpoint_id, target_addr, target_port]
PUNCH_SYNC:     [type=34, from_endpoint_id, from_addr, from_port]
```

The relay server is implemented alongside the client — see Layer 2B below.

## Layer 2B: Relay Server

### Overview

The relay server is a standalone binary that peers connect to for NAT traversal. It is required for testing — without it, the client-side relay protocol and connection manager cannot function.

### File Organization

```
src/Network/Meridian/
  meridian_relay_server.h / meridian_relay_server.c   (new — relay server)
```

A `main()` entry point in `src/relay_main.c` creates the server binary.

### Struct

```c
#define MERIDIAN_RELAY_MAX_CLIENTS 256
#define MERIDIAN_RELAY_ENDPOINT_ID_NONE 0

typedef struct meridian_relay_client_t {
    uint32_t endpoint_id;               // Assigned endpoint ID (0 = unassigned)
    HQUIC connection;                    // QUIC connection handle
    struct sockaddr_storage remote_addr; // Observed remote address
    uint64_t last_activity_ms;          // Last activity timestamp
    bool authenticated;                  // Has completed initial handshake
} meridian_relay_client_t;

typedef struct meridian_relay_server_t {
    refcounter_t refcounter;
    const struct QUIC_API_TABLE* msquic;
    HQUIC registration;
    HQUIC configuration;
    HQUIC listener;                      // QUIC listener for incoming connections
    uint16_t listen_port;                // Port to listen on
    meridian_relay_client_t clients[MERIDIAN_RELAY_MAX_CLIENTS];
    size_t num_clients;
    uint32_t next_endpoint_id;           // Monotonically increasing endpoint ID
    PLATFORMLOCKTYPE(lock);
} meridian_relay_server_t;

typedef struct meridian_relay_server_config_t {
    const char* alpn;                    // ALPN for relay connections
    uint16_t listen_port;                // Port to listen on
    uint32_t idle_timeout_ms;            // Client idle timeout
    uint32_t keepalive_interval_ms;      // Ping interval
    uint32_t max_datagram_size;          // Max datagram payload
} meridian_relay_server_config_t;
```

### Server API

| Function | Description |
|----------|-------------|
| `meridian_relay_server_create(msquic, config)` | Create relay server instance |
| `meridian_relay_server_destroy(server)` | Destroy relay server |
| `meridian_relay_server_start(server)` | Start listening for connections |
| `meridian_relay_server_stop(server)` | Stop accepting connections, disconnect clients |
| `meridian_relay_server_get_stats(server, &stats)` | Get connection stats (client count, datagrams forwarded, etc.) |

### Server Behavior

**On client connect (ListenerCallback → NEW_CONNECTION):**
1. Accept the QUIC connection
2. Assign next endpoint ID (`next_endpoint_id++`)
3. Store client in `clients[]` with observed source address
4. Send ADDR_RESPONSE to client with their reflexive address and assigned endpoint ID
5. Increment `num_clients`

**On datagram received (ConnectionCallback → DATAGRAM_RECEIVED):**
1. Decode CBOR frame
2. If type is ADDR_REQUEST: respond with ADDR_RESPONSE containing observed source address and endpoint ID
3. If type is DATAGRAM: look up destination endpoint ID in `clients[]`, forward datagram to that client (prepend source endpoint ID)
4. If type is PUNCH_REQUEST: look up target endpoint ID, forward PUNCH_REQUEST to target with sender's address info
5. If type is PUNCH_SYNC: look up target endpoint ID, forward PUNCH_SYNC to target with sender's address info
6. If destination endpoint ID not found: silently drop (or send error)

**On client disconnect (ConnectionCallback → SHUTDOWN_INITIATED):**
1. Remove client from `clients[]`
2. Decrement `num_clients`
3. Optionally notify other clients that this endpoint is gone (similar to iroh's `EndpointGone`)

**Ping/Pong keepalive:**
- Server sends `PING` frame every `keepalive_interval_ms`
- Client must respond with `PONG` frame
- If no response within `idle_timeout_ms`, disconnect client

### Wire Format Extensions

Add ping/pong frames for keepalive:

```c
#define MERIDIAN_PACKET_TYPE_RELAY_PING  34
#define MERIDIAN_PACKET_TYPE_RELAY_PONG  35
#define MERIDIAN_PACKET_TYPE_ENDPOINT_GONE 36
```

```
PING:          [type=34, timestamp_ms]
PONG:          [type=35, timestamp_ms]
ENDPOINT_GONE: [type=36, endpoint_id]
DATAGRAM:      [type=32, src_endpoint_id, dest_endpoint_id, payload]
```

### Entry Point

`src/relay_main.c` provides a minimal CLI:

```c
int main(int argc, char** argv) {
    // Parse args: --port <port> [--alpn <alpn>]
    // Create msquic, registration, configuration
    // Create relay server
    // Start server
    // Block until SIGINT/SIGTERM
    // Stop and destroy
}
```

Build as a separate `meridian_relay` executable in CMakeLists.txt.

## Layer 3: Connection Manager

### Per-Connection State Machine

```c
typedef enum {
    MERIDIAN_CONN_STATE_DIRECT,        // Direct QUIC connection active
    MERIDIAN_CONN_STATE_TRYING_DIRECT, // Attempting direct connection, relay as backup
    MERIDIAN_CONN_STATE_RELAY,         // Relay only, direct attempt failed or not tried
    MERIDIAN_CONN_STATE_RELAY_ONLY     // Symmetric NAT — never try direct
} meridian_conn_state_t;

typedef struct meridian_conn_path_t {
    uint32_t addr;                      // Peer address
    uint16_t port;                      // Peer port
    uint32_t reflexive_addr;            // Peer's discovered public address
    uint16_t reflexive_port;            // Peer's discovered public port
    uint32_t rtt_ms;                    // Measured round-trip time
    uint64_t last_activity_ms;          // Last activity timestamp
    bool active;                        // Path is currently usable
} meridian_conn_path_t;

typedef struct meridian_conn_t {
    refcounter_t refcounter;
    meridian_node_t* peer;              // Peer we're connecting to
    HQUIC direct_connection;            // Direct QUIC connection (NULL if relay-only)
    meridian_relay_t* relay;            // Relay client (NULL if direct-only)
    uint32_t relay_endpoint_id;         // Peer's relay endpoint ID (0 if unknown)
    meridian_conn_state_t state;
    meridian_conn_path_t direct_path;   // Direct path info
    meridian_conn_path_t relay_path;    // Relay path info
    meridian_nat_type_t local_nat_type; // Our NAT type
    meridian_nat_type_t peer_nat_type;  // Peer's NAT type
    uint32_t direct_attempts;           // Count of direct connection attempts
    uint64_t last_direct_attempt_ms;    // Timestamp of last direct attempt
    PLATFORMLOCKTYPE(lock);
} meridian_conn_t;
```

### Connection Manager API

| Function | Description |
|----------|-------------|
| `meridian_conn_create(protocol, peer, relay)` | Create connection to peer |
| `meridian_conn_destroy(conn)` | Destroy connection |
| `meridian_conn_connect(conn)` | Initiate connection (tries direct first if possible) |
| `meridian_conn_disconnect(conn)` | Close connection |
| `meridian_conn_send(conn, data, len)` | Send data — uses best available path |
| `meridian_conn_upgrade_to_direct(conn)` | Attempt direct connection upgrade (call-me-maybe) |
| `meridian_conn_set_peer_nat_type(conn, type)` | Update peer's NAT type |
| `meridian_conn_set_peer_reflexive(conn, addr, port)` | Update peer's discovered public address |
| `meridian_conn_get_state(conn)` | Get current connection state |
| `meridian_conn_get_preferred_path(conn)` | Returns DIRECT or RELAY |
| `meridian_conn_is_direct(conn)` | Check if currently using direct path |
| `meridian_conn_is_relay(conn)` | Check if currently using relay path |

### Connection Strategy

```
When connecting to a peer:

1. Check local NAT type (from detection)
2. If local NAT type is SYMMETRIC:
   - Set state = RELAY_ONLY
   - Skip direct attempt entirely
   - Use relay exclusively

3. If peer address is known:
   - Set state = TRYING_DIRECT
   - Attempt direct QUIC connection to peer's known address
   - Simultaneously connect to relay for backup
   - If direct succeeds within timeout: state = DIRECT, keep relay as fallback
   - If direct fails or times out: state = RELAY

4. If peer address is unknown (only have relay endpoint ID):
   - Set state = RELAY
   - Connect via relay
   - If peer advertises direct address through relay: attempt upgrade to TRYING_DIRECT
```

### Direct Path Upgrade (Call-Me-Maybe)

When both peers are connected via relay and have non-symmetric NATs:

```
1. Peer A sends PUNCH_REQUEST to relay, targeting Peer B's endpoint ID
2. Relay forwards PUNCH_REQUEST to Peer B with Peer A's address info
3. Peer B sends PUNCH_SYNC to Peer A's address (opens NAT mapping)
4. Peer A simultaneously sends PUNCH_SYNC to Peer B's address
5. Both peers attempt direct QUIC connection to each other
6. If direct handshake succeeds:
   - Upgrade state to DIRECT
   - Close relay path (or keep as monitoring fallback)
7. If direct handshake fails:
   - Stay in RELAY state
   - Mark peer NAT type as potentially symmetric
```

### Path Selection

When both direct and relay paths are available:

```c
meridian_conn_path_t* meridian_conn_select_path(meridian_conn_t* conn) {
    // Prefer direct path if active and RTT is reasonable
    if (conn->direct_path.active &&
        conn->direct_path.rtt_ms > 0 &&
        conn->direct_path.rtt_ms < conn->relay_path.rtt_ms) {
        return &conn->direct_path;
    }
    // Fall back to relay
    if (conn->relay_path.active) {
        return &conn->relay_path;
    }
    return NULL;  // No usable path
}
```

Direct path is preferred when its RTT is lower than relay RTT. If both have equal RTT, direct wins (avoids relay overhead). If direct path drops, fall back to relay automatically.

### Integration with meridian_protocol_t

The connection manager integrates into the existing protocol struct:

```c
typedef struct meridian_protocol_t {
    // ... existing fields ...
    meridian_conn_t** connections;           // Dynamic array of managed connections
    size_t num_connections;
    meridian_relay_t* default_relay;          // Default relay server
    // ...
} meridian_protocol_t;
```

**Connection flow changes:**

- `meridian_protocol_connect()` creates a `meridian_conn_t` instead of directly opening a QUIC connection
- The `meridian_conn_t` manages whether the connection goes direct or through relay
- `meridian_protocol_send_packet()` uses `meridian_conn_send()` which selects the best path
- `meridian_protocol_broadcast()` sends to each connection via its preferred path

**Packet handling changes:**

- Incoming PUNCH_REQUEST/PUNCH_SYNC packets are routed to the connection manager
- ADDR_REQUEST/ADDR_RESPONSE packets are handled by the relay module
- Gossip, query, and measure packets work the same regardless of path (direct or relay)

## Thread Safety

All three layers use `PLATFORMLOCKTYPE(lock)`:
- `meridian_rendv_t`: lock for NAT detection state, reflexive address updates
- `meridian_relay_t`: lock for connection state, datagram send/receive
- `meridian_conn_t`: lock for state transitions, path selection, NAT type updates

The connection manager must handle concurrent state transitions:
- Direct path may succeed while relay path is still establishing
- NAT type detection may complete after connection is already established
- Peer may advertise a different address than initially known

All state transitions happen under lock. Callbacks (datagram received, address discovered) acquire lock, update state, release lock.

## Testing Strategy

1. **NAT detection test**: Mock relay responses, verify classification logic for each NAT type
2. **Relay client test**: Verify CBOR encoding/decoding of relay frames, datagram send/receive
3. **Relay server test**: Start server, connect clients, verify endpoint assignment, datagram forwarding, address discovery
4. **Connection manager test**: State machine transitions — direct success, direct timeout, relay fallback, upgrade, symmetric NAT skip
5. **Integration test**: Full flow — start relay server, connect two peers, detect NAT, connect via relay, upgrade to direct, verify path selection

## Build Integration

Add to CMakeLists.txt:
- Source files: `meridian_relay.c`, `meridian_relay_server.c`, `meridian_conn.c`, `relay_main.c`
- Library source: add relay and conn files to POSEIDON_SOURCES
- Executable: `meridian_relay` binary (from `relay_main.c`)
- Test executables: `meridian_relay_test`, `meridian_relay_server_test`, `meridian_conn_test`
- Link with existing poseidon library, cbor, msquic, OpenSSL