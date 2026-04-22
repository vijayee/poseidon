//
// Created by victor on 4/22/26.
//
// iOS XPC transport — compiled only on Apple platforms.
// Bridges XPC callbacks to the poseidon_transport_t interface.
//

#if defined(__APPLE__)

#include "transport.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>

typedef struct {
    void* xpc_connection; // xpc_connection_t — void* to avoid XPC headers in C
} xpc_transport_data_t;

static int xpc_transport_start(poseidon_transport_t* self) {
    (void)self;
    log_info("xpc transport: starting (macOS/iOS only)");
    // On Apple platforms, XPC connections are managed by launchd.
    // The XPC listener callback dispatches to on_message.
    self->running = true;
    return 0;
}

static int xpc_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;
    self->running = false;
    return 0;
}

static int xpc_transport_send(poseidon_transport_t* self, int client_id,
                               const uint8_t* data, size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    // On Apple platforms, send is performed via XPC reply dictionaries.
    return -1;
}

poseidon_transport_t* poseidon_transport_xpc_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    if (service_name == NULL || manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    xpc_transport_data_t* data = get_clear_memory(sizeof(xpc_transport_data_t));
    if (data == NULL) {
        free(transport);
        return NULL;
    }

    transport->name = service_name;
    transport->type = POSEIDON_TRANSPORT_XPC;
    transport->manager = manager;
    transport->loop = data;
    transport->start = xpc_transport_start;
    transport->stop = xpc_transport_stop;
    transport->send = xpc_transport_send;
    platform_lock_init(&transport->lock);

    return transport;
}

#endif // __APPLE__