//
// transport_webtransport.c
//
// WebTransport server transport.
//
// The transport owns a libwtf context and server; libwtf runs its own
// worker threads and delivers the data through the transport's
// session and stream callbacks. There is no poll-dancer loop behind
// this backend and no watcher to tear down.
//
// The listener's callbacks only copy what libwtf handed them and hand
// it to the per-connection actor (wt_connection.c), which runs on the
// transport's scheduler pool. The actor is the only thread that
// decodes frames, answers the client session and writes to the
// stream.
//
// Wire protocol: 4-byte big-endian length prefix followed by a CBOR
// payload, the same framing the other backends use.
//

#include "transport_webtransport.h"

#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Channel/channel_manager.h"
#include "client_session.h"
#include "transport.h"
#include "wt_connection.h"

#include <wtf.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

// Session ids are handed to client_session_create. They only have to be
// unique among the live sessions of one transport.
static uint64_t s_next_session_id = 1;

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The WebTransport-specific state — the libwtf
// context, the server, the port and the certificate paths — lives
// here.
//
// The shared body is the FIRST member, so a pointer to it IS a pointer
// to this struct: the backend is recovered from the transport body with
// a plain upcast.

typedef struct poseidon_wt_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_t base;


    // The libwtf context and the server. Both are created by start and
    // released by stop.
    struct wtf_context* context;
    struct wtf_server* server;

    // The port the transport listens on.
    uint16_t port;

    // Copies of the certificate paths. Freed by the destroy path.
    char* cert_path;
    char* key_path;

    // Guards the connection table below. The libwtf callbacks (on
    // libwtf's own threads) add and close connections; the send path
    // (session actor thread) looks them up; the destroy path (caller
    // thread) walks them.
    platform_mutex_t* conn_lock;
} poseidon_wt_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void _wt_session_callback(const wtf_session_event_t* event);
static void _wt_stream_callback(const wtf_stream_event_t* event);
static wtf_connection_decision_t _wt_connection_validator(
    const wtf_connection_request_t* request, void* user_context);
static void _wt_transport_dispatch(void* state, message_t* msg);
static int _wt_transport_send_to_client(poseidon_transport_t* transport,
                                         int client_id,
                                         const uint8_t* data,
                                         size_t len);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

// Recovers the backend struct from the transport body. base is the
// first member, so this is a plain upcast.
static poseidon_wt_transport_t* _wt_from_base(poseidon_transport_t* transport) {
    return (poseidon_wt_transport_t*)transport;
}

// ============================================================================
// HELPERS
// ============================================================================

