/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor machinery.
 */

#include "timer_actor.h"
#include "../Actor/message.h"
#include "../Util/allocator.h"
#include "../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include <internal/timer.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool _timer_actor_track(timer_actor_t* ta, pd_timer_t* timer) {
  if (ta->active_timer_count >= ta->active_timer_capacity) {
    size_t new_cap = ta->active_timer_capacity == 0 ? 8 : ta->active_timer_capacity * 2;
    pd_timer_t** new_arr = realloc(ta->active_timers, new_cap * sizeof(pd_timer_t*));
    if (new_arr == NULL) return false;
    ta->active_timers = new_arr;
    ta->active_timer_capacity = new_cap;
  }
  ta->active_timers[ta->active_timer_count++] = timer;
  return true;
}

/* Remove a timer from the tracked set. Returns true if the timer was found and
   removed, false if it was already gone (e.g. cancelled by a concurrent
   synchronous timer_actor_cancel, or torn down by
   _timer_actor_destroy_all_tracked). Callers must use the return value to
   decide whether it is safe to stop/destroy the timer pointer — operating on
   an untracked (already-freed) timer would be a use-after-free. */
static bool _timer_actor_untrack(timer_actor_t* ta, pd_timer_t* timer) {
  for (size_t i = 0; i < ta->active_timer_count; i++) {
    if (ta->active_timers[i] == timer) {
      ta->active_timers[i] = ta->active_timers[ta->active_timer_count - 1];
      ta->active_timer_count--;
      return true;
    }
  }
  return false;
}

/* Stop, destroy, and free one already-untracked timer. MUST be called
   WITHOUT loop_lock held. pd_timer_destroy's iocp_drain_sync posts a sync
   completion and waits for the pd-loop thread to consume it, and that thread
   takes loop_lock inside _timer_completion_callback — holding loop_lock
   across the wait would deadlock the two threads against each other and
   stretch every drain to its full 5s timeout. On a timed-out drain,
   pd_timer_destroy deliberately leaks the timer struct and its platform
   data instead of freeing them (the loop thread may still be about to
   dereference them) — a leak is recoverable, a use-after-free is not. */
static void _timer_destroy_timer(pd_timer_t* timer) {
  void* user_data = timer->user_data;
  pd_timer_stop(timer);
  pd_timer_destroy(timer);
  free(user_data);
}

/* Dispose of one already-untracked timer on the CURRENT thread. Safe only
   when no loop thread can dispatch callbacks for it — either the loop thread
   has exited (post-join teardown) or the loop never ran a batch. */
static void _timer_actor_dispose_timer(pd_timer_t* timer) {
  void* user_data = timer->user_data;
  pd_timer_destroy(timer);
  free(user_data);
}

/* Queue one already-untracked timer for disposal. On IOCP this destroys
   inline (see _timer_destroy_timer — that path's drain sync is why the
   stop/destroy must stay off the loop thread and off loop_lock). On
   epoll/kqueue the destroying thread only stops the OS timer and parks it;
   the loop thread frees the struct (and its user_data completion payload)
   after the event batch that could still carry this timer's callback has
   been fully processed — freeing it from a foreign thread races that
   captured callback (see _timer_completion_callback's lifetime guard and
   timerfd_callback's unguarded pd_timer_t dereference). */
static void _timer_actor_queue_destroy(timer_actor_t* timer_actor,
                                       pd_timer_t* timer) {
  if (timer == NULL) {
    return;
  }
  if (!timer_actor->destroy_deferred) {
    _timer_destroy_timer(timer);
    return;
  }
  /* On epoll/kqueue pd_timer_stop only disarms/closes the timer's fd and
     unregister's its backend record — it never waits on the loop thread —
     so it runs under loop_lock, serialized against creates and disposes
     touching the same backend structures from other threads. */
  platform_mutex_lock(timer_actor->loop_lock);
  pd_timer_stop(timer);
  size_t count = timer_actor->pending_destroy_count;
  if (count >= timer_actor->pending_destroy_capacity) {
    size_t new_cap = timer_actor->pending_destroy_capacity == 0
      ? 8 : timer_actor->pending_destroy_capacity * 2;
    pd_timer_t** grown = get_memory(new_cap * sizeof(pd_timer_t*));
    if (timer_actor->pending_destroy_timers != NULL) {
      memcpy(grown, timer_actor->pending_destroy_timers,
             count * sizeof(pd_timer_t*));
      free(timer_actor->pending_destroy_timers);
    }
    timer_actor->pending_destroy_timers = grown;
    timer_actor->pending_destroy_capacity = new_cap;
  }
  timer_actor->pending_destroy_timers[count] = timer;
  timer_actor->pending_destroy_count++;
  platform_mutex_unlock(timer_actor->loop_lock);
}

/* Same as _timer_actor_queue_destroy for callers that already hold
   loop_lock (timer_actor_set's OOM branch). */
