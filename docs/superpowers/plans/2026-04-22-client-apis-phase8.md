# Phase 8: iOS XPC Transport + Swift Client Library

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an iOS XPC transport to the daemon (conditionally compiled under `__APPLE__ && TARGET_OS_IPHONE`) and a Swift client library with async/await + Objective-C completion-handler APIs.

**Architecture:** The XPC transport bridges iOS XPC callbacks to the `poseidon_transport_t` interface. On the Swift side, `PoseidonConnection` manages the XPC connection and CBOR frame serialization. `PoseidonClient` provides async/await APIs; `PoseidonClientObjC` wraps each with completion handlers for Objective-C callers.

**Tech Stack:** C (with `#ifdef __APPLE__`), Swift, Objective-C interop, XPC

---

## File Structure

| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ClientAPIs/transport_xpc.c` | iOS XPC transport (conditioned on `__APPLE__`) |
| Create | `src/client_libs/swift/PoseidonConnection.swift` | XPC connection + CBOR frame handling |
| Create | `src/client_libs/swift/PoseidonClient.swift` | Swift async/await API |
| Create | `src/client_libs/swift/PoseidonClientObjC.swift` | Objective-C completion-handler wrappers |

---

### Task 1: iOS XPC transport (C side)

**Files:**
- Create: `src/ClientAPIs/transport_xpc.c`
- Modify: `CMakeLists.txt` — conditional compilation

- [ ] **Step 1: Create transport_xpc.c**

```c
#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "transport.h"
// ... XPC includes and implementation

// XPC creates a connection to the poseidond XPC service.
// Incoming XPC messages are unpacked and forwarded to on_message.
// Responses are packed into XPC reply dictionaries.

poseidon_transport_t* poseidon_transport_xpc_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    // Allocate transport, set type to POSEIDON_TRANSPORT_XPC
    // start() creates xpc_connection_create
    // stop() cancels the connection
    // send() sends an XPC message dictionary
}

#endif // __APPLE__ && TARGET_OS_IPHONE
```

~180 lines. XPC doesn't use poll-dancer — it has its own callback-driven API.

- [ ] **Step 2: Add conditional compilation to CMakeLists.txt**

```cmake
if(APPLE)
    list(APPEND POSEIDON_SOURCES ${CLIENT_APIS_SRC_DIR}/transport_xpc.c)
    target_link_libraries(poseidon PRIVATE "-framework CoreFoundation")
endif()
```

- [ ] **Step 3: Add create function declaration to transport.h**

```c
#if defined(__APPLE__) && TARGET_OS_IPHONE
poseidon_transport_t* poseidon_transport_xpc_create(
    const char* service_name,
    poseidon_channel_manager_t* manager);
#endif
```

- [ ] **Step 4: Commit**

```bash
git add src/ClientAPIs/transport_xpc.c src/ClientAPIs/transport.h CMakeLists.txt
git commit -m "feat: add iOS XPC transport (conditional on Apple platform)"
```

---

### Task 2: Swift client library

**Files:**
- Create: `src/client_libs/swift/PoseidonConnection.swift`
- Create: `src/client_libs/swift/PoseidonClient.swift`
- Create: `src/client_libs/swift/PoseidonClientObjC.swift`

- [ ] **Step 1: Create PoseidonConnection.swift**

```swift
import Foundation

class PoseidonConnection {
    private var connection: xpc_object_t?
    private var requestId: UInt32 = 0
    private let requestLock = NSLock()
    private var pendingRequests: [UInt32: (Result<Data, Error>) -> Void] = [:]
    private var deliveryCallback: ((String, String, Data) -> Void)?
    private var eventCallback: ((UInt8, Data) -> Void)?

    func connect(serviceName: String) async throws { /* xpc_connection_create, set event handler */ }
    func disconnect() { /* xpc_connection_cancel */ }

    func sendRequest(method: UInt8, topicPath: String, payload: Data? = nil) async throws -> Data {
        /* encode CBOR frame, send via XPC, await response */
    }

    func onDelivery(_ handler: @escaping (String, String, Data) -> Void) { deliveryCallback = handler }
    func onEvent(_ handler: @escaping (UInt8, Data) -> Void) { eventCallback = handler }
}
```

~130 lines.

- [ ] **Step 2: Create PoseidonClient.swift**

```swift
class PoseidonClient {
    private let connection: PoseidonConnection

