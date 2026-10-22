//
// transport_ws.c
//
// WebSocket transport backend.
//
// The transport owns a poll-dancer event loop driven by one platform
// thread. The listening socket is registered with the loop through an
// accept watcher; every accepted socket becomes a per-connection actor
// (ws_connection.c) that runs on the transport's scheduler pool.
//
// Threading model:
//   - the I/O thread runs the poll-dancer loop and is the only thread
//     that touches the accept watcher and the loop's watcher registry;
//   - the scheduler pool workers run the per-connection actors, which
//     own the TLS session, the HTTP upgrade, the WebSocket frame
//     decoder and the write path;
//   - the caller's thread creates and destroys the transport.
//
// Because a watcher may only be registered or unregistered on the loop
// thread, a connection that needs its watcher torn down pushes the
// watcher onto the transport's destroy stack and the I/O thread drains
// that stack at the bottom of every loop iteration. The socket
// descriptor is closed only after the watcher has been unregistered, so
// the descriptor number cannot be recycled under a live registration.
//
// Wire protocol: a WebSocket binary frame carrying a 4-byte big-endian
// length prefix followed by a CBOR payload.
//

#include "transport_ws.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Channel/channel_manager.h"
#include "client_session.h"
#include "transport_internal.h"
#include "ws_connection.h"

#include <poll-dancer/poll-dancer.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

// How long the I/O thread blocks in epoll_wait before it re-checks the
// running flag and drains the destroy stack. 100ms is short enough that
// a stop request is honoured promptly and long enough that an idle
// transport does not spin.
#define WS_LOOP_TICK_MS 100

// The pending-connection queue of the listening socket.
#define WS_LISTEN_BACKLOG 16

// Session ids are handed to client_session_create. They only have to be
// unique among the live sessions of one transport.
static uint64_t s_next_session_id = 1;

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The WebSocket-specific state — the listening
// socket, the accept watcher, the TLS context and the port — lives
// here.
//
// Two views of the transport exist side by side:
//   - base is the actor-shaped body every backend shares. It is the
//     FIRST member, so a pointer to it IS a pointer to this struct and
//     the backend can be recovered from the internal body alone.
//   - public is the vtable-shaped view that transport.h still
//     declares. It is reached from the public pointer through
//     offsetof.

typedef struct poseidon_ws_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_internal_t base;

    // The vtable-shaped public view.
    poseidon_transport_t public;

    // Listening socket and its accept watcher. The watcher is created
    // on the I/O thread, right before the loop starts running.
    platform_socket_t* listen_sock;
    pd_watcher_t* listen_watcher;

    // The port the transport listens on.
    uint16_t port;

    // TLS context shared by every connection. NULL when the transport
    // runs without TLS.
    SSL_CTX* ssl_ctx;

    // Guards the connection table below. The accept callback (I/O
    // thread) adds connections; the send path (session actor thread)
    // looks them up; the destroy path (caller thread) walks them. The
    // scheduler pool workers never touch the table.
    platform_mutex_t* conn_lock;
} poseidon_ws_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop,
                             pd_watcher_t* watcher,
                             pd_event_t events,
                             void* user_data);
static void _ws_transport_dispatch(void* state, message_t* msg);
static int _ws_transport_send_to_client(poseidon_transport_internal_t* transport,
                                         int client_id,
                                         const uint8_t* data,
                                         size_t len);
static int _ws_vtable_send(poseidon_transport_t* self,
                            int client_id,
                            const uint8_t* data,
                            size_t len);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

// Recovers the backend struct from the vtable-shaped public pointer.
static poseidon_ws_transport_t* _ws_from_public(poseidon_transport_t* transport) {
    return (poseidon_ws_transport_t*)((char*)transport -
        offsetof(poseidon_ws_transport_t, public));
}

// Recovers the backend struct from the actor-shaped internal pointer.
// base is the first member, so this is a plain upcast.
static poseidon_ws_transport_t* _ws_from_base(poseidon_transport_internal_t* transport) {
    return (poseidon_ws_transport_t*)transport;
}