static void _timer_actor_queue_destroy_locked(timer_actor_t* timer_actor,
                                              pd_timer_t* timer) {
  if (timer == NULL) {
    return;
  }
  if (!timer_actor->destroy_deferred) {
    _timer_destroy_timer(timer);
    return;
  }
  pd_timer_stop(timer);
  size_t count = timer_actor->pending_destroy_count;
  if (count >= timer_actor->pending_destroy_capacity) {
    size_t new_cap = timer_actor->pending_destroy_capacity == 0
      ? 8 : timer_actor->pending_destroy_capacity * 2;
    pd_timer_t** grown = get_memory(new_cap * sizeof(pd_timer_t*));
    if (timer_actor->pending_destroy_timers != NULL) {
      memcpy(grown, timer_actor->pending_destroy_timers,
             count * sizeof(pd_timer_t*));
      free(timer_actor->pending_destroy_timers);
    }
    timer_actor->pending_destroy_timers = grown;
    timer_actor->pending_destroy_capacity = new_cap;
  }
  timer_actor->pending_destroy_timers[count] = timer;
  timer_actor->pending_destroy_count++;
}

/* Swap out and dispose the pending-destroy queue. MUST run on the timer
   actor's pd-loop thread (after pd_loop_run_once has returned, so every
   captured timer callback has been fully processed), or after the loop
   thread has exited (post-join teardown) or the loop never ran a batch. */
/* The entire drain holds loop_lock: the dispose's pd_timer_destroy mutates
   the loop's watcher list and the backend's registration records, the same
   structures pd_timer_create/pd_timer_stop mutate from pool workers under
   this lock. On poll-dancer builds with thread-safety off the loop's own
   mutex is a stub, so loop_lock is the only serializer between the loop
   thread and the workers. On epoll/kqueue no drained call waits, so holding
   the lock cannot deadlock; IOCP never defers, so its no-lock-across-drain
   rule is untouched. */
static void _timer_actor_drain_pending_destroys(timer_actor_t* timer_actor) {
  pd_timer_t** timers;
  size_t count;
  platform_mutex_lock(timer_actor->loop_lock);
  timers = timer_actor->pending_destroy_timers;
  count = timer_actor->pending_destroy_count;
  timer_actor->pending_destroy_timers = NULL;
  timer_actor->pending_destroy_count = 0;
  timer_actor->pending_destroy_capacity = 0;
  for (size_t i = 0; i < count; i++) {
    if (timers[i] == NULL) continue;
    _timer_actor_dispose_timer(timers[i]);
  }
  platform_mutex_unlock(timer_actor->loop_lock);
  free(timers);
}

/* Destroy every tracked timer. pd_timer_destroy drains the IOCP via
   iocp_drain_sync, which posts a sync completion and waits for the loop
   thread to process it; if the loop thread has already been joined the drain
   hits its 5s timeout per timer, which is the source of the per-test teardown
   stall. Performing this teardown before joining the loop thread keeps the
   drain fast.

   Only the untracking happens under loop_lock — the disposal runs outside it
   (see _timer_actor_queue_destroy): on IOCP the destroy waits for the pd-loop
   thread, which takes loop_lock inside _timer_completion_callback, so holding
   the lock across a drain wait would deadlock the two threads against each
   other; on epoll/kqueue the timer is parked on the pending-destroy queue and
   the loop thread disposes it after the captured batch has been processed.
   Callers must first ensure no set/cancel or dispatch is in flight:
   timer_actor_stop and timer_actor_destroy set ACTOR_FLAG_DESTROY up front
   (which makes actor_run skip new dispatches), and the pool is either joined
   (test teardown calls scheduler_pool_stop first) or drained via
   scheduler_pool_wait_for_idle. A synchronous cancel (or debounce dispatch)
   that is waiting on loop_lock is handled safely: by the time it acquires
   the lock, this teardown has already swapped out the whole tracked array,
   so its _timer_actor_untrack lookup returns false and it skips the freed
   pointer rather than touching it. */
static void _timer_actor_destroy_all_tracked(timer_actor_t* timer_actor) {
  platform_mutex_lock(timer_actor->loop_lock);
  /* Swap the whole tracked array out under the lock, then destroy the timers
     OUTSIDE the lock (see _timer_destroy_timer): pd_timer_destroy waits on
     the pd-loop thread, which needs loop_lock inside
     _timer_completion_callback. Holding the lock across the wait would
     deadlock the two threads against each other and stretch every drain to
     its full 5s timeout — for teardown, one 5s stall per tracked timer. */
  pd_timer_t** timers = timer_actor->active_timers;
  size_t count = timer_actor->active_timer_count;
  timer_actor->active_timers = NULL;
  timer_actor->active_timer_count = 0;
  timer_actor->active_timer_capacity = 0;
  platform_mutex_unlock(timer_actor->loop_lock);
  for (size_t i = 0; i < count; i++) {
    if (timers[i] == NULL) continue;
    _timer_actor_queue_destroy(timer_actor, timers[i]);
  }
  free(timers);
}

