# Meridian Relay Server

A lightweight QUIC relay server for the Meridian protocol. Forwards datagrams between connected peers.

Licensed under GPLv3 — see the [LICENSE](../../../LICENSE) file.

## Building

### Standalone

The relay server can be built independently of the full Poseidon daemon. It only requires msquic, OpenSSL, and pthreads.

```bash
cd src/Network/Meridian/relay
mkdir build && cd build
cmake ..
make
```

To install:

```bash
make install  # defaults to /usr/local
# or
cmake --install . --prefix /opt/meridian-relay
```

#### Dependencies

- **msquic** — Microsoft QUIC library. Automatically built from the project's `deps/msquic/` if present.
  - For a pre-built msquic, set `-DMSQUIC_DIR=/path/to/msquic-config.cmake/directory`.
  - For a system-installed msquic, ensure CMake can find it via `find_package(msquic)`.
- **OpenSSL** — TLS/crypto (required by msquic on Linux)
- **pthreads** — POSIX threads

### In-Tree

The relay server is also built as part of the full Poseidon project:

```bash
mkdir build && cd build
cmake ../..
make
```

This produces the `meridian_relay` binary alongside the rest of the project targets.

## Usage

```
meridian_relay [OPTIONS]

Options:
  -p, --port PORT             Relay server port (default: 9000)
  -c, --max-clients NUM       Maximum concurrent clients (default: 256)
  -t, --idle-timeout MS       Connection idle timeout (default: 30000 ms)
  -k, --keepalive MS          Keep-alive interval (default: 10000 ms)
  -d, --max-datagram SIZE     Maximum datagram size (default: 1400 bytes)
  -h, --help                  Show help
  -v, --version               Show version

Example:
  meridian_relay -p 9000 -c 256
```