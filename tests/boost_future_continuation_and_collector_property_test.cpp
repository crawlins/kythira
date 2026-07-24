// **Feature: boost-future-backend, Property 6: thenValue/thenError Chaining**
// **Feature: boost-future-backend, Property 7: Future Flattening**
// **Feature: boost-future-backend, Property 8: delay/within Timing**
// **Feature: boost-future-backend, Property 10: FutureCollector Semantics**
//
// Converts the proven scenarios from the boost-future-backend spike's
// runtime_test.cpp (20/20 passing against real vendored Boost headers) into
// the project's Boost.Test property-test convention. Covers continuation
// chaining (value and exception paths), flattening a callback that itself
// returns a Future<U> (including the U=void case, which required the
// extract_boost_future()+if constexpr fix documented in spike-notes.md),
// ensure(), delay/within timing behavior, and every FutureCollector
// operation (collectAll, collectAny, collectAnyWithoutException, collectN).
//
// Validates: Requirements 5.1-5.6, 6.1-6.5, 7.1-7.5, 9.1-9.6 (.kiro/specs/boost-future-backend/)
#define BOOST_TEST_MODULE boost_future_continuation_and_collector_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 60

#include <raft/future_boost.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kythira::boost_backend;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(boost_future_continuation_and_collector_tests)

// -- thenValue / thenError chaining -----------------------------------

BOOST_AUTO_TEST_CASE(then_value_transforms_result, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(10).thenValue([](int v) { return v * 2; });
    BOOST_CHECK_EQUAL(std::move(f).get(), 20);
}

BOOST_AUTO_TEST_CASE(then_value_chain_multiple, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(1)
                 .thenValue([](int v) { return v + 1; })
                 .thenValue([](int v) { return v + 1; })
                 .thenValue([](int v) { return v + 1; });
    BOOST_CHECK_EQUAL(std::move(f).get(), 4);
}

BOOST_AUTO_TEST_CASE(then_error_recovers_from_exception, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeExceptionalFuture<int>(
                 std::make_exception_ptr(std::runtime_error("boom")))
                 .thenError([](std::exception_ptr) { return 99; });
    BOOST_CHECK_EQUAL(std::move(f).get(), 99);
}

BOOST_AUTO_TEST_CASE(then_error_not_invoked_on_success, *boost::unit_test::timeout(10)) {
    bool error_handler_called = false;
    auto f = FutureFactory::makeFuture(5).thenError([&](std::exception_ptr) {
        error_handler_called = true;
        return -1;
    });
    BOOST_CHECK_EQUAL(std::move(f).get(), 5);
    BOOST_CHECK(!error_handler_called);
}

BOOST_AUTO_TEST_CASE(exception_propagates_through_then_value, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeExceptionalFuture<int>(
                 std::make_exception_ptr(std::runtime_error("propagate me")))
                 .thenValue([](int v) { return v * 2; });
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(void_future_then_value_chain, *boost::unit_test::timeout(10)) {
    int side_effect = 0;
    auto f = FutureFactory::makeReadyFuture().thenValue([&](kythira::unit) { side_effect = 1; });
    std::move(f).get();
    BOOST_CHECK_EQUAL(side_effect, 1);
}

BOOST_AUTO_TEST_CASE(ensure_runs_on_success, *boost::unit_test::timeout(10)) {
    bool ran = false;
    auto f = FutureFactory::makeFuture(3).ensure([&] { ran = true; });
    BOOST_CHECK_EQUAL(std::move(f).get(), 3);
    BOOST_CHECK(ran);
}

BOOST_AUTO_TEST_CASE(ensure_runs_on_exception, *boost::unit_test::timeout(10)) {
    bool ran = false;
    auto f =
        FutureFactory::makeExceptionalFuture<int>(std::make_exception_ptr(std::runtime_error("x")))
            .ensure([&] { ran = true; });
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
    BOOST_CHECK(ran);
}

// -- Flattening: a thenValue callback returning a Future<U> -----------

BOOST_AUTO_TEST_CASE(then_value_flattens_returned_future, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(2).thenValue(
        [](int v) { return FutureFactory::makeFuture(v * 10); });
    BOOST_CHECK_EQUAL(std::move(f).get(), 20);
}

// Property 7 specifically: the U=void flattening case, which required
// extract_boost_future().then() + if constexpr instead of chaining
// .thenValue()/.thenError() directly on the inner future (ill-formed for
// a void lambda parameter) — see spike-notes.md finding on this. Uses a
// Promise<void>-backed Future<void> (not makeReadyFuture(), which returns
// Future<kythira::unit> rather than the Future<void> specialization) so
// this actually exercises the U=void branch, not the general-T one.
BOOST_AUTO_TEST_CASE(then_value_flattens_returned_void_future, *boost::unit_test::timeout(10)) {
    bool inner_ran = false;
    auto f = FutureFactory::makeFuture(1).thenValue([&](int) {
        inner_ran = true;
        Promise<void> inner_promise;
        Future<void> inner_future = inner_promise.getFuture();
        inner_promise.setValue(kythira::unit{});
        return inner_future;
    });
    std::move(f).get();
    BOOST_CHECK(inner_ran);
}

BOOST_AUTO_TEST_CASE(then_value_flattens_exceptional_inner_future, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(1).thenValue([](int) {
        return FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("inner failure")));
    });
    BOOST_CHECK_THROW(std::move(f).get(), std::runtime_error);
}