static debounce_entry_t* _timer_actor_find_debounce(timer_actor_t* ta,
                                                      actor_t* target,
                                                      uint32_t completion_type) {
  for (size_t i = 0; i < MAX_DEBOUNCE_KEYS; i++) {
    if (ta->debounce_map[i].target == target &&
        ta->debounce_map[i].completion_type == completion_type) {
      return &ta->debounce_map[i];
    }
  }
  return NULL;
}

static debounce_entry_t* _timer_actor_alloc_debounce(timer_actor_t* ta,
                                                       actor_t* target,
                                                       uint32_t completion_type) {
  /* Check for an existing entry first. */
  debounce_entry_t* existing = _timer_actor_find_debounce(ta, target, completion_type);
  if (existing != NULL) {
    return existing;
  }
  /* Find an empty slot. */
  for (size_t i = 0; i < MAX_DEBOUNCE_KEYS; i++) {
    if (ta->debounce_map[i].target == NULL) {
      return &ta->debounce_map[i];
    }
  }
  return NULL;
}

static void _timer_completion_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  (void)events;
  timer_completion_payload_t* completion = (timer_completion_payload_t*)user_data;
  timer_actor_t* timer_actor = completion->timer_actor;

  /* Lifetime guard: this callback runs on the pd-loop thread with no
     synchronization against a concurrent timer_actor_cancel /
     timer_actor_cancel_target / _timer_actor_destroy_all_tracked on another
     thread. Those paths untrack the timer (remove it from active_timers /
     debounce_map) under loop_lock, then free its user_data (this
     `completion`) and destroy the timer outside the lock — the untrack
     happens before the free and before pd_timer_destroy, so by the time the
     timer
     is destroyed (and this callback can no longer fire) the completion is
     already freed. But poll-dancer may have already dispatched this callback
     on the loop thread before the untrack+destroy acquired loop_lock — in
     that window `completion` is still valid (not yet freed), yet the timer
     is about to be torn down.

     Re-verify under loop_lock that the timer backing this `completion` is
     still tracked. The lookup compares pointer VALUES (no dereference of
     freed memory): we compare `completion` against each tracked timer's
     user_data and each debounce entry's completion_payload. If not found,
     the timer has been cancelled/destroyed (or is mid-teardown holding
     loop_lock on the other thread) and we must NOT touch
     `completion->timer_actor->actor` — drop the firing. This mirrors the
     F8 re-check in the TIMER_COMPLETION dispatch (see timer_actor.c:242-288)
     and the timer_actor_cancel_target contract. */
  platform_mutex_lock(timer_actor->loop_lock);
  bool tracked = false;
  for (size_t index = 0; index < timer_actor->active_timer_count; index++) {
    pd_timer_t* tracked_timer = timer_actor->active_timers[index];
    if (tracked_timer == NULL) {
      continue;
    }
    if (tracked_timer->user_data == completion) {
      tracked = true;
      break;
    }
  }
  if (!tracked) {
    for (size_t index = 0; index < MAX_DEBOUNCE_KEYS; index++) {
      if (timer_actor->debounce_map[index].completion_payload == completion) {
        tracked = true;
        break;
      }
    }
  }
  platform_mutex_unlock(timer_actor->loop_lock);
  if (!tracked) {
    /* The timer was cancelled/destroyed out from under this in-flight
       firing. Dropping is safe: the target's completion is still delivered
       by the cancel_target / debounce_flush path, or — if the target itself
       is being torn down — was deliberately suppressed. */
    return;
  }

  /* Allocate a fresh copy for each firing — actor_send takes ownership and
     frees via payload_destroy. The original completion stays alive for
     repeating timers until pd_timer_destroy is called. */
  timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
  *copy = *completion;
  message_t msg = {0};
  msg.type = TIMER_COMPLETION;
  msg.payload = copy;
  msg.payload_destroy = free;
  actor_send(&timer_actor->actor, &msg);
}

