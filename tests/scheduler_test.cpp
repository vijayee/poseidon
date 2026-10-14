//
// Smoke test for the work-stealing scheduler pool vendored under
// poseidon/src/Scheduler/. This test does NOT exercise actor dispatch — it
// only verifies the lifecycle: create, start, idle-wait, stop, destroy.
// The actor-machinery tests live in actor_test.cpp.
//

#include <gtest/gtest.h>

extern "C" {
#include "Scheduler/scheduler.h"
}

class SchedulerPoolTest : public ::testing::Test {};

TEST_F(SchedulerPoolTest, CreateStartStopDestroy) {
    // Use 2 workers to dodge the single-worker degenerate case. Note: a
    // worker_count of 0 would deadlock inside scheduler_pool_create's
    // barrier setup (worker_count+1 parties, no worker spawns), so the
    // minimum is 1.
    constexpr size_t WORKERS = 2;

    scheduler_pool_t* pool = scheduler_pool_create(WORKERS);
    ASSERT_NE(pool, nullptr);

    scheduler_pool_start(pool);

    // No actors injected; idle wait must return promptly.
    int idle_ret = scheduler_pool_wait_for_idle(pool);
    EXPECT_EQ(idle_ret, 0);

    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    // No thread leaks observable without a tracer; the test passes if it
    // returns without hanging or aborting.
    SUCCEED();
}
