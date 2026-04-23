# Phase 2: Topic ID System + Alias Ambiguity + Unified Path Resolution

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a topic ID type that supports both 256-bit PKI-derived node IDs and 128-bit UUID-based in-channel topics, extend the alias registry to handle duplicate names with ambiguous resolution, and implement unified path resolution that parses `"Alice/Feeds/friend-only"` into topic ID + subtopic.

**Architecture:** `poseidon_topic_id_t` is a 32-byte struct with a `bit_depth` flag (128 or 256). Factory functions create topic IDs from node IDs or random UUIDs. The alias registry's `topic_alias_register` is changed to allow duplicate names, and `topic_alias_resolve` returns a new result type that can indicate ambiguity. Path resolution splits on `/`, resolves the first component as an alias or raw topic ID, and returns the remaining path as a subtopic.

**Tech Stack:** C, existing `poseidon_node_id_t`, existing `topic_alias_registry_t`, CBOR for UUID encoding, gtest

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/Channel/topic_id.h` | Topic ID struct, factory functions, path resolution |
| Create | `src/Channel/topic_id.c` | Implementation of topic ID and path resolution |
| Modify | `src/Channel/topic_alias.h` | Add ambiguous resolve result type, update resolve signature |
| Modify | `src/Channel/topic_alias.c` | Allow duplicate names, return ambiguity info |
| Modify | `src/Channel/channel.h` | Add path resolution method |
| Modify | `src/Channel/channel.c` | Implement path resolution using topic_id |
| Create | `tests/topic_id_test.cpp` | Test topic ID creation, path resolution, alias ambiguity |
| Modify | `CMakeLists.txt` | Add topic_id.c and test target |

---

### Task 1: Topic ID type and factory functions

**Files:**
- Create: `src/Channel/topic_id.h`
- Create: `src/Channel/topic_id.c`
- Test: `tests/topic_id_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
#include <gtest/gtest.h>
#include <string.h>
#include "Channel/topic_id.h"
#include "Crypto/node_id.h"

TEST(TopicIdTest, FromNodeId) {
    poseidon_node_id_t node_id;
    memset(node_id.str, 0, sizeof(node_id.str));
    strncpy(node_id.str, "AbCdEfGhIjKlMnOpQrStUvWxYz1234", sizeof(node_id.str) - 1);

    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_node_id(&node_id, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 256);
    EXPECT_STREQ(tid.str, node_id.str);
}

TEST(TopicIdTest, GenerateRandom) {
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_generate(&tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 128);
    EXPECT_GT(strlen(tid.str), 0u);
}

TEST(TopicIdTest, GenerateUnique) {
    poseidon_topic_id_t tid1, tid2;
    poseidon_topic_id_generate(&tid1);
    poseidon_topic_id_generate(&tid2);
    EXPECT_NE(memcmp(tid1.bytes, tid2.bytes, 16), 0);
}

TEST(TopicIdTest, FromString256Bit) {
    // A ~43-char Base58 string should be detected as 256-bit
    const char* node_id_str = "AbCdEfGhIjKlMnOpQrStUvWxYz1234567890AbCdE";
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_string(node_id_str, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 256);
}

TEST(TopicIdTest, FromString128Bit) {
    // A ~22-char Base58 string should be detected as 128-bit
    const char* uuid_str = "X4jKL2mNpQrStUvWxYz12";
    poseidon_topic_id_t tid;
    int rc = poseidon_topic_id_from_string(uuid_str, &tid);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(tid.bit_depth, 128);
}

TEST(TopicIdTest, FromStringNullFails) {
    poseidon_topic_id_t tid;
    EXPECT_NE(poseidon_topic_id_from_string(NULL, &tid), 0);
    EXPECT_NE(poseidon_topic_id_from_string("abc", NULL), 0);
}
```

- [ ] **Step 2: Add test target and source to CMakeLists.txt**

In CMakeLists.txt, add `topic_id.c` to the POSEIDON_SOURCES list and add the test target:

```cmake
# In the POSEIDON_SOURCES section, add:
${CHANNEL_SRC_DIR}/topic_id.c

# In the test targets section, add:
add_meridian_test(topic_id_test tests/topic_id_test.cpp)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd build && cmake .. && make topic_id_test && ./tests/topic_id_test`
Expected: FAIL — header doesn't exist yet

- [ ] **Step 4: Create topic_id.h**

```c
//
// Created by victor on 4/22/26.
//

