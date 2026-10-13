//
// Vendored from liboffs/src/Util/atomic_compat.h.
// Local modifications:
//   - include guard renamed OFFS_ATOMIC_COMPAT_H -> POSEIDON_ATOMIC_COMPAT_H
//   - the C++ <atomic> include is kept as in liboffs. Liboffs pulls in
//     <atomic> unconditionally for the std::atomic<T> path; poseidon's
//     C-only callers (Workers/queue.h, Workers/join.h, RefCounter/refcounter.h)
//     are unaffected because they are compiled as C, not C++. The previous
//     poseidon comment warning C++ callers to include <atomic> themselves
//     is now obsolete; this header does it for them.
//

#ifndef POSEIDON_ATOMIC_COMPAT_H
#define POSEIDON_ATOMIC_COMPAT_H

#ifdef __cplusplus
#include <atomic>
#define ATOMIC(T) std::atomic<T>
#define ATOMIC_STORE(ptr, val) (ptr)->store(val)
#define ATOMIC_LOAD(ptr) (ptr)->load()
#define ATOMIC_EXCHANGE(ptr, val) (ptr)->exchange(val)
#define ATOMIC_FETCH_ADD(ptr, val) (ptr)->fetch_add(val)
#else
#include <stdatomic.h>
#define ATOMIC(T) _Atomic(T)
#define ATOMIC_STORE(ptr, val) atomic_store(ptr, val)
#define ATOMIC_LOAD(ptr) atomic_load(ptr)
#define ATOMIC_EXCHANGE(ptr, val) atomic_exchange(ptr, val)
#define ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add(ptr, val)
/* ATOMIC_VAR_INIT was deprecated in C11 and is not provided by MSVC's
   <stdatomic.h>. GCC's <stdatomic.h> expands it to a plain expression in
   parens, which makes `x = ATOMIC_VAR_INIT(0);` a valid assignment.
   Emulate that behavior for MSVC so existing call sites compile. */
#if defined(_MSC_VER)
  #define ATOMIC_VAR_INIT(value) (value)
#endif
#endif

#endif //POSEIDON_ATOMIC_COMPAT_H
