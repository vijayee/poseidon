//
// Created by victor on 3/30/25.
//
// Vendored from liboffs on 2026-09-26 (commit 1.2, actor-migration branch).
// Poseidon-specific mutations:
//   * Include guard renamed: OFFS_ALLOCATOR_H -> POSEIDON_ALLOCATOR_H
// No header drops. No cascade. NULL is reachable via log.h (vendored per a
// later pass). Body is byte-equivalent to liboffs/src/Util/allocator.h.

#ifndef POSEIDON_ALLOCATOR_H
#define POSEIDON_ALLOCATOR_H
#include <stdlib.h>
void*  get_memory(size_t size);
void*  get_clear_memory(size_t size);

#endif //POSEIDON_ALLOCATOR_H
