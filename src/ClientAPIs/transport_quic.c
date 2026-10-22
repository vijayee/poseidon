//
// transport_quic.c
//
// QUIC transport for the ClientAPI — uses msquic for connections.
//
// The transport owns the msquic registration, configuration and
// listener. msquic runs its own worker threads and delivers the data
// through the transport's callbacks; there is no poll-dancer loop
// behind this backend and no watcher to tear down.
//
// The listener's callbacks only copy what msquic handed them and hand
// it to the per-connection actor (quic_connection.c), which runs on
// the transport's scheduler pool. The actor is the only thread that
// decodes frames, answers the client session and writes to the
// stream.
//
// Wire protocol: 4-byte big-endian length prefix followed by a CBOR
// payload, the same framing the other backends use.
//

#include "transport_quic.h"

#include "../Platform/platform.h"
#include "../Platform/poseidon_msquic.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/vec.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "../Buffer/buffer.h"
#include "../Channel/channel_manager.h"
#include "client_session.h"
#include "quic_connection.h"
#include "transport_internal.h"

#include <msquic.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define QUIC_ALPN "poseidon_client"

// Session ids are handed to client_session_create. They only have to be
// unique among the live sessions of one transport.
static uint64_t s_next_session_id = 1;

// ============================================================================
// BACKEND PRIVATE STATE
// ============================================================================
//
// The shared transport body (transport_internal.h) carries the fields
// every backend needs. The QUIC-specific state — the msquic handles,
// the port and the certificate paths — lives here.
//
// Two views of the transport exist side by side:
//   - base is the actor-shaped body every backend shares. It is the
//     FIRST member, so a pointer to it IS a pointer to this struct and
//     the backend can be recovered from the internal body alone.
//   - public is the vtable-shaped view that transport.h still
//     declares. It is reached from the public pointer through
//     offsetof.

typedef struct poseidon_quic_transport_t {
    // The shared transport body. First so that a pointer to this
    // struct IS a pointer to the shared body.
    poseidon_transport_internal_t base;

    // The vtable-shaped public view.
    poseidon_transport_t public;

    // The msquic handles. The registration is opened by create, the
    // configuration and the listener by start.
    const struct QUIC_API_TABLE* api;
    HQUIC registration;
    HQUIC configuration;
    HQUIC listener;

    // The port the transport listens on.
    uint16_t port;

    // Copies of the certificate paths. Freed by the destroy path.
    char* cert_path;
    char* key_path;

    // Guards the connection table below. The msquic callbacks (on
    // msquic's own threads) add and close connections; the send path
    // (session actor thread) looks them up; the destroy path (caller
    // thread) walks them.
    platform_mutex_t* conn_lock;
} poseidon_quic_transport_t;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static QUIC_STATUS QUIC_API _quic_listener_callback(HQUIC listener,
                                                     void* context,
                                                     QUIC_LISTENER_EVENT* event);
static QUIC_STATUS QUIC_API _quic_connection_callback(HQUIC connection,
                                                       void* context,
                                                       QUIC_CONNECTION_EVENT* event);
static QUIC_STATUS QUIC_API _quic_stream_callback(HQUIC stream,
                                                   void* context,
                                                   QUIC_STREAM_EVENT* event);
static void _quic_transport_dispatch(void* state, message_t* msg);
static int _quic_transport_send_to_client(poseidon_transport_internal_t* transport,
                                          int client_id,
                                          const uint8_t* data,
                                          size_t len);
static int _quic_vtable_send(poseidon_transport_t* self,
                             int client_id,
                             const uint8_t* data,
                             size_t len);

// ============================================================================
// VIEW CONVERSIONS
// ============================================================================

// Recovers the backend struct from the vtable-shaped public pointer.
static poseidon_quic_transport_t* _quic_from_public(poseidon_transport_t* transport) {
    return (poseidon_quic_transport_t*)((char*)transport -
        offsetof(poseidon_quic_transport_t, public));
}

