# Phase 3: ClientAPI Protocol — CBOR Frames, Encode/Decode, Session Management

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the CBOR-based client-daemon wire protocol — request/response/event frame types, method codes (1-11), error codes (0-8), event types (1-4), per-client session state, and encode/decode functions. This is the protocol layer that all transports speak.

**Architecture:** Frames are CBOR arrays with a frame_type byte prefix. The client session tracks request IDs, subscriptions, and the transport connection. Encode/decode functions serialize/deserialize frames without touching I/O. The session is the bridge between transport reads and the channel manager.

**Tech Stack:** C, CBOR (libcbor), gtest

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientAPIs/client_protocol.h` | Frame types, method/event/error codes, encode/decode declarations |
| Create | `src/ClientAPIs/client_protocol.c` | CBOR frame serialization/deserialization |
| Create | `src/ClientAPIs/client_session.h` | Per-client session state (subscriptions, request tracking) |
| Create | `src/ClientAPIs/client_session.c` | Session lifecycle and management |
| Create | `tests/client_protocol_test.cpp` | Test encode/decode round-trips |
| Create | `tests/client_session_test.cpp` | Test session lifecycle |
| Modify | `CMakeLists.txt` | Add source files and test targets |

---

### Task 1: Client protocol encode/decode

**Files:**
- Create: `src/ClientAPIs/client_protocol.h`
- Create: `src/ClientAPIs/client_protocol.c`
- Create: `tests/client_protocol_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
#include <gtest/gtest.h>
#include <cbor.h>
#include <string.h>
#include "ClientAPIs/client_protocol.h"

TEST(ClientProtocolTest, EncodeDecodeRequest) {
    uint8_t payload[] = {0x01, 0x02, 0x03};
    cbor_item_t* frame = client_protocol_encode_request(
        42, CLIENT_METHOD_SUBSCRIBE, "X4jKL2mNpQrStUvWxYz12/Feeds",
        payload, sizeof(payload));
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    int rc = client_protocol_decode(frame, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_REQUEST);
    EXPECT_EQ(decoded.request_id, 42u);
    EXPECT_EQ(decoded.method, CLIENT_METHOD_SUBSCRIBE);
    EXPECT_STREQ(decoded.topic_path, "X4jKL2mNpQrStUvWxYz12/Feeds");

    cbor_decref(&frame);
}

TEST(ClientProtocolTest, EncodeDecodeResponse) {
    cbor_item_t* frame = client_protocol_encode_response(
        42, CLIENT_ERROR_OK, "X4jKL2mNpQrStUvWxYz12");
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    int rc = client_protocol_decode(frame, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_RESPONSE);
    EXPECT_EQ(decoded.request_id, 42u);
    EXPECT_EQ(decoded.error_code, CLIENT_ERROR_OK);
    EXPECT_STREQ(decoded.result_data, "X4jKL2mNpQrStUvWxYz12");

    cbor_decref(&frame);
}

TEST(ClientProtocolTest, EncodeDecodeEvent) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    cbor_item_t* frame = client_protocol_encode_event(
        CLIENT_EVENT_DELIVERY, "X4jKL2mNpQrStUvWxYz12", "Feeds/friend-only",
        data, sizeof(data));
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    int rc = client_protocol_decode(frame, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_EVENT);
    EXPECT_EQ(decoded.event_type, CLIENT_EVENT_DELIVERY);

    cbor_decref(&frame);
}

TEST(ClientProtocolTest, EncodeAdminRequest) {
    uint8_t signature[64];
    memset(signature, 0xAA, sizeof(signature));

    cbor_item_t* frame = client_protocol_encode_admin_request(
        7, CLIENT_METHOD_CHANNEL_DESTROY, "X4jKL2mNpQrStUvWxYz12",
        signature, sizeof(signature), NULL, 0);
    ASSERT_NE(frame, nullptr);

    client_frame_t decoded;
    int rc = client_protocol_decode(frame, &decoded);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(decoded.frame_type, CLIENT_FRAME_REQUEST);
    EXPECT_EQ(decoded.method, CLIENT_METHOD_CHANNEL_DESTROY);

    cbor_decref(&frame);
}

TEST(ClientProtocolTest, DecodeInvalidReturnsError) {
    client_frame_t decoded;
    EXPECT_NE(client_protocol_decode(NULL, &decoded), 0);

    cbor_item_t* bad = cbor_build_uint8(99);
    EXPECT_NE(client_protocol_decode(bad, &decoded), 0);
    cbor_decref(&bad);
}
```

- [ ] **Step 2: Create client_protocol.h**

```c
//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_PROTOCOL_H
#define POSEIDON_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <cbor.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FRAME TYPES
// ============================================================================

