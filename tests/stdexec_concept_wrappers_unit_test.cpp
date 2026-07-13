// **Feature: stdexec-future-backend** — plain (non-property) unit test
// mirroring folly_concept_wrappers_unit_test.cpp's structure: basic
// construction/access/behavior checks for Try/Promise/Future, one
// BOOST_AUTO_TEST_SUITE per wrapper type. Adapted to the stdexec
// backend's actual API surface — Future<T> has no direct value/exception
// constructor the way the Folly wrapper's kythira::Future(T)/
// kythira::Future(folly::exception_wrapper) convenience constructors do,
// so construction here goes through FutureFactory/Promise, matching how
// this backend's real call sites are expected to construct futures.
// **Validates: Requirements 5.4, 5.5, 6.6, 7.1, 7.2, 7.3, 7.4, 7.5, 8.1,
// 8.2, 8.3**
#define BOOST_TEST_MODULE stdexec_concept_wrappers_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/future_stdexec.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

using namespace kythira::stdexec_backend;

namespace {
constexpr int test_value = 42;
constexpr const char* test_string = "test_message";
constexpr auto test_timeout = std::chrono::milliseconds{100};
constexpr auto short_timeout = std::chrono::milliseconds{10};
}  // namespace

// ============================================================================
// Try Wrapper Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(stdexec_try_wrapper_tests)

BOOST_AUTO_TEST_CASE(try_default_constructor, *boost::unit_test::timeout(15)) {
    Try<int> t;
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK(!t.hasException());
}

BOOST_AUTO_TEST_CASE(try_value_constructor, *boost::unit_test::timeout(15)) {
    Try<int> t(test_value);
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK(!t.hasException());
    BOOST_CHECK_EQUAL(t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(try_exception_constructor, *boost::unit_test::timeout(15)) {
    auto ex = std::make_exception_ptr(std::runtime_error(test_string));
    Try<int> t(ex);
    BOOST_CHECK(!t.hasValue());
    BOOST_CHECK(t.hasException());
    BOOST_CHECK_THROW(t.value(), std::runtime_error);
    BOOST_CHECK(t.exception() != nullptr);
}

BOOST_AUTO_TEST_CASE(try_null_exception_ptr_throws, *boost::unit_test::timeout(15)) {
    BOOST_CHECK_THROW(Try<int>(std::exception_ptr{}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(try_const_value_access, *boost::unit_test::timeout(15)) {
    const Try<int> t(test_value);
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK_EQUAL(t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(try_string_type, *boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    Try<std::string> t(test_str);
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK_EQUAL(t.value(), test_str);
}

BOOST_AUTO_TEST_CASE(try_void_specialization, *boost::unit_test::timeout(15)) {
    Try<void> t;
    BOOST_CHECK(t.hasValue());
    BOOST_CHECK_NO_THROW(t.value());

    Try<void> t_err(std::make_exception_ptr(std::runtime_error(test_string)));
    BOOST_CHECK(t_err.hasException());
    BOOST_CHECK_THROW(t_err.value(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Promise/SemiPromise Wrapper Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(stdexec_promise_wrapper_tests)

BOOST_AUTO_TEST_CASE(promise_default_constructor_not_fulfilled, *boost::unit_test::timeout(15)) {
    Promise<int> p;
    BOOST_CHECK(!p.isFulfilled());
    p.setValue(test_value);  // avoid a broken_promise on destruction
}

BOOST_AUTO_TEST_CASE(promise_set_value_marks_fulfilled, *boost::unit_test::timeout(15)) {
    Promise<int> p;
    auto f = p.getFuture();
    p.setValue(test_value);
    BOOST_CHECK(p.isFulfilled());
    BOOST_CHECK_EQUAL(std::move(f).get(), test_value);
}

BOOST_AUTO_TEST_CASE(promise_set_exception_marks_fulfilled, *boost::unit_test::timeout(15)) {
    Promise<int> p;
    auto f = p.getFuture();
    p.setException(std::make_exception_ptr(std::runtime_error(test_string)));
    BOOST_CHECK(p.isFulfilled());
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(promise_get_semi_future_observes_the_same_channel,
                     *boost::unit_test::timeout(15)) {
    Promise<int> p;
    auto sf = p.getSemiFuture();
    p.setValue(test_value);
    BOOST_CHECK_EQUAL(std::move(sf).get(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Future Wrapper Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(stdexec_future_wrapper_tests)

BOOST_AUTO_TEST_CASE(future_from_unfulfilled_promise_not_ready, *boost::unit_test::timeout(15)) {
    Promise<int> p;
    auto f = p.getFuture();
    BOOST_CHECK(!f.isReady());
    p.setValue(test_value);
}

BOOST_AUTO_TEST_CASE(future_factory_value_is_immediately_ready, *boost::unit_test::timeout(15)) {
    auto f = FutureFactory::makeFuture(test_value);
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_factory_exceptional_via_exception_ptr, *boost::unit_test::timeout(15)) {
    auto ex_ptr = std::make_exception_ptr(std::runtime_error(test_string));
    auto f = FutureFactory::makeExceptionalFuture<int>(ex_ptr);
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(future_then_value_chaining, *boost::unit_test::timeout(30)) {
    auto f = FutureFactory::makeFuture(test_value);
    auto f2 = std::move(f).thenValue([](int val) { return val * 2; });
    BOOST_CHECK_EQUAL(std::move(f2).get(), test_value * 2);
}

BOOST_AUTO_TEST_CASE(future_then_value_void_return, *boost::unit_test::timeout(30)) {
    auto f = FutureFactory::makeFuture(test_value);
    bool callback_called = false;
    auto f2 = std::move(f).thenValue([&callback_called](int) { callback_called = true; });
    std::move(f2).get();  // Should not throw
    BOOST_CHECK(callback_called);
}

BOOST_AUTO_TEST_CASE(future_then_error_handling, *boost::unit_test::timeout(30)) {
    auto ex_ptr = std::make_exception_ptr(std::runtime_error(test_string));
    auto f = FutureFactory::makeExceptionalFuture<int>(ex_ptr);
    auto f2 = std::move(f).thenError([](std::exception_ptr) { return test_value; });
    BOOST_CHECK_EQUAL(std::move(f2).get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_wait_timeout, *boost::unit_test::timeout(60)) {
    Promise<int> p;
    auto f = p.getFuture();
    BOOST_CHECK(!f.isReady());
    BOOST_CHECK(!f.wait(short_timeout));
    p.setValue(test_value);
    BOOST_CHECK(f.wait(test_timeout));
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_string_type, *boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    auto f = FutureFactory::makeFuture(test_str);
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), test_str);
}

BOOST_AUTO_TEST_CASE(future_move_semantics, *boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    auto f = FutureFactory::makeFuture(std::move(test_str));
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), test_string);
}

BOOST_AUTO_TEST_CASE(future_ensure_runs_on_success_and_error, *boost::unit_test::timeout(15)) {
    int cleanup_calls = 0;
    auto f1 = FutureFactory::makeFuture(test_value).ensure([&cleanup_calls] { ++cleanup_calls; });
    std::move(f1).get();
    BOOST_CHECK_EQUAL(cleanup_calls, 1);

    auto f2 = FutureFactory::makeExceptionalFuture<int>(
                  std::make_exception_ptr(std::runtime_error(test_string)))
                  .ensure([&cleanup_calls] { ++cleanup_calls; });
    BOOST_CHECK_THROW(std::move(f2).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(cleanup_calls, 2);
}

BOOST_AUTO_TEST_SUITE_END()
