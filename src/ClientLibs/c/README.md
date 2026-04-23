# Poseidon C Client Library

A C client library for connecting to a Poseidon daemon over Unix sockets or TCP.

## Building Standalone

```bash
cd src/ClientLibs/c
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make
make install
```

### Dependencies

- **libcbor** — automatically built from `deps/libcbor/` if present, or found via pkg-config
- **OpenSSL** (libcrypto) — for ED25519 key generation and signing
- **pthreads** — for receive thread

### Using in Your Project

```cmake
find_package(PoseidonClient REQUIRED)
target_link_libraries(myapp PRIVATE PoseidonClient::poseidon_client_static)
# or
target_link_libraries(myapp PRIVATE PoseidonClient::poseidon_client_shared)
```

```c
#include <poseidon/poseidon_client.h>
#include <poseidon/poseidon_key_pair.h>
#include <poseidon/poseidon_channel_config.h>
```

## Building In-Tree

The C client library is also built as part of the full Poseidon project. The root `CMakeLists.txt` includes it via `add_subdirectory()`.

## API

| Function | Description |
|----------|-------------|
| `poseidon_client_connect(url)` | Connect to daemon (`unix://` or `tcp://`) |
| `poseidon_client_disconnect(client)` | Disconnect and free |
| `poseidon_client_channel_create(client, name, ...)` | Create a channel |
| `poseidon_client_channel_join(client, topic, ...)` | Join a channel |
| `poseidon_client_channel_leave(client, topic)` | Leave a channel |
| `poseidon_client_channel_destroy(client, topic, key)` | Delete a channel (owner key required) |
| `poseidon_client_channel_modify(client, topic, config, key)` | Modify channel config (owner key required) |
| `poseidon_client_subscribe(client, topic)` | Subscribe to a topic |
| `poseidon_client_unsubscribe(client, topic)` | Unsubscribe |
| `poseidon_client_publish(client, topic, data, len)` | Publish data |
| `poseidon_client_on_message(client, cb, ctx)` | Set message callback |
| `poseidon_client_on_event(client, cb, ctx)` | Set event callback |