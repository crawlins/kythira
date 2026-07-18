// **Feature: stdexec-future-backend**
// single_shot_channel<T> is the highest-risk new primitive in this spec —
// it bridges Folly's push-style Promise::setValue (fulfilled from
// arbitrary external code, on any thread, at an arbitrary later time) onto
// stdexec's pull-style senders. This file gets its own dedicated,
// higher-iteration-count suite per design.md's Testing Strategy, covering
// every interleaving named there: fulfillment-before-connect,
// connect-before-fulfillment, concurrent fulfillment races, and
// destroy-before-fulfill.
#define BOOST_TEST_MODULE single_shot_channel_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(single_shot_channel_tests)

// **Property 7: Single-Shot Channel Exactly-Once Completion**
// *For any* interleaving of set_value/set_error calls (from any thread)
// and receiver connect/start calls, the channel should complete its
// receiver exactly once, with the value/error from whichever fulfillment
// call is accepted, and a second fulfillment attempt should throw without
// corrupting the first result.
// **Validates: Requirements 6.2, 6.3, 6.4**

BOOST_AUTO_TEST_CASE(property_fulfill_before_connect, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        single_shot_channel<int> ch;
        ch.set_value(i);
        auto [v] = stdexec::sync_wait(ch.get_sender()).value();
        BOOST_CHECK_EQUAL(v, i);
    }
}

BOOST_AUTO_TEST_CASE(property_connect_before_fulfill, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        single_shot_channel<int> ch;
        std::thread fulfiller([&ch, i] {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ch.set_value(i);
        });
        auto [v] = stdexec::sync_wait(ch.get_sender()).value();
        BOOST_CHECK_EQUAL(v, i);
        fulfiller.join();
    }
}

BOOST_AUTO_TEST_CASE(property_error_before_connect, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        single_shot_channel<int> ch;
        ch.set_error(std::make_exception_ptr(std::runtime_error("err" + std::to_string(i))));
        BOOST_CHECK_THROW(stdexec::sync_wait(ch.get_sender()), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(property_error_after_connect, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        single_shot_channel<int> ch;
        std::thread fulfiller([&ch, i] {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ch.set_error(std::make_exception_ptr(std::runtime_error("err" + std::to_string(i))));
        });
        BOOST_CHECK_THROW(stdexec::sync_wait(ch.get_sender()), std::runtime_error);
        fulfiller.join();
    }
}

// High-iteration stress test for the fulfill-vs-connect race window itself:
// start() and set_value() racing from two different threads, repeated many
// times to shake out any window where both paths could run concurrently
// without the atomic CAS correctly serializing them.
BOOST_AUTO_TEST_CASE(property_start_and_fulfill_race_stress, *boost::unit_test::timeout(60)) {
    constexpr int stress_iterations = 500;
    for (int i = 0; i < stress_iterations; ++i) {
        single_shot_channel<int> ch;
        std::atomic<bool> ready{false};
        int observed = -1;
        std::thread starter([&] {
            while (!ready.load(std::memory_order_acquire)) {
            }
            auto [v] = stdexec::sync_wait(ch.get_sender()).value();
            observed = v;
        });
        ready.store(true, std::memory_order_release);
        ch.set_value(i);
        starter.join();
        // Checked on the main thread, after join() — Boost.Test's
        // assertion/logging machinery isn't thread-safe, so BOOST_CHECK*
        // must never be called from a non-main thread (confirmed via
        // ThreadSanitizer flagging exactly this pattern as a data race
        // inside boost::unit_test::unit_test_log_t, unrelated to any
        // single_shot_channel code).
        BOOST_CHECK_EQUAL(observed, i);
    }
}

// Concurrent fulfillment attempts from multiple threads — only one should
// win, the rest should throw std::logic_error, and the channel's eventual
// result should be internally consistent (whichever value won, sync_wait
// observes exactly that one, never a torn/mixed result).
BOOST_AUTO_TEST_CASE(property_concurrent_fulfillment_exactly_one_wins,
                     *boost::unit_test::timeout(60)) {
    constexpr int contenders = 16;
    for (int iteration = 0; iteration < 50; ++iteration) {
        single_shot_channel<int> ch;
        std::atomic<int> successes{0};
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        threads.reserve(contenders);
        for (int t = 0; t < contenders; ++t) {
            threads.emplace_back([&, t] {
                try {
                    ch.set_value(t);
                    successes.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::logic_error&) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        BOOST_CHECK_EQUAL(successes.load(), 1);
        BOOST_CHECK_EQUAL(failures.load(), contenders - 1);

        // The winning value should be stable and observable exactly once —
        // calling sync_wait must not throw and must not hang.
        auto [winner] = stdexec::sync_wait(ch.get_sender()).value();
        BOOST_CHECK(winner >= 0 && winner < contenders);
    }
}

// **Property 8: Single-Shot Channel Broken-Promise Semantics**
// *For any* promise destroyed before fulfillment while its future is
// already connected and started, the operation should complete with an
// error, never hang.
// **Validates: Requirement 6.5**
//
// single_shot_channel itself has no "promise" concept (that's
// SemiPromise/Promise, Requirement 6.6) — this suite exercises the
// building block directly: try_set_error() is what a promise wrapper's
// destructor calls, and this confirms it correctly completes a
// still-waiting receiver rather than leaving it connected with no result.

BOOST_AUTO_TEST_CASE(property_try_set_error_completes_a_waiting_receiver,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        single_shot_channel<int> ch;
        bool fulfilled = false;
        std::thread completer([&ch, &fulfilled] {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            fulfilled = ch.try_set_error(std::make_exception_ptr(std::runtime_error("broken")));
        });
        BOOST_CHECK_THROW(stdexec::sync_wait(ch.get_sender()), std::runtime_error);
        completer.join();
        // See property_start_and_fulfill_race_stress's comment: checked
        // after join(), not inside the thread — Boost.Test's own
        // assertion machinery isn't thread-safe.
        BOOST_CHECK(fulfilled);
    }
}

BOOST_AUTO_TEST_CASE(try_set_error_on_already_fulfilled_channel_returns_false,
                     *boost::unit_test::timeout(10)) {
    single_shot_channel<int> ch;
    ch.set_value(1);
    bool ok = ch.try_set_error(std::make_exception_ptr(std::runtime_error("late")));
    BOOST_CHECK(!ok);
    // The original value must still be observable — a losing try_set_error
    // must not have corrupted it.
    auto [v] = stdexec::sync_wait(ch.get_sender()).value();
    BOOST_CHECK_EQUAL(v, 1);
}

BOOST_AUTO_TEST_CASE(double_fulfillment_throws_without_corrupting_first_result,
                     *boost::unit_test::timeout(10)) {
    single_shot_channel<int> ch;
    ch.set_value(42);
    BOOST_CHECK_THROW(ch.set_value(43), std::logic_error);
    BOOST_CHECK_THROW(ch.set_error(std::make_exception_ptr(std::runtime_error("x"))),
                      std::logic_error);
    auto [v] = stdexec::sync_wait(ch.get_sender()).value();
    BOOST_CHECK_EQUAL(v, 42);
}

BOOST_AUTO_TEST_CASE(void_channel_round_trips, *boost::unit_test::timeout(10)) {
    single_shot_channel<kythira::unit> ch;
    ch.set_value(kythira::unit{});
    BOOST_CHECK(ch.is_fulfilled());
    stdexec::sync_wait(ch.get_sender());
}

BOOST_AUTO_TEST_SUITE_END()
