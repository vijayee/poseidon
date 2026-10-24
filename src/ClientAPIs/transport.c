//
// transport.c
//
// Shared transport entry points that are not specific to any one
// backend: the default configuration and the generic lifecycle
// adapters.
//
// The transports used to carry a vtable: start, stop, send and destroy
// were function pointers on the transport body. The vtable is gone —
// the body is a plain actor-shaped struct now — and these adapters
// dispatch on its type field to reach the backend's own
// implementation.
//
// The adapters exist because not every caller knows the backend at
// compile time: the client session sends through whatever transport its
// session was created on, and the daemon drives four of them from one
// place.
//

#include "transport.h"

#include "transport_unix.h"
#include "transport_tcp.h"
#include "transport_ws.h"
#include "transport_webtransport.h"
#include "transport_quic.h"
#ifdef __ANDROID__
#include "transport_binder.h"
#endif
#ifdef __APPLE__
#include "transport_xpc.h"
#endif

#include <string.h>

poseidon_transport_config_t poseidon_transport_config_defaults(void) {
    poseidon_transport_config_t config;
    memset(&config, 0, sizeof(config));
    /* The Unix transport is opt-in, and its path has no default.
     *
     * Upstream keeps the same rule: the transport exists only when a path
     * was named on the command line (liboffs/examples/off_server/main.c
     * parses "--unix <path>" and creates the transport only when that
     * argument is present). A hard-coded default under /var/run needs
     * root to bind and root to remove, so a daemon started by an
     * ordinary user cannot listen on it at all. */
    config.enable_unix = false;
    config.unix_socket_path = NULL;
    config.tcp_port = 9090;
    config.ws_port = 9091;
    config.quic_port = 9092;
    config.wt_port = 9093;
    return config;
}

int poseidon_transport_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    switch (transport->type) {
    case POSEIDON_TRANSPORT_UNIX_SOCKET:
        return poseidon_transport_unix_start(transport);
    case POSEIDON_TRANSPORT_TCP:
        return poseidon_transport_tcp_start(transport);
    case POSEIDON_TRANSPORT_WEBSOCKET:
        return poseidon_transport_ws_start(transport);
    case POSEIDON_TRANSPORT_WEBTRANSPORT:
        return poseidon_transport_webtransport_start(transport);
    case POSEIDON_TRANSPORT_QUIC:
        return poseidon_transport_quic_start(transport);
#ifdef __ANDROID__
    case POSEIDON_TRANSPORT_BINDER:
        return poseidon_transport_binder_start(transport);
#endif
#ifdef __APPLE__
    case POSEIDON_TRANSPORT_XPC:
        return poseidon_transport_xpc_start(transport);
#endif
    default:
        log_error("poseidon_transport_start: unknown transport type %d",
                  (int)transport->type);
        return -1;
    }
}

int poseidon_transport_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    switch (transport->type) {
    case POSEIDON_TRANSPORT_UNIX_SOCKET:
        return poseidon_transport_unix_stop(transport);
    case POSEIDON_TRANSPORT_TCP:
        return poseidon_transport_tcp_stop(transport);
    case POSEIDON_TRANSPORT_WEBSOCKET:
        return poseidon_transport_ws_stop(transport);
    case POSEIDON_TRANSPORT_WEBTRANSPORT:
        return poseidon_transport_webtransport_stop(transport);
    case POSEIDON_TRANSPORT_QUIC:
        return poseidon_transport_quic_stop(transport);
#ifdef __ANDROID__
    case POSEIDON_TRANSPORT_BINDER:
        return poseidon_transport_binder_stop(transport);
#endif
#ifdef __APPLE__
    case POSEIDON_TRANSPORT_XPC:
        return poseidon_transport_xpc_stop(transport);
#endif
    default:
        return 0;
    }
}

int poseidon_transport_send(poseidon_transport_t* transport,
                            int client_id,
                            const uint8_t* data,
                            size_t len) {
    if (transport == NULL) return -1;

    switch (transport->type) {
    case POSEIDON_TRANSPORT_UNIX_SOCKET:
        return poseidon_transport_unix_send(transport, client_id, data, len);
    case POSEIDON_TRANSPORT_TCP:
        return poseidon_transport_tcp_send(transport, client_id, data, len);
    case POSEIDON_TRANSPORT_WEBSOCKET:
        return poseidon_transport_ws_send(transport, client_id, data, len);
    case POSEIDON_TRANSPORT_WEBTRANSPORT:
        return poseidon_transport_webtransport_send(transport, client_id, data,
                                                    len);
    case POSEIDON_TRANSPORT_QUIC:
        return poseidon_transport_quic_send(transport, client_id, data, len);
#ifdef __ANDROID__
    case POSEIDON_TRANSPORT_BINDER:
        return poseidon_transport_binder_send(transport, client_id, data, len);
#endif
#ifdef __APPLE__
    case POSEIDON_TRANSPORT_XPC:
        return poseidon_transport_xpc_send(transport, client_id, data, len);
#endif
    default:
        return -1;
    }
}

void poseidon_transport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    switch (transport->type) {
    case POSEIDON_TRANSPORT_UNIX_SOCKET:
        poseidon_transport_unix_destroy(transport);
        return;
    case POSEIDON_TRANSPORT_TCP:
        poseidon_transport_tcp_destroy(transport);
        return;
    case POSEIDON_TRANSPORT_WEBSOCKET:
        poseidon_transport_ws_destroy(transport);
        return;
    case POSEIDON_TRANSPORT_WEBTRANSPORT:
        poseidon_transport_webtransport_destroy(transport);
        return;
    case POSEIDON_TRANSPORT_QUIC:
        poseidon_transport_quic_destroy(transport);
        return;
#ifdef __ANDROID__
    case POSEIDON_TRANSPORT_BINDER:
        poseidon_transport_binder_destroy(transport);
        return;
#endif
#ifdef __APPLE__
    case POSEIDON_TRANSPORT_XPC:
        poseidon_transport_xpc_destroy(transport);
        return;
#endif
    default:
        log_error("poseidon_transport_destroy: unknown transport type %d",
                  (int)transport->type);
        return;
    }
}