#define CLIENT_FRAME_REQUEST  0x01
#define CLIENT_FRAME_RESPONSE 0x02
#define CLIENT_FRAME_EVENT    0x03

// ============================================================================
// METHOD CODES
// ============================================================================

#define CLIENT_METHOD_CHANNEL_CREATE   1
#define CLIENT_METHOD_CHANNEL_JOIN     2
#define CLIENT_METHOD_CHANNEL_LEAVE   3
#define CLIENT_METHOD_CHANNEL_DESTROY 4
#define CLIENT_METHOD_CHANNEL_MODIFY   5
#define CLIENT_METHOD_SUBSCRIBE       6
#define CLIENT_METHOD_UNSUBSCRIBE     7
#define CLIENT_METHOD_PUBLISH         8
#define CLIENT_METHOD_ALIAS_REGISTER  9
#define CLIENT_METHOD_ALIAS_UNREGISTER 10
#define CLIENT_METHOD_ALIAS_RESOLVE   11

// ============================================================================
// ERROR CODES
// ============================================================================

#define CLIENT_ERROR_OK                0
#define CLIENT_ERROR_UNKNOWN_METHOD    1
#define CLIENT_ERROR_INVALID_PARAMS    2
#define CLIENT_ERROR_CHANNEL_NOT_FOUND 3
#define CLIENT_ERROR_ALIAS_AMBIGUOUS  4
#define CLIENT_ERROR_NOT_AUTHORIZED   5
#define CLIENT_ERROR_CHANNEL_EXISTS   6
#define CLIENT_ERROR_TOO_MANY_CHANNELS 7
#define CLIENT_ERROR_TRANSPORT        8

// ============================================================================
// EVENT TYPES
// ============================================================================

#define CLIENT_EVENT_DELIVERY     1
#define CLIENT_EVENT_CHANNEL_JOINED 2
#define CLIENT_EVENT_CHANNEL_LEFT   3
#define CLIENT_EVENT_PEER_EVENT     4

// ============================================================================
// STRING LIMITS
// ============================================================================

#define CLIENT_MAX_TOPIC_PATH 256
#define CLIENT_MAX_RESULT_DATA 1024
#define CLIENT_MAX_SUBTOPIC  256
#define CLIENT_MAX_PAYLOAD   65536
#define CLIENT_MAX_SIGNATURE 64

// ============================================================================
// DECODED FRAME
// ============================================================================

typedef struct client_frame_t {
    uint8_t frame_type;       /**< CLIENT_FRAME_REQUEST/RESPONSE/EVENT */
    uint32_t request_id;      /**< Matches request/response */
    uint8_t method;           /**< Method code (requests only) */
    uint8_t error_code;        /**< Error code (responses only) */
    uint8_t event_type;       /**< Event type (events only) */
    char topic_path[CLIENT_MAX_TOPIC_PATH];    /**< Topic path or channel topic */
    char subtopic[CLIENT_MAX_SUBTOPIC];         /**< Subtopic (events only) */
    char result_data[CLIENT_MAX_RESULT_DATA];   /**< Result data (responses) */
    uint8_t payload[CLIENT_MAX_PAYLOAD];         /**< Binary payload */
    size_t payload_len;
    uint8_t signature[CLIENT_MAX_SIGNATURE];     /**< Admin signature */
    size_t signature_len;
    char name[64];            /**< Alias name (ALIAS_REGISTER/UNREGISTER/RESOLVE) */
    // Ambiguous alias candidates
    char* candidates[8];
    size_t num_candidates;
} client_frame_t;

// ============================================================================
// ENCODE FUNCTIONS
// ============================================================================

cbor_item_t* client_protocol_encode_request(uint32_t request_id, uint8_t method,
                                             const char* topic_path,
                                             const uint8_t* payload, size_t payload_len);

cbor_item_t* client_protocol_encode_admin_request(uint32_t request_id, uint8_t method,
                                                   const char* topic_path,
                                                   const uint8_t* signature, size_t sig_len,
                                                   const uint8_t* config_data, size_t config_len);

cbor_item_t* client_protocol_encode_response(uint32_t request_id, uint8_t error_code,
                                              const char* result_data);

cbor_item_t* client_protocol_encode_event(uint8_t event_type,
                                           const char* topic_id,
                                           const char* subtopic,
                                           const uint8_t* data, size_t data_len);

// ============================================================================
// DECODE FUNCTION
// ============================================================================

int client_protocol_decode(const cbor_item_t* item, client_frame_t* out);

// ============================================================================
// SERIALIZE TO BUFFER
// ============================================================================