static void _timer_actor_dispatch(void* state, message_t* msg) {
  timer_actor_t* timer_actor = (timer_actor_t*)state;

  switch (msg->type) {
    case TIMER_DEBOUNCE: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
      /* Untrack the existing debounce timer under loop_lock, but stop and
         destroy it OUTSIDE the lock (see _timer_destroy_timer): the destroy
         waits on the pd-loop thread, which needs loop_lock inside
         _timer_completion_callback. The untracked timer can still fire
         before its destroy completes, but its callback's tracked re-check
         then fails and the firing is dropped. */
      pd_timer_t* old_timer = NULL;
      pd_timer_t* failed_timer = NULL;
      platform_mutex_lock(timer_actor->loop_lock);
      if (entry != NULL && entry->timer != NULL) {
        old_timer = entry->timer;
        _timer_actor_untrack(timer_actor, old_timer);
        entry->timer = NULL;
        entry->completion_payload = NULL;
      }
      /* Allocate a slot for this debounce key. */
      if (entry == NULL) {
        entry = _timer_actor_alloc_debounce(timer_actor, payload->target,
                                             payload->completion_type);
      }
      timer_completion_payload_t* completion = get_clear_memory(sizeof(timer_completion_payload_t));
      completion->timer_id = 0;
      completion->target = payload->target;
      completion->completion_type = payload->completion_type;
      completion->timer_actor = timer_actor;
      pd_timer_t* timer = pd_timer_create(
          timer_actor->loop, payload->timeout_ms, payload->interval_ms,
          _timer_completion_callback, completion);
      if (timer != NULL && entry != NULL) {
        pd_timer_start(timer);
        if (_timer_actor_track(timer_actor, timer)) {
          completion->timer_id = (uint64_t)(uintptr_t)timer;
          entry->target = payload->target;
          entry->completion_type = payload->completion_type;
          entry->timer = timer;
          entry->completion_payload = completion;
        } else {
          /* Tracking failed (OOM) — stop and destroy the timer; destroy
             outside the lock like every other pd_timer_destroy. */
          failed_timer = timer;
          /* Clear the debounce entry since the timer wasn't set up. */
          if (entry->timer == NULL) {
            entry->target = NULL;
            entry->completion_type = 0;
          }
        }
      } else {
        if (timer != NULL) {
          failed_timer = timer;
        } else {
          free(completion);
        }
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      if (old_timer != NULL) {
        _timer_actor_queue_destroy(timer_actor, old_timer);
      }
      if (failed_timer != NULL) {
        _timer_actor_queue_destroy(timer_actor, failed_timer);
      }
      /* payload (timer_debounce_payload_t) is read-only to this dispatch —
         the fields are copied into the timer's completion payload above.
         Let actor_run free it via msg->payload_destroy (free). The previous
         CONSUME here (NULLing msg fields) leaked the payload because the
         dispatch never freed it. */
      break;
    }
    case TIMER_DEBOUNCE_FLUSH: {
      timer_debounce_payload_t* payload = (timer_debounce_payload_t*)msg->payload;
      debounce_entry_t* entry = _timer_actor_find_debounce(
          timer_actor, payload->target, payload->completion_type);
      pd_timer_t* old_timer = NULL;
      platform_mutex_lock(timer_actor->loop_lock);
      if (entry != NULL && entry->timer != NULL) {
        /* Cancel the pending debounce timer: untrack and clear the entry
           under loop_lock; the stop/destroy runs OUTSIDE the lock (see
           _timer_destroy_timer — the destroy waits on the pd-loop thread,
           which needs loop_lock inside _timer_completion_callback). The
           untracked timer can still fire before its destroy completes, but
           its callback's tracked re-check then fails and the firing is
           dropped, so no competing completion is enqueued. */
        old_timer = entry->timer;
        _timer_actor_untrack(timer_actor, old_timer);
        /* Clear the debounce entry. */
        entry->target = NULL;
        entry->completion_type = 0;
        entry->timer = NULL;
        entry->completion_payload = NULL;
        /* Build the completion to forward to the target. Send it ONE-hop
           directly to the target instead of two-hop through the timer_actor:
           the entry was just cleared above, so the TIMER_COMPLETION
           dispatch's F8 re-check (which only forwards when the target is
           still in the debounce_map / active_timers) would otherwise drop
           the completion. The cancel+clear happened under loop_lock, so
           this flush's own completion is the only one for this key.
           actor_send to a destroyed target drops the payload safely; a
           freed target is the destroyer's responsibility (the destroyer
           calls timer_actor_cancel_target before freeing, which removes
           the entry, making this flush a no-op). */
        timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
        copy->target = payload->target;
        copy->completion_type = payload->completion_type;
        copy->timer_id = 0;
        copy->timer_actor = timer_actor;
        message_t target_msg = {0};
        target_msg.type = copy->completion_type;
        target_msg.payload = copy;
        target_msg.payload_destroy = free;
        platform_mutex_unlock(timer_actor->loop_lock);
        _timer_actor_queue_destroy(timer_actor, old_timer);
        actor_send(copy->target, &target_msg);
      } else {
        platform_mutex_unlock(timer_actor->loop_lock);
      }
      /* payload (timer_debounce_payload_t) fields are copied into the
         completion copy above. Let actor_run free the original via
         msg->payload_destroy. */
      break;
    }
    case TIMER_COMPLETION: {
      timer_completion_payload_t* completion = (timer_completion_payload_t*)msg->payload;
      /* F8 re-check: before actor_send-ing to completion->target, confirm
         the target is still tracked in the debounce_map or active_timers.
         timer_actor_cancel_target removes the target from both BEFORE the
         target is freed; if the target isn't tracked, this completion is
         stale (the target was canceled/freed) and must be dropped without
         dereferencing completion->target. The lookup compares pointer VALUES
         (no dereference), so a freed target's stale pointer value is safe
         to compare. actor_send would otherwise read freed flags -> UAF.

         Both debounce timers (TIMER_DEBOUNCE dispatch) and direct
         timer_actor_set timers are tracked in active_timers, so checking
         both the debounce_map and active_timers covers every completion
         source. The completion payload carried in msg->payload is owned by
         actor_run (payload_destroy = free) and is freed regardless of
         whether we drop or forward, so the drop path leaks nothing. */
      platform_mutex_lock(timer_actor->loop_lock);
      bool target_tracked = false;
      for (size_t index = 0; index < MAX_DEBOUNCE_KEYS; index++) {
        if (timer_actor->debounce_map[index].target == completion->target) {
          target_tracked = true;
          break;
        }
      }
      if (!target_tracked) {
        for (size_t index = 0; index < timer_actor->active_timer_count;
             index++) {
          pd_timer_t* tracked_timer = timer_actor->active_timers[index];
          if (tracked_timer == NULL) {
            continue;
          }
          timer_completion_payload_t* tracked_completion =
              (timer_completion_payload_t*)tracked_timer->user_data;
          if (tracked_completion != NULL &&
              tracked_completion->target == completion->target) {
            target_tracked = true;
            break;
          }
        }
      }
      platform_mutex_unlock(timer_actor->loop_lock);
      if (!target_tracked) {
        /* Drop the stale completion — the target was canceled/freed. The
           payload is freed by actor_run via msg->payload_destroy. */
        break;
      }
      /* Allocate a fresh copy of the completion to forward to the target
       * actor. The original payload's fields are copied into `copy` and the
       * original is then freed by actor_run's post-dispatch cleanup via
       * msg->payload_destroy (free). */
      timer_completion_payload_t* copy = get_clear_memory(sizeof(timer_completion_payload_t));
      *copy = *completion;
      message_t target_msg = {0};
      target_msg.type = copy->completion_type;
      target_msg.payload = copy;
      target_msg.payload_destroy = free;
      actor_send(copy->target, &target_msg);
      break;
    }
    default:
      break;
  }
}

