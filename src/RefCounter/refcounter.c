//
// Vendored from liboffs (vijayee/liboffs src/RefCounter/refcounter.c).
// Surgical modifications for poseidon:
//   1. Drop the #ifndef OFFS_ATOMIC / #else platform-mutex branch — poseidon
//      has no src/Platform/platform.h and the liboffs packed_state atomic
//      path is strictly stronger (single 32-bit CAS transferring count/yield/
//      pending atomically vs. the original yield-CAS-then-count-add race).
//   2. No OFFS_ATOMIC macro is needed because the lock branch is gone.
//   3. The struct field is ATOMIC_TYPE(uint32_t) — poseidon's
//      src/Util/atomic_compat.h uses ATOMIC_TYPE(T), not liboffs's ATOMIC(T).
//      The C11 atomic_*_explicit functions are unchanged.
//   4. Added <stddef.h> for NULL (liboffs got it transitively via
//      Platform/platform.h; that header does not exist in poseidon).
//
#include "refcounter.h"
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdatomic.h>

void refcounter_init(refcounter_t* refcounter) {
  atomic_store_explicit(&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 0;
}

void refcounter_init_actor(refcounter_t* refcounter) {
  atomic_store_explicit(&refcounter->packed_state,
                        refcounter_pack(1, 0, 0), memory_order_release);
  refcounter->is_actor = 1;
}

void refcounter_yield(refcounter_t* refcounter) {
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint8_t yield = refcounter_packed_yield(state);
    if (yield == 0xFFu) return;  /* saturate; never wrap to 0 */
    desired = refcounter_pack(refcounter_packed_count(state), (uint8_t)(yield + 1),
                              refcounter_packed_pending(state));
  } while (!atomic_compare_exchange_weak_explicit(
      &refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
}

void* refcounter_reference(refcounter_t* refcounter) {
  if (refcounter == NULL) return NULL;
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      uint8_t new_yield = (uint8_t)(yield - 1);
      uint16_t new_count = count;
      uint8_t new_pending = pending;
      if (pending > 0) {
        new_pending = (uint8_t)(pending - 1);
        new_count = (uint16_t)(count - 1);
      }
      desired = refcounter_pack(new_count, new_yield, new_pending);
    } else if (count < USHRT_MAX) {
      desired = refcounter_pack((uint16_t)(count + 1), yield, pending);
    } else {
      desired = state;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &refcounter->packed_state, &state, desired,
      memory_order_release, memory_order_relaxed));
  return refcounter;
}

void refcounter_dereference(refcounter_t* refcounter) {
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      if (pending == 0xFFu) return;  /* saturate */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
    } else {
      desired = state;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
}

bool refcounter_dereference_is_zero(refcounter_t* refcounter) {
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  uint32_t desired;
  bool is_zero;
  do {
    uint16_t count = refcounter_packed_count(state);
    uint8_t  yield = refcounter_packed_yield(state);
    uint8_t  pending = refcounter_packed_pending(state);
    if (yield > 0) {
      if (pending == 0xFFu) return false;  /* saturate; not zero */
      desired = refcounter_pack(count, yield, (uint8_t)(pending + 1));
      is_zero = false;
    } else if (count > 0) {
      desired = refcounter_pack((uint16_t)(count - 1), yield, pending);
      is_zero = (count == 1) && (pending == 0);
    } else {
      desired = state;
      is_zero = (pending == 0);
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &refcounter->packed_state, &state, desired,
      memory_order_acq_rel, memory_order_relaxed));
  return is_zero;
}

uint16_t refcounter_count(refcounter_t* refcounter) {
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_count(state);
}

uint8_t refcounter_pending_derefs(refcounter_t* refcounter) {
  uint32_t state = atomic_load_explicit(&refcounter->packed_state, memory_order_relaxed);
  return refcounter_packed_pending(state);
}

refcounter_t* refcounter_consume(refcounter_t** refcounter) {
  refcounter_t* holder = *refcounter;
  refcounter_yield(holder);
  *refcounter = NULL;
  return holder;
}

void refcounter_destroy_lock(refcounter_t* refcounter) {
  (void)refcounter;
}
