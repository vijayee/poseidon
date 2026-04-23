# Quasar Message ID and Dedup Filter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Use the de-wonk skill at the end of implementation to audit all changes.

**Goal:** Add a unique, serializable message ID to Quasar route messages and a per-node dedup bloom filter to prevent processing duplicate messages.

**Architecture:** New `quasar_message_id_t` module following WaveDB's `transaction_id_t` pattern — CLOCK_MONOTONIC timestamp + atomic counter for thread-safe generation, network byte order serialization for wire format. Dedup bloom filter on `quasar_t` checks message IDs before processing.

**Tech Stack:** C11 (`<stdatomic.h>`), POSIX `clock_gettime(CLOCK_MONOTONIC)`, `pthread_once`, existing `bloom_filter_t`

---

## File Structure

| File | Responsibility |
|------|---------------|
| Create: `src/Network/Quasar/quasar_message_id.h` | Type definition, API declarations |
| Create: `src/Network/Quasar/quasar_message_id.c` | ID generation, comparison, serialization |
| Modify: `src/Network/Quasar/quasar.h` | Add `id` field to route message, `seen` field to quasar_t, update create signatures |
| Modify: `src/Network/Quasar/quasar.c` | Wire up ID generation and dedup filter |
| Modify: `CMakeLists.txt` | Add `quasar_message_id.c` to build |
| Modify: `tests/quasar_test.cpp` | Add tests for message ID and dedup |

---

### Task 1: Create quasar_message_id.h

**Files:**
- Create: `src/Network/Quasar/quasar_message_id.h`

- [ ] **Step 1: Write the header file**

```c
//
// Created by victor on 4/20/26.
//

#ifndef POSEIDON_QUASAR_MESSAGE_ID_H
#define POSEIDON_QUASAR_MESSAGE_ID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// MESSAGE ID TYPE
// ============================================================================

/**
 * Unique identifier for Quasar route messages.
 * Provides total ordering and serialization for network transmission.
 * Follows WaveDB's transaction_id_t pattern: CLOCK_MONOTONIC timestamp
 * with atomic counter for same-timestamp uniqueness.
 */
typedef struct quasar_message_id_t {
    uint64_t time;    /**< Seconds from CLOCK_MONOTONIC */
    uint64_t nanos;   /**< Nanoseconds within second */
    uint64_t count;   /**< Atomic sequence counter for same-timestamp uniqueness */
} quasar_message_id_t;

/** Size in bytes of a serialized message ID */
#define QUASAR_MESSAGE_ID_SIZE 24

// ============================================================================
// INITIALIZATION
// ============================================================================

/**
 * Initializes the global message ID generator.
 * Must be called once before quasar_message_id_get_next().
 * Safe to call multiple times (uses pthread_once / InitOnceExecuteOnce).
 */
void quasar_message_id_init(void);

// ============================================================================
// GENERATION
// ============================================================================

/**
 * Generates a unique message ID.
 * Thread-safe, no lock needed. Uses CLOCK_MONOTONIC + atomic counter.
 *
 * @return  New unique message ID
 */
quasar_message_id_t quasar_message_id_get_next(void);

// ============================================================================
// COMPARISON
// ============================================================================

/**
 * Compares two message IDs lexicographically (time, nanos, count).
 *
 * @param a  First ID
 * @param b  Second ID
 * @return   1 if a > b, -1 if a < b, 0 if equal
 */
int quasar_message_id_compare(const quasar_message_id_t* a, const quasar_message_id_t* b);

// ============================================================================
// SERIALIZATION
// ============================================================================

/**
 * Serializes a message ID to 24 bytes in network byte order.
 *
 * @param id  Message ID to serialize
 * @param buf Output buffer (must have space for QUASAR_MESSAGE_ID_SIZE bytes)
 */
void quasar_message_id_serialize(const quasar_message_id_t* id, uint8_t* buf);

/**
 * Deserializes a message ID from 24 bytes in network byte order.
 *
 * @param id  Output: deserialized message ID
 * @param buf Input buffer (QUASAR_MESSAGE_ID_SIZE bytes)
 */
void quasar_message_id_deserialize(quasar_message_id_t* id, const uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_QUASAR_MESSAGE_ID_H
```

- [ ] **Step 2: Commit**

```bash
git add src/Network/Quasar/quasar_message_id.h
git commit -m "feat: add quasar_message_id.h header with type and API declarations"
```

---

### Task 2: Create quasar_message_id.c

**Files:**
- Create: `src/Network/Quasar/quasar_message_id.c`