// Finds the connection whose client session carries session_id. The
// session id is the connection's identity on the wire: the client
// session reports it back on every reply.
//
// A closed connection is skipped: its session id may have been handed
// to a new session in the meantime, and the reply has to go to the
// live one.
static poseidon_wt_connection_t* _find_connection_by_session_id(
    poseidon_wt_transport_t* transport,
    uint32_t session_id) {
    for (int i = 0; i < transport->base.connections.length; i++) {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (ATOMIC_LOAD(&conn->closed)) continue;
        if (conn->session_id == session_id) {
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
// the listener-level messages. The WebTransport backend does not
// currently need to receive cross-actor messages, so the dispatch is a
// no-op that keeps the actor contract satisfied.

static void _wt_transport_dispatch(void* state, message_t* msg) {
    (void)state;
    (void)msg;
}

// ============================================================================
// SESSION CALLBACK
// ============================================================================
//
// Runs on a libwtf worker thread. The CONNECTED event is the only one
// that creates a connection: at that point the session's context is
// the transport itself.

static void _wt_session_callback(const wtf_session_event_t* event) {
    if (event == NULL) return;

    switch (event->type) {
    case WTF_SESSION_EVENT_CONNECTED: {
        // user_context is the transport: no connection exists yet for
        // this session.
        poseidon_wt_transport_t* transport =
            (poseidon_wt_transport_t*)event->user_context;
        if (transport == NULL) {
            log_error("webtransport: CONNECTED with no transport context");
            wtf_session_close(event->session, 0, "internal error");
            break;
        }

        if (!ATOMIC_LOAD(&transport->base.running)) {
            wtf_session_close(event->session, 0, "server stopping");
            break;
        }

        // Bind a client session to the connection. The session is the
        // per-client state the channel manager registry holds.
        uint32_t session_id = (uint32_t)s_next_session_id++;
        client_session_t* session =
            client_session_create(session_id, transport->base.manager, transport->base.pool);
        if (session == NULL) {
            log_error("webtransport: failed to create client session");
            wtf_session_close(event->session, 0, "out of memory");
            break;
        }
        // The session id is the connection's identity on the wire.
        session->client_fd = (int)session_id;
        session->transport = &transport->base;

        poseidon_wt_connection_t* conn = NULL;
        if (!poseidon_wt_connection_create(&transport->base, session,
                                            session_id, &conn)) {
            log_error("webtransport: failed to create connection actor");
            client_session_destroy(session);
            wtf_session_close(event->session, 0, "out of memory");
            break;
        }

        // From now on the session's context is the connection.
        wtf_session_set_context(event->session, conn);

        platform_mutex_lock(transport->conn_lock);
        vec_push(&transport->base.connections, (void*)conn);
        platform_mutex_unlock(transport->conn_lock);

        ATOMIC_FETCH_ADD(&transport->base.active_connections, 1);

        if (transport->base.manager != NULL) {
            poseidon_channel_manager_register_session(transport->base.manager,
                                                       session);
        }

        log_info("webtransport: session connected, client %u", session_id);
        break;
    }

    case WTF_SESSION_EVENT_DISCONNECTED: {
        // user_context is the connection.
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)event->user_context;
        if (conn == NULL) break;

        log_info("webtransport: session disconnected (error=0x%x, reason=%s)",
                 event->disconnected.error_code,
                 event->disconnected.reason ? event->disconnected.reason : "");

        // Tell the actor to tear the connection down. The connection
        // itself is released by the transport's teardown, once the
        // actor has been quiesced.
        poseidon_wt_connection_close(conn, true);
        break;
    }

    case WTF_SESSION_EVENT_DRAINING: {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)event->user_context;
        if (conn != NULL) {
            log_info("webtransport: session draining for client %u",
                     conn->session_id);
        }
        break;
    }

    case WTF_SESSION_EVENT_STREAM_OPENED: {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)event->user_context;
        if (conn == NULL) break;

        wtf_stream_t* stream = event->stream_opened.stream;
        wtf_stream_type_t stream_type = event->stream_opened.stream_type;

        if (stream_type == WTF_STREAM_UNIDIRECTIONAL) {
            log_debug("webtransport: ignoring unidirectional stream");
            wtf_stream_close(stream);
            break;
        }

        if (conn->stream != NULL) {
            log_debug("webtransport: closing extra bidi stream");
            wtf_stream_close(stream);
            break;
        }

        // The stream belongs to the connection's actor from now on.
        wtf_stream_set_callback(stream, _wt_stream_callback);
        wtf_stream_set_context(stream, conn);
        poseidon_wt_connection_stream_opened(conn, stream);

        log_info("webtransport: bidi stream opened for client %u",
                 conn->session_id);
        break;
    }

    case WTF_SESSION_EVENT_DATAGRAM_SEND_STATE_CHANGE:
    case WTF_SESSION_EVENT_DATAGRAM_RECEIVED:
        // Not using datagrams
        break;

    default:
        break;
    }
}

// ============================================================================
// STREAM CALLBACK
// ============================================================================
//
// Runs on a libwtf worker thread. The callback only copies what it
// received and hands it to the connection's actor.

