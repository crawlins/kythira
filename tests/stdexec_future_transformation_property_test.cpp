// **Feature: stdexec-future-backend, Property 12: Continuation and
// Transformation Fidelity (part 1 — thenValue/thenTry/thenError)**
// For any Future<T> and any pure-value/pure-error continuation, or any
// continuation that itself returns a Future<U>, thenValue/thenTry/thenError
// should apply the continuation exactly once, propagate its result (with
// automatic flattening for Future-returning continuations), and leave
// unrelated completions (value passing through thenError, error passing
// through thenValue) unchanged.
// **Validates: Requirements 9.1, 9.2, 9.3**
#define BOOST_TEST_MODULE stdexec_future_transformation_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_stdexec.hpp>

#include <atomic>
#include <stdexcept>
#include <string>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 200;
}

BOOST_AUTO_TEST_SUITE(stdexec_future_transformation_tests)

// thenValue (non-flattening): applied exactly once, transforms the value.
BOOST_AUTO_TEST_CASE(property_then_value_transforms_value, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::atomic<int> calls{0};
        auto future = FutureFactory::makeFuture(i).thenValue([&calls](int v) {
            calls.fetch_add(1);
            return v * 2;
        });
        BOOST_CHECK_EQUAL(std::move(future).get(), i * 2);
        BOOST_CHECK_EQUAL(calls.load(), 1);
    }
}

// thenValue does not run on an errored future — the error passes through.
BOOST_AUTO_TEST_CASE(then_value_is_skipped_on_error, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future = FutureFactory::makeExceptionalFuture<int>(
                      std::make_exception_ptr(std::runtime_error("boom")))
                      .thenValue([&calls](int v) {
                          calls.fetch_add(1);
                          return v;
                      });
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(calls.load(), 0);
}

// thenValue automatic flattening — callback returns a Future<U>, result is
// Future<U> not Future<Future<U>>.
BOOST_AUTO_TEST_CASE(property_then_value_flattens_future_returning_callback,
                     *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto future = FutureFactory::makeFuture(i).thenValue(
            [](int v) { return FutureFactory::makeFuture(v + 1); });
        static_assert(std::is_same_v<decltype(future), Future<int>>);
        BOOST_CHECK_EQUAL(std::move(future).get(), i + 1);
    }
}

// thenValue void handling.
BOOST_AUTO_TEST_CASE(then_value_void_future_chains, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future = FutureFactory::makeFuture().thenValue([&calls] { calls.fetch_add(1); });
    std::move(future).get();
    BOOST_CHECK_EQUAL(calls.load(), 1);
}

// thenTry: invoked exactly once regardless of value or error completion.
BOOST_AUTO_TEST_CASE(property_then_try_invoked_once_on_value, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::atomic<int> calls{0};
        auto future = FutureFactory::makeFuture(i).thenTry([&calls](Try<int> t) {
            calls.fetch_add(1);
            return t.value() * 10;
        });
        BOOST_CHECK_EQUAL(std::move(future).get(), i * 10);
        BOOST_CHECK_EQUAL(calls.load(), 1);
    }
}

BOOST_AUTO_TEST_CASE(property_then_try_invoked_once_on_error, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        std::atomic<int> calls{0};
        auto future = FutureFactory::makeExceptionalFuture<int>(
                          std::make_exception_ptr(std::runtime_error("e" + std::to_string(i))))
                          .thenTry([&calls](Try<int> t) {
                              calls.fetch_add(1);
                              return t.hasException() ? -1 : t.value();
                          });
        BOOST_CHECK_EQUAL(std::move(future).get(), -1);
        BOOST_CHECK_EQUAL(calls.load(), 1);
    }
}

// thenTry automatic flattening.
BOOST_AUTO_TEST_CASE(then_try_flattens_future_returning_callback, *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeFuture(5).thenTry(
        [](Try<int> t) { return FutureFactory::makeFuture(t.value() + 100); });
    static_assert(std::is_same_v<decltype(future), Future<int>>);
    BOOST_CHECK_EQUAL(std::move(future).get(), 105);
}

BOOST_AUTO_TEST_CASE(then_try_void_future, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future = FutureFactory::makeFuture().thenTry([&calls](Try<void> t) {
        calls.fetch_add(1);
        BOOST_CHECK(t.hasValue());
    });
    std::move(future).get();
    BOOST_CHECK_EQUAL(calls.load(), 1);
}

// thenError: transforms only the error path; a successful value passes
// through unchanged and the callback is never invoked.
BOOST_AUTO_TEST_CASE(then_error_is_skipped_on_success, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future = FutureFactory::makeFuture(7).thenError([&calls](std::exception_ptr) {
        calls.fetch_add(1);
        return -1;
    });
    BOOST_CHECK_EQUAL(std::move(future).get(), 7);
    BOOST_CHECK_EQUAL(calls.load(), 0);
}

BOOST_AUTO_TEST_CASE(property_then_error_recovers_with_value, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto future = FutureFactory::makeExceptionalFuture<int>(
                          std::make_exception_ptr(std::runtime_error("x")))
                          .thenError([i](std::exception_ptr) { return i; });
        BOOST_CHECK_EQUAL(std::move(future).get(), i);
    }
}

// thenError automatic flattening.
BOOST_AUTO_TEST_CASE(then_error_flattens_future_returning_callback,
                     *boost::unit_test::timeout(10)) {
    auto future =
        FutureFactory::makeExceptionalFuture<int>(std::make_exception_ptr(std::runtime_error("x")))
            .thenError([](std::exception_ptr) { return FutureFactory::makeFuture(42); });
    static_assert(std::is_same_v<decltype(future), Future<int>>);
    BOOST_CHECK_EQUAL(std::move(future).get(), 42);
}

BOOST_AUTO_TEST_CASE(then_error_void_future, *boost::unit_test::timeout(10)) {
    std::atomic<int> calls{0};
    auto future =
        FutureFactory::makeExceptionalFuture<void>(std::make_exception_ptr(std::runtime_error("x")))
            .thenError([&calls](std::exception_ptr) { calls.fetch_add(1); });
    std::move(future).get();
    BOOST_CHECK_EQUAL(calls.load(), 1);
}

// Chaining multiple continuations in sequence.
BOOST_AUTO_TEST_CASE(property_chained_continuations, *boost::unit_test::timeout(60)) {
    for (int i = 0; i < property_test_iterations; ++i) {
        auto future = FutureFactory::makeFuture(i)
                          .thenValue([](int v) { return v + 1; })
                          .thenValue([](int v) { return v * 2; })
                          .thenTry([](Try<int> t) { return t.value() - 3; });
        BOOST_CHECK_EQUAL(std::move(future).get(), (i + 1) * 2 - 3);
    }
}

// A throwing continuation propagates as the future's exception.
BOOST_AUTO_TEST_CASE(then_value_callback_throwing_propagates_as_exception,
                     *boost::unit_test::timeout(10)) {
    auto future = FutureFactory::makeFuture(1).thenValue(
        [](int) -> int { throw std::runtime_error("callback threw"); });
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