// Recovers the backend struct from the actor-shaped internal pointer.
// base is the first member, so this is a plain upcast.
static poseidon_quic_transport_t* _quic_from_base(poseidon_transport_internal_t* transport) {
    return (poseidon_quic_transport_t*)transport;
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
static poseidon_quic_connection_t* _find_connection_by_session_id(
    poseidon_quic_transport_t* transport,
    uint32_t session_id) {
    for (int i = 0; i < transport->base.connections.length; i++) {
        poseidon_quic_connection_t* conn =
            (poseidon_quic_connection_t*)transport->base.connections.data[i];
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
// the listener-level messages. The QUIC backend does not currently need
// to receive cross-actor messages, so the dispatch is a no-op that
// keeps the actor contract satisfied.

static void _quic_transport_dispatch(void* state, message_t* msg) {
    (void)state;
    (void)msg;
}

// ============================================================================
// STREAM CALLBACK
// ============================================================================
//
// Runs on an msquic worker thread. The callback only copies what
// msquic handed it and hands it to the connection's actor.

static QUIC_STATUS QUIC_API _quic_stream_callback(HQUIC stream,
                                                   void* context,
                                                   QUIC_STREAM_EVENT* event) {
    poseidon_quic_connection_t* conn = (poseidon_quic_connection_t*)context;
    if (conn == NULL) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        log_info("quic transport: stream started for client %u", conn->session_id);
        poseidon_quic_connection_stream_ready(conn);
        break;

    case QUIC_STREAM_EVENT_RECEIVE: {
        const QUIC_BUFFER* buffers = event->RECEIVE.Buffers;
        uint32_t buffer_count = event->RECEIVE.BufferCount;

        size_t total_received = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            total_received += buffers[i].Length;
        }
        if (total_received == 0) {
            break;
        }

        // The buffers belong to msquic, so the bytes are copied into
        // one block that the connection's actor owns.
        uint8_t* copy = (uint8_t*)get_clear_memory(total_received);
        if (copy == NULL) {
            log_error("quic transport: failed to copy the received bytes");
            break;
        }
        size_t offset = 0;
        for (uint32_t i = 0; i < buffer_count; i++) {
            memcpy(copy + offset, buffers[i].Buffer, buffers[i].Length);
            offset += buffers[i].Length;
        }

        poseidon_quic_connection_received(conn, copy, total_received);
        // The actor owns the copy now.
        break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // Free the send buffer: the caller retains ownership and must
        // free it in SEND_COMPLETE (including cancelled sends).
        if (event->SEND_COMPLETE.ClientContext != NULL) {
            free(event->SEND_COMPLETE.ClientContext);
        }
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // The peer is done sending; the actor answers with a graceful
        // shutdown of its own.
        poseidon_quic_connection_peer_shutdown(conn);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        log_info("quic transport: stream shutdown complete for client %u",
                 conn->session_id);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// CONNECTION CALLBACK
// ============================================================================
//
// Runs on an msquic worker thread.

static QUIC_STATUS QUIC_API _quic_connection_callback(HQUIC connection,
                                                       void* context,
                                                       QUIC_CONNECTION_EVENT* event) {
    poseidon_quic_transport_t* transport = (poseidon_quic_transport_t*)context;
    if (transport == NULL) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        log_info("quic transport: client connected");
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        if (!ATOMIC_LOAD(&transport->base.running)) {
            return QUIC_STATUS_CONNECTION_REFUSED;
        }

        HQUIC stream = event->PEER_STREAM_STARTED.Stream;

        // Bind a client session to the connection. The session is the
        // per-client state the channel manager registry holds.
        uint32_t session_id = (uint32_t)s_next_session_id++;
        client_session_t* session =
            client_session_create(session_id, transport->base.manager);
        if (session == NULL) {
            log_error("quic transport: failed to create client session");
            transport->api->ConnectionShutdown(
                connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            return QUIC_STATUS_SUCCESS;
        }
        // The session id is the connection's identity on the wire.
        session->client_fd = (int)session_id;
        session->transport = &transport->public;

        poseidon_quic_connection_t* conn = NULL;
        if (!poseidon_quic_connection_create(&transport->base, session,
                                             connection, stream, session_id,
                                             &conn)) {
            log_error("quic transport: failed to create connection actor");
            client_session_destroy(session);
            transport->api->ConnectionShutdown(
                connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            return QUIC_STATUS_SUCCESS;
        }

        transport->api->SetCallbackHandler(stream,
                                           (void*)_quic_stream_callback, conn);

        platform_mutex_lock(transport->conn_lock);
        vec_push(&transport->base.connections, (void*)conn);
        platform_mutex_unlock(transport->conn_lock);

        ATOMIC_FETCH_ADD(&transport->base.active_connections, 1);

        if (transport->base.manager != NULL) {
            poseidon_channel_manager_register_session(transport->base.manager,
                                                       session);
        }

        log_info("quic transport: stream started, client %u", session_id);
        break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        // The connection is gone: every stream it carried is closed
        // with it. The connections themselves are released by the
        // transport's teardown, once their actors are quiesced.
        platform_mutex_lock(transport->conn_lock);
        for (int i = 0; i < transport->base.connections.length; i++) {
            poseidon_quic_connection_t* conn =
                (poseidon_quic_connection_t*)transport->base.connections.data[i];
            if (conn == NULL) continue;
            if (conn->connection != connection) continue;
            poseidon_quic_connection_close(conn, true);
        }
        platform_mutex_unlock(transport->conn_lock);

        transport->api->ConnectionClose(connection);
        break;
    }

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// LISTENER CALLBACK
// ============================================================================
//
// Runs on an msquic worker thread.

static QUIC_STATUS QUIC_API _quic_listener_callback(HQUIC listener,
                                                     void* context,
                                                     QUIC_LISTENER_EVENT* event) {
    (void)listener;
    poseidon_quic_transport_t* transport = (poseidon_quic_transport_t*)context;
    if (transport == NULL) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
        const QUIC_NEW_CONNECTION_INFO* info = event->NEW_CONNECTION.Info;

        static const uint8_t poseidon_alpn_len = sizeof(QUIC_ALPN) - 1;
        if (info->NegotiatedAlpnLength != poseidon_alpn_len ||
            memcmp(info->NegotiatedAlpn, QUIC_ALPN, poseidon_alpn_len) != 0) {
            log_warn("quic transport: rejecting connection with wrong ALPN");
            return QUIC_STATUS_CONNECTION_REFUSED;
        }

        HQUIC connection = event->NEW_CONNECTION.Connection;

        transport->api->SetCallbackHandler(connection,
                                           (void*)_quic_connection_callback,
                                           transport);

        QUIC_STATUS status = transport->api->ConnectionSetConfiguration(
            connection, transport->configuration);
        if (QUIC_FAILED(status)) {
            log_error("quic transport: ConnectionSetConfiguration failed: 0x%x",
                      status);
            return status;
        }

        log_info("quic transport: accepted new connection");
        return QUIC_STATUS_SUCCESS;
    }

    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        log_info("quic transport: listener stopped");
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

poseidon_transport_t* poseidon_transport_quic_create(uint16_t port,
                                                      const char* cert_path,
                                                      const char* key_path,
                                                      poseidon_channel_manager_t* manager) {
    if (manager == NULL || cert_path == NULL || key_path == NULL) return NULL;

    const struct QUIC_API_TABLE* api = poseidon_msquic_open();
    if (api == NULL) {
        log_error("quic transport: failed to open msquic");
        return NULL;
    }

    HQUIC registration = NULL;
    if (!poseidon_msquic_open_registration("poseidon_client", &registration)) {
        log_error("quic transport: failed to open msquic registration");
        poseidon_msquic_close();
        return NULL;
    }

    poseidon_quic_transport_t* transport =
        (poseidon_quic_transport_t*)get_clear_memory(
            sizeof(poseidon_quic_transport_t));
    if (transport == NULL) {
        poseidon_msquic_close_registration("poseidon_client");
        poseidon_msquic_close();
        return NULL;
    }

    poseidon_transport_internal_t* base = &transport->base;

    base->manager = manager;
    base->on_message = NULL;
    base->type = POSEIDON_TRANSPORT_QUIC;
    base->name = "quic";
    base->thread = NULL;
    base->pool = NULL;
    base->loop = NULL;
    vec_init(&base->connections);
    ATOMIC_STORE(&base->running, 0);
    ATOMIC_STORE(&base->active_connections, 0);
    destroy_stack_init(&base->destroy_stack);

    transport->api = api;
    transport->registration = registration;
    transport->configuration = NULL;
    transport->listener = NULL;
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
        poseidon_msquic_close_registration("poseidon_client");
        poseidon_msquic_close();
        return NULL;
    }

    // The transport body is actor-shaped: bind it to the scheduler
    // pool the manager uses, so the transport's actor and the
    // per-connection actors live on the same pool as the channel
    // manager and the client sessions.
    base->pool = manager->actor.pool;
    actor_init(&base->actor, base, _quic_transport_dispatch, base->pool);

    // The vtable-shaped public view lives in the same allocation as
    // the actor-shaped body, side by side.
    transport->public.name = transport->base.name;
    transport->public.type = POSEIDON_TRANSPORT_QUIC;
    transport->public.thread = NULL;
    transport->public.loop = NULL;
    transport->public.manager = manager;
    transport->public.on_message = NULL;
    transport->public.ctx = NULL;
    transport->public.lock = transport->conn_lock;
    transport->public.running = false;
    transport->public.start = poseidon_transport_quic_start;
    transport->public.stop = poseidon_transport_quic_stop;
    transport->public.send = _quic_vtable_send;
    transport->public.destroy = poseidon_transport_quic_destroy;

    return &transport->public;
}

int poseidon_transport_quic_start(poseidon_transport_t* transport) {
    if (transport == NULL) return -1;

    poseidon_quic_transport_t* quic_transport = _quic_from_public(transport);
    if (ATOMIC_LOAD(&quic_transport->base.running)) {
        return 0;
    }

    const struct QUIC_API_TABLE* api = quic_transport->api;

    QUIC_BUFFER alpn;
    alpn.Buffer = (uint8_t*)QUIC_ALPN;
    alpn.Length = sizeof(QUIC_ALPN) - 1;

    QUIC_STATUS status = api->ConfigurationOpen(
        quic_transport->registration, &alpn, 1, NULL, 0, NULL,
        &quic_transport->configuration);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ConfigurationOpen failed: 0x%x", status);
        return -1;
    }

    QUIC_CERTIFICATE_FILE cert_file;
    cert_file.CertificateFile = quic_transport->cert_path;
    cert_file.PrivateKeyFile = quic_transport->key_path;

    QUIC_CREDENTIAL_CONFIG cred_config;
    memset(&cred_config, 0, sizeof(cred_config));
    cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_config.CertificateFile = &cert_file;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    status = api->ConfigurationLoadCredential(quic_transport->configuration,
                                               &cred_config);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ConfigurationLoadCredential failed: 0x%x",
                  status);
        api->ConfigurationClose(quic_transport->configuration);
        quic_transport->configuration = NULL;
        return -1;
    }

    status = api->ListenerOpen(quic_transport->registration,
                                _quic_listener_callback, quic_transport,
                                &quic_transport->listener);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ListenerOpen failed: 0x%x", status);
        api->ConfigurationClose(quic_transport->configuration);
        quic_transport->configuration = NULL;
        return -1;
    }

    QUIC_ADDR listen_addr;
    QuicAddrSetFamily(&listen_addr, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&listen_addr, quic_transport->port);

    status = api->ListenerStart(quic_transport->listener, &alpn, 1,
                                 &listen_addr);
    if (QUIC_FAILED(status)) {
        log_error("quic transport: ListenerStart failed: 0x%x", status);
        api->ListenerClose(quic_transport->listener);
        quic_transport->listener = NULL;
        api->ConfigurationClose(quic_transport->configuration);
        quic_transport->configuration = NULL;
        return -1;
    }

    ATOMIC_STORE(&quic_transport->base.running, 1);
    quic_transport->public.running = true;
    log_info("quic transport: listening on port %u",
             (unsigned)quic_transport->port);
    return 0;
}

int poseidon_transport_quic_stop(poseidon_transport_t* transport) {
    if (transport == NULL) return 0;

    poseidon_quic_transport_t* quic_transport = _quic_from_public(transport);
    if (!ATOMIC_LOAD(&quic_transport->base.running)) {
        return 0;
    }

    ATOMIC_STORE(&quic_transport->base.running, 0);
    quic_transport->public.running = false;

    const struct QUIC_API_TABLE* api = quic_transport->api;

    // Stop the listener first to prevent new connections.
    if (quic_transport->listener != NULL) {
        api->ListenerStop(quic_transport->listener);
        api->ListenerClose(quic_transport->listener);
        quic_transport->listener = NULL;
    }

    // Ask the connections to close, then shut the connections down.
    platform_mutex_lock(quic_transport->conn_lock);
    for (int i = 0; i < quic_transport->base.connections.length; i++) {
        poseidon_quic_connection_t* conn =
            (poseidon_quic_connection_t*)quic_transport->base.connections.data[i];
        if (conn == NULL) continue;
        if (ATOMIC_LOAD(&conn->closed)) continue;
        poseidon_quic_connection_close(conn, true);
        if (conn->connection != NULL) {
            api->ConnectionShutdown(conn->connection,
                                    QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        }
    }
    platform_mutex_unlock(quic_transport->conn_lock);

    // Wait for msquic to deliver the SHUTDOWN_COMPLETE callbacks.
    platform_sleep_ms(100);

    if (quic_transport->configuration != NULL) {
        api->ConfigurationClose(quic_transport->configuration);
        quic_transport->configuration = NULL;
    }

    return 0;
}

void poseidon_transport_quic_destroy(poseidon_transport_t* transport) {
    if (transport == NULL) return;

    poseidon_quic_transport_t* quic_transport = _quic_from_public(transport);
    poseidon_transport_internal_t* base = &quic_transport->base;

    if (ATOMIC_LOAD(&base->running)) {
        poseidon_transport_quic_stop(transport);
    }

    // Quiesce the connection actors before the msquic handles go away:
    // a running dispatch may still be writing to a stream, and the
    // stream belongs to the configuration.
    for (int i = 0; i < base->connections.length; i++) {
        poseidon_quic_connection_t* conn =
            (poseidon_quic_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;
        actor_destroy(&conn->actor);
    }

    // The msquic handles go next: closing the registration and the
    // global msquic state joins the worker threads, so no callback can
    // fire after this.
    if (quic_transport->configuration != NULL) {
        quic_transport->api->ConfigurationClose(quic_transport->configuration);
        quic_transport->configuration = NULL;
    }
    if (quic_transport->listener != NULL) {
        quic_transport->api->ListenerClose(quic_transport->listener);
        quic_transport->listener = NULL;
    }
    poseidon_msquic_close_registration("poseidon_client");

    // Wait for the rest of the pool to go quiet as well: a client
    // session actor may still be dispatching a delivery that reaches
    // back into this transport.
    if (base->pool != NULL &&
        !ATOMIC_LOAD(&base->pool->terminate)) {
        scheduler_pool_wait_for_idle(base->pool);
    }

    // Release the connection resources. The actors are quiesced, so
    // nothing touches these fields any more.
    platform_mutex_lock(quic_transport->conn_lock);
    for (int i = base->connections.length - 1; i >= 0; i--) {
        poseidon_quic_connection_t* conn =
            (poseidon_quic_connection_t*)base->connections.data[i];
        if (conn == NULL) continue;

        conn->is_closing = 1;
        ATOMIC_STORE(&conn->closed, 1);
        conn->stream = NULL;
        conn->connection = NULL;
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
        poseidon_quic_connection_destroy(conn);
    }
    vec_deinit(&base->connections);
    platform_mutex_unlock(quic_transport->conn_lock);

    // The shared body is actor-shaped: destroy its actor before the
    // transport's memory goes away.
    actor_destroy(&base->actor);

    destroy_stack_destroy(&base->destroy_stack);

    if (quic_transport->conn_lock != NULL) {
        platform_mutex_destroy(quic_transport->conn_lock);
        quic_transport->conn_lock = NULL;
    }

    free(quic_transport->cert_path);
    free(quic_transport->key_path);
    quic_transport->cert_path = NULL;
    quic_transport->key_path = NULL;

    free(quic_transport);

    poseidon_msquic_close();
}

// ============================================================================
// SEND
// ============================================================================

// Hands the payload to the connection's write actor. The connection
// frames it and serializes the write, so a reply never interleaves
// with another reply on the same stream.
static int _quic_transport_send_to_client(poseidon_transport_internal_t* transport,
                                          int client_id,
                                          const uint8_t* data,
                                          size_t len) {
    if (transport == NULL || (data == NULL && len > 0)) return -1;

    poseidon_quic_transport_t* quic_transport = _quic_from_base(transport);

    platform_mutex_lock(quic_transport->conn_lock);
    poseidon_quic_connection_t* conn =
        _find_connection_by_session_id(quic_transport, (uint32_t)client_id);
    if (conn != NULL) {
        // The enqueue happens under the lock, so a connection that is
        // being torn down cannot be freed while the frame is on its
        // way to it.
        poseidon_quic_connection_write(conn, data, len);
    }
    platform_mutex_unlock(quic_transport->conn_lock);

    if (conn == NULL) {
        return -1;
    }

    return 0;
}

// The vtable-shaped send hook. The public view and the internal body
// are two members of the same allocation, so the internal body is
// reached through the public pointer.
static int _quic_vtable_send(poseidon_transport_t* self,
                             int client_id,
                             const uint8_t* data,
                             size_t len) {
    if (self == NULL) return -1;
    poseidon_quic_transport_t* quic_transport = _quic_from_public(self);
    return _quic_transport_send_to_client(&quic_transport->base, client_id,
                                          data, len);
}

int poseidon_transport_quic_send(poseidon_transport_t* transport,
                                 int client_id,
                                 const uint8_t* data,
                                 size_t len) {
    if (transport == NULL) return -1;
    poseidon_quic_transport_t* quic_transport = _quic_from_public(transport);
    return _quic_transport_send_to_client(&quic_transport->base, client_id,
                                          data, len);
}
