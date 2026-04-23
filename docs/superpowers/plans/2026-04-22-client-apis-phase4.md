# Phase 4: Daemon Entry Point + Unix Transport

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the Poseidon daemon (`poseidond`) that initializes msquic, work pool, timing wheel, channel manager, and starts configured transports. Implement the Unix domain socket transport using poll-dancer for event-driven I/O. End-to-end local testing: client connects via Unix socket, sends CBOR requests, daemon processes them.

**Architecture:** The daemon parses command-line config with per-transport enable flags. Each enabled transport gets its own thread with a poll-dancer event loop. The Unix transport creates a listening socket, accepts connections, reads CBOR frames, dispatches to `client_session_handle_request`, and writes responses back. The transport interface (`poseidon_transport_t`) is a vtable that each transport implements.

**Tech Stack:** C, poll-dancer, CBOR, Unix domain sockets, pthreads

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientAPIs/transport.h` | Transport interface definition |
| Create | `src/ClientAPIs/transport_unix.c` | Unix domain socket transport |
| Create | `src/poseidond.c` | Daemon main |
| Modify | `CMakeLists.txt` | Add new source files, executable target |

---

### Task 1: Transport interface and Unix transport

**Files:**
- Create: `src/ClientAPIs/transport.h`
- Create: `src/ClientAPIs/transport_unix.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create transport.h**

```c
//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_TRANSPORT_H
#define POSEIDON_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdint.h>
#include "client_session.h"
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TRANSPORT TYPES
// ============================================================================

typedef enum {
    POSEIDON_TRANSPORT_UNIX_SOCKET,
    POSEIDON_TRANSPORT_TCP,
    POSEIDON_TRANSPORT_WEBSOCKET,
    POSEIDON_TRANSPORT_QUIC,
    POSEIDON_TRANSPORT_BINDER,
    POSEIDON_TRANSPORT_XPC
} poseidon_transport_type_t;

// ============================================================================
// TRANSPORT CONFIG
// ============================================================================

typedef struct poseidon_transport_config_t {
    bool enable_unix;
    bool enable_tcp;
    bool enable_ws;
    bool enable_quic;
    bool enable_binder;
    bool enable_xpc;
    const char* unix_socket_path;   /**< default: "/var/run/poseidond.sock" */
    uint16_t tcp_port;              /**< default: 9090 */
    uint16_t ws_port;               /**< default: 9091 */
    uint16_t quic_port;            /**< default: 9092 */
    const char* tls_cert_path;      /**< required when tcp or ws enabled */
    const char* tls_key_path;       /**< required when tcp or ws enabled */
} poseidon_transport_config_t;

poseidon_transport_config_t poseidon_transport_config_defaults(void);

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

typedef struct poseidon_transport_t poseidon_transport_t;

typedef void (*poseidon_transport_on_message_cb)(poseidon_transport_t* transport,
                                                  int client_id,
                                                  const uint8_t* data,
                                                  size_t len);

struct poseidon_transport_t {
    const char* name;
    poseidon_transport_type_t type;
    PLATFORMTHREADTYPE thread;
    void* loop;                              /**< poll-dancer event loop (or NULL for Binder/XPC) */
    poseidon_channel_manager_t* manager;    /**< shared, thread-safe */
    poseidon_transport_on_message_cb on_message;
    PLATFORMLOCKTYPE(lock);
    volatile bool running;

    int (*start)(poseidon_transport_t* self);
    int (*stop)(poseidon_transport_t* self);
    int (*send)(poseidon_transport_t* self, int client_id,
                const uint8_t* data, size_t len);
};

// ============================================================================
// TRANSPORT LIFECYCLE
// ============================================================================

/**
 * Creates a Unix domain socket transport.
 *
 * @param socket_path  Path for the Unix socket (e.g., "/var/run/poseidond.sock")
 * @param manager      Shared channel manager
 * @return             New transport, or NULL on failure
 */
poseidon_transport_t* poseidon_transport_unix_create(const char* socket_path,
                                                      poseidon_channel_manager_t* manager);

/**
 * Destroys a transport. Stops the transport thread if running.
 *
 * @param transport  Transport to destroy
 */
void poseidon_transport_destroy(poseidon_transport_t* transport);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_TRANSPORT_H
```

