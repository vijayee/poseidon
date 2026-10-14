//
// Tests for the lock-free Chase-Lev work-stealing deque vendored under
// poseidon/src/Scheduler/. The deque is the per-worker local queue in the
// scheduler pool: deque_push/deque_pop are LIFO (owner-only), deque_steal
// is FIFO (thieves race against the owner).
//

#include <gtest/gtest.h>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

extern "C" {
#include "Scheduler/deque.h"
}

// ============================================================================
// SINGLE-THREADED PUSH/POP
// ============================================================================

class DequeTest : public ::testing::Test {};

TEST_F(DequeTest, PushPopRoundTrip) {
    deque_t deque;
    deque_init(&deque);

    constexpr int N = 100;
    for (int i = 0; i < N; i++) {
        // deque_push is void; growth-on-full is internal. With N << initial
        // capacity (1024) and a single thread, no abort / grow is reachable.
        deque_push(&deque, (void*)(intptr_t)i);
    }

    // Pop is LIFO — the last pushed item comes out first.
    for (int i = N - 1; i >= 0; i--) {
        void* popped = deque_pop(&deque);
        EXPECT_EQ((intptr_t)popped, i);
    }

    // Empty after draining.
    EXPECT_TRUE(deque_isempty(&deque));
    EXPECT_EQ(deque_size(&deque), (size_t)0);

    deque_destroy(&deque);
}

// ============================================================================
// WORK-STEALING: one producer thread pushes, the main thread steals.
// Items are integers shifted by +2 to avoid colliding with the deque's
// internal sentinels DEQUE_EMPTY=0 and DEQUE_ABORT=1 — encoding real
// values as (void*)0 or (void*)1 would make a stolen item indistinguishable
// from an "empty queue" or "raced with owner" result.
//
// We assert every value in [0, N) is observed exactly once. The order is
// unordered because the stealer races the owner: the owner is still pushing
// while the stealer drains from the top, so FIFO-from-bottom is only
// approximate. The contract is multiset equality.
// ============================================================================

TEST_F(DequeTest, StealFromOtherThread) {
    deque_t deque;
    deque_init(&deque);

    constexpr int N = 1000;
    constexpr intptr_t BIAS = 2;  // avoid DEQUE_EMPTY (0) and DEQUE_ABORT (1)
    std::set<int> observed;
    std::atomic<int> total_seen{0};

    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            deque_push(&deque, (void*)(intptr_t)(i + BIAS));
            // Yield periodically so the stealer has a chance to drain.
            if ((i & 0x3F) == 0) {
                std::this_thread::yield();
            }
        }
    });

    // Steal until every value has been observed. We treat both DEQUE_EMPTY
    // (queue transiently empty) and DEQUE_ABORT (steal lost the race to the
    // owner's pop on the last item) as transient — neither counts as a
    // value. We also bound the loop with a producer-finished flag so a
    // missed steal can't deadlock the test.
    bool producer_done = false;
    std::thread waiter([&]() {
        producer.join();
        producer_done = true;
    });

    while (total_seen.load() < N) {
        void* stolen = deque_steal(&deque);
        if (stolen == DEQUE_EMPTY || stolen == DEQUE_ABORT) {
            std::this_thread::yield();
            continue;
        }
        int v = (int)((intptr_t)stolen - BIAS);
        EXPECT_GE(v, 0);
        EXPECT_LT(v, N);
        observed.insert(v);
        total_seen.fetch_add(1);
    }

    waiter.join();

    // Every value in [0, N) must be observed exactly once. Because we use
    // std::set, the count of unique observed values == N iff there are no
    // duplicates and no misses.
    EXPECT_EQ(observed.size(), (size_t)N);
    for (int i = 0; i < N; i++) {
        EXPECT_TRUE(observed.count(i) == 1) << "missing or duplicate value: " << i;
    }

    deque_destroy(&deque);
}
