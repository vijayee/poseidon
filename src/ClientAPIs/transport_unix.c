//
// transport_unix.c
//
// Unix domain socket transport backend.
//
// The transport owns a poll-dancer event loop driven by one platform
// thread. The listening socket is registered with the loop through an
// accept watcher; every accepted socket becomes a per-connection actor
// (unix_connection.c) that runs on the transport's scheduler pool.
//
// Threading model:
//   - the I/O thread runs the poll-dancer loop and is the only thread
//     that touches the accept watcher and the loop's watcher registry;
//   - the scheduler pool workers run the per-connection actors, which
//     own frame decoding and the write path;
//   - the caller's thread creates and destroys the transport.
//
// Because a watcher may only be registered or unregistered on the loop
// thread, a connection that needs its watcher torn down pushes the
// watcher onto the transport's destroy stack and the I/O thread drains
// that stack at the bottom of every loop iteration. The socket
// descriptor is closed only after the watcher has been unregistered, so
// the descriptor number cannot be recycled under a live registration.
//
// Wire protocol: 4-byte big-endian length prefix followed by the
// payload. The connection actor strips the prefix and hands the payload
// to the on_message callback; the send path adds the prefix back.
//

#include "transport_unix.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Channel/channel_manager.h"
#include "client_session.h"
#include "transport.h"
#include "unix_connection.h"

#include <poll-dancer/poll-dancer.h>

#include <errno.h>
#include <endian.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define UNIX_FRAME_HEADER_SIZE 4

// How long the I/O thread blocks in epoll_wait before it re-checks the
// running flag and drains the destroy stack. 100ms is short enough that
// a stop request is honoured promptly and long enough that an idle
// transport does not spin.
#define UNIX_LOOP_TICK_MS 100

// Session ids are handed to client_session_create. They only have to be
// unique among the live sessions of one transport.
static uint64_t s_next_session_id = 1;

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The Unix-specific state — the listening socket,
// the accept watcher, the socket path and the user context — lives
// here.
//
// The shared body is the FIRST member, so a pointer to it IS a pointer
// to this struct: the backend is recovered from the transport body with
// a plain upcast.

typedef struct poseidon_unix_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_t base;


    // Listening socket and its accept watcher. The watcher is created
    // on the I/O thread, right before the loop starts running.
    platform_socket_t* listen_sock;
    pd_watcher_t* listen_watcher;

    // Copy of the socket path. Freed by the destroy path, after
    // platform_local_cleanup has unlinked the socket file.
    char* socket_path;

    // Opaque user context handed to the create function.
    void* ctx;

    // Guards the connection table below. The accept callback (I/O
    // thread) adds connections; the destroy path (caller thread) walks
    // them. The scheduler pool workers never touch the table.
    platform_mutex_t* conn_lock;

    // Scheduler pool owned by this transport. Set only when the
    // transport had to create one of its own because no channel
    // manager supplied one; the transport then owns its lifecycle.
    scheduler_pool_t* own_pool;
} poseidon_unix_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop,
                             pd_watcher_t* watcher,
                             pd_event_t events,
                             void* user_data);
static void _unix_transport_dispatch(void* state, message_t* msg);
static int _unix_transport_send_to_client(poseidon_transport_t* transport,
                                           int client_id,
                                           const uint8_t* data,
                                           size_t len);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

// Recovers the backend struct from the transport body. base is the
// first member, so this is a plain upcast.
static poseidon_unix_transport_t* _unix_from_base(poseidon_transport_t* transport) {
    return (poseidon_unix_transport_t*)transport;
}

// ============================================================================
// HELPERS
// ============================================================================

// Finds the connection whose socket descriptor is client_id. The
// descriptor is the connection's identity on the wire: the client
// library reports it back on every reply.
static poseidon_unix_connection_t* _find_connection_by_fd(
    poseidon_unix_transport_t* transport,
    int client_id) {
    for (int i = 0; i < transport->base.connections.length; i++) {
        poseidon_unix_connection_t* conn =
            (poseidon_unix_connection_t*)transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (platform_socket_fd(conn->sock) == client_id) {
            return conn;
        }
    }
    return NULL;
}