static void _wt_stream_callback(const wtf_stream_event_t* event) {
    poseidon_wt_connection_t* conn = (poseidon_wt_connection_t*)event->user_context;
    if (conn == NULL) return;

    switch (event->type) {
    case WTF_STREAM_EVENT_DATA_RECEIVED: {
        wtf_buffer_t* buffers = event->data_received.buffers;
        uint32_t buffer_count = event->data_received.buffer_count;

        size_t total_received = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            total_received += buffers[i].length;
        }
        if (total_received == 0) {
            break;
        }

        // The buffers belong to libwtf, so the bytes are copied into
        // one block that the connection's actor owns.
        uint8_t* copy = (uint8_t*)get_clear_memory(total_received);
        if (copy == NULL) {
            log_error("webtransport: failed to copy the received bytes");
            break;
        }
        size_t offset = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            memcpy(copy + offset, buffers[i].data, buffers[i].length);
            offset += buffers[i].length;
        }

        poseidon_wt_connection_received(conn, copy, total_received);
        // The actor owns the copy now.
        break;
    }

    case WTF_STREAM_EVENT_SEND_COMPLETE: {
        // Free the send buffer: the caller retains ownership and must
        // free it in SEND_COMPLETE (including cancelled sends).
        wtf_buffer_t* buffers = event->send_complete.buffers;
        if (buffers != NULL && buffers->data != NULL) {
            free(buffers->data);
        }
        break;
    }

    case WTF_STREAM_EVENT_PEER_CLOSED: {
        log_debug("webtransport: peer closed stream for client %u",
                  conn->session_id);
        poseidon_wt_connection_stream_closed(conn);
        break;
    }

    case WTF_STREAM_EVENT_CLOSED: {
        log_info("webtransport: stream closed for client %u", conn->session_id);
        poseidon_wt_connection_stream_closed(conn);
        break;
    }

    case WTF_STREAM_EVENT_ABORTED: {
        log_warn("webtransport: stream aborted (error=0x%x) for client %u",
                 event->aborted.error_code, conn->session_id);
        poseidon_wt_connection_stream_closed(conn);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// CONNECTION VALIDATOR — accept all connections for now
// ============================================================================

static wtf_connection_decision_t _wt_connection_validator(
    const wtf_connection_request_t* request, void* user_context) {
    (void)user_context;
    if (request != NULL) {
        log_info("webtransport: connection request from %s%s",
                 request->origin ? request->origin : "",
                 request->path ? request->path : "");
    }
    return WTF_CONNECTION_ACCEPT;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_webtransport_create(
    uint16_t port, const char* cert_path, const char* key_path,
    poseidon_channel_manager_t* manager) {
    if (manager == NULL || cert_path == NULL || key_path == NULL) return NULL;

    poseidon_wt_transport_t* transport =
        (poseidon_wt_transport_t*)get_clear_memory(
            sizeof(poseidon_wt_transport_t));
    if (transport == NULL) return NULL;

    poseidon_transport_t* base = &transport->base;

    base->manager = manager;
    base->on_message = NULL;
    base->type = POSEIDON_TRANSPORT_WEBTRANSPORT;
    base->name = "webtransport";
    base->thread = NULL;
    base->pool = NULL;
    base->loop = NULL;
    vec_init(&base->connections);
    ATOMIC_STORE(&base->running, 0);
    ATOMIC_STORE(&base->active_connections, 0);
    destroy_stack_init(&base->destroy_stack);

    transport->context = NULL;
    transport->server = NULL;
    transport->port = port;
    transport->cert_path = strdup(cert_path);
    transport->key_path = strdup(key_path);
    transport->conn_lock = platform_mutex_create();
    if (transport->cert_path == NULL || transport->key_path == NULL ||
        transport->conn_lock == NULL) {
        if (transport->conn_lock != NULL) {
            platform_mutex_destroy(transport->conn_lock);
        }
        free(transport->cert_path);
        free(transport->key_path);
        destroy_stack_destroy(&base->destroy_stack);
        free(transport);
        return NULL;
    }

    // The transport body is actor-shaped: bind it to the scheduler
    // pool the manager uses, so the transport's actor and the
    // per-connection actors live on the same pool as the channel
    // manager and the client sessions.
    base->pool = manager->actor.pool;
    actor_init(&base->actor, base, _wt_transport_dispatch, base->pool);


    return &transport->base;
}

int poseidon_transport_webtransport_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_wt_transport_t* wt_transport = _wt_from_base(transport);
    if (ATOMIC_LOAD(&wt_transport->base.running)) {
        return 0;
    }

    // Create libwtf context
    wtf_context_config_t ctx_config;
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.log_level = WTF_LOG_LEVEL_WARN;
    ctx_config.worker_thread_count = 2;
    ctx_config.enable_load_balancing = false;
    ctx_config.disable_encryption = false;
    ctx_config.execution_profile = WTF_EXECUTION_PROFILE_LOW_LATENCY;

    wtf_result_t wr = wtf_context_create(&ctx_config, &wt_transport->context);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: context create failed: %s",
                  wtf_result_to_string(wr));
        return -1;
    }

    // Configure certificate
    wtf_certificate_config_t cert_config;
    memset(&cert_config, 0, sizeof(cert_config));
    cert_config.cert_type = WTF_CERT_TYPE_FILE;
    cert_config.cert_data.file.cert_path = wt_transport->cert_path;
    cert_config.cert_data.file.key_path = wt_transport->key_path;

    // Create server — pass the transport as user_context so the session
    // callback can retrieve it on the first CONNECTED event before any
    // connection exists.
    wtf_server_config_t srv_config;
    memset(&srv_config, 0, sizeof(srv_config));
    srv_config.host = NULL;
    srv_config.port = wt_transport->port;
    srv_config.cert_config = &cert_config;
    srv_config.max_sessions_per_connection = 32;
    srv_config.max_streams_per_session = 256;
    srv_config.max_data_per_session = 0;
    srv_config.idle_timeout_ms = 60000;
    srv_config.handshake_timeout_ms = 10000;
    srv_config.enable_0rtt = false;
    srv_config.enable_migration = false;
    srv_config.connection_validator = _wt_connection_validator;
    srv_config.session_callback = _wt_session_callback;
    srv_config.user_context = wt_transport;

    wr = wtf_server_create(wt_transport->context, &srv_config,
                            &wt_transport->server);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: server create failed: %s",
                  wtf_result_to_string(wr));
        wtf_context_destroy(wt_transport->context);
        wt_transport->context = NULL;
        return -1;
    }

    // Start server
    wr = wtf_server_start(wt_transport->server);
    if (wr != WTF_SUCCESS) {
        log_error("webtransport: server start failed: %s",
                  wtf_result_to_string(wr));
        wtf_server_destroy(wt_transport->server);
        wt_transport->server = NULL;
        wtf_context_destroy(wt_transport->context);
        wt_transport->context = NULL;
        return -1;
    }

    ATOMIC_STORE(&wt_transport->base.running, 1);
    log_info("webtransport: listening on port %u", (unsigned)wt_transport->port);
    return 0;
}

