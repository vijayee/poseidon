/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon memory-pool slab allocator used by the actor machinery.
 *
 * Vendored from liboffs/src/Actor/pool.h. See "Local modifications" at EOF.
 */

#ifndef POSEIDON_POOL_H
#define POSEIDON_POOL_H

#include "../Platform/platform.h"
#include <stddef.h>
#include <stdint.h>

#define POOL_MIN_BITS     4
#define POOL_MAX_BITS     11
#define POOL_COUNT        (POOL_MAX_BITS - POOL_MIN_BITS + 1)
#define POOL_ALIGN        1024
#define POOL_MIN_THRESHOLD 32
#define POOL_MAX_THRESHOLD 1024

typedef struct pool_item_t {
  struct pool_item_t* next;
} pool_item_t;

typedef struct pool_local_t {
  pool_item_t* head;
  size_t length;
  char* start;
  char* end;
} pool_local_t;

typedef struct pool_global_t {
  pool_item_t* head;
  size_t count;
  platform_mutex_t* lock;
} pool_global_t;

void*  pool_alloc(size_t index);
void   pool_free(size_t index, void* pointer);
size_t pool_index(size_t size);
size_t pool_used_size(size_t index);
void   pool_thread_cleanup(void);

#define POOL_ALLOC(TYPE)  ((TYPE*)pool_alloc(pool_index(sizeof(TYPE))))
#define POOL_FREE(TYPE, VALUE)  pool_free(pool_index(sizeof(TYPE)), (VALUE))

/* ---- Local modifications (vs upstream) -----------------------------------
 *
 * - Include guard renamed: OFFS_POOL_H -> POSEIDON_POOL_H.
 * - L2 provenance added: this file is a vendored copy of liboffs's pool.h,
 *   surgically transplanted to live under poseidon/src/Actor/.
 * - Header comment rewritten in poseidon's voice (slab allocator for the
 *   actor machinery); the size-class table is unchanged.
 *
 * No behaviour changes; no symbols renamed; the pool_t layout is byte-for-byte
 * the liboffs layout so that poseidon's Workers/ can adopt the same
 * allocator without a translation layer.
 * ------------------------------------------------------------------------- */

#endif // POSEIDON_POOL_H