// ============================================================================
// DISPATCH
// ============================================================================
//
// The transport body is actor-shaped: it owns an actor_t that carries
// the listener-level messages. The Unix backend does not currently
// need to receive cross-actor messages (the per-connection actors
// talk to their peers directly), so the dispatch is a no-op that keeps
// the actor contract satisfied. It exists so that the transport can be
// bound to the scheduler pool like every other actor in the codebase,
// and so that a future listener-level message has a home.

static void _unix_transport_dispatch(void* state, message_t* msg) {
    (void)state;
    (void)msg;
}

// ============================================================================
// ACCEPT CALLBACK
// ============================================================================
//
// Runs on the I/O thread. Accepts one connection, binds a client
// session to it and registers the connection actor.

static void _accept_callback(pd_loop_t* loop,
                             pd_watcher_t* watcher,
                             pd_event_t events,
                             void* user_data) {
    (void)loop;
    (void)watcher;

    poseidon_unix_transport_t* transport = (poseidon_unix_transport_t*)user_data;
    if (transport == NULL) return;

    if (!(events & PD_EVENT_READ)) {
        return;
    }

    platform_socket_t* client_sock =
        platform_local_accept(transport->listen_sock);
    if (client_sock == NULL) {
        // A failed accept is normally transient (the peer gave up
        // between the readiness notification and the accept call).
        // Only the permanent errors are worth logging.
        if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK ||
            errno == EOPNOTSUPP) {
            log_error("unix transport: accept failed permanently (errno=%d)",
                      errno);
        }
        return;
    }

    // The accepted socket inherits the listener's blocking mode, so
    // it has to be switched explicitly before the connection's read
    // callback starts pulling from it.
    platform_socket_set_nonblocking(client_sock);

    int client_fd = platform_socket_fd(client_sock);

    // Bind a client session to the connection. The session is the
    // per-client state the channel manager registry holds.
    client_session_t* session =
        client_session_create((uint32_t)s_next_session_id++, transport->base.manager, transport->base.pool);
    if (session == NULL) {
        log_error("unix transport: failed to create client session");
        platform_socket_destroy(client_sock);
        return;
    }
    session->client_fd = client_fd;
    session->transport = &transport->base;

    if (transport->base.manager != NULL) {
        poseidon_channel_manager_register_session(transport->base.manager,
                                                   session);
    }

    poseidon_unix_connection_t* conn = NULL;
    if (!poseidon_unix_connection_create(&transport->base, client_sock,
                                         session, &conn)) {
        log_error("unix transport: failed to create connection actor");
        if (transport->base.manager != NULL) {
            poseidon_channel_manager_unregister_session(
                transport->base.manager, session);
        }
        client_session_destroy(session);
        platform_socket_destroy(client_sock);
        return;
    }

    platform_mutex_lock(transport->conn_lock);
    vec_push(&transport->base.connections, (void*)conn);
    platform_mutex_unlock(transport->conn_lock);

    ATOMIC_FETCH_ADD(&transport->base.active_connections, 1);

    // POSIX listeners stay in the listening state after an accept, so
    // there is nothing to re-arm. The call is a no-op here and exists
    // so the transport does not have to know which platform it runs on.
    platform_local_rearm(transport->listen_sock);
}

// ============================================================================
// SERVER THREAD
// ============================================================================
//
// Runs the poll-dancer loop until the transport is stopped.

