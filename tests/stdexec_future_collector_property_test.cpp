// **Feature: stdexec-future-backend, Property 13: Collective Operation
// Fidelity**
// For any vector of Future<T>, FutureCollector::collectAll should preserve
// input ordering and aggregate every result (value or error) without
// cancelling siblings on individual failure; collectAny/
// collectAnyWithoutException should return exactly one first-completed
// (respectively first-successful) result tagged with its original index;
// collectN should return exactly the first N completions tagged with their
// original indices, still running (and discarding) the remainder rather
// than cancelling them.
// **Validates: Requirements 10.1, 10.2, 10.3, 10.4, 12.4**
#define BOOST_TEST_MODULE stdexec_future_collector_property_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 90

#include <raft/future_stdexec.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace kythira::stdexec_backend;

namespace {
constexpr int property_test_iterations = 50;
}

BOOST_AUTO_TEST_SUITE(stdexec_future_collector_tests)

BOOST_AUTO_TEST_CASE(static_assert_concept_compliance, *boost::unit_test::timeout(10)) {
    static_assert(kythira::future_collector<FutureCollector, Future<int>>);
    BOOST_TEST(true);
}

// collectAll preserves input ordering and aggregates both values and
// errors without cancelling siblings.
BOOST_AUTO_TEST_CASE(property_collect_all_preserves_order_and_aggregates,
                     *boost::unit_test::timeout(60)) {
    for (int iter = 0; iter < property_test_iterations; ++iter) {
        std::vector<Future<int>> futures;
        constexpr int n = 8;
        for (int i = 0; i < n; ++i) {
            if (i % 3 == 0) {
                futures.push_back(FutureFactory::makeExceptionalFuture<int>(
                    std::make_exception_ptr(std::runtime_error("err" + std::to_string(i)))));
            } else {
                futures.push_back(FutureFactory::makeFuture(i * 10));
            }
        }
        auto results = std::move(FutureCollector::collectAll(std::move(futures))).get();
        BOOST_REQUIRE_EQUAL(results.size(), static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            if (i % 3 == 0) {
                BOOST_CHECK(results[i].hasException());
            } else {
                BOOST_REQUIRE(results[i].hasValue());
                BOOST_CHECK_EQUAL(results[i].value(), i * 10);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(collect_all_empty_vector, *boost::unit_test::timeout(10)) {
    auto results = std::move(FutureCollector::collectAll(std::vector<Future<int>>{})).get();
    BOOST_CHECK(results.empty());
}

// collectAny returns exactly one (index, Try<T>) result — the first to
// complete, whether value or error.
BOOST_AUTO_TEST_CASE(property_collect_any_returns_first_completion,
                     *boost::unit_test::timeout(60)) {
    for (int iter = 0; iter < property_test_iterations; ++iter) {
        std::vector<Future<int>> futures;
        futures.push_back(FutureFactory::makeFuture(iter));
        Promise<int> slow_promise;
        futures.push_back(slow_promise.getFuture());
        auto [idx, result] = std::move(FutureCollector::collectAny(std::move(futures))).get();
        BOOST_CHECK_EQUAL(idx, 0u);
        BOOST_REQUIRE(result.hasValue());
        BOOST_CHECK_EQUAL(result.value(), iter);
        // Let the slow promise's future complete naturally in the
        // background (discarded) rather than leaking a broken_promise on
        // an unfulfilled promise going out of scope.
        slow_promise.setValue(-1);
    }
}

BOOST_AUTO_TEST_CASE(collect_any_rejects_empty_vector, *boost::unit_test::timeout(10)) {
    auto future = FutureCollector::collectAny(std::vector<Future<int>>{});
    BOOST_CHECK_THROW(std::move(future).get(), std::invalid_argument);
}

// collectAnyWithoutException skips failures and returns the first success;
// if every future fails, the aggregate is exceptional.
BOOST_AUTO_TEST_CASE(property_collect_any_without_exception_skips_failures,
                     *boost::unit_test::timeout(60)) {
    for (int iter = 0; iter < property_test_iterations; ++iter) {
        std::vector<Future<int>> futures;
        futures.push_back(FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("fail-a"))));
        futures.push_back(FutureFactory::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("fail-b"))));
        futures.push_back(FutureFactory::makeFuture(iter));
        auto [idx, value] =
            std::move(FutureCollector::collectAnyWithoutException(std::move(futures))).get();
        BOOST_CHECK_EQUAL(idx, 2u);
        BOOST_CHECK_EQUAL(value, iter);
    }
}

