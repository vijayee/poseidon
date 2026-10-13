# WebTransport Server Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WebTransport server transport using libwtf, enabling browser clients to connect via the native WebTransport API.

**Architecture:** Add libwtf (Apache 2.0, msquic-based WebTransport library) as a git submodule. Wrap its server API in a `poseidon_transport_t` that bridges WebTransport sessions/streams to our existing client_session + frame protocol (4-byte length + CBOR). No client-side changes needed — browser clients use the native WebTransport API, and C clients continue using raw QUIC.

**Tech Stack:** C11, libwtf, msquic (shared), HTTP/3 + WebTransport over QUIC

---

## File Structure

| File | Change | Purpose |
|------|--------|---------|
| `deps/libwtf/` | New submodule | WebTransport C library (server API) |
| `CMakeLists.txt` | Modify | Add libwtf subdirectory, link to targets |
| `src/ClientAPIs/transport.h` | Modify | Add WEBTRANSPORT enum, config fields, create declaration |
| `src/ClientAPIs/transport_unix.c` | Modify | Add `wt_port` to config defaults |
| `src/ClientAPIs/transport_webtransport.c` | New | WebTransport server transport implementation |
| `src/poseidond.c` | Modify | Add `--enable-wt` / `--wt-port` CLI flags |

---

## Key Design Decisions

1. **Single bidi stream per session:** Browser client opens one bidirectional stream for the frame protocol. This matches our TCP/QUIC model exactly. Additional streams are logged but ignored.

2. **libwtf owns its msquic instance:** libwtf calls `MsQuicOpen2` internally (separate registration from our raw QUIC transport). Both coexist on different ports with different ALPNs (`"h3"` vs `"poseidon_client"`).

3. **No C client changes:** The C client library uses raw QUIC (`quic://`). WebTransport is for browser clients that have the native WebTransport API.

4. **Same frame protocol:** 4-byte big-endian length prefix + CBOR payload, identical to all other transports. Browser code already handles this in the Node.js WebTransport transport.

5. **Connection validation:** Server accepts all connections initially. Origin/path validation can be added later via libwtf's `wtf_connection_validator_t` callback.

---

### Task 1: Add libwtf as a git submodule

**Files:**
- Create: `deps/libwtf/` (submodule)

