# Phase 7: Android Binder Transport + Kotlin Client Library

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an Android Binder IPC transport to the daemon (conditionally compiled under `__ANDROID__`) and a Kotlin client library with coroutine-based + Java-compatible blocking APIs.

**Architecture:** The Binder transport is a thin C shim that bridges Android Binder callbacks to the `poseidon_transport_t` interface. On the Kotlin side, `PoseidonConnection` manages the Binder connection and CBOR serialization. `PoseidonClient` provides suspend-fun APIs; `PoseidonClientJava` wraps each with `runBlocking` for Java callers.

**Tech Stack:** C (with `#ifdef __ANDROID__`), Kotlin, Android SDK, Binder

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientAPIs/transport_binder.c` | Android Binder transport (conditioned on `__ANDROID__`) |
| Create | `src/client_libs/android/PoseidonConnection.kt` | Binder connection + CBOR frame handling |
| Create | `src/client_libs/android/PoseidonClient.kt` | Kotlin coroutine API |
| Create | `src/client_libs/android/PoseidonClientJava.kt` | Java-compatible blocking wrappers |

---

### Task 1: Android Binder transport (C side)

**Files:**
- Create: `src/ClientAPIs/transport_binder.c`
- Modify: `CMakeLists.txt` — conditional compilation

- [ ] **Step 1: Create transport_binder.c**

```c
#ifdef __ANDROID__

#include "transport.h"
// ... Android Binder includes and implementation

// The Binder transport creates an Android Service that clients bind to.
// On incoming Binder transactions, data is unpacked and forwarded to
// the on_message callback, which dispatches to client_session_handle_request.
// Responses are packed into Binder reply parcels.

poseidon_transport_t* poseidon_transport_binder_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    // Allocate transport, set type to POSEIDON_TRANSPORT_BINDER
    // start() registers the Binder service
    // stop() unregisters
    // send() writes to the Binder client's file descriptor
}

#endif // __ANDROID__
```

The full implementation is ~200 lines. Binder doesn't use poll-dancer — it has its own callback-driven API. The transport wrapper bridges those callbacks to the `on_message` callback.

- [ ] **Step 2: Add conditional compilation to CMakeLists.txt**

```cmake
if(ANDROID)
    list(APPEND POSEIDON_SOURCES ${CLIENT_APIS_SRC_DIR}/transport_binder.c)
    target_link_libraries(poseidon PRIVATE binder)
endif()
```

- [ ] **Step 3: Add create function declaration to transport.h**

```c
#ifdef __ANDROID__
poseidon_transport_t* poseidon_transport_binder_create(
    const char* service_name,
    poseidon_channel_manager_t* manager);
#endif
```

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPIs/transport_binder.c src/ClientAPIs/transport.h CMakeLists.txt
git commit -m "feat: add Android Binder transport (conditional on __ANDROID__)"
```

---

### Task 2: Kotlin client library

**Files:**
- Create: `src/client_libs/android/PoseidonConnection.kt`
- Create: `src/client_libs/android/PoseidonClient.kt`
- Create: `src/client_libs/android/PoseidonClientJava.kt`

- [ ] **Step 1: Create PoseidonConnection.kt**

Handles Binder connection to the daemon, CBOR frame serialization/deserialization, request ID tracking, and response matching.

```kotlin
package com.poseidon.client

import android.content.ComponentName
import android.content.Context
import android.os.IBinder
import android.os.Parcel
import android.util.Log
import org.jetbrains.anko.coroutines.experimental.bg

class PoseidonConnection(private val context: Context) {
    private var binder: IPoseidonService? = null
    private var requestId = 0
    private val pendingRequests = mutableMapOf<Int, (ByteArray?) -> Unit>()
    private var deliveryCallback: ((String, String, ByteArray) -> Unit)? = null
    private var eventCallback: ((Int, ByteArray) -> Unit)? = null

    suspend fun connect(serviceIntent: ComponentName) { /* bindService, await connection */ }
    fun disconnect() { /* unbindService */ }

    suspend fun sendRequest(method: Int, topicPath: String, payload: ByteArray? = null): ByteArray { /* encode CBOR, send via Binder, await response */ }

    fun onDelivery(callback: (topicId: String, subtopic: String, data: ByteArray) -> Unit) { deliveryCallback = callback }
    fun onEvent(callback: (eventType: Int, data: ByteArray) -> Unit) { eventCallback = callback }
}
```

