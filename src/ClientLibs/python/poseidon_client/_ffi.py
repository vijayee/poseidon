"""cffi ABI-mode declarations for libposeidon_client."""

from cffi import FFI

ffi = FFI()

# C function signatures declared as strings for cffi ABI mode.
# These must match include/poseidon/poseidon_client.h and related headers.
CDEF = """
    // Opaque types
    typedef struct poseidon_client_t poseidon_client_t;
    typedef struct poseidon_key_pair_t poseidon_key_pair_t;

    // Channel config — mirrors poseidon_channel_config.h
    #define POSEIDON_CHANNEL_MAX_RINGS 10

    typedef struct poseidon_channel_config_t {
        uint32_t ring_sizes[10];
        uint32_t gossip_init_interval_s;
        uint32_t gossip_steady_interval_s;
        uint32_t gossip_num_init_intervals;
        uint32_t quasar_max_hops;
        uint32_t quasar_alpha;
        uint32_t quasar_seen_size;
        uint32_t quasar_seen_hashes;
    } poseidon_channel_config_t;

    // Callback types
    typedef void (*poseidon_message_cb_t)(void *ctx, const char *topic_id,
                                          const char *subtopic,
                                          const uint8_t *data, size_t len);
    typedef void (*poseidon_event_cb_t)(void *ctx, uint8_t event_type,
                                         const uint8_t *data, size_t len);
    typedef void (*poseidon_response_cb_t)(void *ctx, uint32_t request_id,
                                           uint8_t error_code,
                                           const char *result_data);

    // Connection
    poseidon_client_t *poseidon_client_connect(const char *transport_url);
    void poseidon_client_disconnect(poseidon_client_t *client);

    // Channel lifecycle
    int poseidon_client_channel_create(poseidon_client_t *client, const char *name,
                                        char *out_topic_id, size_t buf_size);
    int poseidon_client_channel_join(poseidon_client_t *client, const char *topic_or_alias,
                                      char *out_topic_id, size_t buf_size);
    int poseidon_client_channel_leave(poseidon_client_t *client, const char *topic_id);
    int poseidon_client_channel_destroy(poseidon_client_t *client, const char *topic_id,
                                         const poseidon_key_pair_t *owner_key);
    int poseidon_client_channel_modify(poseidon_client_t *client, const char *topic_id,
                                        const poseidon_channel_config_t *config,
                                        const poseidon_key_pair_t *owner_key);

    // Pub/sub
    int poseidon_client_subscribe(poseidon_client_t *client, const char *topic_path);
    int poseidon_client_unsubscribe(poseidon_client_t *client, const char *topic_path);
    int poseidon_client_publish(poseidon_client_t *client, const char *topic_path,
                                 const uint8_t *data, size_t len);

    // Aliases
    int poseidon_client_alias_register(poseidon_client_t *client, const char *name,
                                        const char *topic_id);
    int poseidon_client_alias_unregister(poseidon_client_t *client, const char *name);

    // Events
    void poseidon_client_on_message(poseidon_client_t *client,
                                     poseidon_message_cb_t cb, void *ctx);
    void poseidon_client_on_event(poseidon_client_t *client,
                                   poseidon_event_cb_t cb, void *ctx);
    void poseidon_client_on_response(poseidon_client_t *client,
                                      poseidon_response_cb_t cb, void *ctx);

    // Key pair
    poseidon_key_pair_t *poseidon_key_pair_load_from_pem(const char *filepath);
    void poseidon_key_pair_destroy(poseidon_key_pair_t *kp);

    // Channel config defaults
    poseidon_channel_config_t poseidon_channel_config_defaults(void);
"""

ffi.cdef(CDEF)

# Lazy-load the shared library on first access.
# The library must be installed (via CMake install of the C client library)
# and discoverable via the system's dynamic linker.
_lib = None
_lib_lock = __import__("threading").Lock()


def _load_lib():
    global _lib
    if _lib is None:
        with _lib_lock:
            if _lib is None:
                _lib = ffi.dlopen("poseidon_client")
    return _lib