static void* _timer_actor_thread(void* arg) {
  platform_thread_setup_stack();
  timer_actor_t* timer_actor = (timer_actor_t*)arg;

  while (atomic_load(&timer_actor->running)) {
    int result = pd_loop_run_once(timer_actor->loop, 100);
    if (result < 0) {
      break;
    }
    /* Dispose queued timers only after this batch has been fully
       dispatched: a destroyed timer's callback may have been captured in
       this batch's events and dereferenced here — freeing the timer before
       the dispatch would be a use-after-free (see
       pending_destroy_timers in timer_actor.h). */
    _timer_actor_drain_pending_destroys(timer_actor);
  }

  /* Cover the early-exit paths (error break, running cleared): the loop
     thread is about to return, so its captured batches are done and queued
     timers are safe to dispose here. */
  _timer_actor_drain_pending_destroys(timer_actor);

  return NULL;
}

timer_actor_t* timer_actor_create(scheduler_pool_t* pool) {
  timer_actor_t* timer_actor = get_clear_memory(sizeof(timer_actor_t));
  actor_init(&timer_actor->actor, timer_actor, _timer_actor_dispatch, pool);
  /* IOCP destroys inline; every other backend (epoll, kqueue) defers timer
     disposal to the loop thread — see _timer_actor_queue_destroy and the
     pending_destroy_timers contract in timer_actor.h. */
  timer_actor->destroy_deferred =
      strcmp(pd_platform_name(), "iocp") != 0;
  timer_actor->loop = pd_loop_create(NULL);
  if (timer_actor->loop == NULL) {
    /* actor_init registered this actor in the pool's registry; leaving it
       registered would let a later registry traversal dereference freed
       memory. Detach before freeing. */
    actor_detach_pool(&timer_actor->actor);
    free(timer_actor);
    return NULL;
  }
  timer_actor->loop_lock = platform_mutex_create();
  if (timer_actor->loop_lock == NULL) {
    actor_detach_pool(&timer_actor->actor);
    pd_loop_destroy(timer_actor->loop);
    free(timer_actor);
    return NULL;
  }
  atomic_store(&timer_actor->running, 1);
  timer_actor->thread = platform_thread_create(_timer_actor_thread, timer_actor);
  if (timer_actor->thread == NULL) {
    atomic_store(&timer_actor->running, 0);
    actor_detach_pool(&timer_actor->actor);
    /* The loop thread never ran, so no event batch was ever captured —
       queued timers (a dispatch that raced the thread creation) are safe to
       dispose on this thread. */
    _timer_actor_drain_pending_destroys(timer_actor);
    platform_mutex_destroy(timer_actor->loop_lock);
    pd_loop_destroy(timer_actor->loop);
    free(timer_actor);
    return NULL;
  }
  return timer_actor;
}

