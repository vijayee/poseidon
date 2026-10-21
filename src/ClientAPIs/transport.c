//
// transport.c
//
// Shared transport entry points that are not specific to any one
// backend: the default configuration and the generic destroy.
//
// The generic destroy exists because the vtable-shaped
// poseidon_transport_t still carries a `destroy` hook that a backend
// may install to override the default teardown (the QUIC backend
// does, because it has to release its msquic registration). When no
// hook is installed, the generic path stops the transport if it is
// running and releases the shared lock.
//
// Once every backend has moved to the actor-shaped transport body the
// generic destroy becomes unnecessary: each backend's own destroy
// knows exactly what it owns. It is kept here until then so that the
// TCP, WebSocket, QUIC and WebTransport backends keep linking.
//

#include "transport.h"

#include <string.h>

poseidon_transport_config_t poseidon_transport_config_defaults(void) {
    poseidon_transport_config_t config;
    memset(&config, 0, sizeof(config));
    config.enable_unix = true;
    config.unix_socket_path = "/var/run/poseidond.sock";
    config.tcp_port = 9090;
    config.ws_port = 9091;
    config.quic_port = 9092;
    config.wt_port = 9093;
    return config;
}

void poseidon_transport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    if (transport->destroy != NULL) {
        transport->destroy(transport);
        return;
    }

    if (transport->running && transport->stop != NULL) {
        transport->stop(transport);
    }

    if (transport->lock != NULL) {
        platform_mutex_destroy(transport->lock);
        transport->lock = NULL;
    }

    free(transport);
}
