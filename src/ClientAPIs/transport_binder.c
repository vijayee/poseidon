//
// Created by victor on 4/22/26.
//
// Android Binder transport — compiled only under __ANDROID__.
// Bridges Binder IPC callbacks to the poseidon_transport_t interface.
//

#ifdef __ANDROID__

#include "transport.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include <string.h>
#include <pthread.h>

typedef struct {
    pd_loop_t* loop;
} binder_transport_data_t;

static int binder_transport_start(poseidon_transport_t* self) {
    (void)self;
    log_info("binder transport: starting (Android only)");
    // On Android, the Binder service is registered via JNI.
    // The Java side calls back into native code via the on_message callback.
    self->running = true;
    return 0;
}

static int binder_transport_stop(poseidon_transport_t* self) {
    if (!self->running) return 0;
    self->running = false;
    return 0;
}

static int binder_transport_send(poseidon_transport_t* self, int client_id,
                                const uint8_t* data, size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    // On Android, send is performed via Binder reply parcels.
    // The JNI bridge handles this.
    return -1;
}

poseidon_transport_t* poseidon_transport_binder_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    if (service_name == NULL || manager == NULL) return NULL;

    poseidon_transport_t* transport = get_clear_memory(sizeof(poseidon_transport_t));
    if (transport == NULL) return NULL;

    binder_transport_data_t* data = get_clear_memory(sizeof(binder_transport_data_t));
    if (data == NULL) {
        free(transport);
        return NULL;
    }

    transport->name = service_name;
    transport->type = POSEIDON_TRANSPORT_BINDER;
    transport->manager = manager;
    transport->loop = data;
    transport->start = binder_transport_start;
    transport->stop = binder_transport_stop;
    transport->send = binder_transport_send;
    platform_lock_init(&transport->lock);

    return transport;
}

#endif // __ANDROID__