static void* _server_thread(void* arg) {
    poseidon_unix_transport_t* transport = (poseidon_unix_transport_t*)arg;
    if (transport == NULL) return NULL;

    platform_thread_setup_stack();

    // The accept watcher is created here, on the I/O thread, so that
    // it is registered with the loop on the thread that runs it.
    transport->listen_watcher =
        platform_socket_watcher_create(transport->base.loop,
                                       transport->listen_sock,
                                       PD_EVENT_READ,
                                       _accept_callback,
                                       transport);
    if (transport->listen_watcher == NULL) {
        log_error("unix transport: failed to create accept watcher");
        return NULL;
    }
    pd_watcher_start(transport->listen_watcher);

    while (ATOMIC_LOAD(&transport->base.running)) {
        // Drain the destroy stack before blocking again: watchers
        // that a connection actor asked to tear down are freed here,
        // on the I/O thread, so the unregister never races a live
        // registration.
        destroy_stack_drain(&transport->base.destroy_stack);
        pd_loop_run_once(transport->base.loop, UNIX_LOOP_TICK_MS);
    }

    // One last drain: a connection actor may have pushed a watcher
    // after the loop noticed the stop flag.
    destroy_stack_drain(&transport->base.destroy_stack);

    if (transport->listen_watcher != NULL) {
        pd_watcher_stop(transport->listen_watcher);
        pd_watcher_destroy(transport->listen_watcher);
        transport->listen_watcher = NULL;
    }

    pd_loop_stop(transport->base.loop);

    return NULL;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_unix_create(const char* socket_path,
                                                      poseidon_channel_manager_t* manager,
                                                      poseidon_transport_on_message_cb on_message,
                                                      void* ctx) {
    if (socket_path == NULL) return NULL;

    poseidon_unix_transport_t* transport =
        (poseidon_unix_transport_t*)get_clear_memory(
            sizeof(poseidon_unix_transport_t));
    if (transport == NULL) return NULL;

    poseidon_transport_t* base = &transport->base;

    base->loop = pd_loop_create(NULL);
    if (base->loop == NULL) {
        free(transport);
        return NULL;
    }

    base->manager = manager;
    base->on_message = on_message;
    base->type = POSEIDON_TRANSPORT_UNIX_SOCKET;
    base->name = NULL;
    base->thread = NULL;
    base->pool = NULL;
    vec_init(&base->connections);
    ATOMIC_STORE(&base->running, 0);
    ATOMIC_STORE(&base->active_connections, 0);
    destroy_stack_init(&base->destroy_stack);

    transport->listen_sock = NULL;
    transport->listen_watcher = NULL;
    transport->ctx = ctx;
    transport->own_pool = NULL;
    transport->conn_lock = platform_mutex_create();
    if (transport->conn_lock == NULL) {
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport);
        return NULL;
    }

    transport->socket_path = strdup(socket_path);
    if (transport->socket_path == NULL) {
        platform_mutex_destroy(transport->conn_lock);
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport);
        return NULL;
    }

    // The transport body is actor-shaped: bind it to the scheduler
    // pool the manager uses, so the transport's actor lives on the
    // same pool as the channel manager and the client sessions.
    //
    // A transport without a manager still needs a pool: the
    // per-connection actors dispatch their messages through it. An
    // actor with a NULL pool is never scheduled, so its mailbox would
    // fill up and never be drained. The transport therefore creates
    // and owns a pool of its own in that case.
    if (manager != NULL) {
        base->pool = manager->actor.pool;
    } else {
        base->pool = scheduler_pool_create(1);
        if (base->pool == NULL) {
            platform_mutex_destroy(transport->conn_lock);
            destroy_stack_destroy(&base->destroy_stack);
            pd_loop_destroy(base->loop);
            free(transport->socket_path);
            free(transport);
            return NULL;
        }
        scheduler_pool_start(base->pool);
        transport->own_pool = base->pool;
    }
    actor_init(&base->actor, base, _unix_transport_dispatch, base->pool);

    // The socket inherits the process umask at bind, which locks out
    // non-root clients under a root umask. Publish a world-accessible
    // mode on the bound inode; a chmod failure keeps the daemon
    // running with the umask-derived mode rather than failing startup.
    transport->listen_sock = platform_local_listen(socket_path);
    if (transport->listen_sock == NULL) {
        log_error("unix transport: failed to listen on %s (errno=%d)",
                  socket_path, errno);
        actor_destroy(&base->actor);
        if (transport->own_pool != NULL) {
            scheduler_pool_stop(transport->own_pool);
            scheduler_pool_destroy(transport->own_pool);
        }
        platform_mutex_destroy(transport->conn_lock);
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport->socket_path);
        free(transport);
        return NULL;
    }

    if (chmod(socket_path, (mode_t)0666) != 0) {
        log_warn("unix transport: chmod(%s, 0666) failed (errno=%d); "
                 "socket keeps its umask-derived mode",
                 socket_path, errno);
    }

    platform_socket_set_nonblocking(transport->listen_sock);


    return &transport->base;
}