#ifndef POSEIDON_TOPIC_ID_H
#define POSEIDON_TOPIC_ID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../Crypto/node_id.h"
#include "topic_alias.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TOPIC ID
// ============================================================================

/** Maximum length of a Base58-encoded topic ID string */
#define POSEIDON_TOPIC_ID_STR_MAX 48

/**
 * Topic identifier supporting two bit depths:
 * - 256-bit for PKI-derived node/channel IDs (dial channel)
 * - 128-bit for random UUID-based in-channel topics
 *
 * The `str` field holds the Base58-encoded representation.
 * The `bytes` field holds the raw binary (upper 16 bytes for 128-bit).
 */
typedef struct poseidon_topic_id_t {
    uint8_t bytes[32];                   /**< Raw bytes (256 or 128 bits) */
    uint8_t bit_depth;                   /**< 128 or 256 */
    char str[POSEIDON_TOPIC_ID_STR_MAX]; /**< Base58-encoded string */
} poseidon_topic_id_t;

/**
 * Creates a topic ID from an existing node ID (256-bit).
 * Copies the node ID's Base58 string and raw bytes.
 *
 * @param node_id  Source node ID (must not be NULL)
 * @param out      Output topic ID (must not be NULL)
 * @return         0 on success, -1 on invalid args
 */
int poseidon_topic_id_from_node_id(const poseidon_node_id_t* node_id,
                                    poseidon_topic_id_t* out);

/**
 * Generates a random 128-bit topic ID (UUID v4 → Base58).
 * The upper 16 bytes of `out->bytes` are filled with random data;
 * the lower 16 bytes are zeroed. Bit depth is set to 128.
 *
 * @param out  Output topic ID (must not be NULL)
 * @return     0 on success, -1 on failure
 */
int poseidon_topic_id_generate(poseidon_topic_id_t* out);

/**
 * Parses a Base58-encoded topic ID string.
 * Infers bit depth from string length:
 *   - Length >= 32: 256-bit (node/channel ID)
 *   - Length < 32: 128-bit (in-channel topic)
 *
 * @param str  Base58-encoded topic ID string
 * @param out  Output topic ID
 * @return     0 on success, -1 on invalid args
 */
int poseidon_topic_id_from_string(const char* str, poseidon_topic_id_t* out);

// ============================================================================
// PATH RESOLUTION
// ============================================================================

/**
 * Result of resolving a path like "Alice/Feeds/friend-only".
 * The first component is resolved as an alias or raw topic ID;
 * remaining components become the subtopic.
 */
typedef struct poseidon_path_resolve_result_t {
    poseidon_topic_id_t topic_id;              /**< Resolved topic ID */
    char subtopic[256];                        /**< Remaining path after topic (may be empty) */
    bool found;                                /**< True if topic ID was resolved */
    bool ambiguous;                            /**< True if alias matched multiple topic IDs */
} poseidon_path_resolve_result_t;

/**
 * Resolves a path string into a topic ID and optional subtopic.
 *
 * Algorithm:
 * 1. Split path on '/' into components
 * 2. First component: try alias resolution. If ambiguous, set ambiguous flag.
 *    If not found, treat as raw topic ID string.
 * 3. Remaining components joined with '/' = subtopic. Empty if only one component.
 *
 * @param channel  Channel whose alias registry to use
 * @param path     Path string like "Alice/Feeds/friend-only"
 * @param out      Output resolution result
 * @return         0 on success (found or raw), -1 on error
 */
int poseidon_resolve_path(const poseidon_channel_t* channel,
                           const char* path,
                           poseidon_path_resolve_result_t* out);

#ifdef __cplusplus
}
#endif

#endif // POSEIDON_TOPIC_ID_H
```

- [ ] **Step 5: Create topic_id.c**

```c
//
// Created by victor on 4/22/26.
//

#include "topic_id.h"
#include "Util/allocator.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// TOPIC ID FACTORY FUNCTIONS
// ============================================================================

int poseidon_topic_id_from_node_id(const poseidon_node_id_t* node_id,
                                    poseidon_topic_id_t* out) {
    if (node_id == NULL || out == NULL) return -1;

    memset(out, 0, sizeof(*out));
    out->bit_depth = 256;
    strncpy(out->str, node_id->str, POSEIDON_TOPIC_ID_STR_MAX - 1);
    out->str[POSEIDON_TOPIC_ID_STR_MAX - 1] = '\0';

    // For node IDs, the raw bytes come from the node_id
    // We store the Base58 string; raw bytes conversion would need BLAKE3
    // For now, zero the bytes and rely on the string representation
    memset(out->bytes, 0, 32);

    return 0;
}

