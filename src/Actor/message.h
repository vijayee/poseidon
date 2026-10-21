/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor messages.
 */

#ifndef POSEIDON_MESSAGE_H
#define POSEIDON_MESSAGE_H

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
  /* Tear down the connection and free the actor. Refcount==0 path only. */
  MERIDIAN_CONN_DESTROY,

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
   * subscriber thread, and the in-process notice handlers)
   * dispatch + drain synchronously on the calling thread using
   * actor_run(actor, 32) so the public function return values
   * (poseidon_channel_t* / int) remain available to the wrapper
   * through borrowed int** / X** out-params on the payload
   * struct.
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
} message_type_e;

/* Base message. The actor's dispatch receives one of these; type selects the
 * payload layout, payload is heap-allocated by the sender and freed via
 * payload_destroy by the receiver (or by message_queue on drop). */
typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

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
 * and 11 MERIDIAN_CONN_* families are unchanged. As with the Meridian
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
 * 11 MERIDIAN_CONN_*, and 11 POSEIDON_CHANNEL_MANAGER_* families are
 * unchanged. As with the prior two migrations, every connection-level
 * mutator (write, close, watcher-update, watcher-stop) routes through
 * actor_send on the per-connection actor or the listener actor, with
 * the listener draining the transport's destroy_stack_t at the bottom of
 * each pd_loop_run_once iteration so watchers torn down off the I/O
 * thread are freed back on the I/O thread.
 */
