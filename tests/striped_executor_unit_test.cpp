// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file striped_executor_unit_test.cpp
/// @brief Unit tests for `kythira::striped_serial_executor` (task 10).
///
/// The property that matters is not "tasks run" — it is that **no key is ever
/// entered concurrently**, and that this holds with far more keys than threads.
/// A re-entrance detector is therefore the centre of this file: a plain
/// counter would pass against an executor that ran everything in parallel.

#define BOOST_TEST_MODULE striped_executor_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/striped_executor.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

using kythira::striped_serial_executor;

namespace {

/// Flags a key that is entered while already inside it. `std::atomic` rather
/// than a mutex because a mutex would *serialise* the very overlap this is
/// trying to catch.
class reentrance_detector {
public:
    explicit reentrance_detector(std::size_t keys) : _inside(keys) {}

    auto enter(std::size_t key) -> void {
        if (_inside[key].fetch_add(1, std::memory_order_acq_rel) != 0) {
            _violations.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto leave(std::size_t key) -> void { _inside[key].fetch_sub(1, std::memory_order_acq_rel); }
    [[nodiscard]] auto violations() const -> std::size_t {
        return _violations.load(std::memory_order_relaxed);
    }

private:
    std::deque<std::atomic<int>> _inside;
    std::atomic<std::size_t> _violations{0};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(striped_executor_unit)

BOOST_AUTO_TEST_CASE(the_default_pool_is_bounded_and_never_empty) {
    BOOST_CHECK_GE(striped_serial_executor::default_stripe_count(), 1u);
    BOOST_CHECK_LE(striped_serial_executor::default_stripe_count(), 8u);

    // A zero request is corrected rather than rejected: an executor with no
    // threads would accept work and never run it, which is the worst failure
    // mode available here.
    striped_serial_executor zero{0};
    BOOST_CHECK_EQUAL(zero.stripe_count(), 1u);
}

BOOST_AUTO_TEST_CASE(a_key_always_maps_to_the_same_stripe) {
    striped_serial_executor pool{4};
    for (std::size_t h = 0; h < 100; ++h) {
        BOOST_CHECK_EQUAL(pool.stripe_for(h), pool.stripe_for(h));
        BOOST_CHECK_LT(pool.stripe_for(h), 4u);
    }
}

BOOST_AUTO_TEST_CASE(work_on_one_stripe_runs_in_the_order_it_was_posted) {
    striped_serial_executor pool{4};
    std::vector<int> order;
    for (int i = 0; i < 50; ++i) {
        BOOST_REQUIRE(pool.post(1, [&order, i] { order.push_back(i); }));
    }
    pool.post_and_wait(1, [] {});

    BOOST_REQUIRE_EQUAL(order.size(), 50u);
    for (int i = 0; i < 50; ++i) {
        BOOST_CHECK_EQUAL(order[static_cast<std::size_t>(i)], i);
    }
}

BOOST_AUTO_TEST_CASE(two_hundred_keys_on_four_threads_are_never_entered_concurrently) {
    // The case the design exists for: the thread count is a machine property,
    // not a shard-count property.
    constexpr std::size_t keys = 200;
    constexpr std::size_t threads = 4;
    constexpr std::size_t rounds = 20;

    striped_serial_executor pool{threads};
    BOOST_CHECK_EQUAL(pool.stripe_count(), threads);

    reentrance_detector detector{keys};
    std::atomic<std::size_t> completed{0};

    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t key = 0; key < keys; ++key) {
            const auto stripe = pool.stripe_for(std::hash<std::size_t>{}(key));
            BOOST_REQUIRE(pool.post(stripe, [&detector, &completed, key] {
                detector.enter(key);
                // Long enough that a genuinely parallel executor would overlap.
                std::this_thread::yield();
                detector.leave(key);
                completed.fetch_add(1, std::memory_order_relaxed);
            }));
        }
    }

    pool.drain_all();
    BOOST_CHECK_EQUAL(completed.load(), keys * rounds);
    BOOST_CHECK_EQUAL(detector.violations(), 0u);
}

BOOST_AUTO_TEST_CASE(the_thread_count_is_independent_of_the_key_count) {
    // Same assertion from the other side: a thousand keys must not produce a
    // thousand threads.
    striped_serial_executor pool{3};
    for (std::size_t key = 0; key < 1000; ++key) {
        BOOST_REQUIRE(pool.post(pool.stripe_for(key), [] {}));
    }
    BOOST_CHECK_EQUAL(pool.stripe_count(), 3u);
    pool.drain_all();
}

BOOST_AUTO_TEST_CASE(post_and_wait_drains_everything_queued_ahead_of_it) {
    // This is what group teardown depends on: destroy the group only after its
    // own queue is empty.
    striped_serial_executor pool{2};
    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        BOOST_REQUIRE(pool.post(0, [&ran] { ran.fetch_add(1); }));
    }
    pool.post_and_wait(0, [] {});
    BOOST_CHECK_EQUAL(ran.load(), 100);
}

BOOST_AUTO_TEST_CASE(post_and_wait_propagates_a_failure_rather_than_swallowing_it) {
    striped_serial_executor pool{2};
    BOOST_CHECK_THROW(pool.post_and_wait(0, [] { throw std::runtime_error("boom"); }),
                      std::runtime_error);
    // And the stripe is still alive afterwards.
    std::atomic<bool> ran{false};
    pool.post_and_wait(0, [&ran] { ran = true; });
    BOOST_CHECK(ran.load());
}

BOOST_AUTO_TEST_CASE(a_throwing_task_does_not_take_its_stripes_thread_with_it) {
    // Every group on the stripe would stop being driven — a silent, total
    // failure for whichever groups happened to hash there.
    striped_serial_executor pool{1};
    BOOST_REQUIRE(pool.post(0, [] { throw std::runtime_error("boom"); }));
    std::atomic<int> after{0};
    pool.post_and_wait(0, [&after] { after.fetch_add(1); });
    BOOST_CHECK_EQUAL(after.load(), 1);
}

BOOST_AUTO_TEST_CASE(post_and_wait_from_inside_its_own_stripe_is_diagnosed_not_hung) {
    // A deadlock here would be indistinguishable from a stuck node, so it is
    // reported as an error instead.
    striped_serial_executor pool{2};
    std::exception_ptr captured;
    pool.post_and_wait(0, [&pool, &captured] {
        try {
            pool.post_and_wait(0, [] {});
        } catch (...) {
            captured = std::current_exception();
        }
    });
    BOOST_REQUIRE(captured != nullptr);
    BOOST_CHECK_THROW(std::rethrow_exception(captured), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_task_may_post_to_its_own_stripe_without_deadlocking) {
    // Fire-and-forget re-entry is legal and is how a continuation schedules a
    // follow-up; only the *waiting* form is refused.
    striped_serial_executor pool{2};
    std::atomic<int> ran{0};
    pool.post(0, [&pool, &ran] {
        ran.fetch_add(1);
        pool.post(0, [&ran] { ran.fetch_add(1); });
    });
    pool.post_and_wait(0, [] {});
    // The inner post may land after our barrier, so drain once more.
    pool.post_and_wait(0, [] {});
    BOOST_CHECK_EQUAL(ran.load(), 2);
}

BOOST_AUTO_TEST_CASE(stop_runs_queued_work_and_joins_every_thread) {
    // Discarding queued work would leak whatever a teardown task was going to
    // release; the queue is short by construction because the tick waits.
    std::atomic<int> ran{0};
    {
        striped_serial_executor pool{2};
        for (int i = 0; i < 20; ++i) {
            pool.post(i % 2, [&ran] { ran.fetch_add(1); });
        }
        pool.stop();
        BOOST_CHECK(pool.stopped());
        // Post after stop is refused rather than silently dropped.
        BOOST_CHECK(!pool.post(0, [&ran] { ran.fetch_add(100); }));
    }
    BOOST_CHECK_EQUAL(ran.load(), 20);
}

BOOST_AUTO_TEST_CASE(stop_is_idempotent_and_the_destructor_stops_too) {
    striped_serial_executor pool{2};
    pool.stop();
    pool.stop();
    BOOST_CHECK(pool.stopped());
}

BOOST_AUTO_TEST_CASE(a_pool_thread_knows_its_own_stripe_and_an_outside_thread_does_not) {
    striped_serial_executor pool{3};
    BOOST_CHECK_EQUAL(striped_serial_executor::current_stripe(), striped_serial_executor::npos);
    for (std::size_t i = 0; i < 3; ++i) {
        std::size_t observed = striped_serial_executor::npos;
        pool.post_and_wait(i,
                           [&observed] { observed = striped_serial_executor::current_stripe(); });
        BOOST_CHECK_EQUAL(observed, i);
    }
}

BOOST_AUTO_TEST_SUITE_END()
