/*
 * SPDX-License-Identifier: MIT
 *
 * Poseidon actor round-trip replies.
 *
 * How a caller that is not the owning actor gets an answer out of an
 * actor. Upstream's peer book is the reference: the caller sends a
 * request that carries a reply block, then waits on a condition
 * variable until the actor has filled the answer in. It does NOT run
 * the actor on the caller's thread — upstream has no inline actor run
 * anywhere, and the reason is here: an actor runs on one thread at a
 * time, so a caller that runs it inline races the pool worker that may
 * have picked the same actor up.
 *
 * The reply block is embedded as the FIRST member of a request payload,
 * so the dispatch can address the block and the whole request through
 * one pointer, the way upstream's peer_book_reply_t is embedded.
 *
 * Ownership, in the three cases the round trip distinguishes:
 *
 *   - answered: the caller reads the result and frees the request.
 *   - timed out: the caller hands the request over (it sets orphaned)
 *     and frees nothing; the actor frees the request when it is done
 *     with it.
 *   - not queued: the actor is gone, nobody will ever look at the
 *     request, and the caller frees it.
 *
 * The mutex and the condvar belong to the request: whoever frees the
 * request destroys them with actor_reply_destroy.
 */

#ifndef POSEIDON_ACTOR_REPLY_H
#define POSEIDON_ACTOR_REPLY_H

#include <stdint.h>
#include <stdbool.h>

#include "actor.h"
#include "../Platform/platform_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The reply plumbing of one request. Embedded as the first member of a
 * request payload.
 */
typedef struct actor_reply_t {
    platform_mutex_t* mutex;    /**< Guards done and orphaned. */
    platform_condvar_t* cv;     /**< Signalled when the answer is in. */
    int done;                   /**< Set by the actor when it has answered. */
    int orphaned;               /**< Set by the caller when it gave up. */
} actor_reply_t;

/**
 * Prepares a reply block: creates the mutex and the condvar.
 *
 * @return 0 on success, -1 on failure.
 */
int actor_reply_init(actor_reply_t* reply);

/**
 * Releases the reply's mutex and condvar. Call it from whatever frees
 * the request shell — the caller on the answered and not-queued paths,
 * the actor on the orphaned path.
 */
void actor_reply_destroy(actor_reply_t* reply);

/**
 * Sends a request to an actor and waits for its answer.
 *
 * The request payload MUST start with the reply block, so that the
 * pointer handed to the actor is the reply block itself and the actor
 * can cast it back to the full request.
 *
 * @param actor      The actor that owns the state the request touches.
 * @param type       The message type.
 * @param reply      The reply block, embedded in the request payload.
 * @param timeout_ms How long the caller waits before giving up.
 * @return 0 when the request was answered, -1 when the caller timed out
 *         (the request was handed to the actor; the caller must not
 *         free it), 1 when the request was never queued (the actor is
 *         gone; the caller frees it).
 */
int actor_round_trip(actor_t* actor, uint32_t type, actor_reply_t* reply,
                     uint32_t timeout_ms);

/**
 * Delivers the answer the dispatch has produced.
 *
 * @return true when the caller had already timed out and handed the
 *         request over — the dispatch must then free the request shell.
 *         false when the answer was delivered, and the caller owns the
 *         request again.
 */
bool actor_reply_complete(actor_reply_t* reply);

#ifdef __cplusplus
}
#endif

#endif /* POSEIDON_ACTOR_REPLY_H */