int poseidon_transport_webtransport_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_wt_transport_t* wt_transport = _wt_from_base(transport);
    if (!ATOMIC_LOAD(&wt_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&wt_transport->base.running, 0);

    // Stop the server first: no new session may connect from here on.
    if (wt_transport->server != NULL) {
        wtf_server_stop(wt_transport->server);
    }

    // Ask the connections to close. Their streams are closed by their
    // own actors.
    platform_mutex_lock(wt_transport->conn_lock);
    for (int i = 0; i < wt_transport->base.connections.length; i++) {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)wt_transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (ATOMIC_LOAD(&conn->closed)) continue;
        poseidon_wt_connection_close(conn, true);
    }
    platform_mutex_unlock(wt_transport->conn_lock);

    return 0;
}

void poseidon_transport_webtransport_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_wt_transport_t* wt_transport = _wt_from_base(transport);
    poseidon_transport_t* base = &wt_transport->base;

    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_webtransport_stop(transport);
    }

    // Quiesce the connection actors before the libwtf context goes
    // away: a running dispatch may still be writing to a stream, and
    // the stream belongs to the context.
    for (int i = 0; i < base->connections.length; i++) {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        actor_destroy(&conn->actor);
    }

    // Now the libwtf handles can go: the context owns the worker
    // threads that call the session and stream callbacks.
    if (wt_transport->server != NULL) {
        wtf_server_destroy(wt_transport->server);
        wt_transport->server = NULL;
    }
    if (wt_transport->context != NULL) {
        wtf_context_destroy(wt_transport->context);
        wt_transport->context = NULL;
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
    // The connections are closed and their sessions released outside
    // the lock: the session wrappers run the session's actor inline and
    // wait for a pool worker that may be reaching back into this
    // transport, and the send path takes this lock.
    for (int i = base->connections.length - 1; i >= 0; i--) {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        conn->is_closing = 1;
        ATOMIC_STORE(&conn->closed, 1);

        conn->stream = NULL;
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
        poseidon_wt_connection_destroy(conn);
    }

    // The connection structs are freed under the lock: the send path
    // resolves a client under the same lock.
    platform_mutex_lock(wt_transport->conn_lock);
    for (int i = base->connections.length - 1; i >= 0; i--) {
        poseidon_wt_connection_t* conn =
            (poseidon_wt_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        free(conn);
    }
    platform_mutex_unlock(wt_transport->conn_lock);
    vec_deinit(&base->connections);

    // The shared body is actor-shaped: destroy its actor before the
    // transport's memory goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);

    if (wt_transport->conn_lock != NULL) {
        platform_mutex_destroy(wt_transport->conn_lock);
        wt_transport->conn_lock = NULL;
    }

    free(wt_transport->cert_path);
    free(wt_transport->key_path);
    wt_transport->cert_path = NULL;
    wt_transport->key_path = NULL;

    free(wt_transport);
}

// ============================================================================
// SEND
// ============================================================================

// Hands the payload to the connection's write actor. The connection
// frames it and serializes the write, so a reply never interleaves
// with another reply on the same stream.
static int _wt_transport_send_to_client(poseidon_transport_t* transport,
                                         int client_id,
                                         const uint8_t* data,
                                         size_t len) {
    if (transport == NULL || (data == NULL && len > 0)) return -1;

    poseidon_wt_transport_t* wt_transport = _wt_from_base(transport);

    platform_mutex_lock(wt_transport->conn_lock);
    poseidon_wt_connection_t* conn =
        _find_connection_by_session_id(wt_transport, (uint32_t)client_id);
    if (conn != NULL) {
        // The enqueue happens under the lock, so a connection that is
        // being torn down cannot be freed while the frame is on its
        // way to it.
        poseidon_wt_connection_write(conn, data, len);
    }
    platform_mutex_unlock(wt_transport->conn_lock);

    if (conn == NULL) {
        return -1;
    }

    return 0;
}

int poseidon_transport_webtransport_send(poseidon_transport_t* transport,
                                         int client_id,
                                         const uint8_t* data,
                                         size_t len) {
    if (transport == NULL) return -1;
    poseidon_wt_transport_t* wt_transport = _wt_from_base(transport);
    return _wt_transport_send_to_client(&wt_transport->base, client_id, data,
                                        len);
}
