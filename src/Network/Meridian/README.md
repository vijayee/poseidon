# Meridian Network Protocol

Meridian is a distributed network protocol designed for peer-to-peer node discovery and latency-based routing. It uses a multi-ring architecture organized by latency buckets, with gossip-based node exchange and rendezvous point support for NAT traversal.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                      meridian_protocol_t                        │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────┐ │
│  │  ring_set    │  │  gossip_     │  │  latency_cache        │ │
│  │  (rings)     │  │  handle      │  │  (measurements)       │ │
│  └──────────────┘  └──────────────┘  └───────────────────────┘ │
│  ┌──────────────┐  ┌──────────────┐                             │
│  │  rendv_      │  │  pool/wheel  │                             │
│  │  handle      │  │  (scheduling)│                             │
│  └──────────────┘  └──────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

## File Overview

### Core Types

| File | Purpose |
|------|---------|
| `meridian.h` / `meridian.c` | Core `meridian_node_t` struct and node lifecycle management. Nodes are reference-counted and have an address, port, and optional rendezvous point. |

### Packet Encoding

| File | Purpose |
|------|---------|
| `meridian_packet.h` / `meridian_packet.c` | CBOR-based serialization for all protocol messages. Defines packet structures for gossip, ping, and response messages. Uses libcbor for encoding/decoding. |

### Protocol Core

| File | Purpose |
|------|---------|
| `meridian_protocol.h` / `meridian_protocol.c` | Main protocol orchestrator. Manages socket I/O, peer connections, and coordinates all subsystems (ring_set, gossip, rendv, measure). Entry point for starting/stopping the protocol. |

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
| `meridian_measure.h` / `meridian_measure.c` | TCP/PING/DNS measurement system with caching. Latency cache stores recent measurements and evicts stale entries. Measure requests track timeout and execute callbacks on completion. |

### NAT Traversal

| File | Purpose |
|------|---------|
| `meridian_rendv.h` / `meridian_rendv.c` | Rendezvous point management and NAT hole-punching support. Handles local/peer rendezvous points, tunnel creation, and NAT type detection (open, full-cone, restricted-cone, port-restricted, symmetric). |

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

### meridian_ring_t
```c
typedef struct meridian_ring_t {
    vec_t(meridian_node_t*) primary;   // Active nodes (replacement candidates)
    vec_t(meridian_node_t*) secondary;  // Candidates waiting for promotion
    bool frozen;                         // If true, no insertions allowed
    uint32_t latency_min_us;            // Latency range for this ring
    uint32_t latency_max_us;
} meridian_ring_t;
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

## Thread Safety

All structures with concurrent access use `PLATFORMLOCKTYPE(lock)`:

- `meridian_ring_set_t.lock` - Protects ring insertions/erasures
- `meridian_gossip_handle_t.lock` - Protects active gossip list
- `meridian_rendv_handle_t.lock` - Protects peer rendezvous list
- `meridian_query_table_t.lock` - Protects query table operations

Platform abstraction supports:
- **POSIX**: pthread_mutex_t
- **Windows**: CRITICAL_SECTION

## Usage Example

```c
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

// Connect to peers
meridian_protocol_connect(proto, peer_addr, peer_port);

// Periodically call
meridian_protocol_gossip(proto);
meridian_protocol_ring_management(proto);

// Find closest node
meridian_node_t* closest = meridian_protocol_find_closest(proto, target_addr, target_port);

// Cleanup
meridian_protocol_destroy(proto);
```