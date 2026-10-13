/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon timer actor.
 *
 * Vendored from liboffs/src/Timer/timer_actor.h. See "Local modifications"
 * at EOF.
 */

#ifndef POSEIDON_TIMER_ACTOR_H
#define POSEIDON_TIMER_ACTOR_H

#include "../Actor/actor.h"
#include "../Platform/platform.h"
#include "../Scheduler/scheduler.h"
#include "../Util/atomic_compat.h"
#include <poll-dancer/types.h>
#include <stdint.h>

#define MAX_DEBOUNCE_KEYS 16

typedef struct debounce_entry_t {
  actor_t* target;
  uint32_t completion_type;
  pd_timer_t* timer;
  void* completion_payload;
} debounce_entry_t;

typedef struct timer_actor_t {
  actor_t actor;
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  /* Active timers tracked for cleanup on destroy. */
  pd_timer_t** active_timers;
  size_t active_timer_count;
  size_t active_timer_capacity;
  /* Debounce map: tracks one timer per (target, completion_type) pair,
     cancelling the previous timer when a new debounce arrives. */
  debounce_entry_t debounce_map[MAX_DEBOUNCE_KEYS];
  /* Serializes pd_timer_create/start/stop/destroy (which mutate the loop's
     watcher list) across threads. timer_actor_set and timer_actor_cancel
     run synchronously on the caller's thread; TIMER_DEBOUNCE and
     TIMER_DEBOUNCE_FLUSH dispatch on a scheduler pool worker; and
     _timer_actor_destroy_all_tracked runs at teardown. Without this lock,
     two of these can race on pd_loop_add_watcher / pd_loop_remove_watcher —
     the watcher array's watcher_count is left inconsistent, and the loop
     later iterates a stale (already-freed) pointer. */
  platform_mutex_t* loop_lock;
  /* Timers untracked but not yet disposed. On backends whose timer destroy
     never waits on the loop thread (epoll, kqueue), an epoll_wait batch the
     loop thread already returned can still dispatch timerfd_callback /
     _timer_completion_callback for an untracked timer — freeing the timer
     struct or its user_data from a foreign thread then races the captured
     callback (use-after-free; vendor upstream had a tracked issue here).
     These timers are therefore disposed on the LOOP thread, after the batch
     that could carry their events has been fully processed; under IOCP
     destroying inline is safe (pd_timer_destroy's drain-sync blocks the
     destroying thread until the loop cannot dispatch the timer again) and
     staying inline avoids a self-wait in iocp_drain_sync when the drain
     runs on the loop thread. */
  pd_timer_t** pending_destroy_timers;
  size_t pending_destroy_count;
  size_t pending_destroy_capacity;
  /* 1 when timer disposal is deferred to the loop thread (epoll / kqueue); 0
     when pd_timer_destroy runs inline on the destroying thread (IOCP). */
  uint8_t destroy_deferred;
} timer_actor_t;

typedef struct {
  uint64_t timeout_ms;
  uint64_t interval_ms;
  actor_t* target;
  uint32_t completion_type;
} timer_debounce_payload_t;

typedef struct {
  uint64_t timer_id;
  actor_t* target;
  uint32_t completion_type;
  timer_actor_t* timer_actor;  // back-reference for two-hop dispatch
} timer_completion_payload_t;

timer_actor_t* timer_actor_create(scheduler_pool_t* pool);
void timer_actor_stop(timer_actor_t* timer_actor);
void timer_actor_destroy(timer_actor_t* timer_actor);
uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type,
                         ATOMIC(uint64_t)* out_timer_id);
void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id);
/* Cancel all debounce timers for `target` and remove its entries from the
   debounce_map and active_timers. Call this BEFORE freeing `target` — any
   in-flight completion already in the timer_actor's mailbox will be dropped
   by the dispatch (the target is no longer in the map / active_timers, so
   the dispatch won't actor_send to it). The map lookup compares pointer
   VALUES (not dereferencing), so a freed target's stale pointer value is
   safe to compare. See concurrency-pass.md F8. */
void timer_actor_cancel_target(timer_actor_t* timer_actor, actor_t* target);
uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type);
void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type);

/* ---- Local modifications (vs upstream) -----------------------------------
 *
 * - Include guard renamed: OFFS_TIMER_ACTOR_H -> POSEIDON_TIMER_ACTOR_H.
 * - L2 provenance added: this file is a vendored copy of liboffs's
 *   timer_actor.h, surgically transplanted to live under
 *   poseidon/src/Timer/.
 * - The vendor-upstream issue-tracker reference ("OFFS-189") in the
 *   pending_destroy_timers comment was rewritten to a neutral phrase so the
 *   header contains no upstream-internal identifiers.
 * - Header comment rewritten in poseidon's voice.
 *
 * No behaviour changes; no symbols renamed; the timer_actor_t layout is
 * byte-for-byte the liboffs layout so that poseidon's Time/ and downstream
 * subsystems can adopt the same timer protocol.
 *
 * NOTE: This header references "../Scheduler/scheduler.h" (forward decl of
 * scheduler_pool_t). Scheduler/ is vendored in commit 1.5; until then the
 * header is present but not consumed by any wired build target, so the
 * include is dormant.
 * ------------------------------------------------------------------------- */

#endif // POSEIDON_TIMER_ACTOR_H
