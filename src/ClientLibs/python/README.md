# Poseidon Python Client Library

Python bindings for the Poseidon P2P pub/sub client library, using cffi ABI mode.

## Prerequisites

- Python 3.9+
- `libposeidon_client` shared library installed (via CMake `install` of the C client)
- cffi >= 1.16

## Installation

```bash
pip install -e .
```

## Usage

```python
from poseidon_client import PoseidonClient, PoseidonError

client = PoseidonClient()

try:
    client.connect("unix:///tmp/poseidon.sock")

    # Create a channel
    topic_id = client.create_channel("my-channel")

    # Subscribe and publish
    client.subscribe(topic_id)
    client.publish(topic_id, b"Hello, Poseidon!")

    # Register a message callback
    client.on_message(lambda topic_id, subtopic, data:
        print(f"Received on {topic_id}/{subtopic}: {data}"))

    # Register an alias
    client.register_alias("my-alias", topic_id)

    # Modify a channel (requires owner key)
    client.modify_channel(topic_id, {"quasar_max_hops": 10}, "/path/to/owner.pem")

    # Destroy a channel (requires owner key)
    client.destroy_channel(topic_id, "/path/to/owner.pem")

finally:
    client.disconnect()
```

### Context Manager

```python
with PoseidonClient() as client:
    client.connect("tcp://localhost:9000")
    topic_id = client.create_channel("my-channel")
```

### Channel Configuration

`modify_channel` accepts an optional config dict:

```python
config = {
    "ring_sizes": [100, 200, 300],
    "gossip_init_interval_s": 5,
    "gossip_steady_interval_s": 30,
    "gossip_num_init_intervals": 10,
    "quasar_max_hops": 8,
    "quasar_alpha": 3,
    "quasar_seen_size": 1000,
    "quasar_seen_hashes": 2,
}
client.modify_channel(topic_id, config, "/path/to/owner.pem")
```

## Error Handling

All errors from the C library are raised as `PoseidonError`:

```python
from poseidon_client import PoseidonError

try:
    client.subscribe("nonexistent")
except PoseidonError as e:
    print(f"Error {e.code}: {e}")
```

## Running Tests

```bash
pip install -e ".[dev]"
pytest
```

Tests mock the C library and do not require `libposeidon_client` to be installed.

## How It Works

cffi ABI mode loads `libposeidon_client.so` at runtime via `ffi.dlopen()`. The C function signatures are declared as strings in Python — no C compilation is needed at install time. Admin operations (`destroy_channel`, `modify_channel`) that require an owner key handle key loading and cleanup internally.