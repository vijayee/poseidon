/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor messages.
 */

#ifndef POSEIDON_MESSAGE_H
#define POSEIDON_MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Forward declarations. Subsystem headers are pulled in by the actor /
 * subsystem that emits each message — keeping this header dependency-free
 * lets the actor machinery compile without dragging in BlockCache / Network /
 * Streams / etc. */
struct actor_t;
typedef struct actor_t actor_t;

struct ofd_t;
typedef struct ofd_t ofd_t;

struct buffer_t;
typedef struct buffer_t buffer_t;

/* Message types. Poseidon-minimal skeleton starts with the TIMER_* family
 * (consumed by Timer/timer_actor.c); additional types are introduced as
 * subsystems are actorized. The MERIDIAN_CONN_* family is added in Phase 2
 * of the actor migration; payload structs live in
 * Network/Meridian/meridian_conn_messages.h. */
typedef enum {
  /* Timer actor: a debounce window elapsed. */
  TIMER_DEBOUNCE,
  /* Timer actor: a debounce window was force-flushed. */
  TIMER_DEBOUNCE_FLUSH,
  /* Timer actor: a direct (non-debounced) timer fired. The payload carries
   * a timer_completion_payload_t (Timer/timer_actor.h) whose completion_type
   * is whatever the originating TIMER_SET (or debounce) recorded. */
  TIMER_COMPLETION,
  /* Timer actor: arm a new direct timer. The payload is the
   * timer_set_payload_t declared in Timer/timer_actor.h. */
  TIMER_SET,

  /* Meridian connection actor: a 1:1 carrier between the connection's
   * msquic callback sites and the actor's serial execution. Reserved
   * payload slots live in Network/Meridian/meridian_conn_messages.h.
   *
   * Naming follows the project-wide convention: <UPPER_SNAKE_CASE> family
   * names are owned by the subsystem's <lower_snake_case>_messages.h. */

  /* Initiate a connection (was: meridian_conn_connect). */
  MERIDIAN_CONN_CONNECT,
  /* Tear down the active connection paths (was: meridian_conn_disconnect).
   * May synchronously fire msquic's close callback, so the dispatch path
   * must tolerate a single re-entry of the actor_send() call. */
  MERIDIAN_CONN_DISCONNECT,
  /* Send a datagram over the active path. Payload: meridian_conn_send_*
   * (Network/Meridian/meridian_conn_messages.h). Borrowed: the caller's
   * buffer must outlive the actor's processing of this message. */
  MERIDIAN_CONN_SEND,
  /* Promote a relay-only connection to direct (was: meridian_conn_upgrade_to_direct). */
  MERIDIAN_CONN_UPGRADE_TO_DIRECT,
  /* msquic callback wired in a later phase: the direct path is up. */
  MERIDIAN_CONN_DIRECT_CONNECTED,
  /* msquic callback wired in a later phase: the relay path is up. */
  MERIDIAN_CONN_RELAY_CONNECTED,
  /* msquic callback wired in a later phase: a path failed. */
  MERIDIAN_CONN_PATH_FAILED,
  /* Reflect a peer NAT-type change (was: meridian_conn_set_peer_nat_type). */
  MERIDIAN_CONN_SET_PEER_NAT_TYPE,
  /* Reflect a peer reflexive address update (was: meridian_conn_set_peer_reflexive). */
  MERIDIAN_CONN_SET_PEER_REFLEXIVE,
  /* Test-only: force conn->state. Bypasses state-machine validation; test
   * fixture helper. (was: direct field assignment in tests). */
  MERIDIAN_CONN_TEST_FORCE_STATE,

  /* --- poseidon_channel_manager actor (subsystem actor) ---
   *
   * Subsystem: Channel/channel_manager.{c,h}
   * Per-subsystem payload layouts live in
   * src/Channel/channel_manager_messages.h, mirroring the
   * meridian_conn_messages.h split that keeps this header
   * dependency-free.
   *
   * 2.10 routes the 11 mutators of poseidon_channel_manager_t
   * (create_channel, join_channel, destroy_channel, the two
   * bootstrap handlers, the two notice handlers, add_tombstone,
   * expire_tombstones, register_session, unregister_session)
   * through actor_send. All callers (client_session.c
   * create/join, channel_manager_bootstrap_intercept from the
   * subscriber thread, and the in-process notice handlers) do a
   * round trip - the request carries a reply the actor answers and
   * the caller waits for it - so the public function return values
   * (poseidon_channel_t* / int) remain available to the wrapper
   * through borrowed int** / X** out-params on the payload
   * struct. 2.21 replaced the last inline drain with this.
   */

  /* Create a new locally-owned channel. The wrapper's stack frame
   * owns the out_channel slot; the handler writes the new
   * poseidon_channel_t* into *out_channel (or NULL on rejection).
   * (was: direct field mutation under mgr->lock). */
  POSEIDON_CHANNEL_MANAGER_MSG_CREATE_CHANNEL,
  /* Join an existing channel via bootstrap request. Same
   * out-channel convention as CREATE_CHANNEL; the handler also
   * records a pending_bootstrap_t entry so the eventual
   * HANDLE_BOOTSTRAP_REPLY can resolve it.
   * (was: direct field mutation under mgr->lock). */
  POSEIDON_CHANNEL_MANAGER_MSG_JOIN_CHANNEL,
  /* Tear down a channel identified by node_id. No return value.
   * (was: direct field mutation under mgr->lock). */
  POSEIDON_CHANNEL_MANAGER_MSG_DESTROY_CHANNEL,
  /* I/O-bound bootstrap reply handler. Splits the work: mutates
   * pending_bootstraps under the actor, then releases for the
   * meridian_protocol_connect + add_seed_node loop. Wrapper
   * reads *out_rc for the int return.
   * (was: lock L317 around the pending mutation, then unlocked
   * I/O — 2.10 preserves the split via actor dispatch). */
  POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REPLY,
  /* Inbound bootstrap request from a peer. If a tombstone exists
   * for the topic, a synthesized delete/modify notice is
   * published on the dial channel synchronously inside the
   * handler; otherwise the request is routed to
   * poseidon_channel_handle_bootstrap_request. Wrapper reads
   * *out_rc for the int return.
   * (was: tombstone check + encode + synchronous publish on
   * mgr->dial_channel inside the request handler — 2.10 keeps
   * the synchronous publish because it is the only way to
   * satisfy the protocol reply before the requester times out).
   */
  POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_BOOTSTRAP_REQUEST,
  /* Inbound delete notice from the network. The wrapper keeps
   * the caller-allocated meridian_channel_delete_notice_t alive
   * across the actor_run drain via the borrowed pointer on the
   * payload. Handler rebuilds a tombstone, finds any matching
   * local channel, deletes it, then re-publishes the notice on
   * the dial channel.
   * (was: direct field mutation under mgr->lock L507). */
  POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_DELETE_NOTICE,
  /* Inbound modify notice. Same pattern as HANDLE_DELETE_NOTICE
   * but the tombstone type is MODIFY and the matched local
   * channel is rejoined with the supplied config.
   * (was: direct field mutation under mgr->lock L557). */
  POSEIDON_CHANNEL_MANAGER_MSG_HANDLE_MODIFY_NOTICE,
  /* Append a tombstone to the manager's ring. The wrapper's
   * tombstone is copied into the actor's local copy, so the
   * caller can free its tombstone immediately after
   * actor_send + actor_run returns.
   * (was: direct append under mgr->lock L633). */
  POSEIDON_CHANNEL_MANAGER_MSG_ADD_TOMBSTONE,
  /* Sweep expired tombstones. Called from tick_all under the
   * timing wheel. No arguments; no return value.
   * (was: in-place sweep under mgr->lock L676). */
  POSEIDON_CHANNEL_MANAGER_MSG_EXPIRE_TOMBSTONES,
  /* Register a client session in the manager's session
   * registry. The session pointer is borrowed (opaque void*);
   * the registry does not take ownership.
   * (was: capacity check + append under mgr->lock L753). */
  POSEIDON_CHANNEL_MANAGER_MSG_REGISTER_SESSION,
  /* Unregister a client session. swap-remove lookup + drop
   * under the actor.
   * (was: swap-remove under mgr->lock L765). */
  POSEIDON_CHANNEL_MANAGER_MSG_UNREGISTER_SESSION,

  /* ========================================================================
   * 2.11 Unix ClientAPI transport — listener + per-connection actor messages
   * ========================================================================
   *
   * Splits the Unix client-side file into two actors: the per-connection
   * actor (POSEIDON_UNIX_CONNECTION_*) and the listener actor
   * (POSEIDON_UNIX_SERVER_*). Payload structs live in
   * src/ClientAPIs/unix_connection_messages.h; this enum only carries
   * the type tags.
   *
   * Net drop vs the message set that lived in the actor-merged form: the
   * CACHE_* family (3 entries, application-level cache invalidation) and
   * OFD_CACHE_RESOLVE_RESULT are dropped because poseidon does not carry
   * an application cache on the transport path. The result is 8 types
   * where the actor-merged form had 10. */
  __POSEIDON_UNIX_MESSAGE_BASE,
  /* Inbound bytes read off the connection's socket. The payload is a
   * poseidon_unix_connection_data_payload_t* holding a buffer_t* with
   * the raw bytes as they arrived; the connection actor reassembles
   * the frames from them. */
  POSEIDON_UNIX_CONNECTION_DATA = __POSEIDON_UNIX_MESSAGE_BASE,
  /* Peer closed or half-closed the socket (PD_EVENT_HANGUP). No
   * payload. */
  POSEIDON_UNIX_CONNECTION_HANGUP,
  /* Watcher reported an error (PD_EVENT_ERROR). The watcher fd is
   * still valid; the connection drains any remaining data and then
   * proceeds through the normal close path. No payload. */
  POSEIDON_UNIX_CONNECTION_ERROR,
  /* The socket became writable again. The connection actor retries the
   * send of the frame that did not fit into the kernel's send buffer.
   * No payload: the pending bytes live in the connection's write
   * buffer. */
  POSEIDON_UNIX_CONNECTION_WRITABLE,
  /* Outbound frame submitted by the session actor. Payload is a
   * poseidon_unix_connection_write_payload_t* (already-framed
   * buffer_t*); freed via the static destroyer registered with the
   * message. */
  POSEIDON_UNIX_CONNECTION_WRITE,
  /* Session actor or timer requested close. Payload is a
   * poseidon_unix_connection_close_payload_t* (graceful bool). */
  POSEIDON_UNIX_CONNECTION_CLOSE,
  /* Per-connection read callback asks the listener to rearm a watcher's
   * event mask. Payload is a
   * poseidon_unix_server_watcher_update_payload_t*; the listener
   * dispatches pd_watcher_update inline on the loop thread. */
  POSEIDON_UNIX_SERVER_UPDATE_WATCHER,
  /* Per-connection destroy or read callback asks the listener to stop
   * and eventually destroy a watcher. Payload is a
   * poseidon_unix_server_watcher_update_payload_t*; the listener
   * calls pd_watcher_stop inline, pushes the watcher onto the
   * transport's destroy_stack_t for deferred pd_watcher_destroy on
   * the next drain, and (if payload->sock is non-NULL) calls
   * platform_socket_destroy on the snapshot fd. */
  POSEIDON_UNIX_SERVER_STOP_WATCHER,

  /* ========================================================================
   * 2.12 TCP ClientAPI transport — per-connection actor messages
   * ========================================================================
   *
   * Splits the TCP client-side file into a listener (transport_tcp.c) and
   * a per-connection actor (tcp_connection.c). Payload structs live in
   * src/ClientAPIs/tcp_connection_messages.h; this enum only carries the
   * type tags.
   *
   * Unlike the Unix family, the TCP connection reads its socket on the
   * actor thread: a TLS connection's SSL object may only be used from a
   * single thread, so the I/O thread's watcher callback only signals
   * readiness and the actor performs the read, the write and the
   * handshake. That is why the family has a READ entry instead of the
   * Unix family's DATA entry: READ carries no payload, the bytes are
   * pulled off the socket by the actor itself. */
  __POSEIDON_TCP_MESSAGE_BASE,
  /* The socket is readable. The connection actor drains the socket and
   * reassembles the frames from the bytes it read. No payload. */
  POSEIDON_TCP_CONNECTION_READ = __POSEIDON_TCP_MESSAGE_BASE,
  /* Peer closed or half-closed the socket (PD_EVENT_HANGUP). No
   * payload. */
  POSEIDON_TCP_CONNECTION_HANGUP,
  /* Watcher reported an error (PD_EVENT_ERROR). No payload. */
  POSEIDON_TCP_CONNECTION_ERROR,
  /* The socket became writable again. The connection actor retries the
   * send of the frame that did not fit into the kernel's send buffer.
   * No payload: the pending bytes live in the connection's write
   * buffer. */
  POSEIDON_TCP_CONNECTION_WRITABLE,
  /* Outbound frame submitted by the session actor. Payload is a
   * poseidon_tcp_connection_write_payload_t* (already-framed
   * buffer_t*); freed via the static destroyer registered with the
   * message. */
  POSEIDON_TCP_CONNECTION_WRITE,
  /* Session actor or timer requested close. Payload is a
   * poseidon_tcp_connection_close_payload_t* (graceful bool). */
  POSEIDON_TCP_CONNECTION_CLOSE,

  /* ========================================================================
   * 2.13 WebSocket ClientAPI transport — per-connection actor messages
   * ========================================================================
   *
   * Splits the WebSocket client-side file into a listener
   * (transport_ws.c) and a per-connection actor (ws_connection.c).
   * Payload structs live in src/ClientAPIs/ws_connection_messages.h;
   * this enum only carries the type tags.
   *
   * The family mirrors the TCP one, for the same reason: a TLS
   * connection's SSL object may only be used from a single thread, so
   * the I/O thread's watcher callback only signals readiness and the
   * actor performs the read, the write and the handshake. READ
   * therefore carries no payload — the bytes are pulled off the socket
   * by the actor itself. */
  __POSEIDON_WS_MESSAGE_BASE,
  /* The socket is readable. The connection actor drains the socket and
   * runs the HTTP upgrade or the WebSocket frame decoder on the bytes
   * it read. No payload. */
  POSEIDON_WS_CONNECTION_READ = __POSEIDON_WS_MESSAGE_BASE,
  /* Peer closed or half-closed the socket (PD_EVENT_HANGUP). No
   * payload. */
  POSEIDON_WS_CONNECTION_HANGUP,
  /* Watcher reported an error (PD_EVENT_ERROR). No payload. */
  POSEIDON_WS_CONNECTION_ERROR,
  /* The socket became writable again. The connection actor retries the
   * send of the frame that did not fit into the kernel's send buffer.
   * No payload: the pending bytes live in the connection's write
   * buffer. */
  POSEIDON_WS_CONNECTION_WRITABLE,
  /* Outbound frame submitted by the session actor. Payload is a
   * poseidon_ws_connection_write_payload_t* (already-framed
   * buffer_t*); freed via the static destroyer registered with the
   * message. */
  POSEIDON_WS_CONNECTION_WRITE,
  /* Session actor or timer requested close. Payload is a
   * poseidon_ws_connection_close_payload_t* (graceful bool). */
  POSEIDON_WS_CONNECTION_CLOSE,

  /* ========================================================================
   * 2.14 WebTransport ClientAPI transport — per-connection actor messages
   * ========================================================================
   *
   * Splits the WebTransport client-side file into a listener
   * (transport_webtransport.c) and a per-connection actor
   * (wt_connection.c). Payload structs live in
   * src/ClientAPIs/wt_connection_messages.h; this enum only carries the
   * type tags.
   *
   * The family mirrors the TCP one, with one difference: there is no
   * socket to read from. libwtf owns the I/O and hands the received
   * bytes to the transport's callback, so DATA carries the bytes
   * themselves instead of a readiness signal. The stream that the
   * session opened is announced with STREAM_OPENED, so the actor is
   * the only thread that ever touches it.
   */
  __POSEIDON_WT_MESSAGE_BASE,
  /* Inbound bytes received on the session's stream. The payload is a
   * poseidon_wt_connection_data_payload_t* holding a copy of the bytes
   * as they arrived; the connection actor reassembles the frames from
   * them. */
  POSEIDON_WT_CONNECTION_DATA = __POSEIDON_WT_MESSAGE_BASE,
  /* Outbound frame submitted by the session actor. Payload is a
   * poseidon_wt_connection_write_payload_t* (already-framed bytes);
   * freed via the static destroyer registered with the message. */
  POSEIDON_WT_CONNECTION_WRITE,
  /* Session actor or timer requested close. Payload is a
   * poseidon_wt_connection_close_payload_t* (graceful bool). */
  POSEIDON_WT_CONNECTION_CLOSE,
  /* The session opened a bidirectional stream. Payload is a
   * poseidon_wt_connection_stream_payload_t* holding the stream
   * pointer; the actor owns it from then on. */
  POSEIDON_WT_CONNECTION_STREAM_OPENED,
  /* The stream closed, was aborted, or the peer closed its side. No
   * payload: the actor drops the stream and tears the connection
   * down. */
  POSEIDON_WT_CONNECTION_STREAM_CLOSED,

  /* ========================================================================
   * 2.15 QUIC ClientAPI transport — per-connection actor messages
   * ========================================================================
   *
   * Splits the QUIC client-side file into a listener (transport_quic.c)
   * and a per-connection actor (quic_connection.c). Payload structs
   * live in src/ClientAPIs/quic_connection_messages.h; this enum only
   * carries the type tags.
   *
   * The family mirrors the WebTransport one: msquic owns the I/O and
   * delivers the data through the transport's callbacks, so DATA
   * carries the bytes themselves instead of a readiness signal.
   */
  __POSEIDON_QUIC_MESSAGE_BASE,
  /* Inbound bytes received on the client's stream. The payload is a
   * poseidon_quic_connection_data_payload_t* holding a copy of the
   * bytes as they arrived; the connection actor reassembles the frames
   * from them. */
  POSEIDON_QUIC_CONNECTION_DATA = __POSEIDON_QUIC_MESSAGE_BASE,
  /* Outbound frame submitted by the session actor. Payload is a
   * poseidon_quic_connection_write_payload_t* (already-framed bytes);
   * freed via the static destroyer registered with the message. */
  POSEIDON_QUIC_CONNECTION_WRITE,
  /* Session actor or timer requested close. Payload is a
   * poseidon_quic_connection_close_payload_t* (graceful bool). */
  POSEIDON_QUIC_CONNECTION_CLOSE,
  /* The stream is ready to carry the outbound frames. No payload. */
  POSEIDON_QUIC_CONNECTION_STREAM_READY,
  /* The peer shut down its sending side. The connection answers with a
   * graceful shutdown of its own. No payload. */
  POSEIDON_QUIC_CONNECTION_PEER_SHUTDOWN,

  /* ========================================================================
   * 2.16 Meridian relay client — work queue
   * ========================================================================
   *
   * The relay client is a foreign-thread subsystem: msquic delivers the
   * connection and the stream events on its own worker threads, and the
   * application calls the public API from its own thread. It follows the
   * shape upstream's relay client uses — the flags that are read across
   * threads are atomic, the connection table and the registered callbacks
   * are guarded by one small mutex, and the actor carries the outbound
   * work that must not run on a callback thread.
   *
   * There is one message: the address request the msquic connection
   * callback hands over, so the request is not built on msquic's worker
   * thread. The public API does its work on the calling thread and
   * returns its result, the way upstream's relay client connect and
   * disconnect do. */
  MERIDIAN_RELAY_ADDR_REQUEST,
  /* The relay connection dropped: reconnect, after the backoff. Upstream's
   * relay client has the same event (liboffs/src/Network/relay_client.c:
   * RELAY_CLIENT_RETRY). */
  MERIDIAN_RELAY_RETRY,

  /* ========================================================================
   * 2.18 Meridian protocol — actor messages
   * ========================================================================
   *
   * The protocol owns the peer and stream tables, the way upstream's
   * network actor owns the peer list. msquic delivers the connection and
   * the stream events on its own worker threads; those events are handed
   * to the protocol's actor, which mutates the tables on its own thread.
   *
   * Payload structs live in
   * src/Network/Meridian/meridian_protocol_messages.h; this enum only
   * carries the type tags. */
  /* A peer connected. The actor adds it to the peer table. */
  MERIDIAN_PROTOCOL_PEER_ADDED,
  /* Drop the peer with this address. The actor closes it and takes it out
   * of the table. */
  MERIDIAN_PROTOCOL_PEER_REMOVED,
  /* A snapshot of the peer table. The actor builds the copy. */
  MERIDIAN_PROTOCOL_PEERS_SNAPSHOT,
  /* Send a packet to one peer. The actor finds the connection and sends. */
  MERIDIAN_PROTOCOL_SEND_PACKET,
  /* Send a measure request to one peer. The actor finds the connection
   * and puts the PING on a stream. */
  MERIDIAN_PROTOCOL_SEND_MEASURE,
  /* A measurement reply arrived. The actor matches it and completes it. */
  MERIDIAN_PROTOCOL_MEASURE_RESPONSE,
  /* Record a measurement result in the latency cache. */
  MERIDIAN_PROTOCOL_MEASURE_RESULT,
  /* Find the closest node in the ring set. */
  MERIDIAN_PROTOCOL_FIND_CLOSEST,
  /* Run the gossip tick. The actor ticks the scheduler and sends. */
  MERIDIAN_PROTOCOL_GOSSIP_TICK,
  /* A gossip reply arrived. The actor matches it and completes it. */
  MERIDIAN_PROTOCOL_GOSSIP_RESPONSE,
  /* Send a packet to every peer. The actor does the fan-out. */
  MERIDIAN_PROTOCOL_BROADCAST,
  /* Tear the protocol down. The actor closes the connections, the
   * listener, the configuration and the registration: it owns the peer
   * table and the HQUIC lifecycle. */
  MERIDIAN_PROTOCOL_STOP,
  /* Open a connection to a peer. The actor checks the peer table, opens
   * the connection and adds the peer: the table is its own. */
  MERIDIAN_PROTOCOL_CONNECT,
  /* A connection died. The actor takes the peer out of the table. */
  MERIDIAN_PROTOCOL_PEER_DISCONNECTED,

  /* ========================================================================
   * 2.19 Meridian rendezvous — actor messages
   * ========================================================================
   *
   * The rendezvous handle follows the shape upstream's nat_detect uses: an
   * actor, a scheduler pool, the state embedded in the struct and no
   * mutex. The tunnel data arrives on msquic's worker threads and is
   * handed to the actor, which delivers it to the application callback.
   *
   * Payload structs live in
   * src/Network/Meridian/meridian_rendv_messages.h; this enum only
   * carries the type tags. */
  /* Set the local rendezvous point. */
  MERIDIAN_RENDV_SET_LOCAL,
  /* Add a peer rendezvous point. */
  MERIDIAN_RENDV_ADD_PEER,
  /* Remove a peer rendezvous by address. */
  MERIDIAN_RENDV_REMOVE_PEER,
  /* Find a peer rendezvous by address. */
  MERIDIAN_RENDV_FIND_PEER,
  /* Register the application callback for tunnel data. */
  MERIDIAN_RENDV_SET_STREAM_CB,
  /* Tunnel data arrived. The actor delivers it to the callback. */
  MERIDIAN_RENDV_STREAM_DATA,
  /* Detect the NAT type. The actor sends the address requests. */
  MERIDIAN_RENDV_DETECT_NAT,
  /* A relay client learned this node's reflexive address. The relay
   * client hands it to the rendezvous's actor as an event, the way
   * upstream's relay client hands the address response to the NAT
   * detector's actor (liboffs/src/Network/relay_client.c). */
  MERIDIAN_RENDV_ADDR_RESPONSE,

  /* ========================================================================
   * 2.20 Quasar overlay — actor messages
   * ========================================================================
   *
   * The overlay's state belongs to the protocol's actor — the network
   * actor, the way upstream keeps the gossip inside it. The routing
   * filter, the local subscriptions, the per-neighbour filters, the seen
   * filter and the application callback all live there, and the overlay
   * reads the peer table directly instead of asking another actor for
   * it. A second actor for the overlay would mean a second actor to
   * wait for, and a worker that waits runs no actor.
   *
   * Payload structs live in src/Network/Quasar/quasar_messages.h; this
   * enum only carries the type tags. */
  /* Register the application callback for delivered messages. */
  MERIDIAN_QUASAR_SET_MESSAGE_CB,
  /* Subscribe to a topic. */
  MERIDIAN_QUASAR_SUBSCRIBE,
  /* Unsubscribe from a topic. */
  MERIDIAN_QUASAR_UNSUBSCRIBE,
  /* Publish a message into the overlay. */
  MERIDIAN_QUASAR_PUBLISH,
  /* Run the propagation walk. */
  MERIDIAN_QUASAR_PROPAGATE,
  /* Run the periodic tick. */
  MERIDIAN_QUASAR_TICK,
  /* Look up a neighbour's filter. */
  MERIDIAN_QUASAR_GET_NEIGHBOR_FILTER,
  /* Create a neighbour's filter if it is missing. */
  MERIDIAN_QUASAR_GET_OR_CREATE_NEIGHBOR_FILTER,
  /* Remove a neighbour's filter. */
  MERIDIAN_QUASAR_REMOVE_NEIGHBOR_FILTER,
  /* A gossip arrived from a peer. */
  MERIDIAN_QUASAR_ON_GOSSIP,
  /* A routed message arrived from a peer. */
  MERIDIAN_QUASAR_ON_ROUTE_MESSAGE,
  /* The overlay is going away: the actor frees the overlay's state. */
  MERIDIAN_QUASAR_DESTROY,
  /* Rebuild the overlay's state with a new configuration. */
  MERIDIAN_QUASAR_RECONFIGURE,

  /* ========================================================================
   * 2.21 Meridian stream — per-stream actor messages
   * ========================================================================
   *
   * A Meridian stream has an actor, the way upstream's stream does. The
   * receive buffer belongs to that actor, and the public entry points
   * post messages to it instead of touching the stream's state in place:
   * the bytes that arrive are handed over, and so is the news that the
   * peer finished sending. The owner's handler then runs on the stream's
   * actor, not on msquic's worker.
   *
   * Payload structs live in
   * src/Network/Meridian/meridian_stream.h; this enum only carries the
   * type tags. */
  /* Bytes arrived. The request owns the copy. */
  MERIDIAN_STREAM_RECEIVE,
  /* The peer finished sending: the actor hands the message to the owner. */
  MERIDIAN_STREAM_COMPLETE,
  /* The stream is going away: the actor answers once it has worked
   * through everything that was queued before it. */
  MERIDIAN_STREAM_CLOSE,
} message_type_e;

