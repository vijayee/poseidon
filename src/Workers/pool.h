//
// Created by victor on 4/20/25.
//

#ifndef WAVEDB_POOL_H
#define WAVEDB_POOL_H
#include <stddef.h>
#include <stdint.h>
#include "../Platform/platform_thread.h"
#include "../RefCounter/refcounter.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  refcounter_t refcounter;
  platform_mutex_t* lock;
  platform_condvar_t* condition;
  platform_barrier_t* barrier;
  platform_condvar_t* shutdown;
  platform_condvar_t* idle;
  size_t idleCount;
  sharded_work_queue_t sharded_queue;
  platform_thread_t** workers;
#if _WIN32
  DWORD* workerIds;
#endif
  size_t size;
  uint8_t stop;
} work_pool_t;

work_pool_t* work_pool_create(size_t size);
void work_pool_destroy(work_pool_t* pool);
int work_pool_enqueue(work_pool_t* pool, work_t* work);
void work_pool_launch(work_pool_t* pool);
void work_pool_shutdown(work_pool_t* pool);
void work_pool_wait_for_shutdown_signal(work_pool_t* pool);
void work_pool_wait_for_idle_signal(work_pool_t* pool);
void work_pool_join_all(work_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif //WAVEDB_POOL_H
