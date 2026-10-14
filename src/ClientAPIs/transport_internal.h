//
// transport_internal.h
//
// Internal types shared by every poseidon transport backend. This header is
// NOT part of the public ClientAPIs surface; it is consumed by the
// <backend>_transport.c / <backend>_connection.c split that each backend
// (Unix, TCP, WS, QUIC, WT, BINDER, XPC) is being migrated to as the
// vtable-style poseidon_transport_t in transport.h is replaced by an
// actor-model implementation.
//
// The struct introduced here, poseidon_transport_internal_t, is the new
// actor-shaped transport body. Backends populate it from their
// poseidon_transport_<backend>_create() factory and bind it to the
// actor-model dispatch via actor_init(). Per-backend listener-private
// state (e.g. the quic_transport_data_t* that holds the HQUIC listener
// handle) is held out-of-band in a backend-specific struct and reached
// from the dispatch callback by upcast; the cross-backend fields on this
// struct are the ones every backend needs.
//
// This header is added before the per-backend splits so the
// <backend>_connection.h headers that the listener dispatch callbacks
// need to refer to (for vec_t(<backend>_connection_t*) connections, once
// they exist) can be added without a header churn across later commits.
// The commit that rewrites transport.h to drop the vtable is the same
// one that will rename this type to poseidon_transport_t and turn it
// into the public struct body.
//

#ifndef POSEIDON_TRANSPORT_INTERNAL_H
#define POSEIDON_TRANSPORT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../Actor/actor.h"
#include "../Platform/platform_thread.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Util/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

// Forward-declared in transport.h. The public struct body for the vtable
// shape lives in transport.h today; the actor-shaped body is here. Both
// names coexist while the backends are still using the vtable interface.
// transport.h is included below; this forward decl is intentionally
// omitted to avoid a redundant declaration.

// Channel manager is referenced by the transport for session registration.
// Pulling channel_manager.h directly would drag cbor.h into every backend
// translation unit, so we forward-declare and let backends include the
// full header only where they actually call into the manager API.
struct poseidon_channel_manager_t;

// Per-backend connection types. The listener's vec_t below stores these
// as void*; backends cast at the point of use. The forward decls keep the
// header self-contained without dragging the <backend>_connection.h
// headers (which will be added in later commits) into every translation
// unit that includes this internal header.
struct unix_connection_t;
struct tcp_connection_t;
struct ws_connection_t;
struct quic_connection_t;
struct wt_connection_t;
struct binder_connection_t;
struct xpc_connection_t;

// Public transport-type enum and callback typedef are pulled from
// transport.h so backends can write the type / on_message fields on
// the internal struct without re-declaring them. This is the only
// dependency on the public transport header; once commit 9 rewrites
// transport.h to drop the vtable, the includes settle naturally.
#include "transport.h"

// ============================================================================
// DESTROY STACK
// ============================================================================
//
// LIFO stack of watchers (and other I/O resources) that must be torn down
// on the loop thread. Callers that detect a teardown condition from
// outside the loop (e.g. a session actor deciding to disconnect) push a
// destroy_node_t onto the stack under destroy_lock, and the loop drains
// the stack at the bottom of each pd_loop_run_once iteration.
typedef struct destroy_node_t {
    pd_watcher_t* watcher;
    struct destroy_node_t* next;
} destroy_node_t;

typedef struct {
    platform_mutex_t* lock;
    destroy_node_t* head;
} destroy_stack_t;

// pd_watcher_t is typedef'd in poll-dancer/types.h. The full header is
// included transitively through ../Platform/platform.h above, so the
// typedef is in scope here; no forward decl is needed.

// Backends that need to drop a watcher from outside the loop thread call
// destroy_stack_push and let the loop-thread destroy_stack_drain (called
// from the bottom of the listener dispatch) do the actual pd_watcher_destroy.
// The mutex is created in destroy_stack_init and torn down in
// destroy_stack_destroy. Implementations land in transport_destroy_stack.c
// during the Unix split (commit 2 of the transport migration); this
// header is declarations only so the helpers do not have to be recompiled
// in every translation unit that includes it.
void destroy_stack_init(destroy_stack_t* stack);
void destroy_stack_destroy(destroy_stack_t* stack);
void destroy_stack_push(destroy_stack_t* stack, pd_watcher_t* watcher);
void destroy_stack_drain(destroy_stack_t* stack);

// ============================================================================
// TRANSPORT INTERNAL STRUCT
// ============================================================================
//
// The new actor-shaped transport body. Backends populate this from their
// poseidon_transport_<backend>_create() factory and bind the actor with
// actor_init(self, self, _<backend>_dispatch, self->pool).
//
// Field ordering note: actor_t is first so that the public convention
// "the struct IS the actor" (used by the per-connection actors and the
// session actor) is uniform across the transport.
struct poseidon_transport_internal_t {
    actor_t actor;

    // I/O loop and the thread that drives it. Backends create the
    // pd_loop_t in their _create and pass the loop to the platform
    // thread entry function as the user data.
    pd_loop_t* loop;
    platform_thread_t* thread;

    // Lifecycle flags. running is set true after the loop and thread are
    // up; it is observed by the destroy path to know whether to push a
    // watcher onto the destroy stack or to tear down directly.
    ATOMIC(uint8_t) running;
    ATOMIC(size_t) active_connections;

    // Shared channel manager. Backends register / unregister sessions
    // with the manager on connection accept and close.
    struct poseidon_channel_manager_t* manager;

    // User-supplied callback for inbound client messages, set at create
    // time and invoked by the backend's dispatch when a frame is decoded.
    poseidon_transport_on_message_cb on_message;

    // Discriminator + name for logs and asserts.
    poseidon_transport_type_t type;
    const char* name;

    // Per-backend connections. Stored as void* because the listener holds
    // <backend>_connection_t pointers (one of the seven forward decls
    // above) and each <backend>_connection.h casts as needed. The vec
    // is owned by the transport and freed in the listener's destroy.
    vec_void_t connections;

    // Worker pool for off-loading work off the I/O thread. NULL means
    // dispatch on the caller thread (i.e. inline actor_run).
    scheduler_pool_t* pool;

    // Destroy stack. Only the loop thread mutates head without the lock;
    // the lock serialises pushes from other threads. See destroy_stack_t
    // above for the contract.
    destroy_stack_t destroy_stack;
};

typedef struct poseidon_transport_internal_t poseidon_transport_internal_t;

#ifdef __cplusplus
}
#endif

#endif //POSEIDON_TRANSPORT_INTERNAL_H