- [ ] **Step 2: Create transport_unix.c**

Implement the Unix domain socket transport using poll-dancer. Key design:

1. `start()` creates a listening socket, creates a poll-dancer loop, starts a watcher on the listening socket for `PD_EVENT_READ`, and runs the loop in a dedicated thread
2. On new connection, creates a `client_session_t` and registers a read watcher for the client fd
3. On read, deserializes CBOR frames, calls `client_session_handle_request`, serializes the response, and sends it back on the client fd
4. `stop()` calls `pd_loop_stop()` and joins the thread
5. `send()` writes data to a client fd (thread-safe via lock)

The transport reads a 4-byte length prefix (network byte order) before each CBOR frame, matching the pattern used by the relay server. This makes frame boundary detection simple.

Implementation should be ~250 lines including the thread function, accept callback, read callback, and send function.

- [ ] **Step 3: Create poseidond.c — daemon main**

```c
//
// Created by victor on 4/22/26.
//

#include "ClientAPIs/transport.h"
#include "Channel/channel_manager.h"
#include "Network/Meridian/msquic_singleton.h"
#include "Workers/pool.h"
#include "Time/wheel.h"
#include "Crypto/key_pair.h"
#include "Util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>

#define DEFAULT_UNIX_SOCKET "/var/run/poseidond.sock"
#define DEFAULT_TCP_PORT 9090
#define DEFAULT_WS_PORT 9091
#define DEFAULT_QUIC_PORT 9092
#define DEFAULT_DIAL_PORT 8000
#define DEFAULT_PORT_RANGE_START 8001
#define DEFAULT_PORT_RANGE_END 8100

static volatile bool g_running = true;
static poseidon_transport_t* g_unix_transport = NULL;

static void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

static poseidon_transport_config_t make_default_config(void) {
    poseidon_transport_config_t config = {
        .enable_unix = true,
        .enable_tcp = false,
        .enable_ws = false,
        .enable_quic = false,
        .enable_binder = false,
        .enable_xpc = false,
        .unix_socket_path = DEFAULT_UNIX_SOCKET,
        .tcp_port = DEFAULT_TCP_PORT,
        .ws_port = DEFAULT_WS_PORT,
        .quic_port = DEFAULT_QUIC_PORT,
        .tls_cert_path = NULL,
        .tls_key_path = NULL
    };
    return config;
}

int main(int argc, char* argv[]) {
    poseidon_transport_config_t config = make_default_config();

    // Parse command line (getopt_long for --enable-*, --port, --tls-*)
    static struct option long_options[] = {
        {"enable-unix",    no_argument, 0, 'U'},
        {"enable-tcp",     no_argument, 0, 'T'},
        {"enable-ws",      no_argument, 0, 'W'},
        {"enable-quic",    no_argument, 0, 'Q'},
        {"dial-port",      required_argument, 0, 'd'},
        {"port-range-start", required_argument, 0, 's'},
        {"port-range-end",   required_argument, 0, 'e'},
        {"unix-socket",    required_argument, 0, 'u'},
        {"tls-cert",       required_argument, 0, 'c'},
        {"tls-key",        required_argument, 0, 'k'},
        {0, 0, 0, 0}
    };

    // ... parse options with getopt_long ...

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_set_level(LOG_LEVEL_INFO);

    // Initialize msquic singleton
    const struct QUIC_API_TABLE* msquic = poseidon_msquic_open();
    if (msquic == NULL) {
        log_error("Failed to open msquic");
        return EXIT_FAILURE;
    }

    // Create work pool and timing wheel
    work_pool_t* pool = work_pool_create(platform_core_count());
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);

    // Create dial channel key pair and manager
    poseidon_key_pair_t* dial_key = poseidon_key_pair_create("ED25519");
    if (dial_key == NULL) {
        log_error("Failed to create dial key pair");
        return EXIT_FAILURE;
    }

    poseidon_channel_manager_t* mgr = poseidon_channel_manager_create(
        dial_key, config.tcp_port ? config.tcp_port : DEFAULT_DIAL_PORT,
        DEFAULT_PORT_RANGE_START, DEFAULT_PORT_RANGE_END,
        pool, wheel);
    if (mgr == NULL) {
        log_error("Failed to create channel manager");
        return EXIT_FAILURE;
    }

    // Start enabled transports
    if (config.enable_unix) {
        g_unix_transport = poseidon_transport_unix_create(
            config.unix_socket_path, mgr);
        if (g_unix_transport != NULL) {
            g_unix_transport->start(g_unix_transport);
            log_info("Unix transport listening on %s", config.unix_socket_path);
        }
    }

    // Main loop
    while (g_running) {
        platform_usleep(100000);
        poseidon_channel_manager_tick_all(mgr);
        poseidon_channel_manager_gossip_all(mgr);
    }

    // Shutdown
    if (g_unix_transport) {
        g_unix_transport->stop(g_unix_transport);
        poseidon_transport_destroy(g_unix_transport);
    }
    poseidon_channel_manager_destroy(mgr);
    poseidon_key_pair_destroy(dial_key);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
    poseidon_msquic_close();

    return EXIT_SUCCESS;
}
```

