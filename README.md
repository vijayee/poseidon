<p align="center">
  <img src="poseidon_logo.svg" alt="Poseidon" width="256">
</p>

# Poseidon

Poseidon is a peer-to-peer publish-subscribe network for application data. Think of it like IRC, but instead of person-to-person chat, it enables ad hoc P2P networks between applications. Nodes discover each other, form overlay networks, and exchange data without centralized infrastructure.

## Architecture

Poseidon layers two research protocols:

- **Meridian** — P2P network overlay providing latency-based ring routing, node discovery via gossip, and closest-node queries. Each node maintains multiple rings of peers sorted by latency buckets (logarithmic exponent base), enabling efficient nearest-neighbor lookups.
- **Quasar** — Pub/sub overlay built on top of Meridian. Uses elastic and attenuated Bloom filters for content-based routing and topic subscription matching.

Communication between nodes uses **QUIC** (via [MSQUIC](https://github.com/microsoft/msquic)) for encrypted, multiplexed, low-latency transport. Messages are encoded with **CBOR** (via [libcbor](https://github.com/PJK/libcbor)).

### NAT Traversal

Poseidon includes a three-layer NAT traversal system:

1. **NAT detection** — Connects to relay servers to discover the local node's reflexive address and classifies its NAT type (open, full cone, restricted cone, port restricted cone, or symmetric)
2. **Relay server/client** — QUIC-based message broker that forwards datagrams between peers, assigns endpoint IDs, and provides address discovery responses
3. **Connection manager** — Per-connection state machine (`DIRECT → TRYING_DIRECT → RELAY → RELAY_ONLY`) that prefers direct paths when possible, falls back to relay, and automatically upgrades to direct connections via hole-punching (call-me-maybe)

Symmetric NAT peers are automatically detected and routed relay-only, skipping direct attempts entirely.

## Features

- **Latency-based ring routing** — Meridian rings organize peers by latency, enabling O(log n) closest-node queries
- **Gossip protocol** — Nodes exchange membership information to discover new peers
- **Elastic Bloom filters** — Quasar subscriptions use elastic and attenuated Bloom filters for space-efficient topic matching
- **QUIC transport** — All P2P connections use MSQUIC for encrypted, multiplexed streams and datagrams
- **CBOR wire format** — All protocol messages encoded in CBOR for compact, schema-flexible serialization
- **NAT traversal** — Automatic NAT type detection, relay fallback, and direct path upgrade
- **Thread-safe** — Reference counting with `_Atomic` operations and platform-abstracted locks
- **Async execution** — Work pool with priority queues, hierarchical timing wheels, and promise-based callbacks

## Project Structure

```
src/
├── Bloom/              # Bloom filters (bitset, standard, elastic, attenuated)
├── Buffer/             # Binary data manipulation
├── Network/
│   ├── Meridian/       # P2P overlay (rings, gossip, queries, measurement, NAT traversal)
│   └── Quasar/         # Pub/sub overlay
├── RefCounter/         # Lock-free reference counting
├── Time/               # Timing wheels, tickers, debouncers
├── Util/               # Allocator, logging, vectors, threading, paths
└── Workers/            # Work pool, promises, queues, error handling
```

## Dependencies

All dependencies are included as git submodules in `deps/`:

| Dependency | Purpose |
|---|---|
| [MSQUIC](https://github.com/microsoft/msquic) | QUIC protocol implementation |
| [libcbor](https://github.com/PJK/libcbor) | CBOR encoding/decoding |
| [googletest](https://github.com/google/googletest) | C++ test framework |
| [hashmap](https://github.com/tidwall/hashmap.c) | Hash map implementation |
| [xxhash](https://github.com/Cyan4973/xxHash) | Fast hash function |

## Building

### Prerequisites

- CMake 3.16+
- C11-compatible compiler (GCC or Clang)
- C++17-compatible compiler (for tests)
- OpenSSL development libraries (required by MSQUIC)
- `libnuma-dev` (on Linux, for MSQUIC)

### Build Steps

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/vijayee/poseidon.git
cd poseidon

# Configure
mkdir build && cd build
cmake ..

# Build
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

### Build Targets

| Target | Description |
|---|---|
| `poseidon` | Static library with all core components |
| `meridian_relay` | Standalone relay server binary |
| `meridian_node_test` | Node creation and reference counting |
| `meridian_ring_test` | Latency ring management |
| `meridian_packet_test` | CBOR packet encode/decode |
| `meridian_measure_test` | Latency measurement |
| `meridian_query_test` | Closest-node queries |
| `meridian_integration_test` | Multi-node integration |
| `meridian_relay_test` | Relay client/server and packet types |
| `meridian_nat_detect_test` | NAT type classification |
| `meridian_conn_test` | Connection manager state machine |
| `bloom_bitset_test` | Bitset operations |
| `bloom_filter_test` | Standard Bloom filter |
| `elastic_bloom_filter_test` | Elastic Bloom filter |
| `attenuated_bloom_filter_test` | Attenuated Bloom filter |
| `quasar_test` | Quasar pub/sub |

## Running the Relay Server

```bash
./meridian_relay --port 8080 --alpn "meridian-relay"
```

## Style Guide

See [STYLEGUIDE.md](STYLEGUIDE.md) for the full C coding conventions used in this project. Key points:

- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `type_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Destroy functions check `refcounter_count == 0` before freeing
- All locks use `PLATFORMLOCKTYPE(lock)` with `platform_lock`/`platform_unlock`
- Comments explain WHY, not WHAT

## License

See [LICENSE](LICENSE) for details.