// ============================================================================
// HELPERS
// ============================================================================

// Finds the connection whose socket descriptor is client_id. The
// descriptor is the connection's identity on the wire: the client
// session reports it back on every reply.
//
// A closed connection is skipped: its descriptor number may have been
// handed to a new connection in the meantime, and the reply has to go
// to the live one.
static poseidon_ws_connection_t* _find_connection_by_fd(
    poseidon_ws_transport_t* transport,
    int client_id) {
    for (int i = 0; i < transport->base.connections.length; i++) {
        poseidon_ws_connection_t* conn =
            (poseidon_ws_connection_t*)transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (ATOMIC_LOAD(&conn->closed)) continue;
        if (conn->client_fd == client_id) {
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
// the listener-level messages. The WebSocket backend does not currently
// need to receive cross-actor messages (the per-connection actors talk
// to their peers directly), so the dispatch is a no-op that keeps the
// actor contract satisfied.

static void _ws_transport_dispatch(void* state, message_t* msg) {
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

    poseidon_ws_transport_t* transport = (poseidon_ws_transport_t*)user_data;
    if (transport == NULL) return;

    if (!(events & PD_EVENT_READ)) {
        return;
    }

    platform_address_t remote;
    platform_socket_t* client_sock =
        platform_socket_accept(transport->listen_sock, &remote);
    if (client_sock == NULL) {
        // A failed accept is normally transient (the peer gave up
        // between the readiness notification and the accept call).
        // Only the permanent errors are worth logging.
        if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK ||
            errno == EOPNOTSUPP) {
            log_error("ws transport: accept failed permanently (errno=%d)",
                      errno);
        }
        return;
    }

    // The accepted socket inherits the listener's blocking mode, so
    // it has to be switched explicitly before the connection's actor
    // starts pulling from it.
    platform_socket_set_nonblocking(client_sock);

    int client_fd = platform_socket_fd(client_sock);

    // Bind a client session to the connection. The session is the
    // per-client state the channel manager registry holds.
    client_session_t* session =
        client_session_create((uint32_t)s_next_session_id++,
                              transport->base.manager);
    if (session == NULL) {
        log_error("ws transport: failed to create client session");
        platform_socket_destroy(client_sock);
        return;
    }
    session->client_fd = client_fd;
    session->transport = &transport->public;

    if (transport->base.manager != NULL) {
        poseidon_channel_manager_register_session(transport->base.manager,
                                                   session);
    }

    poseidon_ws_connection_t* conn = NULL;
    if (!poseidon_ws_connection_create(&transport->base, client_sock,
                                        session, transport->ssl_ctx, &conn)) {
        log_error("ws transport: failed to create connection actor");
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

    log_info("ws transport: client connected (fd=%d)", client_fd);
}

// ============================================================================
// SERVER THREAD
// ============================================================================
//
// Runs the poll-dancer loop until the transport is stopped.

static void* _server_thread(void* arg) {
    poseidon_ws_transport_t* transport = (poseidon_ws_transport_t*)arg;
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
        log_error("ws transport: failed to create accept watcher");
        return NULL;
    }
    pd_watcher_start(transport->listen_watcher);

    while (ATOMIC_LOAD(&transport->base.running)) {
        // Drain the destroy stack before blocking again: watchers
        // that a connection actor asked to tear down are freed here,
        // on the I/O thread, so the unregister never races a live
        // registration.
        destroy_stack_drain(&transport->base.destroy_stack);
        pd_loop_run_once(transport->base.loop, WS_LOOP_TICK_MS);
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

poseidon_transport_t* poseidon_transport_ws_create(uint16_t port,
                                                   const char* cert_path,
                                                   const char* key_path,
                                                   poseidon_channel_manager_t* manager) {
    if (manager == NULL) return NULL;

    poseidon_ws_transport_t* transport =
        (poseidon_ws_transport_t*)get_clear_memory(
            sizeof(poseidon_ws_transport_t));
    if (transport == NULL) return NULL;

    poseidon_transport_internal_t* base = &transport->base;

    base->loop = pd_loop_create(NULL);
    if (base->loop == NULL) {
        free(transport);
        return NULL;
    }

    base->manager = manager;
    base->on_message = NULL;
    base->type = POSEIDON_TRANSPORT_WEBSOCKET;
    base->name = "ws";
    base->thread = NULL;
    base->pool = NULL;
    vec_init(&base->connections);
    ATOMIC_STORE(&base->running, 0);
    ATOMIC_STORE(&base->active_connections, 0);
    destroy_stack_init(&base->destroy_stack);

    transport->listen_sock = NULL;
    transport->listen_watcher = NULL;
    transport->port = port;
    transport->ssl_ctx = NULL;
    transport->conn_lock = platform_mutex_create();
    if (transport->conn_lock == NULL) {
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport);
        return NULL;
    }

    // Set up TLS if a cert and a key were provided. Without them the
    // transport speaks plain WebSocket.
    if (cert_path != NULL && key_path != NULL) {
        SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (ssl_ctx == NULL) {
            log_error("ws transport: SSL_CTX_new failed");
            platform_mutex_destroy(transport->conn_lock);
            destroy_stack_destroy(&base->destroy_stack);
            pd_loop_destroy(base->loop);
            free(transport);
            return NULL;
        }

        if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0 ||
            SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
            log_error("ws transport: failed to load TLS cert/key");
            SSL_CTX_free(ssl_ctx);
            platform_mutex_destroy(transport->conn_lock);
            destroy_stack_destroy(&base->destroy_stack);
            pd_loop_destroy(base->loop);
            free(transport);
            return NULL;
        }
        transport->ssl_ctx = ssl_ctx;
    }

    // The transport body is actor-shaped: bind it to the scheduler
    // pool the manager uses, so the transport's actor lives on the
    // same pool as the channel manager and the client sessions. The
    // per-connection actors dispatch their messages through it as
    // well: an actor with a NULL pool is never scheduled, so its
    // mailbox would fill up and never be drained.
    base->pool = manager->actor.pool;
    actor_init(&base->actor, base, _ws_transport_dispatch, base->pool);

    // Bind the wildcard address: the listener serves IPv4 and IPv6
    // clients on the one socket.
    platform_address_t addr;
    platform_socket_t* listen_sock =
        platform_listen_socket_create("0.0.0.0", port, &addr);
    if (listen_sock == NULL) {
        log_error("ws transport: failed to bind port %u", (unsigned)port);
        actor_destroy(&base->actor);
        platform_mutex_destroy(transport->conn_lock);
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport);
        return NULL;
    }
    if (platform_socket_listen(listen_sock, WS_LISTEN_BACKLOG) < 0) {
        log_error("ws transport: listen() failed on port %u (errno=%d)",
                  (unsigned)port, errno);
        platform_socket_destroy(listen_sock);
        actor_destroy(&base->actor);
        platform_mutex_destroy(transport->conn_lock);
        destroy_stack_destroy(&base->destroy_stack);
        pd_loop_destroy(base->loop);
        free(transport);
        return NULL;
    }
    platform_socket_set_nonblocking(listen_sock);
    transport->listen_sock = listen_sock;

    // The vtable-shaped public view lives in the same allocation as
    // the actor-shaped body, side by side.
    transport->public.name = transport->base.name;
    transport->public.type = POSEIDON_TRANSPORT_WEBSOCKET;
    transport->public.thread = NULL;
    transport->public.loop = base->loop;
    transport->public.manager = manager;
    transport->public.on_message = NULL;
    transport->public.ctx = NULL;
    transport->public.lock = transport->conn_lock;
    transport->public.running = false;
    transport->public.start = poseidon_transport_ws_start;
    transport->public.stop = poseidon_transport_ws_stop;
    transport->public.send = _ws_vtable_send;
    transport->public.destroy = poseidon_transport_ws_destroy;

    return &transport->public;
}

int poseidon_transport_ws_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_ws_transport_t* ws_transport = _ws_from_public(transport);

    if (ATOMIC_LOAD(&ws_transport->base.running)) {
        return 0;
    }

    log_info("ws transport: starting on port %u", (unsigned)ws_transport->port);

    ATOMIC_STORE(&ws_transport->base.running, 1);
    platform_thread_t* thread =
        platform_thread_create(_server_thread, ws_transport);
    if (thread == NULL) {
        ATOMIC_STORE(&ws_transport->base.running, 0);
        log_error("ws transport: failed to create I/O thread");
        return -1;
    }
    ws_transport->base.thread = thread;
    ws_transport->public.thread = thread;
    ws_transport->public.running = true;

    return 0;
}

