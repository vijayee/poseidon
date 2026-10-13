//
// Created by victor on 4/22/25.
//
#include "pool.h"
#include "../Util/allocator.h"
#include "../Util/memory_pool.h"
#include "../Util/log.h"

#if _WIN32
DWORD WINAPI workerFunction (LPVOID args);
#else
void* workerFunction (void* args);
#endif

work_pool_t* work_pool_create(size_t size) {
 work_pool_t* pool = get_clear_memory(sizeof(work_pool_t));
 pool->workers = get_clear_memory(sizeof(platform_thread_t*) * size);
#if _WIN32
 pool->workerIds = get_clear_memory(sizeof(DWORD)* size);
#endif
 pool->size = size;
 refcounter_init((refcounter_t*) pool);
 sharded_work_queue_init(&pool->sharded_queue);
 pool->lock = platform_mutex_create();
 pool->condition = platform_condvar_create();
 pool->shutdown = platform_condvar_create();
 pool->idle = platform_condvar_create();
 pool->barrier = platform_barrier_create((unsigned int)(size + 1));
 return pool;
}
void work_pool_destroy(work_pool_t* pool) {
  refcounter_dereference((refcounter_t*) pool);
  if (refcounter_count((refcounter_t*) pool) == 0) {
    sharded_work_queue_destroy(&pool->sharded_queue);
    platform_barrier_destroy(pool->barrier);
    platform_condvar_destroy(pool->shutdown);
    platform_condvar_destroy(pool->condition);
    platform_condvar_destroy(pool->idle);
    platform_mutex_destroy(pool->lock);
    free(pool->workers);
#if _WIN32
    free(pool->workerIds);
#endif
    free(pool);
  }
}
int work_pool_enqueue(work_pool_t* pool, work_t* work) {
  platform_mutex_lock(pool->lock);
  bool stopped = pool->stop;
  platform_mutex_unlock(pool->lock);

  if (stopped) return 1;

  sharded_work_enqueue(&pool->sharded_queue, work);
  platform_condvar_signal(pool->condition);
  return 0;
}
void work_pool_launch(work_pool_t* pool) {
  for (size_t i = 0; i < pool->size; i++) {
    pool->workers[i] = platform_thread_create((platform_thread_fn_t)workerFunction, pool);
    if (pool->workers[i] == NULL) {
      log_error("Failed to Start worker threads");
      abort();
    }
  }
  platform_barrier_wait(pool->barrier);
}

void work_pool_shutdown(work_pool_t* pool){
  platform_mutex_lock(pool->lock);
  pool->stop = 1;
  platform_condvar_broadcast(pool->condition);
  platform_mutex_unlock(pool->lock);
}

void work_pool_wait_for_shutdown_signal(work_pool_t* pool) {
  platform_mutex_lock(pool->lock);
  platform_condvar_wait(pool->shutdown, pool->lock);
  platform_mutex_unlock(pool->lock);
}

void work_pool_wait_for_idle_signal (work_pool_t* pool) {
  platform_mutex_lock(pool->lock);
  if (pool->idleCount != pool->size) {
    platform_condvar_wait(pool->idle, pool->lock);
  }
  platform_mutex_unlock(pool->lock);
}

void work_pool_join_all(work_pool_t* pool) {
  for (size_t i = 0; i < pool->size; i++) {
    platform_thread_join(pool->workers[i]);
  }
}

#if _WIN32
DWORD WINAPI workerFunction (LPVOID args) {
#else
void* workerFunction(void* args) {
#endif
  work_pool_t* pool = (work_pool_t*) args;
  platform_barrier_wait(pool->barrier);
  while(true) {
    // Fast path: dequeue WITHOUT pool lock (enables 16-way parallelism)
    work_t* work = sharded_work_dequeue(&pool->sharded_queue);

    // Wait path: only acquire pool lock when queue is empty
    if (!work) {
      platform_mutex_lock(pool->lock);

      // Re-check queue while holding lock
      work = sharded_work_dequeue(&pool->sharded_queue);

      // If still empty, wait for signal
      if (!work && !pool->stop) {
        pool->idleCount++;
        if (pool->idleCount == pool->size) {
          platform_condvar_signal(pool->idle);
        }
        platform_condvar_wait(pool->condition, pool->lock);
        pool->idleCount--;

        // Try again after wakeup
        work = sharded_work_dequeue(&pool->sharded_queue);
      }

      bool should_stop = pool->stop;
      platform_mutex_unlock(pool->lock);

      if (!work && should_stop) break;
    }

    if (work) {
      work = (work_t *) refcounter_reference((refcounter_t *) work);
      if (!pool->stop) {
        work->execute(work->ctx);
      } else {
        work->abort(work->ctx);
      }
      work_destroy(work);
    }
  }
  // Drain TLS cache before thread exits so blocks return to global pool
  memory_pool_tls_drain();
#if _WIN32
  return 0;
#else
  return NULL;
#endif
}