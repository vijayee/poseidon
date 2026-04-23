# Phase 1: Bootstrap Wire Protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the channel bootstrap wire protocol so that `poseidon_channel_manager_join_channel` actually discovers existing channel members and connects to them, instead of just subscribing and creating an isolated channel.

**Architecture:** Two new CBOR packet types (CHANNEL_BOOTSTRAP type 40, CHANNEL_BOOTSTRAP_REPLY type 41) are encoded/decoded in `meridian_packet.h/c` following the existing packet pattern. The channel manager tracks pending bootstrap requests and collects replies with a timeout. On receiving a BOOTSTRAP packet, existing channel members reply directly with their connection info. On receiving a BOOTSTRAP_REPLY, the joiner connects to the first responder and seeds its ring.

**Tech Stack:** C, CBOR (libcbor), gtest, existing Meridian + Quasar + Channel infrastructure

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Modify | `src/Network/Meridian/meridian_packet.h` | Add bootstrap packet structs, encode/decode declarations |
| Modify | `src/Network/Meridian/meridian_packet.c` | Implement encode/decode for bootstrap packets |
| Modify | `src/Channel/channel_manager.h` | Add pending bootstrap tracking, modify join_channel signature |
| Modify | `src/Channel/channel_manager.c` | Rewrite join_channel with bootstrap protocol, add reply handler |
| Modify | `src/Channel/channel.h` | Add bootstrap_timeout_ms to config, add bootstrap state tracking |
| Create | `tests/channel_bootstrap_test.cpp` | Test encode/decode and join flow |

---

### Task 1: Bootstrap packet structs and declarations

**Files:**
- Modify: `src/Network/Meridian/meridian_packet.h:97-109` (after existing bootstrap type defines)
- Test: `tests/channel_bootstrap_test.cpp`

- [ ] **Step 1: Write the failing test for bootstrap encode/decode**