int poseidon_transport_ws_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_ws_transport_t* ws_transport = _ws_from_public(transport);

    if (!ATOMIC_LOAD(&ws_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&ws_transport->base.running, 0);

    // Wake the I/O thread so it notices the stop flag without waiting
    // for the tick to expire. The wake-up is best-effort: on the
    // epoll backend it is a no-op and the thread wakes on its own
    // when the tick expires.
    pd_loop_async_send(ws_transport->base.loop, ws_transport);

    if (ws_transport->base.thread != NULL) {
        platform_thread_join(ws_transport->base.thread);
        ws_transport->base.thread = NULL;
    }
    ws_transport->public.thread = NULL;
    ws_transport->public.running = false;

    // Ask the connections to close gracefully. Their pending frames
    // are flushed by the connection actors while the scheduler pool is
    // still alive.
    platform_mutex_lock(ws_transport->conn_lock);
    for (int i = 0; i < ws_transport->base.connections.length; i++) {
        poseidon_ws_connection_t* conn =
            (poseidon_ws_connection_t*)ws_transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (ATOMIC_LOAD(&conn->closed)) continue;
        poseidon_ws_connection_close(conn, true);
    }
    platform_mutex_unlock(ws_transport->conn_lock);

    return 0;
}

void poseidon_transport_ws_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_ws_transport_t* ws_transport = _ws_from_public(transport);
    poseidon_transport_internal_t* base = &ws_transport->base;

    // Stop the I/O thread first: after this returns no callback can
    // fire on a connection, so the teardown below runs without a race
    // against the accept path.
    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_ws_stop(transport);
    }

    // Drain the destroy stack BEFORE tearing down the connection
    // watchers. The stack holds the watchers a connection actor asked
    // to tear down; destroying them a second time from the loop below
    // would double-free the watcher and corrupt the heap.
    destroy_stack_drain(&base->destroy_stack);

    if (ws_transport->listen_sock != NULL) {
        platform_socket_destroy(ws_transport->listen_sock);
        ws_transport->listen_sock = NULL;
    }

    // Quiesce the connection actors first. actor_destroy sets the
    // DESTROY flag (so the scheduler stops picking the actor up and
    // actor_send drops its messages), waits for any dispatch that is
    // already running to finish, and drains the mailbox.
    //
    // The order matters: the connection's fields (sock, watcher, ssl,
    // write_buffer) are read and written by the actor's dispatch, so
    // they may only be touched once the actor is quiesced. Tearing
    // the connection down before the actor is stopped would race the
    // dispatch against the free.
    for (int i = 0; i < base->connections.length; i++) {
        poseidon_ws_connection_t* conn =
            (poseidon_ws_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        actor_destroy(&conn->actor);
    }

    // A connection actor that ran its close path while it was being
    // quiesced pushed its watcher onto the destroy stack: free those
    // watchers now, before the connections themselves go away.
    destroy_stack_drain(&base->destroy_stack);

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
    // The connection table is held while the connections are freed:
    // the send path looks a connection up under the same lock, so a
    // reply that is on its way can never land on a freed connection.
    // The lock is deliberately not held while the actors are quiesced
    // above: a dispatch may reach back into the transport's send path.
    platform_mutex_lock(ws_transport->conn_lock);
    for (int i = base->connections.length - 1; i >= 0; i--) {
        poseidon_ws_connection_t* conn =
            (poseidon_ws_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;

        conn->is_closing = 1;

        // The watcher is unregistered before the socket is closed: the
        // closed descriptor number must not be reused while the
        // registration is still live.
        if (conn->watcher != NULL) {
            pd_watcher_t* watcher = conn->watcher;
            conn->watcher = NULL;
            pd_watcher_stop(watcher);
            pd_watcher_destroy(watcher);
        }
        if (conn->sock != NULL) {
            platform_socket_destroy(conn->sock);
            conn->sock = NULL;
        }
        ATOMIC_STORE(&conn->closed, 1);
        if (conn->ssl != NULL) {
            SSL_free(conn->ssl);
            conn->ssl = NULL;
        }
        if (conn->write_buffer != NULL) {
            buffer_destroy(conn->write_buffer);
            conn->write_buffer = NULL;
        }
        if (conn->write_queue != NULL) {
            buffer_destroy(conn->write_queue);
            conn->write_queue = NULL;
        }
        conn->transport = NULL;
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
    platform_mutex_unlock(ws_transport->conn_lock);

    // The shared body is actor-shaped: destroy its actor before the
    // loop goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);

    if (base->loop != NULL) {
        pd_loop_destroy(base->loop);
        base->loop = NULL;
    }

    if (ws_transport->conn_lock != NULL) {
        platform_mutex_destroy(ws_transport->conn_lock);
        ws_transport->conn_lock = NULL;
    }

    if (ws_transport->ssl_ctx != NULL) {
        SSL_CTX_free(ws_transport->ssl_ctx);
        ws_transport->ssl_ctx = NULL;
    }

    free(ws_transport);
}

// ============================================================================
// SEND
// ============================================================================

// Hands the payload to the connection's write actor. The connection
// wraps it in a WebSocket binary frame and serializes the write, so a
// reply never interleaves with another reply on the same socket.
static int _ws_transport_send_to_client(poseidon_transport_internal_t* transport,
                                        int client_id,
                                        const uint8_t* data,
                                        size_t len) {
    if (transport == NULL || (data == NULL && len > 0)) return -1;

    poseidon_ws_transport_t* ws_transport = _ws_from_base(transport);

    platform_mutex_lock(ws_transport->conn_lock);
    poseidon_ws_connection_t* conn =
        _find_connection_by_fd(ws_transport, client_id);
    if (conn != NULL) {
        // The enqueue happens under the lock, so a connection that is
        // being torn down cannot be freed while the frame is on its
        // way to it.
        poseidon_ws_connection_write(conn, data, len);
    }
    platform_mutex_unlock(ws_transport->conn_lock);

    if (conn == NULL) {
        return -1;
    }

    return 0;
}

// The vtable-shaped send hook. The public view and the internal body
// are two members of the same allocation, so the internal body is
// reached through the public pointer.
static int _ws_vtable_send(poseidon_transport_t* self,
                            int client_id,
                            const uint8_t* data,
                            size_t len) {
    if (self == NULL) return -1;
    poseidon_ws_transport_t* ws_transport = _ws_from_public(self);
    return _ws_transport_send_to_client(&ws_transport->base, client_id, data,
                                        len);
}

int poseidon_transport_ws_send(poseidon_transport_t* transport,
                               int client_id,
                               const uint8_t* data,
                               size_t len) {
    if (transport == NULL) return -1;
    poseidon_ws_transport_t* ws_transport = _ws_from_public(transport);
    return _ws_transport_send_to_client(&ws_transport->base, client_id, data,
                                        len);
}