BOOST_AUTO_TEST_CASE(collect_any_without_exception_all_fail_is_exceptional,
                     *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("fail-a"))));
    futures.push_back(FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("fail-b"))));
    auto future = FutureCollector::collectAnyWithoutException(std::move(futures));
    BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(collect_any_without_exception_void, *boost::unit_test::timeout(10)) {
    std::vector<Future<void>> futures;
    futures.push_back(FutureFactory::makeExceptionalFuture<void>(
        std::make_exception_ptr(std::runtime_error("fail-a"))));
    futures.push_back(FutureFactory::makeFuture());
    auto idx = std::move(FutureCollector::collectAnyWithoutException(std::move(futures))).get();
    BOOST_CHECK_EQUAL(idx, 1u);
}

// collectN returns exactly the first N completions, tagged with original
// indices; remaining futures are still run (and discarded), not cancelled.
BOOST_AUTO_TEST_CASE(property_collect_n_returns_first_n, *boost::unit_test::timeout(60)) {
    for (int iter = 0; iter < property_test_iterations; ++iter) {
        std::vector<Future<int>> futures;
        constexpr std::size_t total = 5;
        constexpr std::size_t n = 3;
        for (std::size_t i = 0; i < total; ++i) {
            futures.push_back(FutureFactory::makeFuture(static_cast<int>(i)));
        }
        auto results = std::move(FutureCollector::collectN(std::move(futures), n)).get();
        BOOST_CHECK_EQUAL(results.size(), n);
    }
}

BOOST_AUTO_TEST_CASE(collect_n_zero_returns_immediately, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeFuture(1));
    auto results = std::move(FutureCollector::collectN(std::move(futures), 0)).get();
    BOOST_CHECK(results.empty());
}

BOOST_AUTO_TEST_CASE(collect_n_rejects_n_greater_than_size, *boost::unit_test::timeout(10)) {
    std::vector<Future<int>> futures;
    futures.push_back(FutureFactory::makeFuture(1));
    auto future = FutureCollector::collectN(std::move(futures), 5);
    BOOST_CHECK_THROW(std::move(future).get(), std::invalid_argument);
}

// Concurrent stress: many futures completing from different threads,
// collected via collectAll — no lost/duplicated results.
BOOST_AUTO_TEST_CASE(collect_all_concurrent_fulfillment, *boost::unit_test::timeout(30)) {
    constexpr std::size_t n = 32;
    std::vector<Future<int>> futures;
    std::vector<Promise<int>> promises(n);
    for (std::size_t i = 0; i < n; ++i) {
        futures.push_back(promises[i].getFuture());
    }
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < n; ++i) {
        threads.emplace_back([&promises, i] {
            std::this_thread::sleep_for(std::chrono::microseconds(100 * (i % 5)));
            promises[i].setValue(static_cast<int>(i));
        });
    }
    auto results = std::move(FutureCollector::collectAll(std::move(futures))).get();
    for (auto& thread : threads) thread.join();
    BOOST_REQUIRE_EQUAL(results.size(), n);
    for (std::size_t i = 0; i < n; ++i) {
        BOOST_REQUIRE(results[i].hasValue());
        BOOST_CHECK_EQUAL(results[i].value(), static_cast<int>(i));
    }
}

BOOST_AUTO_TEST_SUITE_END()