    init(connection: PoseidonConnection) { self.connection = connection }

    func createChannel(name: String) async throws -> String { /* CHANNEL_CREATE */ }
    func joinChannel(topicOrAlias: String) async throws -> String { /* CHANNEL_JOIN */ }
    func leaveChannel(topicId: String) async throws { /* CHANNEL_LEAVE */ }
    func destroyChannel(topicId: String, ownerKey: Data) async throws { /* sign + CHANNEL_DESTROY */ }
    func modifyChannel(topicId: String, config: [String: Any], ownerKey: Data) async throws { /* sign + CHANNEL_MODIFY */ }
    func subscribe(topicPath: String) async throws { /* SUBSCRIBE */ }
    func unsubscribe(topicPath: String) async throws { /* UNSUBSCRIBE */ }
    func publish(topicPath: String, data: Data) async throws { /* PUBLISH */ }
    func registerAlias(name: String, topicId: String) async throws { /* ALIAS_REGISTER */ }
    func unregisterAlias(name: String) async throws { /* ALIAS_UNREGISTER */ }
    func onDelivery(_ handler: @escaping (String, String, Data) -> Void) { connection.onDelivery(handler) }
}
```

~70 lines.

- [ ] **Step 3: Create PoseidonClientObjC.swift**

```swift
@objcMembers
class PoseidonClientObjC: NSObject {
    private let client: PoseidonClient

    init(client: PoseidonClient) { self.client = client }

    func createChannel(name: String, completion: @escaping (String?, Error?) -> Void) {
        Task { do { let result = try await client.createChannel(name: name); completion(result, nil) } catch { completion(nil, error) } }
    }
    func joinChannel(topicOrAlias: String, completion: @escaping (String?, Error?) -> Void) {
        Task { do { let result = try await client.joinChannel(topicOrAlias: topicOrAlias); completion(result, nil) } catch { completion(nil, error) } }
    }
    func leaveChannel(topicId: String, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.leaveChannel(topicId: topicId); completion(nil) } catch { completion(error) } }
    }
    func destroyChannel(topicId: String, ownerKey: Data, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.destroyChannel(topicId: topicId, ownerKey: ownerKey); completion(nil) } catch { completion(error) } }
    }
    func modifyChannel(topicId: String, config: [String: Any], ownerKey: Data, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.modifyChannel(topicId: topicId, config: config, ownerKey: ownerKey); completion(nil) } catch { completion(error) } }
    }
    func subscribe(topicPath: String, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.subscribe(topicPath: topicPath); completion(nil) } catch { completion(error) } }
    }
    func unsubscribe(topicPath: String, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.unsubscribe(topicPath: topicPath); completion(nil) } catch { completion(error) } }
    }
    func publish(topicPath: String, data: Data, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.publish(topicPath: topicPath, data: data); completion(nil) } catch { completion(error) } }
    }
    func registerAlias(name: String, topicId: String, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.registerAlias(name: name, topicId: topicId); completion(nil) } catch { completion(error) } }
    }
    func unregisterAlias(name: String, completion: @escaping (Error?) -> Void) {
        Task { do { try await client.unregisterAlias(name: name); completion(nil) } catch { completion(error) } }
    }
}
```

~30 lines. Each method wraps the async call with `Task { }` and dispatches the result to the completion handler.

- [ ] **Step 4: Commit**

```bash
git add src/client_libs/swift/PoseidonConnection.swift src/client_libs/swift/PoseidonClient.swift src/client_libs/swift/PoseidonClientObjC.swift
git commit -m "feat: add Swift client library with ObjC interop"
```

---

### Task 3: De-wonk audit

- [ ] **Step 1: Audit all files**

Audit for:
1. XPC transport: connection lifecycle, error handling on connection interruption
2. Swift: async/await error propagation, cancellation handling
3. Swift: `Task { }` blocks in ObjC wrappers — verify all paths call the completion handler (including error paths)
4. Memory: CBOR data retained/released correctly in Swift
5. Thread safety: `pendingRequests` dictionary accessed from XPC callback queue and Swift tasks

- [ ] **Step 2: Fix issues**

- [ ] **Step 3: Commit fixes**

```bash
git add -u
git commit -m "fix: de-wonk iOS XPC transport and Swift client"
```