- [ ] **Step 1: Write the implementation**

```c
//
// Created by victor on 4/20/26.
//

#include "quasar_message_id.h"
#include "../../Util/threadding.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>

// Global atomic counter for same-timestamp uniqueness
static atomic_uint_fast64_t g_message_id_count = ATOMIC_VAR_INIT(0);

// One-time initialization control
#if _WIN32
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
#endif

static void quasar_message_id_do_init(void) {
    // Counter is already initialized to 0 via ATOMIC_VAR_INIT
    // No additional setup needed
}

void quasar_message_id_init(void) {
#if _WIN32
    InitOnceExecuteOnce(&g_init_once, quasar_message_id_do_init, NULL, NULL);
#else
    pthread_once(&g_init_once, quasar_message_id_do_init);
#endif
}

quasar_message_id_t quasar_message_id_get_next(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t count = atomic_fetch_add(&g_message_id_count, 1);

    quasar_message_id_t next = {
        .time = (uint64_t)ts.tv_sec,
        .nanos = (uint64_t)ts.tv_nsec,
        .count = count
    };

    return next;
}

int quasar_message_id_compare(const quasar_message_id_t* a, const quasar_message_id_t* b) {
    if (a->time > b->time) return 1;
    if (a->time < b->time) return -1;
    if (a->nanos > b->nanos) return 1;
    if (a->nanos < b->nanos) return -1;
    if (a->count > b->count) return 1;
    if (a->count < b->count) return -1;
    return 0;
}

static void write_uint64(uint8_t* buf, uint64_t val) {
    uint32_t high = htonl((uint32_t)(val >> 32));
    uint32_t low = htonl((uint32_t)(val & 0xFFFFFFFF));
    memcpy(buf, &high, sizeof(uint32_t));
    memcpy(buf + 4, &low, sizeof(uint32_t));
}

static uint64_t read_uint64(const uint8_t* buf) {
    uint32_t high, low;
    memcpy(&high, buf, sizeof(uint32_t));
    memcpy(&low, buf + 4, sizeof(uint32_t));
    return ((uint64_t)ntohl(high) << 32) | (uint64_t)ntohl(low);
}

void quasar_message_id_serialize(const quasar_message_id_t* id, uint8_t* buf) {
    write_uint64(buf, id->time);
    write_uint64(buf + 8, id->nanos);
    write_uint64(buf + 16, id->count);
}

void quasar_message_id_deserialize(quasar_message_id_t* id, const uint8_t* buf) {
    id->time = read_uint64(buf);
    id->nanos = read_uint64(buf + 8);
    id->count = read_uint64(buf + 16);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

In `CMakeLists.txt`, add the new source file after the existing quasar.c line. Find the line:

```
    ${QUASAR_SRC_DIR}/quasar.c
```

Add after it:

```
    ${QUASAR_SRC_DIR}/quasar_message_id.c
```

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --target poseidon`
Expected: Build succeeds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/Network/Quasar/quasar_message_id.c CMakeLists.txt
git commit -m "feat: add quasar_message_id.c with generation, comparison, serialization"
```

---

### Task 3: Write tests for quasar_message_id

**Files:**
- Modify: `tests/quasar_test.cpp`

- [ ] **Step 1: Add message ID test fixture and tests**

Add the following test fixture after the existing `OnRouteMessageTest` fixture in `tests/quasar_test.cpp`:

```cpp
// ============================================================================
// MESSAGE ID TESTS
// ============================================================================

class MessageIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};

TEST_F(MessageIdTest, GetNextReturnsNonZero) {
    quasar_message_id_t id = quasar_message_id_get_next();
    // Count should be 0 (first call) but time/nanos should be non-zero
    EXPECT_GT(id.time, 0u);
}

TEST_F(MessageIdTest, GetNextProducesUniqueIDs) {
    quasar_message_id_t id1 = quasar_message_id_get_next();
    quasar_message_id_t id2 = quasar_message_id_get_next();
    EXPECT_NE(0, quasar_message_id_compare(&id1, &id2));
}

TEST_F(MessageIdTest, CompareSameID) {
    quasar_message_id_t id = quasar_message_id_get_next();
    EXPECT_EQ(0, quasar_message_id_compare(&id, &id));
}

TEST_F(MessageIdTest, CompareOrdering) {
    quasar_message_id_t id1 = quasar_message_id_get_next();
    quasar_message_id_t id2 = quasar_message_id_get_next();
    EXPECT_EQ(-1, quasar_message_id_compare(&id1, &id2));
    EXPECT_EQ(1, quasar_message_id_compare(&id2, &id1));
}