```cpp
#include <gtest/gtest.h>
#include <cbor.h>
#include "Network/Meridian/meridian_packet.h"

TEST(BootstrapPacketTest, EncodeDecodeBootstrap) {
    const char* topic_id = "X4jKL2mNpQrStUvWxYz";
    const char* sender_node_id = "AbCdEfGhIjKlMnOpQrStUvWxYz1234";
    uint64_t timestamp_us = 1745300000000ULL;

    cbor_item_t* encoded = meridian_channel_bootstrap_encode(
        topic_id, sender_node_id, timestamp_us);
    ASSERT_NE(encoded, nullptr);
    ASSERT_TRUE(cbor_array_is_definite(encoded));
    EXPECT_EQ(cbor_array_size(encoded), 4u);

    char out_topic[64] = {0};
    char out_node_id[64] = {0};
    uint64_t out_ts = 0;
    int rc = meridian_channel_bootstrap_decode(encoded, out_topic, sizeof(out_topic),
                                                 out_node_id, sizeof(out_node_id), &out_ts);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(out_topic, topic_id);
    EXPECT_STREQ(out_node_id, sender_node_id);
    EXPECT_EQ(out_ts, timestamp_us);

    cbor_decref(&encoded);
}

TEST(BootstrapPacketTest, EncodeDecodeBootstrapReply) {
    const char* topic_id = "X4jKL2mNpQrStUvWxYz";
    const char* responder_node_id = "ReSpOnDeR1234567890AbCdEf";
    uint32_t addr = 0x7F000001;
    uint16_t port = 9000;
    uint64_t timestamp_us = 1745300000000ULL;
    uint32_t seed_addrs[] = {0xC0A80001, 0xC0A80002};
    uint16_t seed_ports[] = {8080, 8081};
    size_t num_seeds = 2;

    cbor_item_t* encoded = meridian_channel_bootstrap_reply_encode(
        topic_id, responder_node_id, addr, port, timestamp_us,
        seed_addrs, seed_ports, num_seeds);
    ASSERT_NE(encoded, nullptr);

    char out_topic[64] = {0};
    char out_node_id[64] = {0};
    uint32_t out_addr = 0;
    uint16_t out_port = 0;
    uint64_t out_ts = 0;
    uint32_t out_seed_addrs[16] = {0};
    uint16_t out_seed_ports[16] = {0};
    size_t out_num_seeds = 0;

    int rc = meridian_channel_bootstrap_reply_decode(encoded, out_topic, sizeof(out_topic),
                                                      out_node_id, sizeof(out_node_id),
                                                      &out_addr, &out_port, &out_ts,
                                                      out_seed_addrs, out_seed_ports,
                                                      &out_num_seeds, 16);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(out_topic, topic_id);
    EXPECT_STREQ(out_node_id, responder_node_id);
    EXPECT_EQ(out_addr, addr);
    EXPECT_EQ(out_port, port);
    EXPECT_EQ(out_ts, timestamp_us);
    EXPECT_EQ(out_num_seeds, 2u);
    EXPECT_EQ(out_seed_addrs[0], seed_addrs[0]);
    EXPECT_EQ(out_seed_ports[0], seed_ports[0]);

    cbor_decref(&encoded);
}

TEST(BootstrapPacketTest, DecodeInvalidReturnsNull) {
    // Decode NULL should fail
    char buf[64];
    uint64_t ts = 0;
    EXPECT_NE(0, meridian_channel_bootstrap_decode(NULL, buf, sizeof(buf), buf, sizeof(buf), &ts));
}

TEST(BootstrapPacketTest, EncodeDecodeReplyWithNoSeeds) {
    const char* topic_id = "TestTopic123";
    const char* node_id = "NodeId456";
    uint64_t ts = 12345;

    cbor_item_t* encoded = meridian_channel_bootstrap_reply_encode(
        topic_id, node_id, 0, 0, ts, NULL, NULL, 0);
    ASSERT_NE(encoded, nullptr);

    char out_topic[64], out_node_id[64];
    uint32_t out_addr; uint16_t out_port; uint64_t out_ts;
    uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds = 0;

    int rc = meridian_channel_bootstrap_reply_decode(encoded, out_topic, sizeof(out_topic),
                                                      out_node_id, sizeof(out_node_id),
                                                      &out_addr, &out_port, &out_ts,
                                                      seed_addrs, seed_ports, &num_seeds, 16);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(num_seeds, 0u);

    cbor_decref(&encoded);
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

Add after the existing `add_meridian_test(channel_test ...)` line in `CMakeLists.txt`:

```cmake
add_meridian_test(channel_bootstrap_test tests/channel_bootstrap_test.cpp)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd build && cmake .. && make channel_bootstrap_test && ./tests/channel_bootstrap_test`
Expected: FAIL — `meridian_channel_bootstrap_encode` and `meridian_channel_bootstrap_decode` undeclared

- [ ] **Step 4: Add struct definitions and declarations to meridian_packet.h**

After the existing `MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY` define (line 99), and before the Quasar section (line 102), add:

```c
// ============================================================================
// CHANNEL BOOTSTRAP STRUCTURES
// ============================================================================

/**
 * Channel bootstrap request packet.
 * Published via Quasar on the dial channel by a node wanting to join.
 *
 * Wire format (CBOR array):
 * [type(40), string(topic_id), string(sender_node_id), uint64(timestamp_us)]
 */
typedef struct meridian_channel_bootstrap_t {
    uint8_t type;
    char topic_id[64];
    char sender_node_id[64];
    uint64_t timestamp_us;
} meridian_channel_bootstrap_t;

/**
 * Channel bootstrap reply packet.
 * Sent directly back to the requester by existing channel members.
 *
 * Wire format (CBOR array):
 * [type(41), string(topic_id), string(responder_node_id),
 *  uint32(responder_addr), uint16(responder_port), uint64(timestamp_us),
 *  array([uint32(addr), uint16(port)], ...)]
 */
typedef struct meridian_channel_bootstrap_reply_t {
    uint8_t type;
    char topic_id[64];
    char responder_node_id[64];
    uint32_t responder_addr;
    uint16_t responder_port;
    uint64_t timestamp_us;
    uint32_t seed_addrs[16];
    uint16_t seed_ports[16];
    size_t num_seeds;
} meridian_channel_bootstrap_reply_t;

// ============================================================================
// CHANNEL BOOTSTRAP PACKET OPERATIONS
// ============================================================================

cbor_item_t* meridian_channel_bootstrap_encode(const char* topic_id,
                                                const char* sender_node_id,
                                                uint64_t timestamp_us);

