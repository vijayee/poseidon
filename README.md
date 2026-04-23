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
├── Channel/            # Topic channels, subtopics, aliases
├── ClientAPIs/          # Client protocol, sessions, transports (Unix, TCP, WS, QUIC)
├── ClientLibs/
│   ├── c/              # Installable C client library (static + shared, CMake package)
│   ├── node/           # Node.js client (N-API addon + pure JS web, npm package)
│   ├── android/        # Android client library (Kotlin, Binder IPC)
│   └── swift/          # Swift client library (SPM, XPC IPC)
├── Crypto/             # BLAKE3 wrapper, node IDs, key pairs
├── Network/
│   ├── Meridian/       # P2P overlay (rings, gossip, queries, measurement, NAT traversal)
│   │   └── relay/      # Standalone QUIC relay server
│   └── Quasar/         # Pub/sub overlay
├── RefCounter/         # Lock-free reference counting
├── Time/               # Timing wheels, tickers, debouncers
├── Util/               # Allocator, logging, vectors, threading, paths
└── Workers/            # Work pool, promises, queues, error handling

include/
└── poseidon/           # Public headers for the installable C client library
```

## Dependencies

All dependencies are included as git submodules in `deps/`:

| Dependency | Purpose |
|---|---|
| [MSQUIC](https://github.com/microsoft/msquic) | QUIC protocol implementation |
| [libcbor](https://github.com/PJK/libcbor) | CBOR encoding/decoding |
| [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) | Cryptographic hashing |
| [googletest](https://github.com/google/googletest) | C++ test framework |
| [hashmap](https://github.com/tidwall/hashmap.c) | Hash map implementation |
| [xxhash](https://github.com/Cyan4973/xxHash) | Fast hash function |
| [poll-dancer](https://github.com/vijayee/poll-dancer) | Event loop library |

## Building

### Prerequisites

- CMake 3.16+
- C11-compatible compiler (GCC or Clang)
- C++17-compatible compiler (for tests)
- OpenSSL development libraries (required by MSQUIC)
- `libnuma-dev` (on Linux, for MSQUIC)

### Full Project Build

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

### Standalone Builds

Each component can also be built independently:

**C Client Library** — produces static and shared libraries with `find_package(PoseidonClient)` support:

```bash
cd src/ClientLibs/c
mkdir build && cd build
cmake ..
make

# Install
cmake --install . --prefix /usr/local
```

Downstream projects can then use:

```cmake
find_package(PoseidonClient REQUIRED)
target_link_libraries(myapp PRIVATE PoseidonClient::poseidon_client_shared)
```

**Node.js Client** — npm package with native addon and pure JS web client:

```bash
cd src/ClientLibs/node
npm install
npm run build:native   # optional: compile native addon
```

See [node/README.md](src/ClientLibs/node/README.md) for API docs and transport options.

**Relay Server** — standalone QUIC relay binary:

```bash
cd src/Network/Meridian/relay
mkdir build && cd build
cmake ..
make
```

See [relay/README.md](src/Network/Meridian/relay/README.md) for relay-specific options and usage.

### Build Targets

| Target | Description |
|---|---|
| `poseidon` | Static library with all core components |
| `poseidon_client_static` | Installable C client library (static) |
| `poseidon_client_shared` | Installable C client library (shared) |
| `poseidon-client` | Node.js client (npm, native addon + web) |
| `meridian_relay` | Standalone relay server binary |
| `poseidond` | Poseidon daemon |

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

This project is licensed under the GNU General Public License v3 — see the [LICENSE](LICENSE) file for details.