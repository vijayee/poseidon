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

bool actor_reply_complete(actor_reply_t* reply) {
    if (reply == NULL) return false;

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