int meridian_channel_bootstrap_decode(const cbor_item_t* item,
                                       char* topic_id, size_t topic_buf_size,
                                       char* sender_node_id, size_t node_id_buf_size,
                                       uint64_t* timestamp_us);

cbor_item_t* meridian_channel_bootstrap_reply_encode(const char* topic_id,
                                                      const char* responder_node_id,
                                                      uint32_t responder_addr,
                                                      uint16_t responder_port,
                                                      uint64_t timestamp_us,
                                                      const uint32_t* seed_addrs,
                                                      const uint16_t* seed_ports,
                                                      size_t num_seeds);

int meridian_channel_bootstrap_reply_decode(const cbor_item_t* item,
                                             char* topic_id, size_t topic_buf_size,
                                             char* responder_node_id, size_t node_id_buf_size,
                                             uint32_t* responder_addr,
                                             uint16_t* responder_port,
                                             uint64_t* timestamp_us,
                                             uint32_t* seed_addrs,
                                             uint16_t* seed_ports,
                                             size_t* num_seeds,
                                             size_t max_seeds);
```

- [ ] **Step 5: Implement encode/decode in meridian_packet.c**

Add at the end of `meridian_packet.c`, before the utility functions section:

```c
// ============================================================================
// CHANNEL BOOTSTRAP PACKET OPERATIONS
// ============================================================================

cbor_item_t* meridian_channel_bootstrap_encode(const char* topic_id,
                                                const char* sender_node_id,
                                                uint64_t timestamp_us) {
    if (topic_id == NULL || sender_node_id == NULL) return NULL;

    cbor_item_t* array = cbor_new_definite_array(4);
    cbor_array_push(array, cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP));
    cbor_array_push(array, cbor_build_string(topic_id));
    cbor_array_push(array, cbor_build_string(sender_node_id));
    cbor_array_push(array, cbor_build_uint64(timestamp_us));
    return array;
}

int meridian_channel_bootstrap_decode(const cbor_item_t* item,
                                       char* topic_id, size_t topic_buf_size,
                                       char* sender_node_id, size_t node_id_buf_size,
                                       uint64_t* timestamp_us) {
    if (item == NULL || topic_id == NULL || sender_node_id == NULL || timestamp_us == NULL)
        return -1;
    if (!cbor_array_is_definite((cbor_item_t*)item)) return -1;

    size_t arr_size = cbor_array_size((cbor_item_t*)item);
    if (arr_size < 4) return -1;

    cbor_item_t** items = cbor_array_handle((cbor_item_t*)item);

    if (!cbor_isa_uint(items[0]) || cbor_get_uint8(items[0]) != MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP)
        return -1;

    if (!cbor_isa_string(items[1])) return -1;
    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= topic_buf_size) return -1;
    memcpy(topic_id, cbor_string_handle(items[1]), topic_len);
    topic_id[topic_len] = '\0';

    if (!cbor_isa_string(items[2])) return -1;
    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= node_id_buf_size) return -1;
    memcpy(sender_node_id, cbor_string_handle(items[2]), node_id_len);
    sender_node_id[node_id_len] = '\0';

    if (!cbor_isa_uint(items[3])) return -1;
    *timestamp_us = cbor_get_uint64(items[3]);

    return 0;
}

cbor_item_t* meridian_channel_bootstrap_reply_encode(const char* topic_id,
                                                      const char* responder_node_id,
                                                      uint32_t responder_addr,
                                                      uint16_t responder_port,
                                                      uint64_t timestamp_us,
                                                      const uint32_t* seed_addrs,
                                                      const uint16_t* seed_ports,
                                                      size_t num_seeds) {
    if (topic_id == NULL || responder_node_id == NULL) return NULL;
    if (num_seeds > 0 && (seed_addrs == NULL || seed_ports == NULL)) return NULL;

    cbor_item_t* array = cbor_new_definite_array(7);
    cbor_array_push(array, cbor_build_uint8(MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY));
    cbor_array_push(array, cbor_build_string(topic_id));
    cbor_array_push(array, cbor_build_string(responder_node_id));
    cbor_array_push(array, cbor_build_uint32(responder_addr));
    cbor_array_push(array, cbor_build_uint16(responder_port));
    cbor_array_push(array, cbor_build_uint64(timestamp_us));

    cbor_item_t* seeds = cbor_new_definite_array(num_seeds);
    for (size_t i = 0; i < num_seeds; i++) {
        cbor_item_t* seed = cbor_new_definite_array(2);
        cbor_array_push(seed, cbor_build_uint32(seed_addrs[i]));
        cbor_array_push(seed, cbor_build_uint16(seed_ports[i]));
        cbor_array_push(seeds, seed);
    }
    cbor_array_push(array, seeds);

    return array;
}

