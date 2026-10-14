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
  /* Tear down the connection and free the actor. Refcount==0 path only. */
  MERIDIAN_CONN_DESTROY,
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
 * Phase 2 (commit 2.1) adds the MERIDIAN_CONN_* family (10 entries) above
 * the local-modifications block. The enum is the global message_type_e;
 * Meridian connection payload structs live in the per-subsystem header
 * Network/Meridian/meridian_conn_messages.h, keeping message.h free of
 * Meridian / msquic / network dependencies. The Timer trim is preserved
 * exactly; the change is purely additive to the enum.
 */
