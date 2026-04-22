//
// Created by victor on 4/22/26.
//
// QUIC transport for ClientAPI — uses msquic singleton for connections.
// Currently a stub that returns errors. Full implementation requires
// QUIC listener, stream, and connection callback handlers.
//

#include "transport.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>

// ============================================================================
// QUIC TRANSPORT — STUB IMPLEMENTATION
//
// Full implementation requires:
// 1. msquic ListenerOpen/ListenerStart with ALPN "poseidon_client"
// 2. Connection callbacks for QUIC connections
// 3. Stream callbacks for bi-directional data flow
// 4. CBOR frame serialization/deserialization on streams
// 5. Thread-safe send via stream write
// ============================================================================

static int quic_transport_start(poseidon_transport_t* self) {
    (void)self;
    log_error("quic transport: not implemented");
    return -1;
}

static int quic_transport_stop(poseidon_transport_t* self) {
    (void)self;
    return -1;
}

static int quic_transport_send(poseidon_transport_t* self, int client_id,
                                const uint8_t* data, size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    return -1;
}

poseidon_transport_t* poseidon_transport_quic_create(uint16_t port,
                                                      poseidon_channel_manager_t* manager) {
    if (manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    transport->name = (const char*)(uintptr_t)port;
    transport->type = POSEIDON_TRANSPORT_QUIC;
    transport->manager = manager;
    transport->start = quic_transport_start;
    transport->stop = quic_transport_stop;
    transport->send = quic_transport_send;
    platform_lock_init(&transport->lock);

    log_warn("quic transport: created as stub (not yet implemented)");
    return transport;
}