- [ ] **Step 1: Add the submodule**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/poseidon
git submodule add https://github.com/andrewmd5/libwtf.git deps/libwtf
git submodule update --init --recursive
```

- [ ] **Step 2: Verify the submodule was added**

```bash
git submodule status deps/libwtf
ls deps/libwtf/include/wtf.h
```

Expected: submodule shows commit hash, header file exists.

---

### Task 2: Update CMakeLists.txt to build libwtf

**Files:**
- Modify: `CMakeLists.txt`

libwtf must be built with `WTF_USE_EXTERNAL_MSQUIC=ON` so it uses our existing msquic instead of its bundled submodule. This avoids building msquic twice and ensures ABI compatibility.

- [ ] **Step 1: Add libwtf subdirectory after msquic**

Insert after the `add_subdirectory(deps/msquic)` line:

```cmake
# Build libwtf (WebTransport over HTTP/3) using our existing msquic
set(WTF_USE_EXTERNAL_MSQUIC ON CACHE BOOL "" FORCE)
set(WTF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WTF_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libwtf)
```

- [ ] **Step 2: Add libwtf sources to poseidon library**

Add `transport_webtransport.c` to `POSEIDON_SOURCES` after `transport_quic.c`:

```cmake
${CLIENT_APIS_SRC_DIR}/transport_webtransport.c
```

- [ ] **Step 3: Add libwtf include directory**

Add to `target_include_directories` for poseidon:

```cmake
${CMAKE_CURRENT_SOURCE_DIR}/deps/libwtf/include
```

- [ ] **Step 4: Link libwtf to poseidon and poseidond**

Add to `target_link_libraries` for both poseidon and poseidond:

```cmake
wtf
```

- [ ] **Step 5: Build and verify**

```bash
cd build && cmake .. && cmake --build . -- -j$(nproc)
```

Expected: libwtf builds successfully, linked into poseidon.

---

### Task 3: Update transport.h for WebTransport

**Files:**
- Modify: `src/ClientAPIs/transport.h`

- [ ] **Step 1: Add WEBTRANSPORT to transport type enum**

After `POSEIDON_TRANSPORT_QUIC`, add:

```c
POSEIDON_TRANSPORT_WEBTRANSPORT,
```

- [ ] **Step 2: Add WebTransport fields to config struct**

Add to `poseidon_transport_config_t`:

```c
bool enable_wt;
uint16_t wt_port;
```

- [ ] **Step 3: Add WebTransport create declaration**

After the QUIC create declaration, add:

```c
/**
 * Creates a WebTransport server transport.
 *
 * Uses libwtf (HTTP/3 + WebTransport over QUIC) for browser-compatible
 * connections. Requires TLS certificate and key.
 *
 * @param port       Listen port
 * @param cert_path  TLS certificate PEM file (required)
 * @param key_path   TLS private key PEM file (required)
 * @param manager    Shared channel manager
 * @return           New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_webtransport_create(uint16_t port,
                                                             const char* cert_path,
                                                             const char* key_path,
                                                             poseidon_channel_manager_t* manager);
```

- [ ] **Step 4: Build to verify header changes compile**

---

### Task 4: Implement transport_webtransport.c

**Files:**
- Create: `src/ClientAPIs/transport_webtransport.c`

This is the core implementation. It wraps libwtf's server API and bridges WebTransport sessions to our client_session + frame protocol.

**Data structures:**

```c
// Per-session state (one browser client = one WebTransport session)
typedef struct wt_client_t {
    wtf_session_t* session;
    wtf_stream_t* stream;       // Primary bidi stream for frame protocol
    client_session_t* client_session;
    uint8_t read_buf[WT_READ_BUF_SIZE];
    size_t read_len;
    poseidon_transport_t* transport;
    bool shutting_down;
} wt_client_t;

// Transport internal state
typedef struct wt_transport_data_t {
    wtf_context_t* context;
    wtf_server_t* server;
    wt_client_t* clients[WT_MAX_CLIENTS];
    size_t num_clients;
    uint16_t port;
    char* cert_path;
    char* key_path;
    PLATFORMLOCKTYPE(lock);
    bool stopping;
} wt_transport_data_t;
```

**Lifecycle:**

1. `webtransport_start`:
   - Create `wtf_context_t` with context config
   - Create `wtf_server_t` with cert config, session callback, connection validator
   - Start server

2. `webtransport_stop`:
   - Set stopping flag
   - Stop server (`wtf_server_stop`)
   - Close all client sessions
   - Destroy server and context

3. `webtransport_send`:
   - Lookup client by client_id (session_id)
   - Serialize frame header + CBOR into buffer
   - Call `wtf_stream_send` on the client's stream

**Callbacks:**

- **Session callback**: On CONNECTED → create client_session, register with channel manager. On STREAM_OPENED → if no stream assigned, assign it; if stream already assigned, log and close extra stream. On DISCONNECTED → cleanup.
- **Stream callback**: On DATA_RECEIVED → append to read_buf, run frame-parsing loop (same as TCP/Unix/QUIC). On PEER_CLOSED → mark stream closed. On SEND_COMPLETE → free send buffer.
- **Connection validator**: Accept all connections (return `WTF_CONNECTION_ACCEPT`).

**Frame processing:** Same 4-byte big-endian length + CBOR protocol. Reuse `read_frame_header`/`write_frame_header` pattern from other transports.

**Key implementation details:**
- `client_id` for send routing uses `session_id` from `client_session_t` (same pattern as QUIC transport)
- Send buffers must remain valid until SEND_COMPLETE callback — allocate header+payload, free in SEND_COMPLETE
- libwtf callbacks run on msquic worker threads — use transport lock for client list modifications
- `transport->destroy` function pointer set to `webtransport_transport_destroy` for proper cleanup

- [ ] **Step 1: Write transport_webtransport.c with full implementation**

- [ ] **Step 2: Build and fix any compile errors**

---

### Task 5: Update config defaults and daemon

**Files:**
- Modify: `src/ClientAPIs/transport_unix.c` (config defaults)
- Modify: `src/poseidond.c`

- [ ] **Step 1: Add WebTransport fields to config defaults**

In `poseidon_transport_config_defaults()`, add:

```c
config.enable_wt = false;
config.wt_port = 9093;
```

- [ ] **Step 2: Add CLI flags to poseidond.c**

Add `--enable-wt` and `--wt-port` options:

```c
{"enable-wt",       no_argument,       0, 'B'},
{"wt-port",         required_argument, 0, 'b'},
```

And in the switch:

```c
case 'B': config.enable_wt = true; break;
case 'b': config.wt_port = (uint16_t)atoi(optarg); break;
```

- [ ] **Step 3: Add WebTransport transport startup in poseidond.c**

After the QUIC transport block, add:

```c
if (config.enable_wt) {
    if (config.tls_cert_path == NULL || config.tls_key_path == NULL) {
        log_error("poseidond: WebTransport requires --tls-cert and --tls-key");
    } else {
        g_wt_transport = poseidon_transport_webtransport_create(
            config.wt_port, config.tls_cert_path, config.tls_key_path, mgr);
        if (g_wt_transport != NULL) {
            if (g_wt_transport->start(g_wt_transport) == 0) {
                log_info("poseidond: WebTransport listening on port %u", config.wt_port);
            } else {
                log_error("poseidond: failed to start WebTransport transport");
                poseidon_transport_destroy(g_wt_transport);
                g_wt_transport = NULL;
            }
        } else {
            log_error("poseidond: failed to create WebTransport transport");
        }
    }
}
```

- [ ] **Step 4: Add WebTransport shutdown in poseidond.c**

Add cleanup for `g_wt_transport` in the shutdown section.

- [ ] **Step 5: Update help text**

Add WebTransport options to `print_usage`.

- [ ] **Step 6: Build and verify daemon compiles**

---

### Task 6: De-wonk audit

- [ ] **Step 1: Read every file created or modified**
- [ ] **Step 2: Run de-wonk question: "Is anything unimplemented, stubbed, disabled, broken, or weird?"**
- [ ] **Step 3: Fix all CRITICAL and HIGH issues**
- [ ] **Step 4: Re-audit until no new issues**