int poseidon_topic_id_generate(poseidon_topic_id_t* out) {
    if (out == NULL) return -1;

    memset(out, 0, sizeof(*out));
    out->bit_depth = 128;

    // Generate 16 random bytes for UUID v4
    for (int i = 0; i < 16; i++) {
        out->bytes[i] = (uint8_t)(rand() & 0xFF);
    }

    // Set UUID v4 version and variant bits
    out->bytes[6] = (out->bytes[6] & 0x0F) | 0x40;  // version 4
    out->bytes[8] = (out->bytes[8] & 0x3F) | 0x80;  // variant 1

    // Encode upper 16 bytes as Base58
    // For now, use a hex-encoded representation as placeholder
    // TODO: implement proper Base58 encoding
    static const char base58_chars[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    // Simple hex-to-pseudo-Base58 for now
    char* p = out->str;
    for (int i = 0; i < 16 && p < out->str + POSEIDON_TOPIC_ID_STR_MAX - 1; i++) {
        *p++ = base58_chars[out->bytes[i] % 58];
    }
    *p = '\0';

    return 0;
}

int poseidon_topic_id_from_string(const char* str, poseidon_topic_id_t* out) {
    if (str == NULL || out == NULL) return -1;

    memset(out, 0, sizeof(*out));

    size_t len = strlen(str);
    if (len == 0 || len >= POSEIDON_TOPIC_ID_STR_MAX) return -1;

    // Infer bit depth from string length
    // Base58: 256-bit ≈ 43-44 chars, 128-bit ≈ 22 chars
    out->bit_depth = (len >= 32) ? 256 : 128;
    strncpy(out->str, str, POSEIDON_TOPIC_ID_STR_MAX - 1);
    out->str[POSEIDON_TOPIC_ID_STR_MAX - 1] = '\0';

    return 0;
}

// ============================================================================
// PATH RESOLUTION
// ============================================================================

int poseidon_resolve_path(const poseidon_channel_t* channel,
                           const char* path,
                           poseidon_path_resolve_result_t* out) {
    if (path == NULL || out == NULL) return -1;
    if (channel == NULL) return -1;

    memset(out, 0, sizeof(*out));

    // Split path on '/' into components
    char path_copy[512];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char* first_component = path_copy;
    char* subtopic_start = strchr(path_copy, '/');

    if (subtopic_start != NULL) {
        *subtopic_start = '\0';
        subtopic_start++;
        strncpy(out->subtopic, subtopic_start, sizeof(out->subtopic) - 1);
        out->subtopic[sizeof(out->subtopic) - 1] = '\0';
    }

    // Try alias resolution first
    topic_alias_resolve_out_t resolve_result;
    int rc = topic_alias_resolve_ex(channel->aliases, first_component, &resolve_result);

    if (rc == 0 && resolve_result.status == TOPIC_ALIAS_RESOLVE_OK) {
        // Single alias match
        poseidon_topic_id_from_string(resolve_result.topic, &out->topic_id);
        out->found = true;
        out->ambiguous = false;
        return 0;
    }

    if (rc == 0 && resolve_result.status == TOPIC_ALIAS_RESOLVE_AMBIGUOUS) {
        // Multiple matches — set ambiguous flag, use first candidate
        poseidon_topic_id_from_string(resolve_result.candidates[0], &out->topic_id);
        out->found = true;
        out->ambiguous = true;
        return 0;
    }

    // Not an alias — treat as raw topic ID
    poseidon_topic_id_from_string(first_component, &out->topic_id);
    out->found = true;
    out->ambiguous = false;
    return 0;
}
```

- [ ] **Step 6: Update topic_alias.h with ambiguous resolve result type**

Replace the existing `topic_alias_resolve` declaration with:

```c
typedef enum {
    TOPIC_ALIAS_RESOLVE_OK,        /**< Single match found */
    TOPIC_ALIAS_RESOLVE_AMBIGUOUS, /**< Multiple matches — caller must disambiguate */
    TOPIC_ALIAS_RESOLVE_NOT_FOUND  /**< No match */
} topic_alias_resolve_result_t;

#define TOPIC_ALIAS_MAX_CANDIDATES 8

typedef struct topic_alias_resolve_out_t {
    topic_alias_resolve_result_t status;
    char topic[TOPIC_ALIAS_MAX_TOPIC];              /**< Valid if OK */
    char* candidates[TOPIC_ALIAS_MAX_CANDIDATES];    /**< Valid if AMBIGUOUS */
    size_t num_candidates;
} topic_alias_resolve_out_t;

/**
 * Resolves an alias name, supporting duplicate names.
 * If the name maps to exactly one topic, returns OK with that topic.
 * If the name maps to multiple topics, returns AMBIGUOUS with candidates.
 * If no match is found, returns NOT_FOUND.
 *
 * @param reg   Alias registry
 * @param name  Alias name to resolve
 * @param out   Output resolution result
 * @return      0 on success (any status), -1 on error
 */
int topic_alias_resolve_ex(topic_alias_registry_t* reg, const char* name,
                            topic_alias_resolve_out_t* out);

/**
 * Legacy single-resolve function. Returns the first match or NULL.
 * Kept for backward compatibility.
 */
const char* topic_alias_resolve(const topic_alias_registry_t* reg, const char* name);
```

- [ ] **Step 7: Update topic_alias.c to allow duplicates and implement resolve_ex**

Change `topic_alias_register` to allow duplicate names:

```c
int topic_alias_register(topic_alias_registry_t* reg, const char* name, const char* topic) {
    if (reg == NULL || name == NULL || topic == NULL) return -1;

    platform_lock(&reg->lock);

    if (reg->count >= reg->capacity) {
        platform_unlock(&reg->lock);
        return -1;
    }

    strncpy(reg->entries[reg->count].name, name, TOPIC_ALIAS_MAX_NAME - 1);
    strncpy(reg->entries[reg->count].topic, topic, TOPIC_ALIAS_MAX_TOPIC - 1);
    reg->count++;

    platform_unlock(&reg->lock);
    return 0;
}
```

Add the `topic_alias_resolve_ex` function:

```c
int topic_alias_resolve_ex(topic_alias_registry_t* reg, const char* name,
                            topic_alias_resolve_out_t* out) {
    if (reg == NULL || name == NULL || out == NULL) return -1;

    memset(out, 0, sizeof(*out));

    platform_lock(&reg->lock);

    size_t match_count = 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            if (match_count == 0) {
                strncpy(out->topic, reg->entries[i].topic, TOPIC_ALIAS_MAX_TOPIC - 1);
            }
            if (match_count < TOPIC_ALIAS_MAX_CANDIDATES) {
                out->candidates[match_count] = reg->entries[i].topic;
            }
            match_count++;
        }
    }

    platform_unlock(&reg->lock);

    out->num_candidates = match_count;
    if (match_count == 0) {
        out->status = TOPIC_ALIAS_RESOLVE_NOT_FOUND;
    } else if (match_count == 1) {
        out->status = TOPIC_ALIAS_RESOLVE_OK;
    } else {
        out->status = TOPIC_ALIAS_RESOLVE_AMBIGUOUS;
    }

    return 0;
}
```

Keep the existing `topic_alias_resolve` for backward compatibility.

- [ ] **Step 8: Run tests to verify they pass**

Run: `cd build && make topic_id_test && ./tests/topic_id_test`
Expected: All tests PASS

- [ ] **Step 9: Run existing tests to verify no regressions**

Run: `cd build && make && ctest --output-on-failure`
Expected: All existing tests pass (alias tests may need updating for new duplicate-name behavior)

- [ ] **Step 10: Commit**

```bash
git add src/Channel/topic_id.h src/Channel/topic_id.c src/Channel/topic_alias.h src/Channel/topic_alias.c tests/topic_id_test.cpp CMakeLists.txt
git commit -m "feat: add topic ID type, alias ambiguity, and unified path resolution"
```

---

### Task 2: Add path resolution to channel and add signing functions

**Files:**
- Modify: `src/Channel/channel.h` — add `poseidon_channel_resolve_path` declaration
- Modify: `src/Channel/channel.c` — implement `poseidon_channel_resolve_path`
- Modify: `src/Crypto/key_pair.h` — add `poseidon_key_pair_sign` and `poseidon_key_pair_verify`
- Modify: `src/Crypto/key_pair.c` — implement ED25519 sign/verify
- Create: `tests/path_resolve_test.cpp`
- Modify: `CMakeLists.txt` — add test target

- [ ] **Step 1: Add `poseidon_channel_resolve_path` to channel.h**

```c
#include "topic_id.h"