~150 lines. CBOR encoding/decoding uses a Kotlin CBOR library (e.g., `kotlinx-serialization-cbor` or a lightweight CBOR parser).

- [ ] **Step 2: Create PoseidonClient.kt**

```kotlin
package com.poseidon.client

class PoseidonClient(private val connection: PoseidonConnection) {
    suspend fun createChannel(name: String): String { /* send CHANNEL_CREATE request, return topic_id */ }
    suspend fun joinChannel(topicOrAlias: String): String { /* send CHANNEL_JOIN */ }
    suspend fun leaveChannel(topicId: String) { /* send CHANNEL_LEAVE */ }
    suspend fun destroyChannel(topicId: String, ownerKey: ByteArray) { /* sign + send CHANNEL_DESTROY */ }
    suspend fun modifyChannel(topicId: String, config: Map<String, Any>, ownerKey: ByteArray) { /* sign + send CHANNEL_MODIFY */ }
    suspend fun subscribe(topicPath: String) { /* send SUBSCRIBE */ }
    suspend fun unsubscribe(topicPath: String) { /* send UNSUBSCRIBE */ }
    suspend fun publish(topicPath: String, data: ByteArray) { /* send PUBLISH */ }
    suspend fun registerAlias(name: String, topicId: String) { /* send ALIAS_REGISTER */ }
    suspend fun unregisterAlias(name: String) { /* send ALIAS_UNREGISTER */ }
    fun onDelivery(callback: (topicId: String, subtopic: String, data: ByteArray) -> Unit) { connection.onDelivery(callback) }
}
```

~80 lines.

- [ ] **Step 3: Create PoseidonClientJava.kt**

```kotlin
package com.poseidon.client

import kotlinx.coroutines.runBlocking

class PoseidonClientJava(private val client: PoseidonClient) {
    fun createChannelBlocking(name: String): String = runBlocking { client.createChannel(name) }
    fun joinChannelBlocking(topicOrAlias: String): String = runBlocking { client.joinChannel(topicOrAlias) }
    fun leaveChannelBlocking(topicId: String) = runBlocking { client.leaveChannel(topicId) }
    fun destroyChannelBlocking(topicId: String, ownerKey: ByteArray) = runBlocking { client.destroyChannel(topicId, ownerKey) }
    fun modifyChannelBlocking(topicId: String, config: Map<String, Any>, ownerKey: ByteArray) = runBlocking { client.modifyChannel(topicId, config, ownerKey) }
    fun subscribeBlocking(topicPath: String) = runBlocking { client.subscribe(topicPath) }
    fun unsubscribeBlocking(topicPath: String) = runBlocking { client.unsubscribe(topicPath) }
    fun publishBlocking(topicPath: String, data: ByteArray) = runBlocking { client.publish(topicPath, data) }
    fun registerAliasBlocking(name: String, topicId: String) = runBlocking { client.registerAlias(name, topicId) }
    fun unregisterAliasBlocking(name: String) = runBlocking { client.unregisterAlias(name) }
}
```

~25 lines. Each method wraps the corresponding suspend function in `runBlocking` for Java callers.

- [ ] **Step 4: Commit**

```bash
git add src/client_libs/android/PoseidonConnection.kt src/client_libs/android/PoseidonClient.kt src/client_libs/android/PoseidonClientJava.kt
git commit -m "feat: add Kotlin client library with Java interop"
```

---

### Task 3: De-wonk audit

- [ ] **Step 1: Audit all files**

Audit for:
1. Binder transport: service registration lifecycle, null binder checks
2. Kotlin: coroutine scope management, cancellation handling
3. Kotlin: thread safety on `pendingRequests` map
4. Java wrapper: `runBlocking` should not be called from Kotlin coroutines (it's for Java callers only)
5. Memory: CBOR byte arrays freed after response received

- [ ] **Step 2: Fix issues**

- [ ] **Step 3: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk Android Binder transport and Kotlin client"
```