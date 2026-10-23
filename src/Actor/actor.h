/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor machinery.
 *
 * Vendored from liboffs/src/Actor/actor.h. See "Local modifications" at EOF.
 */

#ifndef POSEIDON_ACTOR_H
#define POSEIDON_ACTOR_H

#include "message_queue.h"
#include "../Util/atomic_compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACTOR_BATCH_SIZE 32
#define ACTOR_FLAG_SCHEDULED 0x01
#define ACTOR_FLAG_OVERLOADED 0x02
#define ACTOR_FLAG_RUNNING 0x04
#define ACTOR_FLAG_DESTROY 0x08
#define ACTOR_FLAG_PRESSURED 0x10
#define ACTOR_FLAG_MUTED 0x20

/* MUTED + PRESSURED implement the mute-park backpressure protocol: a sender
   whose target mailbox is full is muted and PARKED (off the run queue) until
   the target's mailbox drains and releases it. Contract, caveats (notably
   the open mutual-mute-cycle hazard for heavily bidirectional actor pairs),
   and the diagnosis playbook live in docs/backpressure-and-timer-stall.md. */

/* Actor queue-state: tracks whether the actor is IDLE (in no deque, not
   running), QUEUED (in a worker deque, not running), or RUNNING (being run
   by a worker). This lets actor_destroy wait until the actor is fully out
   of every scheduler deque before freeing the enclosing struct, closing
   the re-queue-vs-destroy UAF. See docs/concurrency-pass.md F5. */
typedef enum {
  ACTOR_QUEUE_IDLE = 0,
  ACTOR_QUEUE_QUEUED = 1,
  ACTOR_QUEUE_RUNNING = 2,
} actor_queue_state_e;

typedef struct scheduler_pool_t scheduler_pool_t;

typedef struct muted_sender_node_t {
  struct actor_t* sender;
  struct muted_sender_node_t* next;
} muted_sender_node_t;

typedef struct actor_t {
  message_queue_t queue;
  ATOMIC(uint8_t) flags;
  /* Mirrors the lifecycle of the actor with respect to worker deques. Set
     to IDLE in actor_init, QUEUED by actor_send / scheduler_inject and by
     the worker's re-queue path, RUNNING by the worker after it wins the
     ACTOR_FLAG_RUNNING CAS, and back to IDLE or QUEUED by the worker when
     actor_run returns. actor_destroy waits for IDLE before freeing, so the
     worker can never push a freeable pointer into a deque. See
     actor_queue_state_e above. */
  ATOMIC(uint8_t) queue_state;
  ATOMIC(muted_sender_node_t*) pressured_senders;
  scheduler_pool_t* pool;
  void* state;
  void (*dispatch)(void* state, message_t* msg);
  /* Intrusive doubly-linked list nodes for the owning pool's actor registry.
     The registry lets scheduler_pool_wait_for_idle enumerate every actor and
     re-inject any whose mailbox is non-empty but which is not scheduled — the
     recovery path for the elusive work-stealing strand (see
     scheduler_pool_wait_for_idle). Only touched under the pool's
     registry_lock; NULL for inline actors (pool == NULL). */
  struct actor_t* registry_prev;
  struct actor_t* registry_next;
} actor_t;

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool);
void actor_destroy(actor_t* actor);
/* Unregister the actor from its pool's registry without draining the queue or
   running backpressure_release. The registry-only subset of actor_destroy, for
   callers that tear an actor down themselves (set ACTOR_FLAG_DESTROY, drain the
   queue, free the struct) and intentionally bypass actor_destroy to avoid
   backpressure_release waking worker threads during shutdown. Without this, a
   freed actor stays in the registry and the next registry traversal would
   dereference freed memory. Idempotent: nulls actor->pool, so a later call (or
   actor_destroy / the pool-destroy detach loop having nulled pool) is a no-op. */
void actor_detach_pool(actor_t* actor);
bool actor_send(actor_t* actor, message_t* msg);
bool actor_run(actor_t* actor, size_t batch_size);
/* Runs the actor on the CALLING thread and returns when its mailbox is
 * drained. Unlike actor_run, which the scheduler's worker calls after it
 * has won the ACTOR_FLAG_RUNNING claim, this claims the actor itself and
 * waits for a pool worker that may already be running it.
 *
 * The synchronous wrappers (build payload -> actor_send -> run -> read the
 * out-field) need this: actor_send injects the actor into the scheduler
 * pool when the mailbox was empty, so a pool worker can pick the very same
 * actor up while the wrapper is still running it. Two threads popping the
 * same mailbox corrupt it: message_queue_pop is not safe against two
 * concurrent pops, and both threads would free the same node.
 *
 * The wrapper's contract is unchanged: when this returns, the message it
 * sent has been dispatched and the out-field the dispatch wrote is visible
 * to the caller. */
bool actor_run_inline(actor_t* actor, size_t batch_size);
void backpressure_apply(actor_t* actor);
void backpressure_release(actor_t* actor);

/* ---- Local modifications (vs upstream) -----------------------------------
 *
 * - Include guard renamed: OFFS_ACTOR_H -> POSEIDON_ACTOR_H.
 * - L2 provenance added: this file is a vendored copy of liboffs's actor.h,
 *   surgically transplanted to live under poseidon/src/Actor/.
 * - Forward decl of `bool` added via <stdbool.h> for actor_send/actor_run.
 * - actor_run_inline added (poseidon-only): upstream has no inline
 *   actor_run caller, so upstream has no need to claim the actor for the
 *   calling thread. Poseidon's synchronous wrappers do.
 *
 * No behaviour changes; no symbols renamed; the actor_t layout is byte-for-byte
 * the liboffs layout so that poseidon's Workers/, Time/, and downstream
 * subsystems can adopt the same mailbox protocol.
 * ------------------------------------------------------------------------- */

#endif // POSEIDON_ACTOR_H