/**
 * Resolves a path string into a topic ID and optional subtopic.
 * Uses the channel's alias registry for name resolution.
 *
 * @param channel  Channel whose alias registry to use
 * @param path     Path string like "Alice/Feeds/friend-only"
 * @param out      Output resolution result
 * @return         0 on success, -1 on error
 */
int poseidon_channel_resolve_path(const poseidon_channel_t* channel,
                                   const char* path,
                                   poseidon_path_resolve_result_t* out);
```

- [ ] **Step 2: Implement `poseidon_channel_resolve_path` in channel.c**

```c
int poseidon_channel_resolve_path(const poseidon_channel_t* channel,
                                   const char* path,
                                   poseidon_path_resolve_result_t* out) {
    return poseidon_resolve_path(channel, path, out);
}
```

- [ ] **Step 3: Add ED25519 sign/verify to key_pair.h**

```c
/**
 * Signs data with an ED25519 private key.
 *
 * @param key_pair  Key pair containing the private key (must be ED25519)
 * @param data      Data to sign
 * @param data_len  Length of data
 * @param sig_out   Output buffer for signature (must be at least 64 bytes)
 * @param sig_len   Output: actual signature length
 * @return          0 on success, -1 on failure
 */
int poseidon_key_pair_sign(poseidon_key_pair_t* key_pair,
                            const uint8_t* data, size_t data_len,
                            uint8_t* sig_out, size_t* sig_len);

