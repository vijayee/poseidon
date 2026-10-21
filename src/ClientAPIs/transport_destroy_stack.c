//
// transport_destroy_stack.c
//
// Cross-backend implementation of the destroy_stack_t helpers declared in
// transport_internal.h. Every listener backend (Unix, TCP, WS, WT, QUIC)
// owns a destroy_stack_t and uses these four routines to defer pd_watcher
// teardown to the loop thread. The drain runs at the bottom of each
// pd_loop_run_once iteration in the listener's I/O thread.
//
// The shape mirrors the per-listener static helpers used in the
// individual backend transports; lifted into a shared translation
// unit so each backend .c does not have to redefine the LIFO list
// management.
//
#include "transport_internal.h"
#include "../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include <string.h>

void destroy_stack_init(destroy_stack_t* stack) {
    stack->lock = platform_mutex_create();
    stack->head = NULL;
}

void destroy_stack_destroy(destroy_stack_t* stack) {
    destroy_stack_drain(stack);
    platform_mutex_destroy(stack->lock);
    stack->lock = NULL;
    stack->head = NULL;
}

void destroy_stack_push(destroy_stack_t* stack, pd_watcher_t* watcher) {
    destroy_node_t* node = get_clear_memory(sizeof(destroy_node_t));
    if (node == NULL) {
        /* The node could not be allocated, so the watcher cannot be
         * handed to the loop thread. Destroying it here runs the
         * unregister on the wrong thread, but a leaked watcher would
         * be worse: the loop would keep dispatching events to freed
         * memory. */
        if (watcher != NULL) {
            pd_watcher_stop(watcher);
            pd_watcher_destroy(watcher);
        }
        return;
    }
    node->watcher = watcher;
    node->next = NULL;
    platform_mutex_lock(stack->lock);
    node->next = stack->head;
    stack->head = node;
    platform_mutex_unlock(stack->lock);
}

void destroy_stack_drain(destroy_stack_t* stack) {
    platform_mutex_lock(stack->lock);
    destroy_node_t* node = stack->head;
    stack->head = NULL;
    platform_mutex_unlock(stack->lock);
    while (node != NULL) {
        destroy_node_t* next = node->next;
        if (node->watcher != NULL) {
            /* pd_watcher_destroy stops the watcher first, so the
             * descriptor is unregistered from the loop before the
             * watcher memory goes away. */
            pd_watcher_destroy(node->watcher);
        }
        free(node);
        node = next;
    }
}
