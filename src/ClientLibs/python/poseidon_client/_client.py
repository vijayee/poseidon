"""PoseidonClient — Pythonic wrapper around the Poseidon C client library."""

from __future__ import annotations

from typing import Callable, Optional

from ._ffi import ffi, _load_lib
from ._errors import PoseidonError, error_from_code

_TOPIC_ID_BUF_SIZE = 256


class PoseidonClient:
    """High-level Python client for Poseidon pub/sub.

    Wraps the C client library via cffi ABI mode. The shared library
    ``libposeidon_client`` must be installed and discoverable by the
    dynamic linker.
    """

    def __init__(self) -> None:
        self._ptr = None  # type: Optional[object]  # cdata pointer
        self._message_cb = None
        self._event_cb = None
        self._response_cb = None
        self._message_handle = None
        self._event_handle = None
        self._response_handle = None

    # -- Connection -----------------------------------------------------------

    def connect(self, url: str) -> None:
        """Connect to a Poseidon daemon.

        Args:
            url: Transport URL, e.g. ``"unix:///path/to/socket"``
                 or ``"tcp://host:port"``.

        Raises:
            PoseidonError: If the connection fails.
        """
        if self._ptr is not None:
            raise PoseidonError("Already connected", -1)
        lib = _load_lib()
        ptr = lib.poseidon_client_connect(url.encode("utf-8"))
        if ptr == ffi.NULL:
            raise PoseidonError("Failed to connect", -1)
        self._ptr = ptr

    def disconnect(self) -> None:
        """Disconnect from the daemon and release resources."""
        if self._ptr is not None:
            _load_lib().poseidon_client_disconnect(self._ptr)
            self._ptr = None

    # -- Channel lifecycle ----------------------------------------------------

    def create_channel(self, name: str) -> str:
        """Create a new channel and return its topic ID."""
        lib = _load_lib()
        buf = ffi.new("char[]", _TOPIC_ID_BUF_SIZE)
        rc = lib.poseidon_client_channel_create(self._require_connected(), name.encode("utf-8"), buf, _TOPIC_ID_BUF_SIZE)
        if rc != 0:
            raise error_from_code(rc)
        return ffi.string(buf).decode("utf-8")

    def join_channel(self, topic_or_alias: str) -> str:
        """Join an existing channel by topic ID or alias."""
        lib = _load_lib()
        buf = ffi.new("char[]", _TOPIC_ID_BUF_SIZE)
        rc = lib.poseidon_client_channel_join(self._require_connected(), topic_or_alias.encode("utf-8"), buf, _TOPIC_ID_BUF_SIZE)
        if rc != 0:
            raise error_from_code(rc)
        return ffi.string(buf).decode("utf-8")

    def leave_channel(self, topic_id: str) -> None:
        """Leave a channel."""
        rc = _load_lib().poseidon_client_channel_leave(self._require_connected(), topic_id.encode("utf-8"))
        if rc != 0:
            raise error_from_code(rc)

    def destroy_channel(self, topic_id: str, owner_key_pem: str) -> None:
        """Destroy a channel (requires the owner key).

        Args:
            topic_id: The channel topic ID.
            owner_key_pem: Path to the PEM file containing the owner key.
        """
        lib = _load_lib()
        kp = lib.poseidon_key_pair_load_from_pem(owner_key_pem.encode("utf-8"))
        if kp == ffi.NULL:
            raise PoseidonError("Failed to load owner key from PEM", -1)
        try:
            rc = lib.poseidon_client_channel_destroy(self._require_connected(), topic_id.encode("utf-8"), kp)
            if rc != 0:
                raise error_from_code(rc)
        finally:
            lib.poseidon_key_pair_destroy(kp)

    def modify_channel(self, topic_id: str, config: dict, owner_key_pem: str) -> None:
        """Modify channel configuration (requires the owner key).

        Args:
            topic_id: The channel topic ID.
            config: Dict with optional keys matching
                :c:type:`poseidon_channel_config_t` fields:
                ``ring_sizes``, ``gossip_init_interval_s``,
                ``gossip_steady_interval_s``, ``gossip_num_init_intervals``,
                ``quasar_max_hops``, ``quasar_alpha``,
                ``quasar_seen_size``, ``quasar_seen_hashes``.
            owner_key_pem: Path to the PEM file containing the owner key.
        """
        lib = _load_lib()
        cconfig = lib.poseidon_channel_config_defaults()
        _apply_config(cconfig, config)

        kp = lib.poseidon_key_pair_load_from_pem(owner_key_pem.encode("utf-8"))
        if kp == ffi.NULL:
            raise PoseidonError("Failed to load owner key from PEM", -1)
        try:
            rc = lib.poseidon_client_channel_modify(self._require_connected(), topic_id.encode("utf-8"), cconfig, kp)
            if rc != 0:
                raise error_from_code(rc)
        finally:
            lib.poseidon_key_pair_destroy(kp)

    # -- Pub/sub --------------------------------------------------------------

    def subscribe(self, topic_path: str) -> None:
        """Subscribe to a topic path."""
        rc = _load_lib().poseidon_client_subscribe(self._require_connected(), topic_path.encode("utf-8"))
        if rc != 0:
            raise error_from_code(rc)

    def unsubscribe(self, topic_path: str) -> None:
        """Unsubscribe from a topic path."""
        rc = _load_lib().poseidon_client_unsubscribe(self._require_connected(), topic_path.encode("utf-8"))
        if rc != 0:
            raise error_from_code(rc)

    def publish(self, topic_path: str, data: bytes) -> None:
        """Publish data to a topic path.

        Args:
            topic_path: The topic path to publish to.
            data: Bytes to publish.
        """
        rc = _load_lib().poseidon_client_publish(self._require_connected(), topic_path.encode("utf-8"), data, len(data))
        if rc != 0:
            raise error_from_code(rc)

    # -- Aliases --------------------------------------------------------------

    def register_alias(self, name: str, topic_id: str) -> None:
        """Register a human-readable alias for a topic ID."""
        rc = _load_lib().poseidon_client_alias_register(self._require_connected(), name.encode("utf-8"), topic_id.encode("utf-8"))
        if rc != 0:
            raise error_from_code(rc)

    def unregister_alias(self, name: str) -> None:
        """Unregister a previously registered alias."""
        rc = _load_lib().poseidon_client_alias_unregister(self._require_connected(), name.encode("utf-8"))
        if rc != 0:
            raise error_from_code(rc)

    # -- Callbacks ------------------------------------------------------------

    def on_message(self, callback: Callable[[str, str, bytes], None]) -> None:
        """Register a message callback.

        Args:
            callback: ``f(topic_id: str, subtopic: str, data: bytes) -> None``
        """
        lib = _load_lib()
        self._message_handle = ffi.new_handle(callback)
        self._message_cb = ffi.callback(
            "void(void *, const char *, const char *, const uint8_t *, size_t)",
            _make_message_callback(),
        )
        lib.poseidon_client_on_message(self._require_connected(), self._message_cb, self._message_handle)

    def on_event(self, callback: Callable[[int, bytes], None]) -> None:
        """Register an event callback.

        Args:
            callback: ``f(event_type: int, data: bytes) -> None``
        """
        lib = _load_lib()
        self._event_handle = ffi.new_handle(callback)
        self._event_cb = ffi.callback(
            "void(void *, uint8_t, const uint8_t *, size_t)",
            _make_event_callback(),
        )
        lib.poseidon_client_on_event(self._require_connected(), self._event_cb, self._event_handle)

    def on_response(self, callback: Callable[[int, int, str], None]) -> None:
        """Register a response callback.

        Args:
            callback: ``f(request_id: int, error_code: int, result_data: str) -> None``
        """
        lib = _load_lib()
        self._response_handle = ffi.new_handle(callback)
        self._response_cb = ffi.callback(
            "void(void *, uint32_t, uint8_t, const char *)",
            _make_response_callback(),
        )
        lib.poseidon_client_on_response(self._require_connected(), self._response_cb, self._response_handle)

    # -- Lifecycle ------------------------------------------------------------

    def __del__(self) -> None:
        try:
            self.disconnect()
        except Exception:
            pass

    def __enter__(self) -> "PoseidonClient":
        return self

    def __exit__(self, *exc) -> None:
        self.disconnect()

    # -- Internal -------------------------------------------------------------

    def _require_connected(self):
        """Return the C pointer, raising if not connected."""
        if self._ptr is None:
            raise PoseidonError("Not connected", -1)
        return self._ptr