// -- delay / within timing ---------------------------------------------

BOOST_AUTO_TEST_CASE(delay_defers_readiness, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(1).delay(50ms);
    BOOST_CHECK(!f.isReady());
    BOOST_CHECK_EQUAL(std::move(f).get(), 1);
}

BOOST_AUTO_TEST_CASE(within_completes_before_timeout, *boost::unit_test::timeout(10)) {
    auto f = FutureFactory::makeFuture(7).within(1s);
    BOOST_CHECK_EQUAL(std::move(f).get(), 7);
}

BOOST_AUTO_TEST_CASE(within_times_out_on_slow_future, *boost::unit_test::timeout(10)) {
    Promise<int> p;
    auto f = p.getFuture().within(50ms);
    BOOST_CHECK_THROW(std::move(f).get(), std::exception);
}

// -- FutureCollector -----------------------------------------------------

BOOST_AUTO_TEST_CASE(collect_all_returns_every_value_in_order, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeFuture(1));
    futures.push_back(FutureFactory::makeFuture(2));
    futures.push_back(FutureFactory::makeFuture(3));
    auto collected = FutureCollector::collectAll(std::move(futures));
    auto results = std::move(collected).get();
    BOOST_REQUIRE_EQUAL(results.size(), 3u);
    BOOST_CHECK_EQUAL(results[0].value(), 1);
    BOOST_CHECK_EQUAL(results[1].value(), 2);
    BOOST_CHECK_EQUAL(results[2].value(), 3);
}

BOOST_AUTO_TEST_CASE(collect_all_captures_per_future_exceptions, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeFuture(1));
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("bad"))));
    auto collected = FutureCollector::collectAll(std::move(futures));
    auto results = std::move(collected).get();
    BOOST_REQUIRE_EQUAL(results.size(), 2u);
    BOOST_CHECK(results[0].hasValue());
    BOOST_CHECK(results[1].hasException());
}

BOOST_AUTO_TEST_CASE(collect_any_returns_first_ready, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    Promise<int> slow_promise;
    futures.push_back(slow_promise.getFuture());
    futures.push_back(FutureFactory::makeFuture(42));
    auto result = FutureCollector::collectAny(std::move(futures));
    auto [index, value] = std::move(result).get();
    BOOST_CHECK_EQUAL(index, 1u);
    BOOST_CHECK_EQUAL(value.value(), 42);
}

BOOST_AUTO_TEST_CASE(collect_any_without_exception_skips_failures, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("first fails"))));
    futures.push_back(FutureFactory::makeFuture(55));
    auto result = FutureCollector::collectAnyWithoutException(std::move(futures));
    auto [index, value] = std::move(result).get();
    BOOST_CHECK_EQUAL(index, 1u);
    BOOST_CHECK_EQUAL(value, 55);
}

BOOST_AUTO_TEST_CASE(collect_any_without_exception_throws_when_all_fail,
                     *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("fail 1"))));
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("fail 2"))));
    auto result = FutureCollector::collectAnyWithoutException(std::move(futures));
    BOOST_CHECK_THROW(std::move(result).get(), std::exception);
}

BOOST_AUTO_TEST_CASE(collect_n_returns_first_n_successes, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeFuture(1));
    futures.push_back(FutureFactory::makeFuture(2));
    futures.push_back(FutureFactory::makeFuture(3));
    auto collected = FutureCollector::collectN(std::move(futures), 2);
    auto results = std::move(collected).get();
    BOOST_CHECK_EQUAL(results.size(), 2u);
}

// collectN matches Folly's collectN semantics exactly: it collects the
// first N futures to complete regardless of success/failure (unlike
// collectAnyWithoutException, it does not retry past a failure) - so a
// failed future among the first N to settle shows up as a failed Try in
// the result, not skipped.
BOOST_AUTO_TEST_CASE(collect_n_includes_failures_among_first_n_to_complete,
                     *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("fails"))));
    futures.push_back(FutureFactory::makeFuture(10));
    futures.push_back(FutureFactory::makeFuture(20));
    auto collected = FutureCollector::collectN(std::move(futures), 2);
    auto results = std::move(collected).get();
    BOOST_CHECK_EQUAL(results.size(), 2u);
    bool saw_failure = false;
    for (const auto& [index, value] : results) {
        if (value.hasException()) {
            saw_failure = true;
        }
    }
    BOOST_CHECK(saw_failure);
}

BOOST_AUTO_TEST_SUITE_END()
