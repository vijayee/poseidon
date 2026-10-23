/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor round-trip replies.
 *
 * The algorithm follows upstream's peer book round trip
 * (liboffs/src/Network/peer_book.c): send the request, wait on the
 * reply's condvar, and on a timeout hand the request to the actor
 * rather than freeing it underneath the actor's feet.
 */

#include "actor_reply.h"
#include "../Platform/platform_thread.h"
#include "../Util/allocator.h"

#include <string.h>

int actor_reply_init(actor_reply_t* reply) {
    if (reply == NULL) return -1;

    reply->mutex = platform_mutex_create();
    if (reply->mutex == NULL) return -1;

    reply->cv = platform_condvar_create();
    if (reply->cv == NULL) {
        platform_mutex_destroy(reply->mutex);
        reply->mutex = NULL;
        return -1;
    }

    reply->done = 0;
    reply->orphaned = 0;
    reply->detached = 0;
    return 0;
}

void actor_reply_destroy(actor_reply_t* reply) {
    if (reply == NULL) return;

    if (reply->mutex != NULL) {
        platform_mutex_destroy(reply->mutex);
        reply->mutex = NULL;
    }
    if (reply->cv != NULL) {
        platform_condvar_destroy(reply->cv);
        reply->cv = NULL;
    }
}

int actor_round_trip(actor_t* actor, uint32_t type, actor_reply_t* reply,
                     uint32_t timeout_ms) {
    if (actor == NULL || reply == NULL) return 1;

    /* A destroyed actor will never dispatch the request, so nobody will
     * ever answer it. This is the only "nobody will process this" case:
     * actor_send's return value is a scheduling hint (was the mailbox
     * empty), not a delivery acknowledgement. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) return 1;

    /* payload_destroy stays NULL: the request shell is owned by the
     * caller-or-actor scheme below, never by the queue. A message
     * dropped during teardown leaks the request rather than pulling it
     * out from under a waiting caller. */
    message_t message;
    memset(&message, 0, sizeof(message));
    message.type = type;
    message.payload = reply;
    message.payload_destroy = NULL;
    actor_send(actor, &message);

    platform_mutex_lock(reply->mutex);
    for (;;) {
        if (reply->done) break;
        if (platform_condvar_timed_wait(reply->cv, reply->mutex, timeout_ms) != 0) {
            if (!reply->done) {
                /* The caller gave up: hand the request to the actor. It
                 * may still be queued or mid-apply, and it frees the
                 * request when it is done with it. */
                reply->orphaned = 1;
                platform_mutex_unlock(reply->mutex);
                return -1;
            }
            break;
        }
    }
    platform_mutex_unlock(reply->mutex);
    return 0;
}

int actor_request(actor_t* actor, uint32_t type, actor_reply_t* reply,
                  uint32_t timeout_ms) {
    if (actor == NULL || reply == NULL) return 1;

    /* A dispatch runs on a pool worker, and a worker that waits for
     * another actor is a worker that runs no actor at all. Inside a
     * dispatch the request goes out detached: the actor does the work
     * and the answer is dropped. */
    if (actor_is_inside_dispatch()) {
        /* A destroyed actor will never dispatch the request, so nobody
         * will ever answer it. The caller still owns it. */
        if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) return 1;

        actor_reply_detach(reply);
        message_t message;
        memset(&message, 0, sizeof(message));
        message.type = type;
        message.payload = reply;
        /* payload_destroy stays NULL: the dispatch that handles the
         * request frees the shell, and a shell freed twice is a crash.
         * A dropped request leaks its shell, which is the trade upstream
         * makes at teardown. */
        message.payload_destroy = NULL;
        actor_send(actor, &message);
        return ACTOR_REQUEST_DETACHED;
    }

    return actor_round_trip(actor, type, reply, timeout_ms);
}

bool actor_answer_send(const actor_answer_t* answer, int result) {
    if (answer == NULL || answer->reply_to == NULL) return false;

    /* The answer is its own message: the worker fills in the result and
     * hands the copy to the actor that asked for it. */
    actor_answer_t* copy = (actor_answer_t*)get_memory(sizeof(actor_answer_t));
    if (copy == NULL) return false;
    copy->reply_to = answer->reply_to;
    copy->reply_type = answer->reply_type;
    copy->token = answer->token;
    copy->result = result;

    message_t message;
    memset(&message, 0, sizeof(message));
    message.type = answer->reply_type;
    message.payload = copy;
    /* The queue frees the answer when the dispatch is done with it. */
    message.payload_destroy = (void (*)(void*))free;
    actor_send(answer->reply_to, &message);
    return true;
}

int actor_reply_wait(actor_reply_t* reply, uint32_t timeout_ms) {
    if (reply == NULL) return -1;

    platform_mutex_lock(reply->mutex);
    for (;;) {
        if (reply->done) break;
        if (platform_condvar_timed_wait(reply->cv, reply->mutex, timeout_ms) != 0) {
            if (!reply->done) {
                platform_mutex_unlock(reply->mutex);
                return -1;
            }
            break;
        }
    }
    platform_mutex_unlock(reply->mutex);
    return 0;
}

void actor_reply_detach(actor_reply_t* reply) {
    if (reply == NULL) return;
    /* Set before the request is sent: the actor can only see the request
     * after the send, and the send is what orders the two. */
    platform_mutex_lock(reply->mutex);
    reply->detached = 1;
    platform_mutex_unlock(reply->mutex);
}

bool actor_reply_complete(actor_reply_t* reply) {
    if (reply == NULL) return false;

    if (reply->detached) {
        /* Nobody is waiting for this one: the actor owns the request and
         * has to free it. */
        return true;
    }

    platform_mutex_lock(reply->mutex);
    if (reply->orphaned) {
        /* The caller timed out and handed us ownership. The work still
         * happened; only the answer is lost. */
        platform_mutex_unlock(reply->mutex);
        return true;
    }
    reply->done = 1;
    platform_condvar_signal(reply->cv);
    platform_mutex_unlock(reply->mutex);
    return false;
}
