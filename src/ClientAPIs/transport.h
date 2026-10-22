//
// Created by victor on 4/22/26.
//
// The public transport header.
//
// poseidon_transport_t is the actor-shaped transport body: every
// backend fills one in from its poseidon_transport_<backend>_create()
// factory and binds the actor with actor_init(). There is no vtable
// any more — the backend's own start / stop / send / destroy functions
// are reached through the generic adapters declared at the bottom of
// this header, which dispatch on the body's type field.
//

#ifndef POSEIDON_TRANSPORT_H
#define POSEIDON_TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../Actor/actor.h"
#include "client_session.h"
#include "../Platform/platform_thread.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include "../Util/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TRANSPORT TYPES
// ============================================================================

typedef enum {
    POSEIDON_TRANSPORT_UNIX_SOCKET,
    POSEIDON_TRANSPORT_TCP,
    POSEIDON_TRANSPORT_WEBSOCKET,
    POSEIDON_TRANSPORT_QUIC,
    POSEIDON_TRANSPORT_WEBTRANSPORT,
    POSEIDON_TRANSPORT_BINDER,
    POSEIDON_TRANSPORT_XPC
} poseidon_transport_type_t;

// ============================================================================
// TRANSPORT CONFIG
// ============================================================================

typedef struct poseidon_transport_config_t {
    bool enable_unix;
    bool enable_tcp;
    bool enable_ws;
    bool enable_quic;
    bool enable_wt;
    bool enable_binder;
    bool enable_xpc;
    const char* unix_socket_path;
    uint16_t tcp_port;
    uint16_t ws_port;
    uint16_t quic_port;
    uint16_t wt_port;
    const char* tls_cert_path;
    const char* tls_key_path;
} poseidon_transport_config_t;

poseidon_transport_config_t poseidon_transport_config_defaults(void);

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

// Channel manager is referenced by the transport for session registration.
// Pulling channel_manager.h directly would drag cbor.h into every backend
// translation unit, so we forward-declare and let backends include the
// full header only where they actually call into the manager API.
struct poseidon_channel_manager_t;

// Per-backend connection types. The transport's connections vec stores
// them as void*; the backends cast at the point of use. The forward decls
// keep this header self-contained without dragging all seven
// <backend>_connection.h headers into every translation unit that
// includes it.
struct poseidon_unix_connection_t;
struct poseidon_tcp_connection_t;
struct poseidon_ws_connection_t;
struct poseidon_wt_connection_t;
struct poseidon_quic_connection_t;
struct poseidon_binder_connection_t;
struct poseidon_xpc_connection_t;

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
// included transitively through ../Platform/platform_thread.h above, so the
// typedef is in scope here; no forward decl is needed.

// Backends that need to drop a watcher from outside the loop thread call
// destroy_stack_push and let the loop-thread destroy_stack_drain (called
// from the bottom of the listener dispatch) do the actual pd_watcher_destroy.
// The mutex is created in destroy_stack_init and torn down in
// destroy_stack_destroy. The implementations live in
// transport_destroy_stack.c.
void destroy_stack_init(destroy_stack_t* stack);
void destroy_stack_destroy(destroy_stack_t* stack);
void destroy_stack_push(destroy_stack_t* stack, pd_watcher_t* watcher);
void destroy_stack_drain(destroy_stack_t* stack);

// ============================================================================
// TRANSPORT INTERFACE
// ============================================================================

// The transport body is defined below; the callback typedef needs the
// name first.
typedef struct poseidon_transport_t poseidon_transport_t;

// The user-supplied on_message callback receives a pointer to the
// transport body so that the connection dispatch can pass its own
// transport up directly.
typedef void (*poseidon_transport_on_message_cb)(poseidon_transport_t* transport,
                                                  int client_id,
                                                  const uint8_t* data,
                                                  size_t len);

// The actor-shaped transport body. Backends populate this from their
// poseidon_transport_<backend>_create() factory and bind the actor with
// actor_init(self, self, _<backend>_dispatch, self->pool).
//
// Field ordering note: actor_t is first so that the public convention
// "the struct IS the actor" (used by the per-connection actors and the
// session actor) is uniform across the transport.
struct poseidon_transport_t {
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

    // Discriminator + name for logs and asserts. The type is also what
    // the generic adapters below dispatch on.
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

// ============================================================================
// GENERIC TRANSPORT LIFECYCLE
// ============================================================================
//
// The backends used to expose their start / stop / send / destroy through
// a vtable on the transport. The vtable is gone: a transport is a plain
// body now, and the generic adapters below dispatch on its type field to
// reach the backend's own implementation.
//
// The per-backend functions (poseidon_transport_<backend>_start and
// friends) are declared in the backend's own header and may be called
// directly when the backend is known at compile time.

/**
 * Starts the transport's I/O thread or server.
 *
 * @return 0 on success, -1 on failure
 */
int poseidon_transport_start(poseidon_transport_t* transport);

/**
 * Stops the transport's I/O thread or server and waits for it to exit.
 */
int poseidon_transport_stop(poseidon_transport_t* transport);

/**
 * Sends a payload to the client identified by client_id. The transport
 * frames the payload and queues it on the client's connection actor.
 *
 * @return 0 on success, -1 if the client is unknown
 */
int poseidon_transport_send(poseidon_transport_t* transport,
                            int client_id,
                            const uint8_t* data,
                            size_t len);

/**
 * Stops the transport and releases every resource it owns. The transport
 * pointer is invalid after this call.
 */
void poseidon_transport_destroy(poseidon_transport_t* transport);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_TRANSPORT_H */
