//
// Created by victor on 3/30/25.
//
// Vendored from liboffs on 2026-09-26 (commit 1.2, actor-migration branch).
// Poseidon-specific mutations:
//   * No header drops. NULL reachable via log.h (per its vendored pass).
// Body is byte-equivalent to liboffs/src/Util/allocator.c.

#include "allocator.h"
#include <stdlib.h>
#include "log.h"

void* get_memory(size_t size) {
  void* mem = malloc(size);
  if (mem == NULL) {
    log_error("Out of memory");
    abort();
  }
  return mem;
}
void* get_clear_memory(size_t size) {
   void* mem =  calloc(1, size);
   if (mem == NULL) {
     log_error("Out of memory");
     abort();
   }
   return mem;
}
