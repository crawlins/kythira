// **Feature: stdexec-future-backend, Property 9: stdexec Future Blocking Get
// Correctness**
// For any stdexec-backed Future<T>, get() should block only until the
// underlying sender completes and then return the value or rethrow the
// exception, matching Folly Future<T>::get() for equivalent completions.
// **Validates: Requirements 7.1, 7.2, 7.3**
#define BOOST_TEST_MODULE stdexec_future_blocking_get_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 100;
}

BOOST_AUTO_TEST_SUITE(stdexec_future_blocking_get_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::future<Future<int>, int>);
    static_assert(kythira::future<Future<void>, void>);
    static_assert(kythira::future<Future<std::string>, std::string>);
    static_assert(kythira::promise<Promise<int>, int>);
    static_assert(kythira::promise<Promise<void>, void>);
    BOOST_TEST(true);
}

// **Property 9**, value path: get() returns the value once the producing
// thread fulfills the promise, whether that happens before or after get()
// starts waiting.
BOOST_AUTO_TEST_CASE(property_get_returns_value_regardless_of_fulfillment_timing,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        Promise<int> promise;
        auto future = promise.getFuture();
        bool fulfill_first = (i % 2 == 0);
        if (fulfill_first) {
            promise.setValue(i);
            BOOST_CHECK_EQUAL(std::move(future).get(), i);
        } else {
            std::thread fulfiller([p = std::move(promise), i]() mutable {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                p.setValue(i);
            });
            BOOST_CHECK_EQUAL(std::move(future).get(), i);
            fulfiller.join();
        }
    }
}

// **Property 9**, error path: get() rethrows the exact exception the
// producer set, matching Folly Future<T>::get() semantics.
BOOST_AUTO_TEST_CASE(property_get_rethrows_the_producers_exception,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        Promise<int> promise;
        auto future = promise.getFuture();
        std::thread fulfiller([p = std::move(promise), i]() mutable {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            p.setException(std::make_exception_ptr(std::runtime_error("err-" + std::to_string(i))));
        });
        bool threw = false;
        try {
            std::move(future).get();
        } catch (const std::runtime_error& e) {
            threw = true;
            BOOST_CHECK_EQUAL(std::string(e.what()), "err-" + std::to_string(i));
        }
        BOOST_CHECK(threw);
        fulfiller.join();
    }
}

// get() should block only as long as necessary, not longer — a fast
// fulfillment should not incur any artificial delay.
BOOST_AUTO_TEST_CASE(get_does_not_add_artificial_delay, *boost::unit_test::timeout(10)) {
    Promise<int> promise;
    auto future = promise.getFuture();
    promise.setValue(1);
    auto start = std::chrono::steady_clock::now();
    auto result = std::move(future).get();
    auto elapsed = std::chrono::steady_clock::now() - start;
    BOOST_CHECK_EQUAL(result, 1);
    BOOST_CHECK(elapsed < std::chrono::milliseconds(100));
}

// isReady()/wait(timeout) correctness — Requirements 7.2/7.3.
BOOST_AUTO_TEST_CASE(is_ready_and_wait_report_status_without_blocking_indefinitely,
                     *boost::unit_test::timeout(10)) {
    Promise<int> promise;
    auto future = promise.getFuture();
    BOOST_CHECK(!future.isReady());
    BOOST_CHECK(!future.wait(std::chrono::milliseconds(10)));

    promise.setValue(42);
    BOOST_CHECK(future.isReady());
    BOOST_CHECK(future.wait(std::chrono::milliseconds(10)));

    // wait() having returned true must not have consumed the future — get()
    // still works afterward.
    BOOST_CHECK_EQUAL(std::move(future).get(), 42);
}

// A factory-created (already-ready) future is always isReady() immediately.
BOOST_AUTO_TEST_CASE(factory_created_future_is_always_ready, *boost::unit_test::timeout(10)) {
    Future<int> future(any_sender_t<int>(stdexec::just(7)));
    BOOST_CHECK(future.isReady());
    BOOST_CHECK(future.wait(std::chrono::milliseconds(0)));
    BOOST_CHECK_EQUAL(std::move(future).get(), 7);
}

// void future round trip.
BOOST_AUTO_TEST_CASE(void_future_get_and_wait, *boost::unit_test::timeout(10)) {
    Promise<void> promise;
    auto future = promise.getFuture();
    BOOST_CHECK(!future.wait(std::chrono::milliseconds(10)));
    promise.setValue();
    BOOST_CHECK(future.wait(std::chrono::milliseconds(10)));
    std::move(future).get();
}

BOOST_AUTO_TEST_SUITE_END()
