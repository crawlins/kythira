// **Feature: stdexec-future-backend, Property 12: Continuation and
// Transformation Fidelity (part 2 — via/delay/within/ensure)**
// For any Future<T>, via(scheduler) should run the continuation on the
// requested scheduler without changing the completion's value/error;
// delay(duration) should shift completion later without blocking the
// calling thread for the duration; within(timeout) should fail with
// future_timeout if the timeout elapses first and otherwise be
// transparent; ensure(func) should run func exactly once on both the
// value and error paths without altering the original completion.
// **Validates: Requirements 9.4, 9.5, 9.6, 12.4**
#define BOOST_TEST_MODULE stdexec_future_scheduling_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <exec/single_thread_context.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 100;
}

BOOST_AUTO_TEST_SUITE(stdexec_future_scheduling_tests)

// via(scheduler): the continuation observes the value unchanged, and runs on
// the requested scheduler's thread (distinct from the calling thread).
BOOST_AUTO_TEST_CASE(property_via_preserves_value_and_runs_on_scheduler,
                     *boost::unit_test::timeout(60)) {
    exec::single_thread_context ctx;
    scheduler_handle handle(ctx.get_scheduler());
    auto caller_id = std::this_thread::get_id();
    for (int i = 0; i < property_test_iterations; ++i) {
        std::thread::id observed_id{};
        auto future = FutureFactory::makeFuture(i).via(&handle).thenValue([&observed_id](int v) {
            observed_id = std::this_thread::get_id();
            return v;
        });
        BOOST_CHECK_EQUAL(std::move(future).get(), i);
        BOOST_CHECK(observed_id != caller_id);
    }
}

// via(scheduler) on an errored future preserves the error unchanged.
BOOST_AUTO_TEST_CASE(via_preserves_error, *boost::unit_test::timeout(10)) {
    exec::single_thread_context ctx;
    scheduler_handle handle(ctx.get_scheduler());
    auto future = FutureFactory::makeExceptionalFuture<int>(
                      std::make_exception_ptr(std::runtime_error("via-err")))
                      .via(&handle);
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
}

// via(void*) / via(scheduler_handle&) concept-compliance overloads behave
// the same as via(scheduler_handle*).
BOOST_AUTO_TEST_CASE(via_reference_and_void_star_overloads_agree, *boost::unit_test::timeout(10)) {
    exec::single_thread_context ctx;
    scheduler_handle handle(ctx.get_scheduler());
    {
        auto future = FutureFactory::makeFuture(1).via(handle);
        BOOST_CHECK_EQUAL(std::move(future).get(), 1);
    }
    {
        void* raw = static_cast<void*>(&handle);
        auto future = FutureFactory::makeFuture(2).via(raw);
        BOOST_CHECK_EQUAL(std::move(future).get(), 2);
    }
}

// delay(): completes with the original value, and does not block the
// calling thread for the delay duration (the delay happens on the timed
// context's own background thread).
BOOST_AUTO_TEST_CASE(delay_does_not_block_calling_thread, *boost::unit_test::timeout(10)) {
    auto start = std::chrono::steady_clock::now();
    auto future = FutureFactory::makeFuture(9).delay(std::chrono::milliseconds(200));
    auto creation_elapsed = std::chrono::steady_clock::now() - start;
    // Creating the delayed future must return promptly; the wait happens
    // inside get(), not inside delay() itself.
    BOOST_CHECK(creation_elapsed < std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(std::move(future).get(), 9);
    auto total_elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK(total_elapsed >= std::chrono::milliseconds(150));
}

BOOST_AUTO_TEST_CASE(delay_preserves_error, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeExceptionalFuture<int>(
                      std::make_exception_ptr(std::runtime_error("delay-err")))
                      .delay(std::chrono::milliseconds(20));
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
}

// within(): a future that completes before the timeout is transparent.
BOOST_AUTO_TEST_CASE(within_transparent_when_original_completes_first,
                     *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeFuture(3).within(std::chrono::milliseconds(500));
    BOOST_CHECK_EQUAL(std::move(future).get(), 3);
}

// within(): a slow future is superseded by future_timeout once the timeout
// elapses, without blocking the calling thread beyond the timeout.
BOOST_AUTO_TEST_CASE(within_times_out_on_slow_future, *boost::unit_test::timeout(10)) {
    Promise<int> promise;
    auto future = promise.getFuture().within(std::chrono::milliseconds(50));
    auto start = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(std::move(future).get(), future_timeout);
    auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK(elapsed < std::chrono::milliseconds(500));
    // The promise is never fulfilled here; its destructor's broken-promise
    // semantics apply independently of the timeout outcome.
    promise.setValue(0);
}

// ensure(): runs func exactly once on the value path, without altering the
// completion.
BOOST_AUTO_TEST_CASE(property_ensure_runs_once_on_value_path, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::atomic<int> calls{0};
        auto future = FutureFactory::makeFuture(i).ensure([&calls] { calls.fetch_add(1); });
        BOOST_CHECK_EQUAL(std::move(future).get(), i);
        BOOST_CHECK_EQUAL(calls.load(), 1);
    }
}

// ensure(): runs func exactly once on the error path, without altering the
// exception.
BOOST_AUTO_TEST_CASE(property_ensure_runs_once_on_error_path, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::atomic<int> calls{0};
        auto future = FutureFactory::makeExceptionalFuture<int>(
                          std::make_exception_ptr(std::runtime_error("e" + std::to_string(i))))
                          .ensure([&calls] { calls.fetch_add(1); });
        BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
        BOOST_CHECK_EQUAL(calls.load(), 1);
    }
}

BOOST_AUTO_TEST_CASE(ensure_void_future, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future = FutureFactory::makeFuture().ensure([&calls] { calls.fetch_add(1); });
    std::move(future).get();
    BOOST_CHECK_EQUAL(calls.load(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