TEST_F(MessageIdTest, SerializeDeserializeRoundTrip) {
    quasar_message_id_t id = quasar_message_id_get_next();
    uint8_t buf[QUASAR_MESSAGE_ID_SIZE];
    quasar_message_id_serialize(&id, buf);
    quasar_message_id_t id2;
    quasar_message_id_deserialize(&id2, buf);
    EXPECT_EQ(0, quasar_message_id_compare(&id, &id2));
}

TEST_F(MessageIdTest, SerializedFieldsAreNetworkByteOrder) {
    quasar_message_id_t id = {0x0102030405060708ull, 0x0A0B0C0D0E0F0102ull, 42};
    uint8_t buf[QUASAR_MESSAGE_ID_SIZE];
    quasar_message_id_serialize(&id, buf);
    // First byte should be 0x01 (most significant byte of time in big-endian)
    EXPECT_EQ(0x01, buf[0]);
    // Deserialize back and verify
    quasar_message_id_t id2;
    quasar_message_id_deserialize(&id2, buf);
    EXPECT_EQ(id.time, id2.time);
    EXPECT_EQ(id.nanos, id2.nanos);
    EXPECT_EQ(id.count, id2.count);
}
```

- [ ] **Step 2: Build and run tests**

Run: `cmake --build build --target quasar_test && ./build/quasar_test --gtest_filter="MessageIdTest.*"`
Expected: All 6 message ID tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/quasar_test.cpp
git commit -m "test: add quasar_message_id tests for generation, comparison, serialization"
```

---

### Task 4: Add message ID to quasar_route_message_t and dedup filter to quasar_t

**Files:**
- Modify: `src/Network/Quasar/quasar.h`

- [ ] **Step 1: Add include and update route message struct**

In `quasar.h`, add the message ID include after the bloom_filter include:

```c
#include "../../Bloom/bloom_filter.h"
```

Add after it:

```c
#include "quasar_message_id.h"
```

Add `quasar_message_id_t id` as the second field in `quasar_route_message_t` (after `refcounter_t refcounter`):

```c
typedef struct quasar_route_message_t {
    refcounter_t refcounter;
    quasar_message_id_t id;           /**< Unique message identifier */
    buffer_t* topic;
    // ... rest unchanged
```

- [ ] **Step 2: Add dedup filter fields to quasar_t**

In `quasar_t`, add `seen` filter and default constants after the `alpha` field:

```c
typedef struct quasar_t {
    // ... existing fields through alpha ...
    bloom_filter_t* seen;                    /**< Per-node dedup: message IDs already processed */
    uint32_t seen_size;                      /**< Dedup filter size in bits */
    uint32_t seen_hashes;                   /**< Dedup filter hash count */
    quasar_delivery_cb_t on_delivery;        /**< Called when a message is delivered locally */
    // ... rest unchanged
```

- [ ] **Step 3: Update quasar_create signature and declaration**

Replace the existing `quasar_create` declaration:

```c
quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha);
```

With:

```c
quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha,
                          uint32_t seen_size, uint32_t seen_hashes);
```

- [ ] **Step 4: Build to verify header changes compile**

Run: `cmake --build build --target poseidon`
Expected: Compilation errors in quasar.c due to signature change (will fix in Task 5).

- [ ] **Step 5: Commit**

```bash
git add src/Network/Quasar/quasar.h
git commit -m "feat: add message ID to route message and dedup filter to quasar_t"
```

---

### Task 5: Wire up quasar.c with message ID and dedup filter

**Files:**
- Modify: `src/Network/Quasar/quasar.c`

- [ ] **Step 1: Add dedup filter defaults and update quasar_create**

In `quasar.c`, add default constants after the existing defaults:

```c
/** Default size in bits for the dedup (seen) bloom filter */
#define QUASAR_DEFAULT_SEEN_SIZE 4096

/** Default number of hash functions for the dedup bloom filter */
#define QUASAR_DEFAULT_SEEN_HASHES 3
```

Update `quasar_create` to accept and initialize the dedup filter:

```c
quasar_t* quasar_create(struct meridian_protocol_t* protocol, uint32_t max_hops, uint32_t alpha,
                          uint32_t seen_size, uint32_t seen_hashes) {
    quasar_t* quasar = get_clear_memory(sizeof(quasar_t));
    quasar->protocol = protocol;
    quasar->routing = attenuated_bloom_filter_create(
        max_hops, QUASAR_DEFAULT_SIZE, QUASAR_DEFAULT_HASH_COUNT,
        QUASAR_DEFAULT_OMEGA, QUASAR_DEFAULT_FP_BITS
    );
    vec_init(&quasar->local_subs);
    quasar->max_hops = max_hops;
    quasar->alpha = alpha;
    quasar->seen = bloom_filter_create(seen_size, seen_hashes);
    quasar->seen_size = seen_size;
    quasar->seen_hashes = seen_hashes;
    quasar->on_delivery = NULL;
    quasar->delivery_ctx = NULL;
    platform_lock_init(&quasar->lock);
    refcounter_init((refcounter_t*)quasar);
    return quasar;
}
```

- [ ] **Step 2: Free dedup filter in quasar_destroy**

In `quasar_destroy`, add `bloom_filter_destroy(quasar->seen)` before `platform_lock_destroy`:

```c
void quasar_destroy(quasar_t* quasar) {
    if (quasar == NULL) return;
    refcounter_dereference((refcounter_t*)quasar);
    if (refcounter_count((refcounter_t*)quasar) == 0) {
        attenuated_bloom_filter_destroy(quasar->routing);
        bloom_filter_destroy(quasar->seen);
        // ... rest unchanged
```

- [ ] **Step 3: Update quasar_route_message_create to generate ID**

Add `quasar_message_id_init()` call at the top of `quasar_route_message_create` and auto-generate the ID:

```c
quasar_route_message_t* quasar_route_message_create(const uint8_t* topic, size_t topic_len,
                                                      const uint8_t* data, size_t data_len,
                                                      uint32_t max_hops,
                                                      size_t neg_size, uint32_t neg_hashes) {
    if (topic == NULL || data == NULL) return NULL;
    quasar_message_id_init();
    quasar_route_message_t* msg = get_clear_memory(sizeof(quasar_route_message_t));
    msg->id = quasar_message_id_get_next();
    msg->topic = buffer_create_from_pointer_copy((uint8_t*)topic, topic_len);
    msg->data = buffer_create_from_pointer_copy((uint8_t*)data, data_len);
    msg->visited = bloom_filter_create(neg_size, neg_hashes);
    msg->hops_remaining = max_hops;
    platform_lock_init(&msg->lock);
    refcounter_init((refcounter_t*)msg);
    return msg;
}
```

- [ ] **Step 4: Add dedup check to quasar_on_route_message**

At the start of `quasar_on_route_message`, after the NULL checks, add dedup check:

```c
int quasar_on_route_message(quasar_t* quasar, quasar_route_message_t* msg, const struct meridian_node_t* from) {
    if (quasar == NULL || msg == NULL) return -1;

    // Dedup check: discard if we've already seen this message
    platform_lock(&quasar->lock);
    if (bloom_filter_contains(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id))) {
        platform_unlock(&quasar->lock);
        return 0;
    }
    bloom_filter_add(quasar->seen, (const uint8_t*)&msg->id, sizeof(msg->id));
    platform_unlock(&quasar->lock);

    // ... rest of function unchanged
```

- [ ] **Step 5: Add dedup for local delivery in quasar_publish**

In `quasar_publish`, when doing local delivery (`hops == 0`), also add the message ID to the seen filter. Update the `hops == 0` block:

```c
    if (found && hops == 0) {
        // Local delivery: this node is subscribed to the topic
        // Generate a message ID for dedup tracking
        quasar_message_id_t msg_id = quasar_message_id_get_next();
        bloom_filter_add(quasar->seen, (const uint8_t*)&msg_id, sizeof(msg_id));
        if (quasar->on_delivery != NULL) {
            quasar_delivery_cb_t cb = quasar->on_delivery;
            void* ctx = quasar->delivery_ctx;
            platform_unlock(&quasar->lock);
            cb(ctx, topic, topic_len, data, data_len);
            return 0;
        }
        platform_unlock(&quasar->lock);
        return 0;
    }
```

- [ ] **Step 6: Build and run all tests**

Run: `cmake --build build --target quasar_test && ./build/quasar_test`
Expected: Some existing tests may fail due to `quasar_create` signature change (fixed in Task 6).

- [ ] **Step 7: Commit**

```bash
git add src/Network/Quasar/quasar.c
git commit -m "feat: wire up message ID generation and dedup filter in quasar.c"
```

---

### Task 6: Fix test call sites for new quasar_create signature

**Files:**
- Modify: `tests/quasar_test.cpp`

- [ ] **Step 1: Update all quasar_create calls in tests**

