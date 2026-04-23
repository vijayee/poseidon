# Phase 6: C Client Library

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the C client library that wraps the CBOR protocol into a native API. Handles connection management, CBOR frame encoding/decoding, request ID tracking, event dispatch, and admin signing.

**Architecture:** The C client connects via transport URL, manages a single connection, assigns request IDs, tracks pending requests for response matching, and dispatches delivery/event callbacks to the user. Admin operations (CHANNEL_DESTROY, CHANNEL_MODIFY) use `poseidon_key_pair_sign` to sign `method_code || topic_id || timestamp_us`.

**Tech Stack:** C, CBOR, OpenSSL (for ED25519 signing), Unix sockets/TCP

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/client_libs/c/poseidon_client.h` | Public API |
| Create | `src/client_libs/c/poseidon_client.c` | Implementation |
| Create | `tests/poseidon_client_test.cpp` | Test client library |
| Modify | `CMakeLists.txt` | Add source and test |

---

### Task 1: C client library implementation

**Files:**
- Create: `src/client_libs/c/poseidon_client.h`
- Create: `src/client_libs/c/poseidon_client.c`
- Create: `tests/poseidon_client_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create poseidon_client.h**

```c
//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_H
#define POSEIDON_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "Channel/channel.h"
#include "ClientAPIs/client_protocol.h"
#include "Crypto/key_pair.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CLIENT TYPES
// ============================================================================

typedef struct poseidon_client_t poseidon_client_t;

typedef void (*poseidon_delivery_cb_t)(void* ctx, const char* topic_id,
                                        const char* subtopic,
                                        const uint8_t* data, size_t len);

typedef void (*poseidon_event_cb_t)(void* ctx, uint8_t event_type,
                                     const uint8_t* data, size_t len);

// ============================================================================
// CONNECTION MANAGEMENT
// ============================================================================

poseidon_client_t* poseidon_client_connect(const char* transport_url);
void poseidon_client_disconnect(poseidon_client_t* client);

// ============================================================================
// CHANNEL LIFECYCLE
// ============================================================================

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

// ============================================================================
// PUB/SUB
// ============================================================================

int poseidon_client_subscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_unsubscribe(poseidon_client_t* client, const char* topic_path);
int poseidon_client_publish(poseidon_client_t* client, const char* topic_path,
                             const uint8_t* data, size_t len);

// ============================================================================
// ALIASES
// ============================================================================

int poseidon_client_alias_register(poseidon_client_t* client, const char* name,
                                    const char* topic_id);
int poseidon_client_alias_unregister(poseidon_client_t* client, const char* name);

// ============================================================================
// EVENTS
// ============================================================================

void poseidon_client_on_delivery(poseidon_client_t* client,
                                  poseidon_delivery_cb_t cb, void* ctx);
void poseidon_client_on_event(poseidon_client_t* client,
                               poseidon_event_cb_t cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_H
```

- [ ] **Step 2: Create poseidon_client.c**

Implementation structure:

```c
#define _POSIX_C_SOURCE 200809L
#include "poseidon_client.h"
#include "ClientAPIs/client_protocol.h"
#include "Channel/channel_manager.h"
#include "Util/allocator.h"
#include "Util/threadding.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

typedef struct pending_request_t {
    uint32_t request_id;
    cbor_item_t** response;
    PLATFORMCONDITIONTYPE(done);
    PLATFORMLOCKTYPE(lock);
} pending_request_t;

#define MAX_PENDING 64

struct poseidon_client_t {
    int fd;
    bool connected;
    uint32_t next_request_id;
    poseidon_delivery_cb_t delivery_cb;
    void* delivery_cb_ctx;
    poseidon_event_cb_t event_cb;
    void* event_cb_ctx;
    pending_request_t pending[MAX_PENDING];
    size_t num_pending;
    PLATFORMLOCKTYPE(lock);
    PLATFORMTHREADTYPE recv_thread;
    volatile bool running;
};
```

Key functions:
- `poseidon_client_connect`: Parses transport URL (`"unix:///var/run/poseidond.sock"`, `"tcp://host:port"`), creates socket, connects
- `poseidon_client_disconnect`: Closes socket, joins receive thread
- Request methods: Allocate request_id, encode CBOR frame, send, wait for response (or return immediately for fire-and-forget methods)
- `poseidon_client_channel_destroy`/`modify`: Sign `method_code || topic_id || timestamp_us` with owner_key using `poseidon_key_pair_sign`, include signature in admin request frame
- Receive thread: Read length-prefixed CBOR frames, decode, match responses to pending requests, dispatch events to callbacks

~400 lines.

- [ ] **Step 3: Write tests**

```cpp
#include <gtest/gtest.h>
#include "client_libs/c/poseidon_client.h"

TEST(PoseidonClientTest, ConnectDisconnect) {
    // Requires a running poseidond on /tmp/poseidon_test.sock
    // Skip if daemon not available
    poseidon_client_t* client = poseidon_client_connect("unix:///tmp/poseidon_test.sock");
    if (client == nullptr) {
        GTEST_SKIP() << "Daemon not available";
    }
    EXPECT_NE(client, nullptr);
    poseidon_client_disconnect(client);
}

TEST(PoseidonClientTest, ChannelCreate) {
    poseidon_client_t* client = poseidon_client_connect("unix:///tmp/poseidon_test.sock");
    if (client == nullptr) {
        GTEST_SKIP() << "Daemon not available";
    }
    char topic_id[64] = {0};
    int rc = poseidon_client_channel_create(client, "test_channel", topic_id, sizeof(topic_id));
    EXPECT_EQ(rc, 0);
    EXPECT_GT(strlen(topic_id), 0u);
    poseidon_client_disconnect(client);
}
```

- [ ] **Step 4: Add to CMakeLists.txt**

```cmake
# Add to POSEIDON_SOURCES:
${CMAKE_SOURCE_DIR}/src/client_libs/c/poseidon_client.c

# Add test target:
add_meridian_test(poseidon_client_test tests/poseidon_client_test.cpp)
```

- [ ] **Step 5: Build and test**

- [ ] **Step 6: Commit**

```bash
git add src/client_libs/c/poseidon_client.h src/client_libs/c/poseidon_client.c tests/poseidon_client_test.cpp CMakeLists.txt
git commit -m "feat: add C client library for ClientAPI protocol"
```

---

### Task 2: De-wonk audit with memory leak check

- [ ] **Step 1: Read all files, audit**

Audit for:
1. Socket fd leaks (connect without close on error paths)
2. CBOR frame leaks in request/response
3. Thread safety: pending request array accessed from send + receive threads
4. Pending request timeout (no timeout for responses — add 5s default)
5. Receive thread cleanup on disconnect (join vs detach)
6. `poseidon_key_pair_sign` called with NULL key_pair in destroy/modify

- [ ] **Step 2: Fix CRITICAL/HIGH issues**

- [ ] **Step 3: Run all tests**

```bash
cd build && make && ctest --output-on-failure
```

- [ ] **Step 4: Run memory leak check**

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make poseidon_client_test
./tests/poseidon_client_test
valgrind --leak-check=full --error-exitcode=1 ./tests/poseidon_client_test
```

Verify: Zero leaks. Fix if found.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk C client library"
```