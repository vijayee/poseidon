//
// Smoke test for the actor machinery vendored under poseidon/src/Actor/.
// The test exercises actor_init / actor_send / actor_run / actor_destroy
// without a scheduler pool, so the actor is "inline" (pool == NULL) and
// the caller drives dispatch synchronously. The scheduler-aware dispatch
// path is exercised in scheduler_test.cpp's neighbourhood by future
// integration tests; this file targets the actor's own behaviour.
//

#include <gtest/gtest.h>
#include <vector>

extern "C" {
#include "Actor/actor.h"
#include "Actor/message.h"
}

namespace {

struct fixture_state_t {
    std::vector<int>* received; // owned by the test
};

void fixture_dispatch(void* state, message_t* msg) {
    fixture_state_t* s = static_cast<fixture_state_t*>(state);
    // The sender of these test messages passes a borrowed int* as
    // payload with a no-op payload_destroy. The dispatch must not
    // free the payload; ownership stays with the test's backing array.
    int v = *(static_cast<int*>(msg->payload));
    s->received->push_back(v);
}

} // namespace

class ActorTest : public ::testing::Test {};

TEST_F(ActorTest, InitSendRunDestroy) {
    // Use a NULL pool so the actor is "inline": actor_send does not
    // call scheduler_inject, and actor_destroy's wait loop on
    // queue_state breaks out immediately. This is the simplest
    // deterministic surface for an actor.
    std::vector<int> received;
    fixture_state_t state{&received};

    // Backing array for the 5 message payloads. The dispatch borrows
    // these pointers; payload_destroy is null so the actor machinery
    // never tries to free them. Stack allocation is safe because the
    // array outlives actor_run.
    int backing[5] = {0, 1, 2, 3, 4};

    actor_t actor;
    actor_init(&actor, &state, fixture_dispatch, /*pool=*/nullptr);

    for (int i = 0; i < 5; i++) {
        message_t msg;
        msg.type = 0;
        msg.payload = &backing[i];
        msg.payload_destroy = nullptr; // borrowed; the test owns backing
        // actor_send returns whether the queue was empty BEFORE this push
        // (i.e. whether this send scheduled the actor for the first time).
        // For an inline actor (pool == NULL) the value is informational: the
        // first send returns true (was empty), subsequent sends return false.
        // We only assert that the call was accepted (didn't get rejected
        // because the actor is being destroyed).
        bool scheduled = actor_send(&actor, &msg);
        // First send is the one that schedules; the rest are follow-up
        // pushes. Either way, accepted is a non-bool signal here; just
        // observe the return without asserting on it.
        (void)scheduled;
    }

    // actor_run drains up to ACTOR_BATCH_SIZE (32) messages; we
    // queued 5 so all must dispatch and actor_run must return false
    // (no more work, queue is empty after this call).
    bool more = actor_run(&actor, ACTOR_BATCH_SIZE);
    EXPECT_FALSE(more);

    // Verify FIFO order.
    ASSERT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(received[i], i) << "order mismatch at index " << i;
    }

    // Destroy: with a NULL pool, the wait loop is a no-op. We
    // assert no hang / abort.
    actor_destroy(&actor);
    SUCCEED();
}

// ============================================================================
// A DISPATCH NEVER WAITS FOR ANOTHER ACTOR
// ============================================================================
//
// The rule, from the two references: a dispatch runs on a pool worker, and
// a worker that waits for another actor is a worker that runs no actor.
// Upstream keeps the peer book's round trip off the pool for exactly this
// reason (liboffs/src/Network/peer_book.h: the stop path "must only be
// called from a thread that may block and is never a pool worker"), and a
// Pony behaviour call is fire-and-forget for the same reason.
//
// The pool here has ONE worker. If the dispatch below waited for the
// second actor, the one worker would be waiting for itself: the request
// would never be dispatched, the answer would never come, and the test
// would hang until the harness's timeout. So this test is a guard: a
// change that makes a dispatch wait again fails here.

extern "C" {
#include "Actor/actor_reply.h"
#include "Scheduler/scheduler.h"
}