Every call to `quasar_create(NULL, 5, 3)` in the test file must be updated to `quasar_create(NULL, 5, 3, 4096, 3)`. Find-and-replace all instances.

Also add `quasar_message_id_init()` to the `QuasarTest::SetUp()` method:

```cpp
class QuasarTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};
```

- [ ] **Step 2: Build and run all tests**

Run: `cmake --build build --target quasar_test && ./build/quasar_test`
Expected: All 29+ tests pass (original 23 + new message ID tests + dedup tests).

- [ ] **Step 3: Commit**

```bash
git add tests/quasar_test.cpp
git commit -m "test: update quasar_create call sites for dedup filter params"
```

---

### Task 7: Add dedup filter tests

**Files:**
- Modify: `tests/quasar_test.cpp`

- [ ] **Step 1: Add dedup filter tests**

Add after the existing `OnRouteMessageTest` fixture:

```cpp
// ============================================================================
// DEDUP FILTER TESTS
// ============================================================================

class DedupFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        quasar_message_id_init();
    }
    void TearDown() override {}
};

TEST_F(DedupFilterTest, DuplicateRouteMessageDiscarded) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_delivery_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Create a route message and deliver it — should trigger callback
    quasar_route_message_t* msg = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));
    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count);

    // Deliver the SAME message again — dedup filter should discard it
    EXPECT_EQ(0, quasar_on_route_message(q, msg, from));
    EXPECT_EQ(1, call_count); // Count unchanged — duplicate was discarded

    quasar_route_message_destroy(msg);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(DedupFilterTest, DifferentMessagesNotDiscarded) {
    quasar_t* q = quasar_create(NULL, 5, 3, 4096, 3);
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";

    int call_count = 0;
    quasar_set_delivery_callback(q, [](void* ctx, const uint8_t*, size_t,
                                        const uint8_t*, size_t) {
        int* count = (int*)ctx;
        (*count)++;
    }, &call_count);

    quasar_subscribe(q, topic, 6, 100);

    // Two different route messages — different IDs — should both be delivered
    quasar_route_message_t* msg1 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    quasar_route_message_t* msg2 = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    meridian_node_t* from = meridian_node_create(htonl(0x0A000001), htons(8080));

    EXPECT_EQ(0, quasar_on_route_message(q, msg1, from));
    EXPECT_EQ(1, call_count);

    EXPECT_EQ(0, quasar_on_route_message(q, msg2, from));
    EXPECT_EQ(2, call_count); // Both delivered — different IDs

    quasar_route_message_destroy(msg1);
    quasar_route_message_destroy(msg2);
    meridian_node_destroy(from);
    quasar_destroy(q);
}

TEST_F(DedupFilterTest, RouteMessageCarriesID) {
    const uint8_t* topic = (const uint8_t*)"sports";
    const uint8_t* data = (const uint8_t*)"goal!";
    quasar_route_message_t* msg = quasar_route_message_create(topic, 6, data, 5, 10, 256, 3);
    ASSERT_NE(nullptr, msg);

    // ID should have been auto-generated (time > 0)
    EXPECT_GT(msg->id.time, 0u);
    EXPECT_GT(msg->id.count + 1, 0u); // count is non-negative

    quasar_route_message_destroy(msg);
}
```

- [ ] **Step 2: Build and run all tests**

Run: `cmake --build build --target quasar_test && ./build/quasar_test`
Expected: All tests pass, including new dedup filter tests.

- [ ] **Step 3: Commit**

```bash
git add tests/quasar_test.cpp
git commit -m "test: add dedup filter tests for duplicate detection and message ID"
```

---

### Task 8: Run full test suite and de-wonk audit

**Files:** All modified files

- [ ] **Step 1: Build and run all bloom filter + quasar tests**

Run: `cmake --build build --target quasar_test bloom_filter_test elastic_bloom_filter_test attenuated_bloom_filter_test bloom_bitset_test && ./build/quasar_test && ./build/bloom_filter_test && ./build/elastic_bloom_filter_test && ./build/attenuated_bloom_filter_test && ./build/bloom_bitset_test`
Expected: All tests pass.

- [ ] **Step 2: Run de-wonk audit**

Use the de-wonk skill to audit all changed files for unimplemented, stubbed, disabled, broken, or weird issues. Fix any CRITICAL or HIGH issues found.

- [ ] **Step 3: Final commit if de-wonk found issues**

```bash
git add -A
git commit -m "fix: address de-wonk audit findings for message ID implementation"
```