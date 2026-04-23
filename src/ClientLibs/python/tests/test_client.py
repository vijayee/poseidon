"""Tests for poseidon_client Python bindings.

These tests mock the C library via unittest.mock, verifying
that the Python wrapper correctly calls into the C API and handles
return values and errors.
"""

from unittest.mock import patch, MagicMock

import pytest


# ---------------------------------------------------------------------------
# Test PoseidonError and error_from_code (no C library needed)
# ---------------------------------------------------------------------------

class TestPoseidonError:
    def test_error_from_code_known(self):
        from poseidon_client._errors import error_from_code
        err = error_from_code(1)
        assert err.code == 1
        assert "Unknown method" in str(err)

    def test_error_from_code_unknown(self):
        from poseidon_client._errors import error_from_code
        err = error_from_code(99)
        assert err.code == 99
        assert "99" in str(err)

    def test_error_from_code_ok(self):
        from poseidon_client._errors import error_from_code
        err = error_from_code(0)
        assert err.code == 0
        assert "OK" in str(err)

    def test_poseidon_error_is_exception(self):
        from poseidon_client._errors import PoseidonError
        err = PoseidonError("test", 42)
        assert isinstance(err, Exception)
        assert err.code == 42


# ---------------------------------------------------------------------------
# Test _apply_config helper (no C library needed)
# ---------------------------------------------------------------------------

class TestConfigHelper:
    def test_apply_config_int_fields(self):
        from poseidon_client._client import _apply_config

        mock_config = MagicMock()
        _apply_config(mock_config, {"quasar_max_hops": 10, "quasar_alpha": 3})
        assert mock_config.quasar_max_hops == 10
        assert mock_config.quasar_alpha == 3

    def test_apply_config_ring_sizes(self):
        from poseidon_client._client import _apply_config

        mock_config = MagicMock()
        mock_config.ring_sizes = [0] * 10
        _apply_config(mock_config, {"ring_sizes": [100, 200, 300]})
        assert mock_config.ring_sizes[0] == 100
        assert mock_config.ring_sizes[1] == 200
        assert mock_config.ring_sizes[2] == 300
        assert mock_config.ring_sizes[3] == 0

    def test_apply_config_empty(self):
        from poseidon_client._client import _apply_config

        mock_config = MagicMock()
        mock_config.ring_sizes = [0] * 10
        _apply_config(mock_config, {})
        # Empty config should not set any attributes via setattr
        # ring_sizes was pre-set, so we just verify no int fields were set
        assert mock_config.method_calls == []

    def test_apply_config_ring_sizes_truncation(self):
        from poseidon_client._client import _apply_config

        mock_config = MagicMock()
        mock_config.ring_sizes = [0] * 10
        _apply_config(mock_config, {"ring_sizes": list(range(15))})
        for i in range(10):
            assert mock_config.ring_sizes[i] == i


# ---------------------------------------------------------------------------
# Test PoseidonClient with mocked C library
# ---------------------------------------------------------------------------