namespace {

constexpr uint32_t TEST_MSG_PING = 1;

/* A request: the reply block first, the way the wrappers build it. */
struct test_request_t {
    actor_reply_t reply;
    actor_t* worker;
};

/* The second actor: it counts the pings it is asked for. */
struct worker_state_t {
    int pings;
};

void worker_dispatch(void* state, message_t* msg) {
    worker_state_t* s = static_cast<worker_state_t*>(state);
    if (msg->type != TEST_MSG_PING) return;
    s->pings++;
    test_request_t* p = static_cast<test_request_t*>(msg->payload);
    if (p == nullptr) return;
    /* The request is the worker's now: the caller detached it, so nobody
     * is waiting for the answer and the worker frees the request. */
    if (actor_reply_complete(&p->reply)) {
        actor_reply_destroy(&p->reply);
        free(p);
    }
}

/* The first actor: its dispatch asks the second actor for a pong. */
struct caller_state_t {
    actor_t* worker;
    int requests;
    int detached;
};

void caller_dispatch(void* state, message_t* msg) {
    caller_state_t* s = static_cast<caller_state_t*>(state);
    test_request_t* p = static_cast<test_request_t*>(msg->payload);
    if (msg->type != TEST_MSG_PING || p == nullptr) return;

    s->requests++;

    /* Ask the other actor. This call is made from inside a dispatch, so
     * it must not wait: the request goes out detached and the worker does
     * the work when it gets to it. */
    test_request_t* inner =
        static_cast<test_request_t*>(calloc(1, sizeof(test_request_t)));
    if (inner != nullptr) {
        inner->worker = s->worker;
        actor_reply_init(&inner->reply);
        int trip = actor_request(s->worker, TEST_MSG_PING, &inner->reply, 1000);
        if (trip == ACTOR_REQUEST_DETACHED) {
            s->detached++;
            /* The worker owns the request now. */
        } else {
            /* Answered, or the caller gave up: the caller owns it. */
            actor_reply_destroy(&inner->reply);
            free(inner);
        }
    }

    /* Answer the test's own request. The test's thread is not running an
     * actor, so it is waiting for this answer — that is the round trip. */
    if (actor_reply_complete(&p->reply)) {
        actor_reply_destroy(&p->reply);
        free(p);
    }
}

} // namespace

TEST_F(ActorTest, ADispatchDoesNotWaitForAnotherActor) {
    scheduler_pool_t* pool = scheduler_pool_create(1);
    ASSERT_NE(nullptr, pool);
    scheduler_pool_start(pool);

    worker_state_t worker_state{0};
    actor_t worker;
    actor_init(&worker, &worker_state, worker_dispatch, pool);

    caller_state_t caller_state{&worker, 0, 0};
    actor_t caller;
    actor_init(&caller, &caller_state, caller_dispatch, pool);

    /* The test's own thread asks the caller actor to run. The test's
     * thread is not running an actor, so this request waits for the
     * answer — that is the round trip, and it is legal here. */
    test_request_t* request =
        static_cast<test_request_t*>(calloc(1, sizeof(test_request_t)));
    ASSERT_NE(nullptr, request);
    request->worker = &worker;
    ASSERT_EQ(0, actor_reply_init(&request->reply));

    message_t msg;
    msg.type = TEST_MSG_PING;
    msg.payload = &request->reply;
    msg.payload_destroy = nullptr;
    actor_send(&caller, &msg);

    /* The caller's dispatch runs on the one worker. If it waited for the
     * second actor, the worker would be waiting for itself and the answer
     * below would never come. */
    int waited = actor_reply_wait(&request->reply, 5000);
    EXPECT_EQ(0, waited) << "the dispatch blocked: the answer never came";

    /* The dispatch detached the request rather than waiting for it. */
    EXPECT_EQ(1, caller_state.requests);
    EXPECT_EQ(1, caller_state.detached);

    /* And the worker did the work: the request was queued, not dropped. */
    scheduler_pool_wait_for_idle(pool);
    EXPECT_EQ(1, worker_state.pings);

    actor_reply_destroy(&request->reply);
    free(request);
    actor_destroy(&caller);
    actor_destroy(&worker);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
}


// ============================================================================
// AN ACTOR CAN HAVE AN ANSWER WITHOUT A WORKER WAITING
// ============================================================================
//
// Upstream's reply_to shape: the request carries the actor the answer
// belongs to, and the worker "replies to reply_to with a summary"
// (liboffs/src/OFFStreams/representation_actor.h). The answer arrives as
// a message, so the actor that is waiting for it is unscheduled while it
// waits — it holds no thread.
//
// The pool has ONE worker. If the waiting blocked, the worker would be
// waiting for itself.

