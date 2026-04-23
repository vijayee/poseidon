# Phase 5: TCP + WebSocket + QUIC Transports

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three network transports (TCP+TLS, WebSocket+TLS, QUIC) to the daemon. Each follows the same `poseidon_transport_t` interface and runs in its own thread with poll-dancer. TCP and WebSocket require TLS certificates; QUIC has built-in encryption.

**Architecture:** Each transport file implements `start/stop/send` and the thread+poll-dancer loop pattern established by the Unix transport. TLS is configured using the daemon's key/cert (same as dial channel PKI). The daemon validates TLS cert availability before starting TCP or WS transports.

**Tech Stack:** C, OpenSSL (TLS), poll-dancer, libcbor, msquic (QUIC transport)

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientAPIs/transport_tcp.c` | TCP + TLS transport |
| Create | `src/ClientAPIs/transport_ws.c` | WebSocket over TLS transport |
| Create | `src/ClientAPIs/transport_quic.c` | QUIC transport via msquic singleton |
| Modify | `CMakeLists.txt` | Add sources |
| Modify | `src/poseidond.c` | Wire up new transports with enable flags |
| Create | `tests/transport_tcp_test.cpp` | Test TCP transport |
| Create | `tests/transport_quic_test.cpp` | Test QUIC transport |

---

### Task 1: TCP + TLS transport

**Files:**
- Create: `src/ClientAPIs/transport_tcp.c`
- Modify: `CMakeLists.txt`
- Create: `tests/transport_tcp_test.cpp`

- [ ] **Step 1: Create transport_tcp.c**

Implements `poseidon_transport_t` for TCP+TLS. Follows the same pattern as `transport_unix.c`:

1. `start()`: Creates listening TCP socket, sets up OpenSSL TLS context with cert/key, creates poll-dancer loop, starts accept watcher in dedicated thread
2. On accept: Wraps client fd with SSL, creates client session, registers read watcher
3. On read: SSL_read → deserialize CBOR → `client_session_handle_request` → SSL_write response
4. `stop()`: `pd_loop_stop()`, join thread, cleanup SSL
5. `send()`: SSL_write to client (thread-safe via lock)

TLS setup:
```c
SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM);
```

~300 lines including TLS handshake handling.

- [ ] **Step 2: Write test**

```cpp
#include <gtest/gtest.h>
#include "ClientAPIs/transport.h"
#include "Channel/channel_manager.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

TEST(TCPTransportTest, CreateAndStart) {
    work_pool_t* pool = work_pool_create(1);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);

    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    // Generate TLS cert/key for testing
    char key_path[256], cert_path[256];
    snprintf(key_path, sizeof(key_path), "/tmp/poseidon_tcp_test_key.pem");
    snprintf(cert_path, sizeof(cert_path), "/tmp/poseidon_tcp_test_cert.pem");
    poseidon_key_pair_generate_tls_files(kp, "testnode", key_path, cert_path);

    poseidon_channel_manager_t* mgr = poseidon_channel_manager_create(
        kp, 16000, 16001, 16100, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    poseidon_transport_t* transport = poseidon_transport_tcp_create(
        16099, key_path, cert_path, mgr);
    ASSERT_NE(transport, nullptr);
    EXPECT_EQ(transport->type, POSEIDON_TRANSPORT_TCP);

    transport->stop(transport);
    poseidon_transport_destroy(transport);
    poseidon_channel_manager_destroy(mgr);
    poseidon_key_pair_destroy(kp);
    unlink(key_path);
    unlink(cert_path);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
}
```

- [ ] **Step 3: Add to CMakeLists.txt and add test target**

- [ ] **Step 4: Build and test**

- [ ] **Step 5: Commit**

```bash
git add src/ClientAPIs/transport_tcp.c tests/transport_tcp_test.cpp CMakeLists.txt
git commit -m "feat: add TCP+TLS transport for ClientAPI"
```

---

### Task 2: WebSocket over TLS transport

**Files:**
- Create: `src/ClientAPIs/transport_ws.c`

- [ ] **Step 1: Create transport_ws.c**

WebSocket framing on top of TLS TCP. After the TLS handshake, the transport performs the WebSocket upgrade handshake (HTTP-style request/response), then reads/writes WebSocket frames (RFC 6455).

Frame format:
- 2-byte header: FIN bit + opcode (0x02 for binary) + mask bit + payload length
- 4-byte mask key (if client→server)
- Payload data

~350 lines including HTTP upgrade handshake parsing and WebSocket frame encode/decode.

- [ ] **Step 2: Add to CMakeLists.txt**

- [ ] **Step 3: Build and verify**

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPIs/transport_ws.c CMakeLists.txt
git commit -m "feat: add WebSocket+TLS transport for ClientAPI"
```

