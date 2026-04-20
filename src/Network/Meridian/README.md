# Meridian Network Protocol

Meridian is a distributed network protocol designed for peer-to-peer node discovery and latency-based routing. It uses a multi-ring architecture organized by latency buckets, with gossip-based node exchange and rendezvous point support for NAT traversal. All transport uses QUIC via MSQUIC for encrypted, multiplexed connections.

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                         meridian_protocol_t                          │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────────┐  │
│  │  ring_set    │  │  gossip_     │  │  latency_cache             │  │
│  │  (rings)     │  │  handle      │  │  (measurements)            │  │
│  └──────────────┘  └──────────────┘  └───────────────────────────┘  │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────────┐  │
│  │  rendv_      │  │  pool/wheel  │  │  connections[]             │  │
│  │  handle      │  │  (scheduling)│  │  (per-peer conn manager)  │  │
│  └──────────────┘  └──────────────┘  └───────────────────────────┘  │
│  ┌──────────────┐  ┌──────────────┐                                 │
│  │  msquic      │  │  default_   │                                 │
│  │  (QUIC API)  │  │  relay      │                                 │
│  └──────────────┘  └──────────────┘                                 │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  QUIC listener (incoming)     connected_peers[] (outgoing)    │  │
│  └──────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

## Transport

All Meridian communication uses **QUIC** (via [MSQUIC](https://github.com/microsoft/msquic)) instead of raw UDP sockets. QUIC provides:

- **Encryption** — TLS 1.3 on every connection, no plaintext traffic
- **Multiplexing** — Multiple streams over a single connection, no head-of-line blocking
- **Datagrams** — Unreliable datagram frames for latency-sensitive gossip and measurement packets
- **Connection migration** — Survives IP address changes (useful for mobile nodes)
- **0-RTT** — Resumed connections skip handshake latency

The protocol instance holds MSQUIC handles (`msquic`, `registration`, `listener`, `configuration`) and a `local_addr` for the QUIC listener. Peer connections are tracked as `HQUIC` handles in `connected_peers[]`.

## File Overview

### Core Types

| File | Purpose |
|------|---------|
| `meridian.h` / `meridian.c` | Core `meridian_node_t` struct and node lifecycle management. Nodes are reference-counted and have an address, port, and optional rendezvous point. |

### Packet Encoding

| File | Purpose |
|------|---------|
| `meridian_packet.h` / `meridian_packet.c` | CBOR-based serialization for all protocol messages. Defines packet structures for gossip, ping, response, and relay/NAT traversal messages. Uses libcbor for encoding/decoding. |

### Protocol Core

| File | Purpose |
|------|---------|
| `meridian_protocol.h` / `meridian_protocol.c` | Main protocol orchestrator. Manages QUIC listener, peer connections, and coordinates all subsystems (ring_set, gossip, rendv, measure, connections). Entry point for starting/stopping the protocol. |

### Ring-Based Node Storage

| File | Purpose |
|------|---------|
| `meridian_ring.h` / `meridian_ring.c` | Multi-ring latency bucketing system. Nodes are stored in rings based on their latency value using a logarithmic bucketing algorithm (log_base). Primary ring holds preferred nodes; secondary holds candidates for replacement. |

### Gossip Protocol

| File | Purpose |
|------|---------|
| `meridian_gossip.h` / `meridian_gossip.c` | Epidemic gossip for node exchange. Uses a two-phase schedule: aggressive initialization with frequent gossips, then steady-state with slower intervals. Manages active gossip queries and tracks pending replies. |

### Query Tracking

| File | Purpose |
|------|---------|
| `meridian_query.h` / `meridian_query.c` | Tracks in-flight queries with timeout support. Query table manages lifecycle of gossip/closest/measure queries. The `tick()` function expires stale queries and transfers ownership to caller via CONSUME pattern. |

### Latency Measurement

| File | Purpose |
|------|---------|
| `meridian_measure.h` / `meridian_measure.c` | Latency measurement system with caching. Latency cache stores recent measurements and evicts stale entries. Measure requests track timeout and execute callbacks on completion. |

### NAT Traversal: Rendezvous

| File | Purpose |
|------|---------|
| `meridian_rendv.h` / `meridian_rendv.c` | Rendezvous point management, QUIC tunnel creation, NAT type detection (open, full-cone, restricted-cone, port-restricted, symmetric), and hole-punching support. Detects NAT type by comparing reflexive addresses from multiple relay servers. |

### NAT Traversal: Relay Server

| File | Purpose |
|------|---------|
| `meridian_relay_server.h` / `meridian_relay_server.c` | QUIC-based relay server that accepts client connections, assigns endpoint IDs, forwards datagrams between peers, and provides address discovery (ADDR_REQUEST/ADDR_RESPONSE). Runs as a standalone binary (`meridian_relay`). |

### NAT Traversal: Relay Client

| File | Purpose |
|------|---------|
| `meridian_relay.h` / `meridian_relay.c` | Relay client that maintains a QUIC connection to a relay server. Sends datagrams to peers via the relay, requests reflexive address discovery, and registers callbacks for incoming datagrams and address responses. |

### NAT Traversal: Connection Manager

| File | Purpose |
|------|---------|
| `meridian_conn.h` / `meridian_conn.c` | Per-connection state machine that manages direct and relay paths. Prefers direct QUIC connections, falls back to relay, and supports automatic upgrade to direct via hole-punching (call-me-maybe). Symmetric NAT peers are routed relay-only. |

## Data Structures

### meridian_node_t
```c
typedef struct meridian_node_t {
    refcounter_t refcounter;    // Reference counting for lifetime management
    uint32_t addr;              // IPv4 address
    uint16_t port;              // Meridian listener port
    uint32_t rendv_addr;        // Rendezvous point address (for NAT traversal)
    uint16_t rendv_port;        // Rendezvous point port
    meridian_node_flags_t flags; // MERIDIAN_NODE_FLAG_RENDEZVOUS if rendezvous node
} meridian_node_t;
```

### meridian_protocol_t
```c
typedef struct meridian_protocol_t {
    refcounter_t refcounter;
    meridian_protocol_state_t state;      // INIT → BOOTSTRAPPING → RUNNING → SHUTTING_DOWN
    meridian_protocol_config_t config;

    // QUIC handles (replace raw UDP sockets)
    const struct QUIC_API_TABLE* msquic;
    HQUIC registration;
    HQUIC listener;                       // QUIC listener for incoming connections
    HQUIC configuration;                  // QUIC configuration (ALPN, TLS)
    struct sockaddr_in local_addr;

    meridian_ring_set_t* ring_set;
    meridian_rendv_handle_t* rendv_handle;
    meridian_gossip_handle_t* gossip_handle;
    meridian_latency_cache_t* latency_cache;

    work_pool_t* pool;
    hierarchical_timing_wheel_t* wheel;

    meridian_node_t* seed_nodes[16];
    size_t num_seed_nodes;

    HQUIC connected_peers[64];            // Active QUIC connections
    meridian_node_t* peer_nodes[64];      // Peer address info
    size_t num_connected_peers;

    // Per-connection path management (direct + relay)
    meridian_conn_t** connections;
    size_t num_connections;
    struct meridian_relay_t* default_relay;

    meridian_protocol_callbacks_t callbacks;
    PLATFORMLOCKTYPE(lock);
    bool running;
} meridian_protocol_t;
```

### meridian_ring_set_t
```c
typedef struct meridian_ring_set_t {
    refcounter_t refcounter;
    uint32_t primary_ring_size;      // Max nodes per primary ring
    uint32_t secondary_ring_size;    // Max nodes per secondary ring
    int32_t exponent_base;           // Log bucket base (e.g., 2 = powers of 2)
    meridian_ring_t rings[MERIDIAN_MAX_RINGS];  // Array of rings
    PLATFORMLOCKTYPE(lock);         // Thread-safe access
} meridian_ring_set_t;
```

### meridian_conn_t (Connection Manager)
```c
typedef struct meridian_conn_t {
    refcounter_t refcounter;
    meridian_node_t* peer;
    HQUIC direct_connection;           // Direct QUIC connection (NULL if relay-only)
    struct meridian_relay_t* relay;     // Relay client (NULL if direct-only)
    uint32_t relay_endpoint_id;        // Peer's relay endpoint ID
    meridian_conn_state_t state;        // DIRECT / TRYING_DIRECT / RELAY / RELAY_ONLY
    meridian_conn_path_t direct_path;   // Direct path metrics
    meridian_conn_path_t relay_path;    // Relay path metrics
    meridian_nat_type_t local_nat_type;  // Our NAT classification
    meridian_nat_type_t peer_nat_type;  // Peer's NAT classification
    uint32_t direct_attempts;           // Count of direct connection attempts
    uint64_t last_direct_attempt_ms;    // Timestamp of last direct attempt
    PLATFORMLOCKTYPE(lock);
} meridian_conn_t;
```

## Algorithms

### Latency-Based Ring Selection

Nodes are assigned to rings using a logarithmic bucketing scheme:

```
ring = floor(log(latency_us) / log(exponent_base))
```

For example, with `exponent_base = 2`:
- Latency 1-1μs → Ring 0
- Latency 2-3μs → Ring 1
- Latency 4-7μs → Ring 2
- Latency 8-15μs → Ring 3
- etc.

This ensures O(log M) rings for maximum latency M, making closest-node lookup efficient.

### Gossip Schedule

The gossip scheduler operates in two phases:

**Initialization Phase** (num_init_intervals iterations):
- Interval: `init_gossip_interval_s` (e.g., 1 second)
- Purpose: Rapidly populate rings and discover peers

**Steady State Phase** (thereafter):
- Interval: `steady_state_gossip_interval_s` (e.g., 30 seconds)
- Purpose: Maintain membership with minimal overhead

### Ring Replacement Algorithm

When `eligible_for_replacement()` returns true for a ring:

1. Primary ring is full
2. Secondary ring has candidates
3. More than `primary_ring_size` non-rendezvous nodes exist

The replacement logic promotes secondary nodes to primary when primary nodes leave.

### Query Expiration (tick)

The `meridian_query_table_tick()` function:

1. **Count** expired queries in the table
2. **Allocate** array for expired queries
3. **Partition** the table:
   - Non-expired queries → compacted to front (write_idx)
   - Expired queries → CONSUME'd to caller ownership
4. **Update** count to write_idx (stale slots at end are unreachable)

Ownership transfer uses `CONSUME(node, type)` which calls `refcounter_consume()` to transfer the table's reference to the caller.

### NAT Type Detection

The NAT type is detected by comparing reflexive addresses from two independent relay servers:

1. Send ADDR_REQUEST via relay A → receive reflexive address A
2. Send ADDR_REQUEST via relay B → receive reflexive address B
3. Compare:
   - Local address matches reflexive → **OPEN**
   - Same reflexive address from both relays (Endpoint-Independent Mapping) → **PORT_RESTRICTED_CONE** (or FULL_CONE / RESTRICTED_CONE depending on filtering)
   - Different reflexive addresses (Endpoint-Dependent Mapping) → **SYMMETRIC**

Symmetric NAT peers cannot establish direct connections — the connection manager routes them relay-only (RELAY_ONLY state).

### Connection Manager State Machine

Each peer connection follows this state machine:

```
                ┌─────────────┐
                │  TRYING_    │──── direct success ───▶ DIRECT
                │  DIRECT     │
                └──────┬──────┘
                       │ direct failed/timeout
                       ▼
                ┌─────────────┐     reflexive addr      ┌─────────────┐
                │    RELAY     │◀─── discovered ────────  │  TRYING_    │
                └──────┬──────┘     (upgrade)           │  DIRECT     │
                       │                                  └─────────────┘
                       │ symmetric NAT detected
                       ▼
                ┌─────────────┐
                │  RELAY_ONLY  │  (no direct attempts)
                └─────────────┘
```

- **TRYING_DIRECT**: Attempt direct QUIC connection with relay as backup
- **RELAY**: Relay-only, but can upgrade to direct if reflexive address is discovered
- **DIRECT**: Direct QUIC connection active (optimal path)
- **RELAY_ONLY**: Symmetric NAT on either side — never attempt direct

The `meridian_conn_send()` function automatically selects the best path: direct if active, otherwise relay.

### Reference Counting Pattern

All major structures follow reference counting lifecycle:

```
Create:  refcounter_init() + refcounter_reference() on insertion
Access:  refcounter_reference() to get temporary reference
Release: refcounter_dereference() when done
Destroy: Check count==0 → free memory
```

The CONSUME macro transfers ownership:
```c
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)
```

## Packet Types

### Core Protocol Packets

| Type | ID | Purpose |
|------|-----|---------|
| GOSSIP | 10 | Periodic node exchange with peers |
| GOSSIP_PULL | 23 | Request gossip from peer |
| PUSH | 11 | Push nodes to peer |
| PULL | 12 | Pull nodes from peer |
| PING | 14 | Latency probe request |
| PONG | 15 | Latency probe response |
| CREATE_RENDV | 16 | Request rendezvous point creation |
| RET_RENDV | 17 | Rendezvous point info response |
| RET_ERROR | 18 | Error response |
| RET_INFO | 19 | Info response |
| RET_PING | 13 | Ping response |
| RET_RESPONSE | 9 | General response with closest node + targets |
| INFO | 22 | Info message |
| REQ_CLOSEST_TCP/PING/DNS | 2/4/6 | Closest node requests |
| REQ_MEASURE_TCP/PING/DNS | 3/5/7 | Measurement requests |
| REQ_CONSTRAINT_TCP/PING/DNS | 1/21/20 | Constraint-based requests |

### Relay / NAT Traversal Packets

| Type | ID | Purpose |
|------|-----|---------|
| ADDR_REQUEST | 30 | Request reflexive address from relay server |
| ADDR_RESPONSE | 31 | Relay server response with reflexive address + endpoint ID |
| RELAY_DATAGRAM | 32 | Datagram forwarded between peers via relay |
| PUNCH_REQUEST | 33 | Hole-punching request (call-me-maybe) sent through relay |
| PUNCH_SYNC | 34 | Hole-punching sync sent directly between peers |
| RELAY_PING | 35 | Keepalive ping from relay server to client |
| RELAY_PONG | 36 | Keepalive response from client to relay |
| ENDPOINT_GONE | 37 | Notification that a peer's endpoint has disconnected |

All relay packets use CBOR encoding, consistent with core protocol messages.

## Thread Safety

All structures with concurrent access use `PLATFORMLOCKTYPE(lock)`:

- `meridian_protocol_t.lock` — Protects protocol state and peer lists
- `meridian_ring_set_t.lock` — Protects ring insertions/erasures
- `meridian_gossip_handle_t.lock` — Protects active gossip list
- `meridian_rendv_handle_t.lock` — Protects peer rendezvous list
- `meridian_query_table_t.lock` — Protects query table operations
- `meridian_conn_t.lock` — Protects per-connection state machine
- `meridian_relay_t.lock` — Protects relay client state
- `meridian_relay_server_t.lock` — Protects relay server client list

Platform abstraction supports:
- **POSIX**: pthread_mutex_t
- **Windows**: CRITICAL_SECTION

## Usage Example

```c
// QUIC infrastructure setup
const QUIC_API_TABLE* msquic;
MsQuicOpen2(&msquic);
HQUIC registration;
msquic->RegistrationOpen(..., &registration);

meridian_protocol_config_t config = {
    .listen_port = 9000,
    .info_port = 9001,
    .primary_ring_size = 8,
    .secondary_ring_size = 16,
    .ring_exponent_base = 2,
    .init_gossip_interval_s = 1,
    .num_init_gossip_intervals = 5,
    .steady_state_gossip_interval_s = 30,
    .gossip_timeout_ms = 5000,
    .pool = my_pool,
    .wheel = my_wheel
};

meridian_protocol_t* proto = meridian_protocol_create(&config);
meridian_protocol_start(proto);

// Add seed nodes
meridian_protocol_add_seed_node(proto, seed_addr, seed_port);

// Connect to peers (connection manager handles direct/relay selection)
meridian_protocol_connect(proto, peer_addr, peer_port);

// Periodically call
meridian_protocol_gossip(proto);
meridian_protocol_ring_management(proto);

// Find closest node
meridian_node_t* closest = meridian_protocol_find_closest(proto, target_addr, target_port);

// Cleanup
meridian_protocol_stop(proto);
meridian_protocol_destroy(proto);
msquic->RegistrationClose(registration);
MsQuicClose(msquic);
```