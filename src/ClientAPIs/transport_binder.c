//
// transport_binder.c
//
// Android Binder transport — compiled only under __ANDROID__.
//
// The Binder service is registered via JNI and the Java side calls back
// into native code. What this file owns is the actor-shaped transport
// body every backend shares, so the platform bridge starts from the
// same place as the other backends.
//
// There is no connection path yet: the bridge has no accept callback
// and no per-connection actor, so the connection table stays empty
// until the platform work arrives. The shape to follow then is the one
// of the Unix and TCP backends.
//

#ifdef __ANDROID__

#include "transport_binder.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Util/vec.h"
#include "transport.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The Binder-specific state — the service name
// and the connection lock — lives here.
//
// The shared body is the FIRST member, so a pointer to it IS a pointer
// to this struct: the backend is recovered from the transport body with
// a plain upcast.

typedef struct poseidon_binder_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_t base;


    // Copy of the Binder service name. Freed by the destroy path.
    char* service_name;

    // Guards the connection table. The platform bridge adds
    // connections; the send path looks them up; the destroy path walks
    // them.
    platform_mutex_t* conn_lock;
} poseidon_binder_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void _binder_transport_dispatch(void* state, message_t* msg);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

static poseidon_binder_transport_t* _binder_from_base(poseidon_transport_t* transport) {
    return (poseidon_binder_transport_t*)((char*)transport -
        offsetof(poseidon_binder_transport_t, public));
}

// ============================================================================
// DISPATCH
// ============================================================================
//
// The transport body is actor-shaped: it owns an actor_t that carries
// the listener-level messages. The Binder backend does not receive
// cross-actor messages yet, so the dispatch is a no-op that keeps the
// actor contract satisfied.

static void _binder_transport_dispatch(void* state, message_t* msg) {
    (void)state;
    (void)msg;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_binder_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    if (service_name == NULL || manager == NULL) return NULL;

    poseidon_binder_transport_t* transport =
        (poseidon_binder_transport_t*)get_clear_memory(
            sizeof(poseidon_binder_transport_t));
    if (transport == NULL) return NULL;

    poseidon_transport_t* base = &transport->base;

    base->manager = manager;
    base->on_message = NULL;
    base->type = POSEIDON_TRANSPORT_BINDER;
    base->name = NULL;
    base->thread = NULL;
    base->pool = NULL;
    base->loop = NULL;
    vec_init(&base->connections);
    ATOMIC_STORE(&base->running, 0);
    ATOMIC_STORE(&base->active_connections, 0);
    destroy_stack_init(&base->destroy_stack);

    transport->service_name = strdup(service_name);
    transport->conn_lock = platform_mutex_create();
    if (transport->service_name == NULL || transport->conn_lock == NULL) {
        if (transport->conn_lock != NULL) {
            platform_mutex_destroy(transport->conn_lock);
        }
        free(transport->service_name);
        destroy_stack_destroy(&base->destroy_stack);
        free(transport);
        return NULL;
    }
    base->name = transport->service_name;

    // The transport body is actor-shaped: bind it to the scheduler
    // pool the manager uses, so the transport's actor lives on the
    // same pool as the channel manager and the client sessions.
    base->pool = manager->actor.pool;
    actor_init(&base->actor, base, _binder_transport_dispatch, base->pool);


    return &transport->base;
}

int poseidon_transport_binder_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_binder_transport_t* binder_transport = _binder_from_base(transport);
    if (ATOMIC_LOAD(&binder_transport->base.running)) {
        return 0;
    }

    // On Android, the Binder service is registered via JNI. The Java
    // side calls back into native code through the on_message
    // callback.
    log_info("binder transport: starting (Android only)");

    ATOMIC_STORE(&binder_transport->base.running, 1);
    return 0;
}

int poseidon_transport_binder_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_binder_transport_t* binder_transport = _binder_from_base(transport);
    if (!ATOMIC_LOAD(&binder_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&binder_transport->base.running, 0);
    return 0;
}

void poseidon_transport_binder_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_binder_transport_t* binder_transport = _binder_from_base(transport);
    poseidon_transport_t* base = &binder_transport->base;

    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_binder_stop(transport);
    }

    // The shared body is actor-shaped: destroy its actor before the
    // transport's memory goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);
    vec_deinit(&base->connections);

    if (binder_transport->conn_lock != NULL) {
        platform_mutex_destroy(binder_transport->conn_lock);
        binder_transport->conn_lock = NULL;
    }

    free(binder_transport->service_name);
    binder_transport->service_name = NULL;
    base->name = NULL;

    free(binder_transport);
}

// ============================================================================
// SEND
// ============================================================================

// The reply parcel is written by the JNI bridge, so there is nothing
// to enqueue here yet.
static int _binder_send(poseidon_transport_t* self,
                               int client_id,
                               const uint8_t* data,
                               size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    return -1;
}

int poseidon_transport_binder_send(poseidon_transport_t* transport,
                                   int client_id,
                                   const uint8_t* data,
                                   size_t len) {
    if (transport == NULL) return -1;
    return _binder_send(transport, client_id, data, len);
}

#endif // __ANDROID__
