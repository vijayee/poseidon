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

/* Message types. Poseidon-minimal skeleton: only the TIMER_* family needed by
 * Timer/timer_actor.c. Additional message types are introduced as subsystems
 * are actorized (Quasar, Meridian, etc.) in later migration phases. */
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
 */
