//
// C/C++ compatibility for atomic types.
//
// C11 uses _Atomic(T) and #include <stdatomic.h>.
// C++ uses std::atomic<T> and #include <atomic>.
// This header provides a unified POSEIDON_ATOMIC_TYPE(T) macro.
//

#ifndef POSEIDON_ATOMIC_COMPAT_H
#define POSEIDON_ATOMIC_COMPAT_H

#ifdef __cplusplus
  #define POSEIDON_ATOMIC_TYPE(T) ::std::atomic<T>
#else
  #include <stdatomic.h>
  #define POSEIDON_ATOMIC_TYPE(T) _Atomic(T)
#endif

#endif // POSEIDON_ATOMIC_COMPAT_H