namespace {

constexpr uint32_t TEST_MSG_ASK = 10;
constexpr uint32_t TEST_MSG_WORK = 11;
constexpr uint32_t TEST_MSG_ANSWER = 12;

/* The request that goes to the worker: the answer comes back to the
 * actor that asked for it. */
struct ask_t {
    actor_reply_t reply;   /**< Must stay first. */
    actor_answer_t answer; /**< Where the answer goes. */
    int request_id;
};

/* The request the test's thread sends to the waiting actor. */
struct outer_t {
    actor_reply_t reply;   /**< Must stay first. */
    int request_id;
};

struct answer_worker_state_t {
    int works;
};

void answering_worker_dispatch(void* state, message_t* msg) {
    answer_worker_state_t* s = static_cast<answer_worker_state_t*>(state);
    if (msg->type != TEST_MSG_WORK) return;
    s->works++;
    ask_t* p = static_cast<ask_t*>(msg->payload);
    if (p == nullptr) return;
    /* The worker answers the actor that asked: the answer goes back as a
     * message, so the asking actor is not holding a thread. */
    actor_answer_send(&p->answer, 42 + p->request_id);
    /* The request was the worker's. */
    actor_reply_destroy(&p->reply);
    free(p);
}

struct waiting_state_t {
    actor_t* self;         /**< This actor: the answer comes back to it. */
    actor_t* worker;
    int asks;
    int answers;
};

void waiting_dispatch(void* state, message_t* msg) {
    waiting_state_t* s = static_cast<waiting_state_t*>(state);

    if (msg->type == TEST_MSG_ASK) {
        outer_t* p = static_cast<outer_t*>(msg->payload);
        if (p == nullptr) return;
        s->asks++;

        /* Ask the worker for an answer. The answer is addressed to this
         * actor, so the request does not wait for it: the worker is free
         * to pick the request up while this dispatch returns. */
        ask_t* ask = static_cast<ask_t*>(calloc(1, sizeof(ask_t)));
        if (ask == nullptr) return;
        ask->request_id = p->request_id;
        ask->answer.reply_to = s->self;
        ask->answer.reply_type = TEST_MSG_ANSWER;
        ask->answer.token = p;   /* the requester's own state, carried back */
        if (actor_reply_init(&ask->reply) != 0) {
            free(ask);
            return;
        }

        message_t out;
        out.type = TEST_MSG_WORK;
        out.payload = ask;
        out.payload_destroy = nullptr;
        actor_send(s->worker, &out);
        return;
    }

    if (msg->type == TEST_MSG_ANSWER) {
        actor_answer_t* answer = static_cast<actor_answer_t*>(msg->payload);
        if (answer == nullptr) return;
        s->answers++;
        /* The answer carries the requester's own state back to it. */
        outer_t* p = static_cast<outer_t*>(answer->token);
        if (p != nullptr) {
            p->reply.result = answer->result;
            if (actor_reply_complete(&p->reply)) {
                /* The caller never got the answer: the actor disposes of
                 * it. */
                actor_reply_destroy(&p->reply);
                free(p);
            }
        }
    }
}

} // namespace

TEST_F(ActorTest, AnActorCanHaveAnAnswerWithoutAWorkerWaiting) {
    scheduler_pool_t* pool = scheduler_pool_create(1);
    ASSERT_NE(nullptr, pool);
    scheduler_pool_start(pool);

    answer_worker_state_t worker_state{0};
    actor_t worker;
    actor_init(&worker, &worker_state, answering_worker_dispatch, pool);

    waiting_state_t waiting_state{nullptr, &worker, 0, 0};
    actor_t waiting;
    actor_init(&waiting, &waiting_state, waiting_dispatch, pool);
    waiting_state.self = &waiting;

    /* The test's thread asks the waiting actor. The test's thread is not
     * running an actor, so this request waits for the answer — that is
     * the round trip, and it is legal here. */
    outer_t* request = static_cast<outer_t*>(calloc(1, sizeof(outer_t)));
    ASSERT_NE(nullptr, request);
    request->request_id = 7;
    ASSERT_EQ(0, actor_reply_init(&request->reply));

    message_t msg;
    msg.type = TEST_MSG_ASK;
    msg.payload = &request->reply;
    msg.payload_destroy = nullptr;
    actor_send(&waiting, &msg);

    /* The waiting actor's dispatch asked the worker and returned; the
     * answer comes back as a message. With one worker, a dispatch that
     * waited would deadlock the pool, and the answer below would never
     * come. */
    int waited = actor_reply_wait(&request->reply, 5000);
    EXPECT_EQ(0, waited) << "the answer never came";
    EXPECT_EQ(42 + 7, request->reply.result);

    EXPECT_EQ(1, waiting_state.asks);
    EXPECT_EQ(1, waiting_state.answers);
    EXPECT_EQ(1, worker_state.works);

    actor_reply_destroy(&request->reply);
    free(request);
    actor_destroy(&waiting);
    actor_destroy(&worker);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
}
