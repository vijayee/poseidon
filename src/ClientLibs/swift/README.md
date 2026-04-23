# Poseidon Swift Client Library

A Swift client library for connecting to a Poseidon daemon via XPC (macOS/iOS).

## Prerequisites

- Swift 5.9+
- macOS 10.15+ or iOS 13.0+

## Building

### Swift Package Manager

Add to your `Package.swift`:

```swift
dependencies: [
    .package(path: "path/to/poseidon/src/ClientLibs/swift")
]
```

Or via URL (once published):

```swift
dependencies: [
    .package(url: "https://github.com/vijayee/poseidon", from: "0.1.0")
]
```

### Xcode

Open `Package.swift` in Xcode or add the package via File > Add Package Dependencies.

## Usage

```swift
import PoseidonClient

let connection = PoseidonConnection()
connection.connect(serviceName: "com.poseidon.daemon")

let client = PoseidonClient(connection: connection)

// Create a channel
let topicId = try await client.createChannel(name: "my-channel")

// Subscribe and publish
try await client.subscribe(topicPath: topicId)
try await client.publish(topicPath: topicId, data: "Hello".data(using: .utf8)!)

// Set delivery callback
client.onDelivery { topicId, subtopic, data in
    print("Received on \(topicId)/\(subtopic): \(data)")
}

// Disconnect
connection.disconnect()
```

## Objective-C Interop

Use `PoseidonClientObjC` for completion-handler based API:

```objc
PoseidonClientObjC *client = [[PoseidonClientObjC alloc] initWithClient:swiftClient];
[client createChannelWithName:@"my-channel" completion:^(NSString *topicId, NSError *error) {
    // ...
}];
```