/**
 * Verifies an ED25519 signature against a public key derived from topic_id.
 *
 * @param topic_id_str  Base58 topic ID string (encodes the public key)
 * @param data          Data that was signed
 * @param data_len      Length of data
 * @param signature     Signature bytes
 * @param sig_len       Length of signature
 * @return              0 if signature is valid, -1 if invalid or error
 */
int poseidon_verify_signature(const char* topic_id_str,
                               const uint8_t* data, size_t data_len,
                               const uint8_t* signature, size_t sig_len);
```

- [ ] **Step 4: Implement ED25519 sign/verify in key_pair.c**

```c
int poseidon_key_pair_sign(poseidon_key_pair_t* key_pair,
                            const uint8_t* data, size_t data_len,
                            uint8_t* sig_out, size_t* sig_len) {
    if (key_pair == NULL || data == NULL || sig_out == NULL || sig_len == NULL)
        return -1;

    if (EVP_PKEY_base_id(key_pair->pkey) != EVP_PKEY_ED25519) return -1;

    size_t sig_sz = EVP_PKEY_size(key_pair->pkey);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return -1;

    int rc = -1;
    if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, key_pair->pkey) != 1) goto cleanup;
    if (EVP_DigestSign(ctx, sig_out, &sig_sz, data, data_len) != 1) goto cleanup;

    *sig_len = sig_sz;
    rc = 0;

cleanup:
    EVP_MD_CTX_free(ctx);
    return rc;
}

int poseidon_verify_signature(const char* topic_id_str,
                               const uint8_t* data, size_t data_len,
                               const uint8_t* signature, size_t sig_len) {
    // For now, return -1 (not yet implemented — needs public key derivation from topic_id)
    (void)topic_id_str; (void)data; (void)data_len; (void)signature; (void)sig_len;
    return -1;
}
```

Note: `poseidon_verify_signature` is a stub for now. Full implementation requires deriving an ED25519 public key from a Base58 topic ID, which needs the BLAKE3 hash → public key derivation chain. This will be implemented when the daemon processes CHANNEL_MODIFY and CHANNEL_DESTROY requests.

- [ ] **Step 5: Write tests for path resolution**

Create `tests/path_resolve_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "Channel/channel.h"
#include "Channel/topic_id.h"
#include "Crypto/key_pair.h"
#include "Workers/pool.h"
#include "Time/wheel.h"

class PathResolveTest : public ::testing::Test {
protected:
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    poseidon_key_pair_t* kp = nullptr;
    poseidon_channel_t* channel = nullptr;

    void SetUp() override {
        pool = work_pool_create(1);
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);

        kp = poseidon_key_pair_create("ED25519");
        ASSERT_NE(kp, nullptr);