int poseidon_transport_unix_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_unix_transport_t* unix_transport = _unix_from_base(transport);

    if (ATOMIC_LOAD(&unix_transport->base.running)) {
        return 0;
    }

    log_info("unix transport: starting on %s", unix_transport->socket_path);

    ATOMIC_STORE(&unix_transport->base.running, 1);
    platform_thread_t* thread =
        platform_thread_create(_server_thread, unix_transport);
    if (thread == NULL) {
        ATOMIC_STORE(&unix_transport->base.running, 0);
        log_error("unix transport: failed to create I/O thread");
        return -1;
    }
    unix_transport->base.thread = thread;

    return 0;
}

int poseidon_transport_unix_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_unix_transport_t* unix_transport = _unix_from_base(transport);

    if (!ATOMIC_LOAD(&unix_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&unix_transport->base.running, 0);

    // Wake the I/O thread so it notices the stop flag without waiting
    // for the tick to expire. The wake-up is best-effort: on the
    // epoll backend it is a no-op and the thread wakes on its own
    // when the tick expires.
    pd_loop_async_send(unix_transport->base.loop, unix_transport);

    if (unix_transport->base.thread != NULL) {
        platform_thread_join(unix_transport->base.thread);
        unix_transport->base.thread = NULL;
    }

    return 0;
}