void timer_actor_stop(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) return;
  if (!atomic_load(&timer_actor->running)) return;

  /* Mark the actor as destroyed so scheduler workers drop pending debounce
     messages. (timer_actor_set / timer_actor_cancel are synchronous and do
     not queue messages.) Without this, processing a debounce message would
     call pd_timer_create/pd_timer_start on a loop whose thread has exited,
     which may hang or misbehave. */
  uint8_t flags = atomic_load(&timer_actor->actor.flags);
  while (!atomic_compare_exchange_strong(&timer_actor->actor.flags, &flags,
                                           flags | ACTOR_FLAG_DESTROY)) {
  }

  /* Destroy tracked timers while the loop thread is still alive so
     pd_timer_destroy's IOCP drain_sync completes promptly; doing this after
     joining the loop thread (the old order) left the drain with no live loop
     to process the posted sync, hitting a 5s timeout per timer. */
  _timer_actor_destroy_all_tracked(timer_actor);

  atomic_store(&timer_actor->running, 0);
  pd_loop_async_send(timer_actor->loop, NULL);
  platform_thread_join(timer_actor->thread);
  timer_actor->thread = NULL;
}

void timer_actor_destroy(timer_actor_t* timer_actor) {
  if (timer_actor == NULL) {
    return;
  }
  scheduler_pool_t* pool = timer_actor->actor.pool;
  actor_destroy(&timer_actor->actor);
  if (pool != NULL && !atomic_load(&pool->terminate)) {
    scheduler_pool_wait_for_idle(pool);
  }
  /* Destroy tracked timers while the loop thread is still alive so
     pd_timer_destroy's IOCP drain_sync completes promptly. The old code
     joined the loop thread first and then destroyed the timers, leaving
     drain_sync with no live loop to process the posted sync — a 5s timeout
     per leftover timer. When timer_actor_stop ran first it only stopped
     (not destroyed) the timers, so this destroy path still owned the
     destroy and still stalled; destroying here before the join fixes both
     the standalone-destroy and stop-then-destroy paths. The helper is a
     no-op if timer_actor_stop already destroyed the tracked timers. */
  _timer_actor_destroy_all_tracked(timer_actor);
  if (atomic_load(&timer_actor->running)) {
    atomic_store(&timer_actor->running, 0);
    pd_loop_async_send(timer_actor->loop, NULL);
    platform_thread_join(timer_actor->thread);
    timer_actor->thread = NULL;
  }
  /* The loop thread has exited (its exit drain already disposed the parked
     timers) or never existed; dispose any straggler queued by a thread that
     raced this teardown. */
  _timer_actor_drain_pending_destroys(timer_actor);
  free(timer_actor->active_timers);
  platform_mutex_destroy(timer_actor->loop_lock);
  pd_loop_stop(timer_actor->loop);
  pd_loop_destroy(timer_actor->loop);
  free(timer_actor);
}

uint64_t timer_actor_set(timer_actor_t* timer_actor, uint64_t timeout_ms,
                         uint64_t interval_ms, actor_t* target,
                         uint32_t completion_type,
                         ATOMIC(uint64_t)* out_timer_id) {
  if (timer_actor == NULL) {
    if (out_timer_id != NULL) atomic_store(out_timer_id, 0);
    return 0;
  }

  /* Synchronous set: create, start, and track the timer inline under
     loop_lock, and fill out_timer_id before returning. This bypasses the
     actor message queue for the same reason timer_actor_cancel does: under
     2+ scheduler workers a TIMER_SET message can strand in the timer_actor's
     queue (pushed but never dispatched when scheduler_pool_wait_for_idle
     returns). A stranded SET leaves out_timer_id at 0, so the caller (e.g.
     network_destroy) skips the matching cancel — the timer is then created
     late, on a future pool drain, after the caller has already moved on, and
     is never cancelled. Its pd_timer (and CreateEvent + CreateTimerQueueTimer
     handles) stays tracked until timer_actor_destroy: a linear per-cycle handle
     leak. Creating synchronously makes the id available immediately, so every
     set has a matching synchronous cancel and no timer is orphaned.

     This mirrors the former TIMER_SET dispatch logic and takes the same
     loop_lock a concurrent TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatch on
     a pool worker (or a synchronous cancel on another thread) takes, so the
     watcher-list mutation (pd_timer_create/pd_timer_start) is serialized.
     pd_timer_create/pd_timer_start do not need the timer_actor's loop thread
     to make progress, so holding loop_lock across them cannot deadlock with
     the loop thread (pd_timer_create/pd_timer_start never wait on it — only
     pd_timer_stop/pd_timer_destroy do, and those must never run under
     loop_lock, see _timer_destroy_timer). The completion payload is
     the timer's user_data and is freed when the timer is cancelled or torn
     down via _timer_actor_destroy_all_tracked. */
  timer_completion_payload_t* completion = get_clear_memory(sizeof(timer_completion_payload_t));
  completion->timer_id = 0;
  completion->target = target;
  completion->completion_type = completion_type;
  completion->timer_actor = timer_actor;

  platform_mutex_lock(timer_actor->loop_lock);
  uint64_t id = 0;
  pd_timer_t* timer = pd_timer_create(
      timer_actor->loop, timeout_ms, interval_ms,
      _timer_completion_callback, completion);
  if (timer != NULL) {
    pd_timer_start(timer);
    if (_timer_actor_track(timer_actor, timer)) {
      id = (uint64_t)(uintptr_t)timer;
      completion->timer_id = id;
    } else {
      /* Tracking failed (OOM) — stop and dispose the timer to avoid leak.
         The completion payload is freed with the timer (see
         _timer_actor_queue_destroy_locked): queueing under the already-held
         loop_lock keeps user_data ownership with the pending-destroy drain
         instead of freeing it here, where the loop thread may still be about
         to dispatch the timer's (already-captured) callback. */
      _timer_actor_queue_destroy_locked(timer_actor, timer);
      completion = NULL;
    }
  } else {
    free(completion);
    completion = NULL;
  }
  platform_mutex_unlock(timer_actor->loop_lock);

  /* id was captured under loop_lock; publish it after unlock. No other thread
     can cancel this timer before out_timer_id is published (cancel needs the
     id, which only comes from out_timer_id). */
  if (out_timer_id != NULL) {
    atomic_store(out_timer_id, id);
  }
  return id;
}