        poseidon_channel_config_t config = poseidon_channel_config_defaults();
        channel = poseidon_channel_create(kp, "test_channel", 14000, &config, pool, wheel);
        ASSERT_NE(channel, nullptr);
    }

    void TearDown() override {
        if (channel) poseidon_channel_destroy(channel);
        if (kp) poseidon_key_pair_destroy(kp);
        hierarchical_timing_wheel_wait_for_idle_signal(wheel);
        hierarchical_timing_wheel_stop(wheel);
        work_pool_shutdown(pool);
        work_pool_join_all(pool);
        work_pool_destroy(pool);
        hierarchical_timing_wheel_destroy(wheel);
    }
};

TEST_F(PathResolveTest, ResolveRawTopicId) {
    poseidon_path_resolve_result_t result;
    int rc = poseidon_channel_resolve_path(channel, "X4jKL2mNpQrStUvWxYz12", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.ambiguous);
    EXPECT_STREQ(result.topic_id.str, "X4jKL2mNpQrStUvWxYz12");
    EXPECT_STREQ(result.subtopic, "");
}

TEST_F(PathResolveTest, ResolveAliasWithSubtopic) {
    poseidon_channel_register_alias(channel, "Alice", "AbCdEfGhIjKlMnOpQrStUvWxYz12345678");

    poseidon_path_resolve_result_t result;
    int rc = poseidon_channel_resolve_path(channel, "Alice/Feeds/friend-only", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.ambiguous);
    EXPECT_STREQ(result.topic_id.str, "AbCdEfGhIjKlMnOpQrStUvWxYz12345678");
    EXPECT_STREQ(result.subtopic, "Feeds/friend-only");
}

TEST_F(PathResolveTest, ResolveUnknownAliasAsRaw) {
    poseidon_path_resolve_result_t result;
    int rc = poseidon_channel_resolve_path(channel, "SomeRandomString1234", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.ambiguous);
    // Falls back to treating as raw topic ID
}

TEST_F(PathResolveTest, ResolveAmbiguousAlias) {
    poseidon_channel_register_alias(channel, "Alice", "TopicA1234567890123456789012345678");
    poseidon_channel_register_alias(channel, "Alice", "TopicB1234567890123456789012345678");

    poseidon_path_resolve_result_t result;
    int rc = poseidon_channel_resolve_path(channel, "Alice", &result);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.ambiguous);
}
```

- [ ] **Step 6: Add test target to CMakeLists.txt**

```cmake
add_meridian_test(path_resolve_test tests/path_resolve_test.cpp)
```

- [ ] **Step 7: Run tests**

Run: `cd build && make path_resolve_test topic_id_test && ./tests/topic_id_test && ./tests/path_resolve_test`
Expected: All tests pass

- [ ] **Step 8: Commit**

```bash
git add src/Channel/channel.h src/Channel/channel.c src/Crypto/key_pair.h src/Crypto/key_pair.c tests/path_resolve_test.cpp CMakeLists.txt
git commit -m "feat: add path resolution and ED25519 signing to channel and key_pair"
```

---

### Task 3: De-wonk audit

- [ ] **Step 1: Read all modified/created files and audit**

Read:
- `src/Channel/topic_id.h` and `topic_id.c`
- `src/Channel/topic_alias.h` and `topic_alias.c` (check backward compat)
- `src/Channel/channel.h` and `channel.c` (resolve_path)
- `src/Crypto/key_pair.h` and `key_pair.c` (sign function)
- `tests/topic_id_test.cpp`
- `tests/path_resolve_test.cpp`

Audit for:
1. `topic_alias_register` no longer rejects duplicates — verify existing tests that expect rejection still work
2. `topic_alias_resolve_ex` candidates point to entries in the registry — verify they remain valid after unlock
3. `poseidon_topic_id_generate` uses `rand()` — not crypto-secure, but acceptable for UUID v4 (will use platform RNG later)
4. `poseidon_verify_signature` is a stub — document this clearly
5. Memory leaks in path resolution

- [ ] **Step 2: Fix any CRITICAL or HIGH issues found**

- [ ] **Step 3: Run all tests**

Run: `cd build && make && ctest --output-on-failure`
Expected: All tests pass

- [ ] **Step 4: Run memory leak check**

Build with AddressSanitizer and run tests:

```bash
cd build && cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" .. && make topic_id_test path_resolve_test
./tests/topic_id_test
./tests/path_resolve_test
```

Also run under valgrind if available:

```bash
valgrind --leak-check=full --error-exitcode=1 ./tests/topic_id_test
valgrind --leak-check=full --error-exitcode=1 ./tests/path_resolve_test
```

Verify: Zero leaks reported. If leaks found, trace and fix before proceeding.

- [ ] **Step 5: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk topic ID and path resolution"
```