/**
 * Serializes a CBOR frame to a byte buffer.
 * Caller must free the returned buffer.
 *
 * @param frame   CBOR frame item
 * @param buf     Output: allocated buffer (caller frees)
 * @param len     Output: buffer length
 * @return        0 on success, -1 on failure
 */
int client_protocol_serialize(const cbor_item_t* frame, uint8_t** buf, size_t* len);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_PROTOCOL_H
```

- [ ] **Step 3: Create client_protocol.c**

Implement all encode/decode functions following the CBOR patterns established in `meridian_packet.c`. Each frame is a CBOR array with a type prefix byte. The implementation is ~300 lines and follows the same pattern: create a definite array, push fields, return.

Key implementation details:
- Request frames: `[0x01, request_id, method, topic_path, payload_bytes]`
- Response frames: `[0x02, request_id, error_code, result_string]`
- Event frames: `[0x03, event_type, topic_id, subtopic, data_bytes]`
- Admin request frames: `[0x01, request_id, method, topic_path, signature_bytes, config_bytes]`
- Decode validates frame_type byte first, then extracts fields

The full implementation is ~300 lines and closely follows `meridian_packet.c` patterns. Use `cbor_build_uint8`, `cbor_build_uint32`, `cbor_build_string`, `cbor_build_bytestring` for encoding and `cbor_get_*` / `cbor_string_handle` for decoding.

- [ ] **Step 4: Add source and test to CMakeLists.txt**

```cmake
# In POSEIDON_SOURCES:
${CLIENT_APIS_SRC_DIR}/client_protocol.c

# In test targets:
add_meridian_test(client_protocol_test tests/client_protocol_test.cpp)
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && cmake .. && make client_protocol_test && ./tests/client_protocol_test`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPIs/client_protocol.h src/ClientAPIs/client_protocol.c tests/client_protocol_test.cpp CMakeLists.txt
git commit -m "feat: add ClientAPI protocol CBOR frame encode/decode"
```

---

### Task 2: Client session management

**Files:**
- Create: `src/ClientAPIs/client_session.h`
- Create: `src/ClientAPIs/client_session.c`
- Create: `tests/client_session_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create client_session.h**

```c
//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_CLIENT_SESSION_H
#define POSEIDON_CLIENT_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "client_protocol.h"
#include "../Channel/channel_manager.h"
#include "../RefCounter/refcounter.h"
#include "../Util/threadding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_SESSION_MAX_SUBSCRIPTIONS 256

typedef struct client_subscription_t {
    char topic_path[CLIENT_MAX_TOPIC_PATH];
    bool active;
} client_subscription_t;

typedef struct client_session_t {
    refcounter_t refcounter;
    uint32_t session_id;
    int client_fd;                                    /**< Transport socket fd (-1 if not connected) */
    poseidon_channel_manager_t* manager;              /**< Shared channel manager */
    client_subscription_t subscriptions[CLIENT_SESSION_MAX_SUBSCRIPTIONS];
    size_t num_subscriptions;
    uint32_t next_request_id;
    PLATFORMLOCKTYPE(lock);
} client_session_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

client_session_t* client_session_create(uint32_t session_id,
                                         poseidon_channel_manager_t* manager);
void client_session_destroy(client_session_t* session);

// ============================================================================
// REQUEST PROCESSING
// ============================================================================

/**
 * Processes an incoming client frame and produces a response.
 * Handles all method codes (1-11).
 *
 * @param session  Client session
 * @param frame    Decoded client frame
 * @param out      Output response frame (CBOR item, caller decrefs)
 * @return         0 on success, -1 on error
 */
int client_session_handle_request(client_session_t* session,
                                    const client_frame_t* frame,
                                    cbor_item_t** out);

/**
 * Subscribes the session to a topic path.
 *
 * @param session    Client session
 * @param topic_path Path like "Alice/Feeds/friend-only"
 * @return          0 on success, -1 on failure
 */
int client_session_subscribe(client_session_t* session, const char* topic_path);

/**
 * Unsubscribes the session from a topic path.
 *
 * @param session    Client session
 * @param topic_path Path to unsubscribe from
 * @return          0 on success, -1 if not subscribed
 */