/* Base message. The actor's dispatch receives one of these; type selects the
 * payload layout, payload is heap-allocated by the sender and freed via
 * payload_destroy by the receiver (or by message_queue on drop). */
typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_MESSAGE_H */

/*
 * Local modifications
 * -------------------
 *
 * Vendored from liboffs/src/Actor/message.h and trimmed to a poseidon-minimal
 * skeleton for commit 1.4 of the actor migration. Compared to the upstream
 * file:
 *
 *   - Include guard renamed OFFS_MESSAGE_H -> POSEIDON_MESSAGE_H.
 *   - File prelude replaced with the poseidon SPDX header.
 *   - Removed the ../BlockCache/block.h and ../Network/node_id.h includes
 *     and replaced block_t / node_id_t references with forward declarations
 *     of actor_t / ofd_t / buffer_t.
 *   - Removed the CLOSEST_NODES_MAX_RING_SAMPLES_MSG macro.
 *   - Removed the entire BlockCache (CACHE_*, SECTION_*, SECTIONS_*,
 *     BLOCK_*, READABLE_*, WRITEABLE_*, CLOSE_STREAM, DEFERRED_DEREF,
 *     INDEX_SAVE, SECTION_WRITE_META, TUPLE_CACHE_*, OFF_STREAM_WRITE,
 *     WRITEABLE_FINALIZE, WRITEABLE_DESCRIPTOR_WRITE, BLOCK_RECIPE_PULL,
 *     RECIPE_BLOCK_DATA, RECIPE_ROTATE, SECTION_WRITE_COMPLETE,
 *     SECTION_READ_COMPLETE, SECTION_DEALLOCATE_COMPLETE, STREAM_NOTIFY),
 *     HTTP (HTTP_CONNECTION_*, HTTP_SERVER_*, OFD_CACHE_*), STREAM_*,
 *     network/peer (NETWORK_*, PEER_*, CONN_STATE_*, CM_*, TOPOLOGY_*,
 *     NETWORK_LOCAL_*, NETWORK_CLOSEST_NODES_*, PEER_BOOK_*,
 *     RESPIRATION_*), transport (UNIX_CONNECTION_*, UNIX_SERVER_*,
 *     TCP_CONNECTION_*, TCP_SERVER_*, WS_CONNECTION_*, WS_SERVER_*,
 *     WT_CONNECTION_*), and ephemeral/pin/registry (CACHE_EPHEMERAL,
 *     CACHE_PIN, CACHE_UNPIN, CACHE_EPHEMERAL_LIST/RESULT, CACHE_PIN_RESULT,
 *     CACHE_UNPIN_RESULT, EPHEMERAL_REGISTRY_*, REPRESENTATION_*) message
 *     type groups.
 *   - Removed all upstream payload structs (cache_*, ofd_cache_*,
 *     registry_*, respiration_*, ephemeral_*, network_local_find_block_*,
 *     network_find_block_*, network_local_store_block_*,
 *     network_store_block_*, network_addr_response_*, network_nat_*,
 *     network_closest_nodes_*, network_measure_nodes_*, etc.) and their
 *     destroy functions.
 *   - Kept only the TIMER_* family (TIMER_DEBOUNCE, TIMER_DEBOUNCE_FLUSH,
 *     TIMER_COMPLETION, TIMER_SET) — the only message_type_e values
 *     consumed by the vendored Timer/timer_actor.c. Additional message
 *     types are introduced in later migration phases as Quasar, Meridian,
 *     and other subsystems are actorized.
 *
 * The trimmed enum lets message_queue.c, actor.c, and pool.c compile with
 * no BlockCache / Network / Streams / transport dependencies, which is the
 * test-pass gate for commit 1.4. Future commits extend the enum as each
 * subsystem is brought onto the actor model.
 *
 * Phase 2 (commit 2.1) adds the MERIDIAN_CONN_* family (11 entries as of 2.9) above
 * the local-modifications block. The enum is the global message_type_e;
 * Meridian connection payload structs live in the per-subsystem header
 * Network/Meridian/meridian_conn_messages.h, keeping message.h free of
 * Meridian / msquic / network dependencies. The Timer trim is preserved
 * exactly; the change is purely additive to the enum.
 *
 * Phase 2 (commit 2.10) adds the POSEIDON_CHANNEL_MANAGER_* family (11 entries)
 * above the local-modifications block. The enum is still the global
 * message_type_e; Channel/channel_manager payload structs live in the
 * per-subsystem header Channel/channel_manager_messages.h, keeping
 * message.h free of Channel / network / notice / tombstone dependencies.
 * The change is purely additive to the enum; the 11 existing TIMER_*
 * and 10 MERIDIAN_CONN_* families are unchanged. As with the Meridian
 * connection migration, all 11 channel-manager mutators route through
 * actor_send + actor_run synchronous drain so caller-side return values
 * (poseidon_channel_t* / int) remain available to the wrappers through
 * borrowed out-param pointers on the payload struct.
 *
 * Phase 2 (commit 2.11) adds the POSEIDON_UNIX_* family (7 entries) above
 * the local-modifications block. The enum is still the global
 * message_type_e; the per-connection and listener payload structs live in
 * the per-subsystem header ClientAPIs/unix_connection_messages.h, keeping
 * message.h free of poll-dancer / platform-socket / client-session
 * dependencies. The change is purely additive to the enum; the 4 TIMER_*,
 * 10 MERIDIAN_CONN_*, and 11 POSEIDON_CHANNEL_MANAGER_* families are
 * unchanged. As with the prior two migrations, every connection-level
 * mutator (write, close, watcher-update, watcher-stop) routes through
 * actor_send on the per-connection actor or the listener actor, with
 * the listener draining the transport's destroy_stack_t at the bottom of
 * each pd_loop_run_once iteration so watchers torn down off the I/O
 * thread are freed back on the I/O thread.
 *
 * Phase 2 (commit 2.12) adds the POSEIDON_TCP_* family (6 entries) above
 * the local-modifications block. The enum is still the global
 * message_type_e; the per-connection payload structs live in the
 * per-subsystem header ClientAPIs/tcp_connection_messages.h, keeping
 * message.h free of poll-dancer / platform-socket / client-session
 * dependencies. The change is purely additive to the enum; the 4 TIMER_*,
 * 10 MERIDIAN_CONN_*, 11 POSEIDON_CHANNEL_MANAGER_* and 8 POSEIDON_UNIX_*
 * families are unchanged. The TCP family mirrors the Unix one, with one
 * deliberate difference: READ replaces DATA, because a TLS connection's
 * SSL object may only be used from a single thread, so the connection
 * actor pulls the bytes off the socket itself instead of receiving them
 * from the I/O thread. Every connection-level mutator (write, close)
 * routes through actor_send on the per-connection actor, and the
 * connection's watcher is torn down through the transport's
 * destroy_stack_t so it is unregistered on the I/O thread.
 *
 * Phase 2 (commit 2.13) adds the POSEIDON_WS_* family (6 entries) above
 * the local-modifications block. The enum is still the global
 * message_type_e; the per-connection payload structs live in the
 * per-subsystem header ClientAPIs/ws_connection_messages.h, keeping
 * message.h free of poll-dancer / platform-socket / client-session
 * dependencies. The change is purely additive to the enum; the 4 TIMER_*,
 * 10 MERIDIAN_CONN_*, 11 POSEIDON_CHANNEL_MANAGER_*, 8 POSEIDON_UNIX_*
 * and 6 POSEIDON_TCP_* families are unchanged. The WebSocket family
 * mirrors the TCP one: READ replaces DATA because the connection actor
 * pulls the bytes off the socket itself, and every connection-level
 * mutator (write, close) routes through actor_send on the per-connection
 * actor, with the watcher torn down through the transport's
 * destroy_stack_t.
 *
 * Phase 2 (commit 2.14) adds the POSEIDON_WT_* family (5 entries) above
 * the local-modifications block. The enum is still the global
 * message_type_e; the per-connection payload structs live in the
 * per-subsystem header ClientAPIs/wt_connection_messages.h, keeping
 * message.h free of poll-dancer / platform-socket / client-session
 * dependencies. The change is purely additive to the enum; the 4
 * TIMER_*, 10 MERIDIAN_CONN_*, 11 POSEIDON_CHANNEL_MANAGER_*, 8
 * POSEIDON_UNIX_*, 6 POSEIDON_TCP_* and 6 POSEIDON_WS_* families are
 * unchanged. The WebTransport family mirrors the TCP one, with one
 * deliberate difference: libwtf owns the I/O and hands the received
 * bytes to the transport's callback, so DATA carries the bytes
 * themselves instead of a readiness signal.
 *
 * Phase 2 (commit 2.15) adds the POSEIDON_QUIC_* family (5 entries)
 * above the local-modifications block. The enum is still the global
 * message_type_e; the per-connection payload structs live in the
 * per-subsystem header ClientAPIs/quic_connection_messages.h, keeping
 * message.h free of poll-dancer / platform-socket / client-session
 * dependencies. The change is purely additive to the enum; the 4
 * TIMER_*, 10 MERIDIAN_CONN_*, 11 POSEIDON_CHANNEL_MANAGER_*, 8
 * POSEIDON_UNIX_*, 6 POSEIDON_TCP_*, 6 POSEIDON_WS_* and 5 POSEIDON_WT_*
 * families are unchanged. The QUIC family mirrors the WebTransport one:
 * msquic owns the I/O and delivers the data through the transport's
 * callbacks, so DATA carries the bytes themselves.
 *
 * Phase 2 (commit 2.19) adds the MERIDIAN_RENDV_* family (7 entries)
 * above the local-modifications block. The enum is still the global
 * message_type_e; the payload structs live in the per-subsystem header
 * Network/Meridian/meridian_rendv_messages.h, keeping message.h free of
 * Meridian / msquic dependencies. The change is purely additive to the
 * enum.
 *
 * Phase 2 (commit 2.18) adds the MERIDIAN_PROTOCOL_* family (12 entries)
 * above the local-modifications block. The enum is still the global
 * message_type_e; the payload structs live in the per-subsystem header
 * Network/Meridian/meridian_protocol_messages.h, keeping message.h free
 * of Meridian / msquic / stream dependencies. The change is purely
 * additive to the enum.
 *
 * Phase 2 (commit 2.16) adds the MERIDIAN_RELAY_* family (1 entry) above
 * the local-modifications block. The enum is still the global
 * message_type_e; the change is purely additive to the enum; the 4
 * TIMER_*, 10 MERIDIAN_CONN_*, 11 POSEIDON_CHANNEL_MANAGER_*, 8
 * POSEIDON_UNIX_*, 6 POSEIDON_TCP_*, 6 POSEIDON_WS_*, 5 POSEIDON_WT_* and
 * 5 POSEIDON_QUIC_* families are unchanged. The relay family carries the
 * one piece of work the relay must not build on msquic's callback thread:
 * the address request. The relay's other state is guarded the way
 * upstream's relay client guards it — atomic flags, one small mutex for
 * the connection table and the registered callbacks, and no msquic call
 * made while that mutex is held.
 */