void timer_actor_cancel(timer_actor_t* timer_actor, uint64_t timer_id) {
  if (timer_actor == NULL || timer_id == 0) {
    return;
  }
  pd_timer_t* timer = (pd_timer_t*)(uintptr_t)timer_id;

  /* Synchronous cancel: untrack, stop, and destroy the timer inline under
     loop_lock, bypassing the actor message queue.

     The queue is a lock-free mailbox drained by the scheduler, and under
     2+ workers a cancel message can strand — be pushed to the timer_actor's
     queue yet never dispatched (the actor is not in any worker's deque when
     scheduler_pool_wait_for_idle returns, so the message sits until
     timer_actor_destroy). A stranded cancel leaves the pd_timer tracked and
     its OS handles (CreateEvent stop_event + CreateTimerQueueTimer) leaked
     until teardown. Cancelling synchronously from the caller destroys the
     timer immediately and releases its handles, with no dependence on
     scheduler delivery.

     This mirrors the stop+destroy sequence _timer_actor_destroy_all_tracked
     uses at teardown, and untracks under the same lock a concurrent
     synchronous timer_actor_set (or a TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH
     dispatch on a pool worker) takes. The stop/destroy itself runs OUTSIDE
     loop_lock (see _timer_destroy_timer): pd_timer_stop is synchronous on
     IOCP (DeleteTimerQueueTimer(INVALID_HANDLE_VALUE) waits for the
     thread-pool callback) and pd_timer_destroy drains the IOCP via
     iocp_drain_sync, which waits on the loop thread — the loop thread takes
     loop_lock inside _timer_completion_callback, so holding loop_lock
     across those waits would deadlock the two threads against each other.
     _timer_actor_untrack returns
     false if the timer was already removed (a duplicate cancel, or teardown
     via _timer_actor_destroy_all_tracked); in that case the pointer is
     already freed and must not be touched. */
  platform_mutex_lock(timer_actor->loop_lock);
  bool was_tracked = _timer_actor_untrack(timer_actor, timer);
  platform_mutex_unlock(timer_actor->loop_lock);
  if (was_tracked) {
    /* Untracked under loop_lock; disposal outside it (see
       _timer_actor_queue_destroy — on IOCP pd_timer_destroy waits on the
       pd-loop thread, which needs loop_lock inside
       _timer_completion_callback, so holding loop_lock across the wait would
       deadlock the two threads; on epoll/kqueue the timer is parked for the
       loop thread, which frees it after the captured batch is processed). */
    _timer_actor_queue_destroy(timer_actor, timer);
  }
}