- [ ] **Step 4: Add to CMakeLists.txt**

```cmake
# In POSEIDON_SOURCES, add:
${CLIENT_APIS_SRC_DIR}/transport_unix.c

# Add poll-dancer as subdirectory (before poseidon library):
add_subdirectory(deps/poll-dancer)

# Add poseidond executable:
add_executable(poseidond src/poseidond.c)
target_link_libraries(poseidond PRIVATE poseidon cbor hashmap pthread blake3 msquic::msquic msquic::platform OpenSSL::SSL OpenSSL::Crypto poll-dancer)
```

- [ ] **Step 5: Build and verify compilation**

Run: `cd build && cmake .. && make poseidond`
Expected: Successful compilation with no errors

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPIs/transport.h src/ClientAPIs/transport_unix.c src/poseidond.c CMakeLists.txt
git commit -m "feat: add daemon entry point and Unix domain socket transport"
```

---

### Task 2: De-wonk audit with memory leak check

- [ ] **Step 1: Read all created files and audit**

Read:
- `src/ClientAPIs/transport.h`
- `src/ClientAPIs/transport_unix.c`
- `src/poseidond.c`

Audit for:
1. Socket file descriptor leaks (accept, socket, etc.)
2. poll-dancer watcher leaks (must destroy watchers on disconnect)
3. Thread safety: channel manager lock held during session operations
4. Shutdown ordering: transports stopped before manager destroyed
5. Client session cleanup on disconnect (who destroys the session?)
6. Frame boundary detection: length-prefix parsing handles partial reads
7. Signal handler only sets `g_running = false` — no malloc/free in handler

- [ ] **Step 2: Fix any CRITICAL or HIGH issues found**

- [ ] **Step 3: Build and run existing tests**

Run: `cd build && make && ctest --output-on-failure`
Expected: All tests pass

- [ ] **Step 4: Run memory leak check on daemon**

Build with ASAN and run the daemon briefly:

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make poseidond
# Run daemon for a few seconds then Ctrl+C
timeout 3 ./poseidond --enable-unix --unix-socket /tmp/poseidon_test.sock || true
```

Verify: No ASAN errors on startup/shutdown. If leaks found, trace and fix.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk daemon and Unix transport"
```