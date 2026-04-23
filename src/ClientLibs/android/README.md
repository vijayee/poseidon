# Poseidon Android Client Library

A Kotlin/Android client library for connecting to a Poseidon daemon via Android Binder IPC.

## Prerequisites

- Android SDK 34+
- Kotlin 1.9+
- Android Gradle Plugin 8.1+

## Building

Include this module in your Android project settings:

```kotlin
// settings.gradle.kts
include(":poseidon-client")
project(":poseidon-client").projectDir = file("path/to/poseidon/src/ClientLibs/android")
```

Then add the dependency:

```kotlin
// app/build.gradle.kts
dependencies {
    implementation(project(":poseidon-client"))
}
```

## Usage

```kotlin
// Bind to the Poseidon daemon service
val connection = PoseidonConnection(context)
connection.connect(ComponentName("com.poseidon.daemon", "com.poseidon.daemon.PoseidonService"))

val client = PoseidonClient(connection)

// Create a channel
val topicId = client.createChannel("my-channel")

// Subscribe and publish
client.subscribe(topicId)
client.publish(topicId, "Hello".toByteArray())

// Set message callback
client.onMessage { topicId, subtopic, data ->
    println("Received on $topicId/$subtopic: ${String(data)}")
}

// Disconnect
connection.disconnect()
```

## Java Interop

Use `PoseidonClientJava` for blocking calls from Java code:

```java
PoseidonClientJava javaClient = new PoseidonClientJava(client);
String topicId = javaClient.createChannelBlocking("my-channel");
```