void timer_actor_cancel_target(timer_actor_t* timer_actor, actor_t* target) {
  if (timer_actor == NULL || target == NULL) {
    return;
  }

  /* Synchronous cancel-by-target: remove every debounce entry and every
     active timer whose completion payload targets `target`, inline under
     loop_lock. This is the F8 fix: a short-lived target actor can be freed
     before an in-flight TIMER_COMPLETION (already enqueued by
     _timer_completion_callback on the pd-loop thread) is processed by the
     timer_actor's dispatch. Without this cancel, the dispatch's
     actor_send(payload->target, ...) would read freed flags -> UAF.

     The destroyer calls cancel_target BEFORE freeing `target`; any
     in-flight completion is then dropped by the TIMER_COMPLETION dispatch
     re-check (the target is no longer in the debounce_map nor in
     active_timers). The re-check compares pointer VALUES (no dereference),
     so a freed target's stale pointer value is safe to compare.

     Takes the same loop_lock as timer_actor_cancel / timer_actor_set / the
     TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatches / teardown, so the
     debounce_map and active_timers mutations are serialized. Only the
     untracking happens under loop_lock; the matched timers are collected
     and their stop/destroy/free runs OUTSIDE the lock (see
     _timer_destroy_timer: pd_timer_destroy waits on the loop thread, which
     takes loop_lock inside _timer_completion_callback, so holding the lock
     across those waits would deadlock the two threads against each other).
     _timer_actor_untrack returns
     false if a timer was already removed (a concurrent cancel, or teardown
     via _timer_actor_destroy_all_tracked); in that case the pointer is
     already freed and must not be touched. */
  pd_timer_t** to_destroy = NULL;
  size_t destroy_count = 0;
  size_t destroy_capacity = 0;
  platform_mutex_lock(timer_actor->loop_lock);

  /* Cancel every debounce entry for this target. Mirrors the per-entry
     cleanup in the TIMER_DEBOUNCE / TIMER_DEBOUNCE_FLUSH dispatches. */
  for (size_t entry_index = 0; entry_index < MAX_DEBOUNCE_KEYS;
       entry_index++) {
    debounce_entry_t* entry = &timer_actor->debounce_map[entry_index];
    if (entry->target != target) {
      continue;
    }
    if (entry->timer == NULL) {
      entry->target = NULL;
      entry->completion_type = 0;
      continue;
    }
    pd_timer_t* timer = entry->timer;
    if (destroy_count == destroy_capacity) {
      size_t new_cap = destroy_capacity == 0 ? 16 : destroy_capacity * 2;
      pd_timer_t** grown = realloc(to_destroy, new_cap * sizeof(pd_timer_t*));
      if (grown == NULL) {
        /* Collector OOM: stop collecting and leave this entry (and every
           later one) fully intact and tracked — a later cancel_target /
           teardown destroys them. Destroying inline under loop_lock would
           re-introduce the lock-across-drain deadlock. */
        break;
      }
      to_destroy = grown;
      destroy_capacity = new_cap;
    }
    to_destroy[destroy_count++] = timer;
    _timer_actor_untrack(timer_actor, timer);
    entry->timer = NULL;
    entry->completion_payload = NULL;
    entry->target = NULL;
    entry->completion_type = 0;
  }

  /* Cancel any non-debounce timers (from timer_actor_set) whose completion
     payload targets this actor. _timer_actor_untrack does swap-removal, so
     on a match do not advance the index — the swapped-in last element must
     be re-checked at the same slot. A while loop (rather than a for-loop
     with index--) avoids the size_t underflow at index 0. */
  size_t index = 0;
  while (index < timer_actor->active_timer_count) {
    pd_timer_t* timer = timer_actor->active_timers[index];
    if (timer == NULL) {
      index++;
      continue;
    }
    timer_completion_payload_t* completion =
        (timer_completion_payload_t*)timer->user_data;
    if (completion == NULL || completion->target != target) {
      index++;
      continue;
    }
    if (destroy_count == destroy_capacity) {
      size_t new_cap = destroy_capacity == 0 ? 16 : destroy_capacity * 2;
      pd_timer_t** grown = realloc(to_destroy, new_cap * sizeof(pd_timer_t*));
      if (grown == NULL) {
        /* Collector OOM: stop collecting; this timer (and any remaining
           matches) stays tracked for a later cancel_target / teardown.
           Destroying inline under loop_lock would re-introduce the
           lock-across-drain deadlock. */
        break;
      }
      to_destroy = grown;
      destroy_capacity = new_cap;
    }
    to_destroy[destroy_count++] = timer;
    _timer_actor_untrack(timer_actor, timer);
    /* Don't advance index — the swap brought the previous last element
       into this slot, and it may also target `target`. */
  }

  platform_mutex_unlock(timer_actor->loop_lock);

  /* Stop/destroy/free outside loop_lock (see _timer_actor_queue_destroy). */
  for (size_t i = 0; i < destroy_count; i++) {
    _timer_actor_queue_destroy(timer_actor, to_destroy[i]);
  }
  free(to_destroy);
}

uint64_t timer_actor_debounce(timer_actor_t* timer_actor,
                              uint64_t timeout_ms, uint64_t interval_ms,
                              actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->timeout_ms = timeout_ms;
  payload->interval_ms = interval_ms;
  payload->target = target;
  payload->completion_type = completion_type;

  message_t msg = {0};
  msg.type = TIMER_DEBOUNCE;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }

  return 0;
}

void timer_actor_debounce_flush(timer_actor_t* timer_actor,
                                actor_t* target, uint32_t completion_type) {
  timer_debounce_payload_t* payload = get_clear_memory(sizeof(timer_debounce_payload_t));
  payload->target = target;
  payload->completion_type = completion_type;
  payload->timeout_ms = 0;
  payload->interval_ms = 0;

  message_t msg = {0};
  msg.type = TIMER_DEBOUNCE_FLUSH;
  msg.payload = payload;
  msg.payload_destroy = free;

  actor_send(&timer_actor->actor, &msg);
  if (atomic_load(&timer_actor->running)) {
    pd_loop_async_send(timer_actor->loop, NULL);
  }
}