class TestPoseidonClientUnit:
    """Unit tests for PoseidonClient using a mocked _load_lib."""

    @pytest.fixture(autouse=True)
    def setup_mock_lib(self):
        """Patch _load_lib with a mock before each test."""
        self.mock_lib = MagicMock()

        # Default: connect returns a truthy mock pointer
        self.mock_lib.poseidon_client_connect.return_value = MagicMock()

        # Key pair mock
        self.mock_lib.poseidon_key_pair_load_from_pem.return_value = MagicMock()

        # All int-returning C functions default to 0 (success)
        for name in (
            "poseidon_client_channel_create", "poseidon_client_channel_join",
            "poseidon_client_channel_leave", "poseidon_client_channel_destroy",
            "poseidon_client_channel_modify", "poseidon_client_subscribe",
            "poseidon_client_unsubscribe", "poseidon_client_publish",
            "poseidon_client_alias_register", "poseidon_client_alias_unregister",
        ):
            getattr(self.mock_lib, name).return_value = 0

        self._patcher = patch("poseidon_client._client._load_lib", return_value=self.mock_lib)
        self._patcher.start()
        yield
        self._patcher.stop()

    def test_connect_success(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        self.mock_lib.poseidon_client_connect.assert_called_once()
        assert client._ptr is not None

    def test_connect_already_connected(self):
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        with pytest.raises(PoseidonError, match="Already connected"):
            client.connect("tcp://localhost:9000")

    def test_connect_failure(self):
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        from poseidon_client._ffi import ffi
        self.mock_lib.poseidon_client_connect.return_value = ffi.NULL
        client = PoseidonClient()
        with pytest.raises(PoseidonError, match="Failed to connect"):
            client.connect("tcp://bad:9999")

    def test_disconnect(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.disconnect()
        assert client._ptr is None
        self.mock_lib.poseidon_client_disconnect.assert_called_once()

    def test_disconnect_idempotent(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.disconnect()
        self.mock_lib.poseidon_client_disconnect.assert_not_called()

    def test_create_channel(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")

        # Mock ffi.new to return a buffer we can control, and ffi.string to decode it
        with patch("poseidon_client._client.ffi") as mock_ffi:
            mock_buf = MagicMock()
            mock_ffi.new.return_value = mock_buf
            mock_ffi.string.return_value = b"test-topic-id"
            mock_ffi.NULL = None

            topic_id = client.create_channel("my-channel")
            assert topic_id == "test-topic-id"

    def test_create_channel_error(self):
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")

        with patch("poseidon_client._client.ffi") as mock_ffi:
            mock_ffi.new.return_value = MagicMock()
            mock_ffi.NULL = None
            self.mock_lib.poseidon_client_channel_create.return_value = 3  # CHANNEL_NOT_FOUND
            with pytest.raises(PoseidonError):
                client.create_channel("bad")

    def test_join_channel(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")

        with patch("poseidon_client._client.ffi") as mock_ffi:
            mock_ffi.new.return_value = MagicMock()
            mock_ffi.string.return_value = b"abc"
            mock_ffi.NULL = None

            topic_id = client.join_channel("my-alias")
            assert topic_id == "abc"

    def test_leave_channel(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.leave_channel("topic-123")
        self.mock_lib.poseidon_client_channel_leave.assert_called_once()

    def test_destroy_channel(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.destroy_channel("topic-123", "/path/to/key.pem")
        self.mock_lib.poseidon_client_channel_destroy.assert_called_once()
        self.mock_lib.poseidon_key_pair_destroy.assert_called_once()

    def test_destroy_channel_key_load_failure(self):
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        from poseidon_client._ffi import ffi
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        self.mock_lib.poseidon_key_pair_load_from_pem.return_value = ffi.NULL
        with pytest.raises(PoseidonError, match="Failed to load owner key"):
            client.destroy_channel("topic-123", "/bad/key.pem")

    def test_modify_channel(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.modify_channel("topic-123", {"quasar_max_hops": 10}, "/path/to/key.pem")
        self.mock_lib.poseidon_client_channel_modify.assert_called_once()
        self.mock_lib.poseidon_key_pair_destroy.assert_called_once()

    def test_subscribe(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.subscribe("topic-123/subtopic")
        self.mock_lib.poseidon_client_subscribe.assert_called_once()

    def test_unsubscribe(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.unsubscribe("topic-123/subtopic")
        self.mock_lib.poseidon_client_unsubscribe.assert_called_once()

    def test_publish(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.publish("topic-123", b"hello world")
        self.mock_lib.poseidon_client_publish.assert_called_once()

    def test_register_alias(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.register_alias("my-alias", "topic-123")
        self.mock_lib.poseidon_client_alias_register.assert_called_once()

    def test_unregister_alias(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        client.unregister_alias("my-alias")
        self.mock_lib.poseidon_client_alias_unregister.assert_called_once()

    def test_not_connected_raises(self):
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        client = PoseidonClient()
        with pytest.raises(PoseidonError, match="Not connected"):
            client.subscribe("topic")

    def test_context_manager(self):
        from poseidon_client._client import PoseidonClient
        with PoseidonClient() as client:
            client.connect("unix:///tmp/poseidon.sock")
            assert client._ptr is not None
        assert client._ptr is None

    def test_on_message_callback(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        received = []
        client.on_message(lambda tid, sub, data: received.append((tid, sub, data)))
        self.mock_lib.poseidon_client_on_message.assert_called_once()

    def test_on_event_callback(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        received = []
        client.on_event(lambda et, data: received.append((et, data)))
        self.mock_lib.poseidon_client_on_event.assert_called_once()

    def test_on_response_callback(self):
        from poseidon_client._client import PoseidonClient
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        received = []
        client.on_response(lambda rid, ec, rd: received.append((rid, ec, rd)))
        self.mock_lib.poseidon_client_on_response.assert_called_once()

    def test_destroy_channel_key_destroyed_on_error(self):
        """Key pair should be destroyed even if channel_destroy fails."""
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        self.mock_lib.poseidon_client_channel_destroy.return_value = 5  # NOT_AUTHORIZED
        with pytest.raises(PoseidonError):
            client.destroy_channel("topic-123", "/path/to/key.pem")
        # Key pair must still be destroyed
        self.mock_lib.poseidon_key_pair_destroy.assert_called_once()

    def test_modify_channel_key_destroyed_on_error(self):
        """Key pair should be destroyed even if channel_modify fails."""
        from poseidon_client._client import PoseidonClient
        from poseidon_client._errors import PoseidonError
        client = PoseidonClient()
        client.connect("unix:///tmp/poseidon.sock")
        self.mock_lib.poseidon_client_channel_modify.return_value = 5  # NOT_AUTHORIZED
        with pytest.raises(PoseidonError):
            client.modify_channel("topic-123", {}, "/path/to/key.pem")
        self.mock_lib.poseidon_key_pair_destroy.assert_called_once()


# ---------------------------------------------------------------------------
# Test imports (without actually loading the shared library)
# ---------------------------------------------------------------------------

class TestImports:
    def test_import_poseidon_error(self):
        from poseidon_client._errors import PoseidonError
        assert PoseidonError is not None

    def test_import_error_from_code(self):
        from poseidon_client._errors import error_from_code
        assert error_from_code is not None