int meridian_channel_bootstrap_reply_decode(const cbor_item_t* item,
                                             char* topic_id, size_t topic_buf_size,
                                             char* responder_node_id, size_t node_id_buf_size,
                                             uint32_t* responder_addr,
                                             uint16_t* responder_port,
                                             uint64_t* timestamp_us,
                                             uint32_t* seed_addrs,
                                             uint16_t* seed_ports,
                                             size_t* num_seeds,
                                             size_t max_seeds) {
    if (item == NULL || topic_id == NULL || responder_node_id == NULL) return -1;
    if (responder_addr == NULL || responder_port == NULL || timestamp_us == NULL) return -1;
    if (seed_addrs == NULL || seed_ports == NULL || num_seeds == NULL) return -1;
    if (!cbor_array_is_definite((cbor_item_t*)item)) return -1;

    size_t arr_size = cbor_array_size((cbor_item_t*)item);
    if (arr_size < 7) return -1;

    cbor_item_t** items = cbor_array_handle((cbor_item_t*)item);

    if (!cbor_isa_uint(items[0]) || cbor_get_uint8(items[0]) != MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY)
        return -1;

    if (!cbor_isa_string(items[1])) return -1;
    size_t topic_len = cbor_string_length(items[1]);
    if (topic_len >= topic_buf_size) return -1;
    memcpy(topic_id, cbor_string_handle(items[1]), topic_len);
    topic_id[topic_len] = '\0';

    if (!cbor_isa_string(items[2])) return -1;
    size_t node_id_len = cbor_string_length(items[2]);
    if (node_id_len >= node_id_buf_size) return -1;
    memcpy(responder_node_id, cbor_string_handle(items[2]), node_id_len);
    responder_node_id[node_id_len] = '\0';

    if (!cbor_isa_uint(items[3])) return -1;
    *responder_addr = (uint32_t)cbor_get_uint32(items[3]);

    if (!cbor_isa_uint(items[4])) return -1;
    *responder_port = (uint16_t)cbor_get_uint16(items[4]);

    if (!cbor_isa_uint(items[5])) return -1;
    *timestamp_us = cbor_get_uint64(items[5]);

    *num_seeds = 0;
    if (cbor_isa_array(items[6])) {
        size_t seed_count = cbor_array_size(items[6]);
        size_t to_read = seed_count < max_seeds ? seed_count : max_seeds;
        cbor_item_t** seed_items = cbor_array_handle(items[6]);
        for (size_t i = 0; i < to_read; i++) {
            if (!cbor_array_is_definite(seed_items[i])) break;
            if (cbor_array_size(seed_items[i]) < 2) break;
            cbor_item_t** pair = cbor_array_handle(seed_items[i]);
            seed_addrs[i] = (uint32_t)cbor_get_uint32(pair[0]);
            seed_ports[i] = (uint16_t)cbor_get_uint16(pair[1]);
            (*num_seeds)++;
        }
    }

    return 0;
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd build && make channel_bootstrap_test && ./tests/channel_bootstrap_test`
Expected: All 4 tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/Network/Meridian/meridian_packet.h src/Network/Meridian/meridian_packet.c tests/channel_bootstrap_test.cpp CMakeLists.txt
git commit -m "feat: add channel bootstrap packet encode/decode"
```

---

### Task 2: Pending bootstrap tracking in channel manager

**Files:**
- Modify: `src/Channel/channel_manager.h:24-35` (add pending bootstrap array)
- Modify: `src/Channel/channel_manager.c` (add create/destroy for pending entries)
- Test: `tests/channel_bootstrap_test.cpp` (add pending bootstrap test)

- [ ] **Step 1: Write the failing test for pending bootstrap tracking**

Add to `tests/channel_bootstrap_test.cpp`:

```cpp
#include "Channel/channel_manager.h"
#include "Channel/channel.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

TEST(BootstrapPendingTest, JoinChannelCreatesPendingEntry) {
    // Create infrastructure
    work_pool_t* pool = work_pool_create(1);
    work_pool_launch(pool);
    hierarchical_timing_wheel_t* wheel = hierarchical_timing_wheel_create(8, pool);
    hierarchical_timing_wheel_run(wheel);

    poseidon_key_pair_t* kp = poseidon_key_pair_create("ED25519");
    ASSERT_NE(kp, nullptr);

    poseidon_channel_manager_t* mgr = poseidon_channel_manager_create(
        kp, 13000, 13001, 13100, pool, wheel);
    ASSERT_NE(mgr, nullptr);

    // Join should create a channel in BOOTSTRAPPING state
    poseidon_channel_t* ch = poseidon_channel_manager_join_channel(mgr, "TestTopic123");
    ASSERT_NE(ch, nullptr);
    EXPECT_EQ(ch->state, POSEIDON_CHANNEL_STATE_BOOTSTRAPPING);

    // Should have a pending bootstrap entry
    EXPECT_GT(mgr->num_pending_bootstraps, 0u);
    EXPECT_STREQ(mgr->pending_bootstraps[0].topic_id, "TestTopic123");

    poseidon_channel_manager_destroy(mgr);
    poseidon_key_pair_destroy(kp);
    hierarchical_timing_wheel_wait_for_idle_signal(wheel);
    hierarchical_timing_wheel_stop(wheel);
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
    hierarchical_timing_wheel_destroy(wheel);
}
```

- [ ] **Step 2: Add pending_bootstrap_t struct and array to channel_manager.h**

Add before the `poseidon_channel_manager_t` struct definition:

```c
#define POSEIDON_MAX_PENDING_BOOTSTRAPS 16
#define POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX 16

typedef struct pending_bootstrap_t {
    char topic_id[64];
    uint64_t timestamp_us;
    poseidon_channel_t* channel;
    uint32_t reply_addrs[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    uint16_t reply_ports[POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX];
    size_t num_replies;
} pending_bootstrap_t;
```

Add to `poseidon_channel_manager_t` struct (after `PLATFORMLOCKTYPE(lock)`):

```c
    pending_bootstrap_t pending_bootstraps[POSEIDON_MAX_PENDING_BOOTSTRAPS];
    size_t num_pending_bootstraps;
```

Add declarations after the channel management section:

```c
/**
 * Handles an incoming CHANNEL_BOOTSTRAP_REPLY.
 * Looks up the matching pending bootstrap by topic_id + timestamp,
 * stores the reply, and if first reply, connects to the responder.
 *
 * @param mgr              Channel manager
 * @param topic_id         Topic ID from the reply
 * @param responder_addr   Responder's Meridian listen address
 * @param responder_port   Responder's Meridian listen port
 * @param timestamp_us     Timestamp from the reply (must match pending request)
 * @param seed_addrs       Seed node addresses from reply
 * @param seed_ports       Seed node ports from reply
 * @param num_seeds        Number of seed nodes
 * @return                 0 on success, -1 if no matching pending request
 */
int poseidon_channel_manager_handle_bootstrap_reply(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds);

/**
 * Called when the daemon receives a CHANNEL_BOOTSTRAP request.
 * If this node is a member of the requested channel, sends a reply
 * directly back to the requester via the dial channel.
 *
 * @param mgr              Channel manager
 * @param topic_id         Topic ID being bootstrapped
 * @param sender_node_id   Node ID of the requester
 * @return                 0 if reply sent, -1 if not a member or error
 */
int poseidon_channel_manager_handle_bootstrap_request(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id);
```

- [ ] **Step 3: Implement pending bootstrap tracking and join_channel rewrite**

In `channel_manager.c`, rewrite `poseidon_channel_manager_join_channel`:

```c
poseidon_channel_t* poseidon_channel_manager_join_channel(
    poseidon_channel_manager_t* mgr,
    const char* topic_str) {
    if (mgr == NULL || topic_str == NULL) return NULL;

    platform_lock(&mgr->lock);

    if (mgr->num_channels >= POSEIDON_CHANNEL_MANAGER_MAX_CHANNELS) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    if (mgr->num_pending_bootstraps >= POSEIDON_MAX_PENDING_BOOTSTRAPS) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    uint16_t port = allocate_port(mgr);
    if (port == 0) {
        platform_unlock(&mgr->lock);
        return NULL;
    }

    poseidon_channel_config_t config = poseidon_channel_config_defaults();
    poseidon_channel_t* channel = poseidon_channel_create(NULL, topic_str, port,
                                                           &config, mgr->pool, mgr->wheel);
    if (channel == NULL) {
        platform_unlock(&mgr->lock);
        return NULL;
    }
    channel->state = POSEIDON_CHANNEL_STATE_BOOTSTRAPPING;

    // Register pending bootstrap
    pending_bootstrap_t* pending = &mgr->pending_bootstraps[mgr->num_pending_bootstraps];
    strncpy(pending->topic_id, topic_str, sizeof(pending->topic_id) - 1);
    pending->timestamp_us = (uint64_t)time(NULL) * 1000000ULL;
    pending->channel = channel;
    pending->num_replies = 0;
    mgr->num_pending_bootstraps++;

    // Subscribe to the topic on the dial channel
    poseidon_channel_subscribe(mgr->dial_channel,
                                (const uint8_t*)topic_str, strlen(topic_str), 300);

    // Publish CHANNEL_BOOTSTRAP via Quasar on the dial channel
    const poseidon_node_id_t* local_id = poseidon_channel_get_node_id(mgr->dial_channel);
    cbor_item_t* bootstrap_pkt = meridian_channel_bootstrap_encode(
        topic_str, local_id->str, pending->timestamp_us);
    if (bootstrap_pkt != NULL) {
        unsigned char* buf = NULL;
        size_t buf_len = 0;
        size_t written = 0;
        cbor_serialize_alloc(bootstrap_pkt, &buf, &buf_len, &written);
        if (buf != NULL && written > 0) {
            poseidon_channel_publish(mgr->dial_channel,
                                      (const uint8_t*)topic_str, strlen(topic_str),
                                      buf, written);
        }
        free(buf);
        cbor_decref(&bootstrap_pkt);
    }

    mgr->channels[mgr->num_channels++] = channel;
    platform_unlock(&mgr->lock);
    return channel;
}
```

Add the bootstrap reply handler:

```c
int poseidon_channel_manager_handle_bootstrap_reply(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    uint32_t responder_addr,
    uint16_t responder_port,
    uint64_t timestamp_us,
    const uint32_t* seed_addrs,
    const uint16_t* seed_ports,
    size_t num_seeds) {
    if (mgr == NULL || topic_id == NULL) return -1;

    platform_lock(&mgr->lock);

    pending_bootstrap_t* pending = NULL;
    for (size_t i = 0; i < mgr->num_pending_bootstraps; i++) {
        if (strcmp(mgr->pending_bootstraps[i].topic_id, topic_id) == 0 &&
            mgr->pending_bootstraps[i].timestamp_us == timestamp_us) {
            pending = &mgr->pending_bootstraps[i];
            break;
        }
    }

    if (pending == NULL) {
        platform_unlock(&mgr->lock);
        return -1;
    }

    // Store reply
    if (pending->num_replies < POSEIDON_BOOTSTRAP_REPLY_ADDRS_MAX) {
        pending->reply_addrs[pending->num_replies] = responder_addr;
        pending->reply_ports[pending->num_replies] = responder_port;
        pending->num_replies++;
    }

    // On first reply, connect to the responder and seed the ring
    if (pending->num_replies == 1 && pending->channel != NULL) {
        meridian_protocol_connect(pending->channel->protocol, responder_addr, responder_port);

        // Add seed nodes
        for (size_t i = 0; i < num_seeds && i < 16; i++) {
            meridian_protocol_connect(pending->channel->protocol, seed_addrs[i], seed_ports[i]);
        }

        // Transition channel to RUNNING
        pending->channel->state = POSEIDON_CHANNEL_STATE_RUNNING;

        // Remove pending entry
        *pending = mgr->pending_bootstraps[mgr->num_pending_bootstraps - 1];
        mgr->num_pending_bootstraps--;
    }

    platform_unlock(&mgr->lock);
    return 0;
}

int poseidon_channel_manager_handle_bootstrap_request(
    poseidon_channel_manager_t* mgr,
    const char* topic_id,
    const char* sender_node_id) {
    if (mgr == NULL || topic_id == NULL || sender_node_id == NULL) return -1;

    platform_lock(&mgr->lock);

    // Check if we are a member of this channel
    poseidon_channel_t* member_channel = NULL;
    for (size_t i = 0; i < mgr->num_channels; i++) {
        const char* ch_topic = poseidon_channel_get_topic(mgr->channels[i]);
        if (ch_topic != NULL && strcmp(ch_topic, topic_id) == 0) {
            member_channel = mgr->channels[i];
            break;
        }
    }

    if (member_channel == NULL) {
        platform_unlock(&mgr->lock);
        return -1;
    }

    // Build and send BOOTSTRAP_REPLY via the dial channel
    const poseidon_node_id_t* local_id = poseidon_channel_get_node_id(mgr->dial_channel);
    uint32_t local_addr = 0;
    uint16_t local_port = member_channel->listen_port;

    cbor_item_t* reply = meridian_channel_bootstrap_reply_encode(
        topic_id, local_id->str, local_addr, local_port,
        (uint64_t)time(NULL) * 1000000ULL, NULL, NULL, 0);

    if (reply != NULL) {
        unsigned char* buf = NULL;
        size_t buf_len = 0;
        size_t written = 0;
        cbor_serialize_alloc(reply, &buf, &buf_len, &written);
        if (buf != NULL && written > 0) {
            // Send directly to the requester via the dial channel
            poseidon_channel_publish(mgr->dial_channel,
                                      (const uint8_t*)topic_id, strlen(topic_id),
                                      buf, written);
        }
        free(buf);
        cbor_decref(&reply);
    }

    platform_unlock(&mgr->lock);
    return 0;
}
```

Initialize `num_pending_bootstraps = 0` in `poseidon_channel_manager_create` (already zeroed by `get_clear_memory`).

- [ ] **Step 4: Update channel.h to allow NULL key_pair in channel_create**

The `poseidon_channel_create` currently requires a non-NULL `key_pair`. For join_channel, the joiner doesn't have the channel's key pair — the channel identity comes from the existing network. Modify `channel.c` `poseidon_channel_create` to accept NULL key_pair (generate one if NULL):

In `src/Channel/channel.c`, change the key_pair NULL check:
```c
// Was: if (key_pair == NULL) return NULL;
// Now: generate a key pair if none provided
if (key_pair == NULL) {
    key_pair = poseidon_key_pair_create("ED25519");
    if (key_pair == NULL) return NULL;
}
```

And track whether we own the key pair by adding a flag to `poseidon_channel_t`:

In `channel.h`, add to the struct:
```c
    bool owns_key_pair;       /**< True if channel allocated its own key pair */
```

In `channel.c`, set the flag:
```c
channel->owns_key_pair = (original_key_pair == NULL);
```

And in `poseidon_channel_destroy`, destroy the key_pair only if we own it:
```c
if (channel->owns_key_pair && channel->key_pair != NULL) {
    poseidon_key_pair_destroy(channel->key_pair);
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && make channel_bootstrap_test && ./tests/channel_bootstrap_test`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/Channel/channel_manager.h src/Channel/channel_manager.c src/Channel/channel.h src/Channel/channel.c tests/channel_bootstrap_test.cpp
git commit -m "feat: rewrite join_channel with bootstrap protocol and pending tracking"
```

---

### Task 3: Wire bootstrap packet dispatch in meridian_protocol

**Files:**
- Modify: `src/Network/Meridian/meridian_protocol.c` (add case 40/41 in packet dispatch)
- Modify: `src/Network/Meridian/meridian_protocol.h` (add channel_manager pointer)
- Test: `tests/channel_bootstrap_test.cpp` (add dispatch integration test)

- [ ] **Step 1: Add channel_manager pointer to meridian_protocol_t**

In `meridian_protocol.h`, add to the `meridian_protocol_t` struct:

```c
    struct poseidon_channel_manager_t* channel_manager;  /**< Set by daemon for bootstrap dispatch */
```

This lets the protocol's packet handler forward bootstrap packets to the channel manager.

- [ ] **Step 2: Add bootstrap dispatch in meridian_protocol_on_packet**

In `meridian_protocol.c`, in the `meridian_protocol_on_packet` function (or wherever incoming packets are dispatched based on type), add cases for types 40 and 41:

```c
    case MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP: {
        if (protocol->channel_manager != NULL) {
            char topic_id[64], sender_node_id[64];
            uint64_t ts;
            if (meridian_channel_bootstrap_decode(decoded, topic_id, sizeof(topic_id),
                                                   sender_node_id, sizeof(sender_node_id), &ts) == 0) {
                poseidon_channel_manager_handle_bootstrap_request(
                    protocol->channel_manager, topic_id, sender_node_id);
            }
        }
        break;
    }
    case MERIDIAN_PACKET_TYPE_CHANNEL_BOOTSTRAP_REPLY: {
        if (protocol->channel_manager != NULL) {
            char topic_id[64], responder_node_id[64];
            uint32_t addr; uint16_t port; uint64_t ts;
            uint32_t seed_addrs[16]; uint16_t seed_ports[16]; size_t num_seeds;
            if (meridian_channel_bootstrap_reply_decode(decoded, topic_id, sizeof(topic_id),
                                                        responder_node_id, sizeof(responder_node_id),
                                                        &addr, &port, &ts,
                                                        seed_addrs, seed_ports, &num_seeds, 16) == 0) {
                poseidon_channel_manager_handle_bootstrap_reply(
                    protocol->channel_manager, topic_id, addr, port, ts,
                    seed_addrs, seed_ports, num_seeds);
            }
        }
        break;
    }
```

- [ ] **Step 3: Add the necessary include**

At the top of `meridian_protocol.c`:
```c
#include "Channel/channel_manager.h"
```

- [ ] **Step 4: Run existing tests to verify no regressions**

Run: `cd build && make && ctest --output-on-failure`
Expected: All existing tests pass (meridian_packet_test, channel_test, etc.)

- [ ] **Step 5: Commit**

```bash
git add src/Network/Meridian/meridian_protocol.h src/Network/Meridian/meridian_protocol.c
git commit -m "feat: wire bootstrap packet dispatch in meridian protocol handler"
```

---

### Task 4: De-wonk audit

- [ ] **Step 1: Read all modified files and run de-wonk**

Read:
- `src/Network/Meridian/meridian_packet.h` (bootstrap section)
- `src/Network/Meridian/meridian_packet.c` (bootstrap encode/decode)
- `src/Channel/channel_manager.h` (pending bootstrap struct)
- `src/Channel/channel_manager.c` (join_channel rewrite, reply handler)
- `src/Channel/channel.h` (owns_key_pair flag)
- `src/Channel/channel.c` (NULL key_pair handling)
- `src/Network/Meridian/meridian_protocol.c` (dispatch cases)
- `tests/channel_bootstrap_test.cpp`

Audit for: unimplemented code, stubbed returns, memory leaks (CBOR items not freed), missing error handling, use-after-free.

Known issues to verify:
1. In `join_channel`, the CBOR serialization uses `cbor_serialize_alloc` — verify `free(buf)` is called
2. In `handle_bootstrap_reply`, the channel protocol pointer may be NULL if channel hasn't been started yet
3. `pending_bootstrap_t.channel` is a raw pointer — verify it's not used after channel destruction

- [ ] **Step 2: Fix any CRITICAL or HIGH issues found**

- [ ] **Step 3: Run all tests**

Run: `cd build && make && ctest --output-on-failure`
Expected: All tests pass

- [ ] **Step 4: Run memory leak check**

Build with AddressSanitizer and run the bootstrap tests:

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make channel_bootstrap_test
./tests/channel_bootstrap_test
```

Also run under valgrind if available:

```bash
valgrind --leak-check=full --error-exitcode=1 ./tests/channel_bootstrap_test
```

Verify: Zero leaks reported. If leaks found, trace and fix before proceeding.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk bootstrap protocol implementation"
```