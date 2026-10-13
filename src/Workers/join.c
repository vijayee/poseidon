//
// Async Join - Wait for multiple async operations to complete
// Created: 2026-04-12
//

#include "join.h"
#include "../Util/allocator.h"
#include <stdlib.h>

async_join_t* async_join_create(int count) {
    if (count <= 0) return NULL;

    async_join_t* join = get_clear_memory(sizeof(async_join_t));
    if (join == NULL) return NULL;

    join->lock = platform_mutex_create();
    join->condition = platform_condvar_create();

    join->results = calloc(count, sizeof(void*));
    if (join->results == NULL) {
        platform_mutex_destroy(join->lock);
        platform_condvar_destroy(join->condition);
        free(join);
        return NULL;
    }

    join->count = count;
    join->pending = count;  // _Atomic init

    return join;
}

void async_join_destroy(async_join_t* join) {
    if (join == NULL) return;

    platform_mutex_destroy(join->lock);
    platform_condvar_destroy(join->condition);
    free(join->results);
    free(join);
}

void async_join_complete(async_join_t* join, int index, void* result) {
    if (join == NULL || index < 0 || index >= join->count) return;

    platform_mutex_lock(join->lock);
    join->results[index] = result;
    join->pending--;
    platform_mutex_unlock(join->lock);

    platform_condvar_signal(join->condition);
}

void async_join_wait(async_join_t* join) {
    if (join == NULL) return;

    platform_mutex_lock(join->lock);
    while (join->pending > 0) {
        platform_condvar_wait(join->condition, join->lock);
    }
    platform_mutex_unlock(join->lock);
}