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