void poseidon_transport_unix_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_unix_transport_t* unix_transport = _unix_from_base(transport);
    poseidon_transport_t* base = &unix_transport->base;

    // Stop the I/O thread first: after this returns no callback can
    // fire on a connection, so the teardown below runs without a race
    // against the accept path.
    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_unix_stop(transport);
    }

    // Drain the destroy stack BEFORE tearing down the connection
    // watchers. The stack holds the watchers a connection actor asked
    // to tear down; destroying them a second time from the loop below
    // would double-free the watcher and corrupt the heap.
    destroy_stack_drain(&base->destroy_stack);

    if (unix_transport->listen_sock != NULL) {
        platform_socket_destroy(unix_transport->listen_sock);
        unix_transport->listen_sock = NULL;
    }

    // Quiesce the connection actors first. actor_destroy sets the
    // DESTROY flag (so the scheduler stops picking the actor up and
    // actor_send drops its messages), waits for any dispatch that is
    // already running to finish, and drains the mailbox.
    //
    // The order matters: the connection's fields (sock, watcher,
    // write_buffer, transport) are read and written by the actor's
    // dispatch, so they may only be touched once the actor is
    // quiesced. Tearing the connection down before the actor is
    // stopped would race the dispatch against the free.
    for (int i = 0; i < base->connections.length; i++) {
        poseidon_unix_connection_t* conn =
            (poseidon_unix_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        actor_destroy(&conn->actor);
    }

    // Wait for the rest of the pool to go quiet as well: a client
    // session actor may still be dispatching a delivery that reaches
    // back into this transport.
    if (base->pool != NULL &&
        !ATOMIC_LOAD(&base->pool->terminate)) {
        scheduler_pool_wait_for_idle(base->pool);
    }

    // Release the connection resources. The actors are quiesced, so
    // nothing touches these fields any more.
    //
    // The connections are released in reverse order so the table
    // shrinks from the tail.
    for (int i = base->connections.length - 1; i >= 0; i--) {
        poseidon_unix_connection_t* conn =
            (poseidon_unix_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;

        conn->is_closing = 1;
        if (conn->sock != NULL) {
            platform_socket_destroy(conn->sock);
            conn->sock = NULL;
        }
        conn->transport = NULL;
        if (conn->watcher != NULL) {
            pd_watcher_t* watcher = conn->watcher;
            conn->watcher = NULL;
            pd_watcher_stop(watcher);
            pd_watcher_destroy(watcher);
        }
        if (conn->write_buffer != NULL) {
            buffer_destroy(conn->write_buffer);
            conn->write_buffer = NULL;
        }
        if (conn->session != NULL) {
            // Detach the session from the transport before the
            // transport's memory goes away: a session that is still
            // registered with the manager must not reach back into a
            // freed transport when it delivers a message.
            conn->session->transport = NULL;
            client_session_cleanup_subscriptions(conn->session);
            if (base->manager != NULL) {
                poseidon_channel_manager_unregister_session(base->manager,
                                                            conn->session);
            }
            client_session_destroy(conn->session);
            conn->session = NULL;
        }
        free(conn);
    }
    vec_deinit(&base->connections);

    if (unix_transport->socket_path != NULL) {
        platform_local_cleanup(unix_transport->socket_path);
        free(unix_transport->socket_path);
        unix_transport->socket_path = NULL;
    }

    // The shared body is actor-shaped: destroy its actor before the
    // loop goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);

    if (base->loop != NULL) {
        pd_loop_destroy(base->loop);
        base->loop = NULL;
    }

    if (unix_transport->conn_lock != NULL) {
        platform_mutex_destroy(unix_transport->conn_lock);
        unix_transport->conn_lock = NULL;
    }

    // The pool the transport created for itself is stopped and
    // destroyed here. A pool that came from the channel manager is
    // owned by the manager and is left alone.
    if (unix_transport->own_pool != NULL) {
        scheduler_pool_stop(unix_transport->own_pool);
        scheduler_pool_destroy(unix_transport->own_pool);
        unix_transport->own_pool = NULL;
        base->pool = NULL;
    }

    free(unix_transport);
}

// ============================================================================
// SEND
// ============================================================================

// Frames the payload with its 4-byte big-endian length prefix and
// hands it to the connection's write actor.
static int _unix_transport_send_to_client(poseidon_transport_t* transport,
                                           int client_id,
                                           const uint8_t* data,
                                           size_t len) {
    if (transport == NULL || (data == NULL && len > 0)) return -1;

    poseidon_unix_transport_t* unix_transport = _unix_from_base(transport);

    platform_mutex_lock(unix_transport->conn_lock);
    poseidon_unix_connection_t* conn =
        _find_connection_by_fd(unix_transport, client_id);
    platform_mutex_unlock(unix_transport->conn_lock);

    if (conn == NULL) {
        return -1;
    }

    // Build the framed payload: 4 header bytes plus the body. The
    // connection's write actor owns the buffer from here on.
    buffer_t* framed = buffer_create(UNIX_FRAME_HEADER_SIZE + len);
    if (framed == NULL) {
        return -1;
    }
    uint32_t be_len = htobe32((uint32_t)len);
    memcpy(framed->data, &be_len, UNIX_FRAME_HEADER_SIZE);
    if (len > 0) {
        memcpy(framed->data + UNIX_FRAME_HEADER_SIZE, data, len);
    }

    poseidon_unix_connection_write(conn, framed);
    return 0;
}

int poseidon_transport_unix_send(poseidon_transport_t* transport,
                                  int client_id,
                                  const uint8_t* data,
                                  size_t len) {
    if (transport == NULL) return -1;
    return _unix_transport_send_to_client(transport, client_id, data, len);
}

int poseidon_transport_unix_send_to_client(
    struct poseidon_transport_t* transport,
    int client_id,
    const uint8_t* data,
    size_t len) {
    return _unix_transport_send_to_client(transport, client_id, data, len);
}

// ============================================================================
// ACCESSORS
// ============================================================================

client_session_t* poseidon_transport_unix_session_for_client_id(
    struct poseidon_transport_t* transport,
    int client_id) {
    if (transport == NULL) return NULL;

    poseidon_unix_transport_t* unix_transport = _unix_from_base(transport);

    platform_mutex_lock(unix_transport->conn_lock);
    poseidon_unix_connection_t* conn =
        _find_connection_by_fd(unix_transport, client_id);
    client_session_t* session = (conn != NULL) ? conn->session : NULL;
    platform_mutex_unlock(unix_transport->conn_lock);

    return session;
}

void* poseidon_transport_unix_context(
    struct poseidon_transport_t* transport) {
    if (transport == NULL) return NULL;
    return _unix_from_base(transport)->ctx;
}
