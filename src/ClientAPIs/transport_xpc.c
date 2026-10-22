//
// transport_xpc.c
//
// iOS XPC transport — compiled only on Apple platforms.
//
// XPC connections are managed by launchd and the listener callback
// dispatches to on_message. What this file owns is the actor-shaped
// transport body every backend shares, so the platform bridge starts
// from the same place as the other backends.
//
// There is no connection path yet: the bridge has no accept callback
// and no per-connection actor, so the connection table stays empty
// until the platform work arrives. The shape to follow then is the one
// of the Unix and TCP backends.
//

#if defined(__APPLE__)

#include "transport_xpc.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Util/vec.h"
#include "transport_internal.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The XPC-specific state — the service name and
// the connection lock — lives here.
//
// Two views of the transport exist side by side:
//   - base is the actor-shaped body every backend shares. It is the
//     FIRST member, so a pointer to it IS a pointer to this struct and
//     the backend can be recovered from the internal body alone.
//   - public is the vtable-shaped view that transport.h still
//     declares. It is reached from the public pointer through
//     offsetof.

typedef struct poseidon_xpc_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_internal_t base;

    // The vtable-shaped public view.
    poseidon_transport_t public;

    // Copy of the XPC service name. Freed by the destroy path.
    char* service_name;

    // Guards the connection table. The platform bridge adds
    // connections; the send path looks them up; the destroy path walks
    // them.
    platform_mutex_t* conn_lock;
} poseidon_xpc_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void _xpc_transport_dispatch(void* state, message_t* msg);
static int _xpc_vtable_send(poseidon_transport_t* self,
                             int client_id,
                             const uint8_t* data,
                             size_t len);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

// Recovers the backend struct from the vtable-shaped public pointer.
static poseidon_xpc_transport_t* _xpc_from_public(poseidon_transport_t* transport) {
    return (poseidon_xpc_transport_t*)((char*)transport -
        offsetof(poseidon_xpc_transport_t, public));
}

// ============================================================================
// DISPATCH
// ============================================================================
//
// The transport body is actor-shaped: it owns an actor_t that carries
// the listener-level messages. The XPC backend does not receive
// cross-actor messages yet, so the dispatch is a no-op that keeps the
// actor contract satisfied.

static void _xpc_transport_dispatch(void* state, message_t* msg) {
    (void)state;
    (void)msg;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_xpc_create(
    const char* service_name,
    poseidon_channel_manager_t* manager) {
    if (service_name == NULL || manager == NULL) return NULL;

    poseidon_xpc_transport_t* transport =
        (poseidon_xpc_transport_t*)get_clear_memory(
            sizeof(poseidon_xpc_transport_t));
    if (transport == NULL) return NULL;

    poseidon_transport_internal_t* base = &transport->base;

    base->manager = manager;
    base->on_message = NULL;
    base->type = POSEIDON_TRANSPORT_XPC;
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
    actor_init(&base->actor, base, _xpc_transport_dispatch, base->pool);

    // The vtable-shaped public view lives in the same allocation as
    // the actor-shaped body, side by side.
    transport->public.name = transport->service_name;
    transport->public.type = POSEIDON_TRANSPORT_XPC;
    transport->public.thread = NULL;
    transport->public.loop = NULL;
    transport->public.manager = manager;
    transport->public.on_message = NULL;
    transport->public.ctx = NULL;
    transport->public.lock = transport->conn_lock;
    transport->public.running = false;
    transport->public.start = poseidon_transport_xpc_start;
    transport->public.stop = poseidon_transport_xpc_stop;
    transport->public.send = _xpc_vtable_send;
    transport->public.destroy = poseidon_transport_xpc_destroy;

    return &transport->public;
}

int poseidon_transport_xpc_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_xpc_transport_t* xpc_transport = _xpc_from_public(transport);
    if (ATOMIC_LOAD(&xpc_transport->base.running)) {
        return 0;
    }

    // On Apple platforms, XPC connections are managed by launchd. The
    // XPC listener callback dispatches to on_message.
    log_info("xpc transport: starting (macOS/iOS only)");

    ATOMIC_STORE(&xpc_transport->base.running, 1);
    xpc_transport->public.running = true;
    return 0;
}

int poseidon_transport_xpc_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_xpc_transport_t* xpc_transport = _xpc_from_public(transport);
    if (!ATOMIC_LOAD(&xpc_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&xpc_transport->base.running, 0);
    xpc_transport->public.running = false;
    return 0;
}

void poseidon_transport_xpc_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_xpc_transport_t* xpc_transport = _xpc_from_public(transport);
    poseidon_transport_internal_t* base = &xpc_transport->base;

    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_xpc_stop(transport);
    }

    // The shared body is actor-shaped: destroy its actor before the
    // transport's memory goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);
    vec_deinit(&base->connections);

    if (xpc_transport->conn_lock != NULL) {
        platform_mutex_destroy(xpc_transport->conn_lock);
        xpc_transport->conn_lock = NULL;
    }

    free(xpc_transport->service_name);
    xpc_transport->service_name = NULL;
    base->name = NULL;

    free(xpc_transport);
}

// ============================================================================
// SEND
// ============================================================================

// The reply dictionary is written by the XPC bridge, so there is
// nothing to enqueue here yet.
static int _xpc_vtable_send(poseidon_transport_t* self,
                             int client_id,
                             const uint8_t* data,
                             size_t len) {
    (void)self;
    (void)client_id;
    (void)data;
    (void)len;
    return -1;
}

int poseidon_transport_xpc_send(poseidon_transport_t* transport,
                                int client_id,
                                const uint8_t* data,
                                size_t len) {
    if (transport == NULL) return -1;
    return _xpc_vtable_send(transport, client_id, data, len);
}

#endif // __APPLE__