---

### Task 3: QUIC transport

**Files:**
- Create: `src/ClientAPIs/transport_quic.c`
- Create: `tests/transport_quic_test.cpp`

- [ ] **Step 1: Create transport_quic.c**

QUIC transport uses the msquic singleton (not poll-dancer — msquic has its own event model). Implements:

1. `start()`: Opens QUIC listener via `msquic->ListenerOpen`/`ListenerStart`, sets ALPN to "poseidon_client"
2. On connection: Creates client session, sets stream callback
3. On stream data: Deserialize CBOR → `client_session_handle_request` → serialize response → send on stream
4. `stop()`: `ListenerClose`, stop all connections
5. `send()`: Opens a QUIC stream and writes data

The QUIC transport reuses the msquic singleton pattern from the Meridian protocol. Callbacks are registered via `QUIC_CALLBACK_HANDLER`.

~350 lines including QUIC callback handlers.

- [ ] **Step 2: Write test and add to CMakeLists.txt**

- [ ] **Step 3: Build and test**

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPIs/transport_quic.c tests/transport_quic_test.cpp CMakeLists.txt
git commit -m "feat: add QUIC transport for ClientAPI via msquic"
```

---

### Task 4: Wire transports into daemon

**Files:**
- Modify: `src/poseidond.c` — add transport creation/start for TCP/WS/QUIC
- Modify: `src/ClientAPIs/transport.h` — add create functions for each transport type

- [ ] **Step 1: Add transport create functions to transport.h**

```c
poseidon_transport_t* poseidon_transport_tcp_create(uint16_t port,
                                                     const char* cert_path,
                                                     const char* key_path,
                                                     poseidon_channel_manager_t* manager);
poseidon_transport_t* poseidon_transport_ws_create(uint16_t port,
                                                    const char* cert_path,
                                                    const char* key_path,
                                                    poseidon_channel_manager_t* manager);
poseidon_transport_t* poseidon_transport_quic_create(uint16_t port,
                                                      poseidon_channel_manager_t* manager);
```

- [ ] **Step 2: Add transport startup to poseidond.c**

After the Unix transport startup, add conditional startup for TCP, WS, QUIC based on `config.enable_*` flags. Validate TLS cert availability before starting TCP or WS transports.

- [ ] **Step 3: Build and verify daemon starts with all transports**

Run: `./poseidond --enable-unix --enable-tcp --enable-quic --tls-cert /path/to/cert --tls-key /path/to/key`
Expected: Daemon logs show all three transports listening

- [ ] **Step 4: Commit**

```bash
git add src/poseidond.c src/ClientAPIs/transport.h
git commit -m "feat: wire TCP, WebSocket, and QUIC transports into daemon"
```

---

### Task 5: De-wonk audit with memory leak check

- [ ] **Step 1: Read all transport files and daemon, audit**

Audit for:
1. SSL object leaks (SSL_new without SSL_free)
2. SSL_CTX leaks in transport create
3. Socket fd leaks on client disconnect
4. Poll-dancer watcher leaks
5. QUIC connection/stream leaks
6. TLS handshake failures leaving partial state
7. Thread cleanup: verify all threads joined on shutdown

- [ ] **Step 2: Fix CRITICAL/HIGH issues**

- [ ] **Step 3: Run all tests**

Run: `cd build && make && ctest --output-on-failure`

- [ ] **Step 4: Run memory leak check**

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make
./tests/transport_tcp_test
./tests/transport_quic_test
valgrind --leak-check=full --error-exitcode=1 ./tests/transport_tcp_test
```

Verify: Zero leaks. Fix if found.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk TCP, WebSocket, QUIC transports"
```