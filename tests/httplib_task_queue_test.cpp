// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file httplib_task_queue_test.cpp
/// @brief kythira::net_bind::growing_task_queue starts workers only on demand,
///        never more than its bound, and runs every job it accepted.

#define BOOST_TEST_MODULE httplib_task_queue_test
#include <boost/test/unit_test.hpp>

#include <raft/httplib_task_queue.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

// Holds every job that enters it until opened.
struct gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open{false};
    std::atomic<int> waiting{0};

    auto wait() -> void {
        ++waiting;
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return open; });
    }
    auto release() -> void {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

auto wait_for(const std::atomic<int>& value, int expected) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (value.load() != expected) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(httplib_task_queue_tests)

BOOST_AUTO_TEST_CASE(zero_workers_is_rejected) {
    BOOST_CHECK_THROW(kythira::net_bind::growing_task_queue{0}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(no_worker_starts_before_a_job, *boost::unit_test::timeout(30)) {
    kythira::net_bind::growing_task_queue queue(4);
    BOOST_TEST(queue.worker_count() == 0U);
}

// Sequential jobs reuse the one worker rather than starting one each.
BOOST_AUTO_TEST_CASE(idle_worker_is_reused, *boost::unit_test::timeout(30)) {
    kythira::net_bind::growing_task_queue queue(4);
    std::atomic<int> done{0};
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(queue.enqueue([&] { ++done; }));
        BOOST_TEST(wait_for(done, i + 1));
        // Let the worker get back to waiting before the next job.
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_TEST(queue.worker_count() == 1U);
    queue.shutdown();
}

// Concurrent jobs start workers up to the bound and no further; jobs past it
// wait and still run.
BOOST_AUTO_TEST_CASE(workers_grow_to_the_bound, *boost::unit_test::timeout(30)) {
    kythira::net_bind::growing_task_queue queue(3);
    gate blocker;
    std::atomic<int> done{0};
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(queue.enqueue([&] {
            blocker.wait();
            ++done;
        }));
    }
    BOOST_TEST(wait_for(blocker.waiting, 3));
    BOOST_TEST(queue.worker_count() == 3U);
    BOOST_TEST(done.load() == 0);

    blocker.release();
    BOOST_TEST(wait_for(done, 5));
    BOOST_TEST(queue.worker_count() == 3U);
    queue.shutdown();
}

// shutdown() runs what was queued, then refuses new work.
BOOST_AUTO_TEST_CASE(shutdown_drains_then_refuses, *boost::unit_test::timeout(30)) {
    kythira::net_bind::growing_task_queue queue(1);
    std::atomic<int> done{0};
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(queue.enqueue([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            ++done;
        }));
    }
    queue.shutdown();
    BOOST_TEST(done.load() == 3);
    BOOST_TEST(!queue.enqueue([] {}));
    queue.shutdown();
}

BOOST_AUTO_TEST_SUITE_END()