# ---------------------------------------------------------------------------
# Callback factories — each returns a new cffi callback that acquires the GIL
# and invokes the Python callable stored via ffi.from_handle().
# ---------------------------------------------------------------------------

def _make_message_callback():
    @ffi.callback("void(void *, const char *, const char *, const uint8_t *, size_t)")
    def _cb(ctx, topic_id, subtopic, data, length):
        py_cb = ffi.from_handle(ctx)
        tid = ffi.string(topic_id).decode("utf-8") if topic_id != ffi.NULL else ""
        sub = ffi.string(subtopic).decode("utf-8") if subtopic != ffi.NULL else ""
        py_cb(tid, sub, bytes(ffi.buffer(data, length)))
    return _cb


def _make_event_callback():
    @ffi.callback("void(void *, uint8_t, const uint8_t *, size_t)")
    def _cb(ctx, event_type, data, length):
        py_cb = ffi.from_handle(ctx)
        py_cb(int(event_type), bytes(ffi.buffer(data, length)))
    return _cb


def _make_response_callback():
    @ffi.callback("void(void *, uint32_t, uint8_t, const char *)")
    def _cb(ctx, request_id, error_code, result_data):
        py_cb = ffi.from_handle(ctx)
        rd = ffi.string(result_data).decode("utf-8") if result_data != ffi.NULL else ""
        py_cb(int(request_id), int(error_code), rd)
    return _cb


# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------

def _apply_config(cconfig, config: dict) -> None:
    """Apply Python dict overrides to a poseidon_channel_config_t struct."""
    if "ring_sizes" in config:
        ring_sizes = config["ring_sizes"]
        for i, val in enumerate(ring_sizes):
            if i >= 10:
                break
            cconfig.ring_sizes[i] = val
    _INT_FIELDS = (
        "gossip_init_interval_s", "gossip_steady_interval_s",
        "gossip_num_init_intervals", "quasar_max_hops", "quasar_alpha",
        "quasar_seen_size", "quasar_seen_hashes",
    )
    for field in _INT_FIELDS:
        if field in config:
            setattr(cconfig, field, config[field])