int client_session_unsubscribe(client_session_t* session, const char* topic_path);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_CLIENT_SESSION_H
```

- [ ] **Step 2: Create client_session.c**

Implement session lifecycle and request handling. Key functions:

- `client_session_create`: Allocates session with `get_clear_memory`, initializes lock, increments next_request_id
- `client_session_destroy`: Frees subscriptions, destroys lock, decrements refcount
- `client_session_handle_request`: Switch on method code, call into channel manager:
  - CHANNEL_CREATE → `poseidon_channel_manager_create_channel`
  - CHANNEL_JOIN → `poseidon_channel_manager_join_channel`
  - CHANNEL_LEAVE → find channel, call `poseidon_channel_manager_destroy_channel`
  - CHANNEL_DESTROY → verify signature, destroy channel
  - CHANNEL_MODIFY → verify signature, update config
  - SUBSCRIBE/UNSUBSCRIBE → delegate to `client_session_subscribe/unsubscribe`
  - PUBLISH → resolve path, publish on channel
  - ALIAS_REGISTER/UNREGISTER/RESOLVE → delegate to channel alias registry
- `client_session_subscribe`: Add to subscriptions array
- `client_session_unsubscribe`: Remove from subscriptions array

For CHANNEL_DESTROY and CHANNEL_MODIFY, verify the ED25519 signature over `method_code || topic_id || timestamp_us`.

- [ ] **Step 3: Write session tests**

```cpp
#include <gtest/gtest.h>
#include "ClientAPIs/client_session.h"
#include "Channel/channel_manager.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

class ClientSessionTest : public ::testing::Test {
protected:
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    poseidon_key_pair_t* kp = nullptr;
    poseidon_channel_manager_t* mgr = nullptr;

    void SetUp() override {
        pool = work_pool_create(1);
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
        kp = poseidon_key_pair_create("ED25519");
        ASSERT_NE(kp, nullptr);
        mgr = poseidon_channel_manager_create(kp, 15000, 15001, 15100, pool, wheel);
        ASSERT_NE(mgr, nullptr);
    }

    void TearDown() override {
        if (mgr) poseidon_channel_manager_destroy(mgr);
        if (kp) poseidon_key_pair_destroy(kp);
        hierarchical_timing_wheel_wait_for_idle_signal(wheel);
        hierarchical_timing_wheel_stop(wheel);
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
        work_pool_destroy(pool);
        hierarchical_timing_wheel_destroy(wheel);
    }
};

TEST_F(ClientSessionTest, CreateDestroy) {
    client_session_t* session = client_session_create(1, mgr);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->session_id, 1u);
    client_session_destroy(session);
}

TEST_F(ClientSessionTest, SubscribeUnsubscribe) {
    client_session_t* session = client_session_create(1, mgr);
    ASSERT_NE(session, nullptr);

    int rc = client_session_subscribe(session, "Alice/Feeds");
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(session->num_subscriptions, 1u);

    rc = client_session_unsubscribe(session, "Alice/Feeds");
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(session->num_subscriptions, 0u);

    client_session_destroy(session);
}
```

- [ ] **Step 4: Add to CMakeLists.txt**

```cmake
# In POSEIDON_SOURCES:
${CLIENT_APIS_SRC_DIR}/client_session.c

# In test targets:
add_meridian_test(client_session_test tests/client_session_test.cpp)
```

- [ ] **Step 5: Run tests**

Run: `cd build && cmake .. && make client_session_test && ./tests/client_session_test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/ClientAPIs/client_session.h src/ClientAPIs/client_session.c tests/client_session_test.cpp CMakeLists.txt
git commit -m "feat: add client session management for ClientAPI protocol"
```

---

### Task 3: De-wonk audit with memory leak check

- [ ] **Step 1: Read all created files and audit**

Read:
- `src/ClientAPIs/client_protocol.h` and `client_protocol.c`
- `src/ClientAPIs/client_session.h` and `client_session.c`
- `tests/client_protocol_test.cpp`
- `tests/client_session_test.cpp`

Audit for:
1. CBOR items leaked in encode functions (every `cbor_build_*` creates a reference that must be decrefed)
2. CBOR items leaked in decode functions (the decoded `cbor_item_t*` passed in — who owns it?)
3. Session subscriptions array bounds checking
4. Thread safety: session lock held during channel manager calls (potential deadlock if manager also locks)
5. `client_frame_t.candidates` points to CBOR string memory — must copy strings, not just point into CBOR items

- [ ] **Step 2: Fix any CRITICAL or HIGH issues found**

- [ ] **Step 3: Run all tests**

Run: `cd build && make && ctest --output-on-failure`
Expected: All tests pass

- [ ] **Step 4: Run memory leak check**

Build with AddressSanitizer and run:

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make client_protocol_test client_session_test
./tests/client_protocol_test
./tests/client_session_test
```

Also run under valgrind if available:

```bash
valgrind --leak-check=full --error-exitcode=1 ./tests/client_protocol_test
valgrind --leak-check=full --error-exitcode=1 ./tests/client_session_test
```

Verify: Zero leaks reported. If leaks found, trace and fix before proceeding.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk ClientAPI protocol and session management"
```