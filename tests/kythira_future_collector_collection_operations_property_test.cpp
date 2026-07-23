#define BOOST_TEST_MODULE KythiraFutureCollectorCollectionOperationsPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/future.hpp>
#include <raft/future_collector.hpp>
#include <concepts/future.hpp>
#include <exception>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <memory>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>
#include <folly/init/Init.h>

using namespace kythira;

// Test constants
namespace {
constexpr int test_value_base = 100;
constexpr const char* test_exception_msg = "collection test exception";
constexpr std::size_t property_test_iterations = 25;
constexpr std::size_t max_collection_size = 10;
constexpr std::chrono::milliseconds short_timeout{100};
constexpr std::chrono::milliseconds long_timeout{1000};
}

// collect_n_successes_with_timeout_test below is the first test in this file
// to call Future::within() on a not-yet-completed future (make_delayed_future
// et al.) - every existing test here only ever wraps already-fulfilled
// futures, for which within() apparently never touches Folly's global
// Timekeeper singleton. A real pending timeout does need it, and this file's
// Boost.Test-generated main() never calls folly::init(), so without this
// fixture the process aborts: "Singleton folly::Timekeeper ... requested
// before registrationComplete() call" - confirmed by reproducing it locally
// before adding this, matching the same fixture already used for the same
// reason in raft_concurrent_read_efficiency_property_test.cpp.
struct GlobalFixture {
    GlobalFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        folly::init(&argc, &argv_ptr);
    }
};

BOOST_GLOBAL_FIXTURE(GlobalFixture);

/**
 * **Feature: folly-concept-wrappers, Property 5: Collection Operations**
 *
 * Property: For any collection of futures, collection operations should return results according to
 * their specified strategy (all, any, first N) with proper ordering and timeout handling
 * **Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**
 */
BOOST_AUTO_TEST_CASE(kythira_future_collector_collection_operations_property_test,
                     *boost::unit_test::timeout(120)) {
    // Test 1: collectAll preserves order and waits for all futures
    {
        std::vector<Future<int>> futures;
        std::vector<int> expected_values;

        // Create futures with known values in specific order
        for (std::size_t i = 0; i < 5; ++i) {
            int value = test_value_base + static_cast<int>(i);
            expected_values.push_back(value);
            futures.push_back(FutureFactory::makeFuture(value));
        }

        auto all_result = FutureCollector::collectAll(std::move(futures));
        auto results = all_result.get();

        // Verify order preservation
        BOOST_CHECK_EQUAL(results.size(), expected_values.size());
        for (std::size_t i = 0; i < results.size(); ++i) {
            BOOST_CHECK(results[i].hasValue());
            BOOST_CHECK_EQUAL(results[i].value(), expected_values[i]);
        }

        BOOST_TEST_MESSAGE("collectAll preserves order correctly");
    }

    // Test 2: collectAll handles mixed success/failure scenarios
    {
        std::vector<Future<int>> futures;
        std::vector<bool> should_succeed = {true, false, true, false, true};

        for (std::size_t i = 0; i < should_succeed.size(); ++i) {
            if (should_succeed[i]) {
                int value = test_value_base + static_cast<int>(i);
                futures.push_back(FutureFactory::makeFuture(value));
            } else {
                std::string exception_msg =
                    std::string(test_exception_msg) + "_" + std::to_string(i);
                auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
                futures.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
            }
        }

        auto all_result = FutureCollector::collectAll(std::move(futures));
        auto results = all_result.get();

        // Verify that all results are present and in correct order
        BOOST_CHECK_EQUAL(results.size(), should_succeed.size());
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (should_succeed[i]) {
                BOOST_CHECK(results[i].hasValue());
                BOOST_CHECK_EQUAL(results[i].value(), test_value_base + static_cast<int>(i));
            } else {
                BOOST_CHECK(results[i].hasException());
            }
        }

        BOOST_TEST_MESSAGE("collectAll handles mixed success/failure correctly");
    }

    // Test 3: collectAny returns first completed future with correct index
    {
        std::vector<Future<int>> futures;

        // Create futures where we know which one will complete first
        futures.push_back(
            FutureFactory::makeFuture(test_value_base));  // This should complete first
        futures.push_back(
            FutureFactory::makeFuture(test_value_base + 1));  // This should also be ready
        futures.push_back(
            FutureFactory::makeFuture(test_value_base + 2));  // This should also be ready

        auto any_result = FutureCollector::collectAny(std::move(futures));
        auto result = any_result.get();

        std::size_t index = std::get<0>(result);
        Try<int> try_value = std::get<1>(result);

        // Since all futures are ready, any of them could be returned
        BOOST_CHECK(index < 3);
        BOOST_CHECK(try_value.hasValue());

        int expected_value = test_value_base + static_cast<int>(index);
        BOOST_CHECK_EQUAL(try_value.value(), expected_value);

        BOOST_TEST_MESSAGE("collectAny returns correct index and value");
    }

    // Test 4: collectAnyWithoutException returns first successful future
    {
        std::vector<Future<int>> futures;

        // Add some failed futures first
        for (std::size_t i = 0; i < 2; ++i) {
            std::string exception_msg = std::string(test_exception_msg) + "_" + std::to_string(i);
            auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
            futures.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
        }

        // Add successful futures
        for (std::size_t i = 0; i < 3; ++i) {
            int value = test_value_base + static_cast<int>(i);
            futures.push_back(FutureFactory::makeFuture(value));
        }

        auto any_success_result = FutureCollector::collectAnyWithoutException(std::move(futures));
        auto result = any_success_result.get();

        std::size_t index = std::get<0>(result);
        int value = std::get<1>(result);

        // Should return one of the successful futures (indices 2, 3, or 4)
        BOOST_CHECK(index >= 2 && index < 5);

        int expected_value = test_value_base + static_cast<int>(index - 2);
        BOOST_CHECK_EQUAL(value, expected_value);

        BOOST_TEST_MESSAGE("collectAnyWithoutException returns first successful future");
    }

    // Test 5: collectN returns exactly N futures with correct indices
    {
        std::vector<Future<int>> futures;

        // Create more futures than we'll collect
        for (std::size_t i = 0; i < 7; ++i) {
            int value = test_value_base + static_cast<int>(i);
            futures.push_back(FutureFactory::makeFuture(value));
        }

        std::size_t n = 3;
        auto n_result = FutureCollector::collectN(std::move(futures), n);
        auto results = n_result.get();

        // Should return exactly N results
        BOOST_CHECK_EQUAL(results.size(), n);

        // Verify that all returned results are valid and have correct values
        std::vector<std::size_t> returned_indices;
        for (const auto& result : results) {
            std::size_t index = std::get<0>(result);
            Try<int> try_value = std::get<1>(result);

            BOOST_CHECK(index < 7);  // Should be valid index
            BOOST_CHECK(try_value.hasValue());

            int expected_value = test_value_base + static_cast<int>(index);
            BOOST_CHECK_EQUAL(try_value.value(), expected_value);

            returned_indices.push_back(index);
        }

        // Verify no duplicate indices
        std::sort(returned_indices.begin(), returned_indices.end());
        auto unique_end = std::unique(returned_indices.begin(), returned_indices.end());
        BOOST_CHECK_EQUAL(std::distance(returned_indices.begin(), unique_end),
                          static_cast<long>(n));

        BOOST_TEST_MESSAGE("collectN returns exactly N futures with correct indices");
    }

    // Test 6: Property-based testing with random collections
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> size_dist(1, max_collection_size);
    std::uniform_real_distribution<double> success_rate_dist(0.3, 0.9);

    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        std::size_t collection_size = size_dist(gen);
        double success_rate = success_rate_dist(gen);

        // Create random collection of futures
        std::vector<Future<int>> futures;
        std::vector<bool> should_succeed;

        for (std::size_t i = 0; i < collection_size; ++i) {
            bool success = (static_cast<double>(gen()) / std::mt19937::max()) < success_rate;
            should_succeed.push_back(success);

            if (success) {
                int value = test_value_base + static_cast<int>(iteration * 100 + i);
                futures.push_back(FutureFactory::makeFuture(value));
            } else {
                std::string exception_msg =
                    "iteration_" + std::to_string(iteration) + "_index_" + std::to_string(i);
                auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
                futures.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
            }
        }

        // Test collectAll with random collection
        {
            std::vector<Future<int>> futures_copy;
            for (std::size_t i = 0; i < collection_size; ++i) {
                if (should_succeed[i]) {
                    int value = test_value_base + static_cast<int>(iteration * 100 + i);
                    futures_copy.push_back(FutureFactory::makeFuture(value));
                } else {
                    std::string exception_msg =
                        "iteration_" + std::to_string(iteration) + "_index_" + std::to_string(i);
                    auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
                    futures_copy.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
                }
            }

            auto all_result = FutureCollector::collectAll(std::move(futures_copy));
            auto results = all_result.get();

            // Verify order and completeness
            BOOST_CHECK_EQUAL(results.size(), collection_size);
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (should_succeed[i]) {
                    BOOST_CHECK(results[i].hasValue());
                    int expected_value = test_value_base + static_cast<int>(iteration * 100 + i);
                    BOOST_CHECK_EQUAL(results[i].value(), expected_value);
                } else {
                    BOOST_CHECK(results[i].hasException());
                }
            }
        }

        // Test collectAny with random collection
        {
            std::vector<Future<int>> futures_copy;
            for (std::size_t i = 0; i < collection_size; ++i) {
                if (should_succeed[i]) {
                    int value = test_value_base + static_cast<int>(iteration * 100 + i);
                    futures_copy.push_back(FutureFactory::makeFuture(value));
                } else {
                    std::string exception_msg =
                        "iteration_" + std::to_string(iteration) + "_index_" + std::to_string(i);
                    auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
                    futures_copy.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
                }
            }

            auto any_result = FutureCollector::collectAny(std::move(futures_copy));
            auto result = any_result.get();

            std::size_t index = std::get<0>(result);
            Try<int> try_value = std::get<1>(result);

            // Verify index is valid
            BOOST_CHECK(index < collection_size);

            // Verify result matches expectation for that index
            if (should_succeed[index]) {
                BOOST_CHECK(try_value.hasValue());
                int expected_value = test_value_base + static_cast<int>(iteration * 100 + index);
                BOOST_CHECK_EQUAL(try_value.value(), expected_value);
            } else {
                BOOST_CHECK(try_value.hasException());
            }
        }

        // Test collectN with random collection (if collection is large enough)
        if (collection_size >= 2) {
            std::size_t n = std::min(collection_size - 1, std::size_t(3));

            std::vector<Future<int>> futures_copy;
            for (std::size_t i = 0; i < collection_size; ++i) {
                if (should_succeed[i]) {
                    int value = test_value_base + static_cast<int>(iteration * 100 + i);
                    futures_copy.push_back(FutureFactory::makeFuture(value));
                } else {
                    std::string exception_msg =
                        "iteration_" + std::to_string(iteration) + "_index_" + std::to_string(i);
                    auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
                    futures_copy.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
                }
            }

            auto n_result = FutureCollector::collectN(std::move(futures_copy), n);
            auto results = n_result.get();

            // Verify exactly N results
            BOOST_CHECK_EQUAL(results.size(), n);

            // Verify all indices are valid and unique
            std::vector<std::size_t> indices;
            for (const auto& result : results) {
                std::size_t index = std::get<0>(result);
                Try<int> try_value = std::get<1>(result);

                BOOST_CHECK(index < collection_size);
                indices.push_back(index);

                // Verify result matches expectation for that index
                if (should_succeed[index]) {
                    BOOST_CHECK(try_value.hasValue());
                    int expected_value =
                        test_value_base + static_cast<int>(iteration * 100 + index);
                    BOOST_CHECK_EQUAL(try_value.value(), expected_value);
                } else {
                    BOOST_CHECK(try_value.hasException());
                }
            }

            // Verify no duplicate indices
            std::sort(indices.begin(), indices.end());
            auto unique_end = std::unique(indices.begin(), indices.end());
            BOOST_CHECK_EQUAL(std::distance(indices.begin(), unique_end), static_cast<long>(n));
        }
    }

    // Test 7: Timeout handling with collectAllWithTimeout and collectAnyWithTimeout
    {
        // Test collectAllWithTimeout with immediate futures (should not timeout)
        std::vector<Future<int>> immediate_futures;
        for (std::size_t i = 0; i < 3; ++i) {
            int value = test_value_base + static_cast<int>(i);
            immediate_futures.push_back(FutureFactory::makeFuture(value));
        }

        auto timeout_all_result =
            FutureCollector::collectAllWithTimeout(std::move(immediate_futures), long_timeout);
        auto timeout_results = timeout_all_result.get();

        BOOST_CHECK_EQUAL(timeout_results.size(), 3);
        for (std::size_t i = 0; i < timeout_results.size(); ++i) {
            BOOST_CHECK(timeout_results[i].hasValue());
            BOOST_CHECK_EQUAL(timeout_results[i].value(), test_value_base + static_cast<int>(i));
        }

        // Test collectAnyWithTimeout with immediate futures (should not timeout)
        std::vector<Future<int>> immediate_futures2;
        immediate_futures2.push_back(FutureFactory::makeFuture(test_value_base));

        auto timeout_any_result =
            FutureCollector::collectAnyWithTimeout(std::move(immediate_futures2), long_timeout);
        auto timeout_result = timeout_any_result.get();

        std::size_t index = std::get<0>(timeout_result);
        Try<int> try_value = std::get<1>(timeout_result);

        BOOST_CHECK_EQUAL(index, 0);
        BOOST_CHECK(try_value.hasValue());
        BOOST_CHECK_EQUAL(try_value.value(), test_value_base);

        BOOST_TEST_MESSAGE("Timeout handling works correctly with immediate futures");
    }

    // Test 8: Error handling edge cases
    {
        // Test collectAnyWithoutException with all failed futures
        std::vector<Future<int>> all_failed_futures;
        for (std::size_t i = 0; i < 3; ++i) {
            std::string exception_msg = "all_failed_" + std::to_string(i);
            auto ex = folly::exception_wrapper(std::runtime_error(exception_msg));
            all_failed_futures.push_back(FutureFactory::makeExceptionalFuture<int>(ex));
        }

        auto all_failed_result =
            FutureCollector::collectAnyWithoutException(std::move(all_failed_futures));

        // This should throw since no futures succeed
        BOOST_CHECK_THROW(all_failed_result.get(), std::exception);

        // Test collectN with n = collection_size (should return all)
        std::vector<Future<int>> exact_size_futures;
        for (std::size_t i = 0; i < 4; ++i) {
            int value = test_value_base + static_cast<int>(i);
            exact_size_futures.push_back(FutureFactory::makeFuture(value));
        }

        auto exact_n_result = FutureCollector::collectN(std::move(exact_size_futures), 4);
        auto exact_results = exact_n_result.get();

        BOOST_CHECK_EQUAL(exact_results.size(), 4);
        for (auto& exact_result : exact_results) {
            std::size_t index = std::get<0>(exact_result);
            Try<int> try_value = std::get<1>(exact_result);

            BOOST_CHECK(index < 4);
            BOOST_CHECK(try_value.hasValue());
        }

        BOOST_TEST_MESSAGE("Error handling edge cases work correctly");
    }

    // Test 9: Void future collections
    {
        // Test collectAll with void futures
        std::vector<Future<void>> void_futures;
        void_futures.push_back(FutureFactory::makeFuture());
        void_futures.push_back(FutureFactory::makeFuture());

        auto void_all_result = FutureCollector::collectAll(std::move(void_futures));
        auto void_results = void_all_result.get();

        BOOST_CHECK_EQUAL(void_results.size(), 2);
        BOOST_CHECK(void_results[0].hasValue());
        BOOST_CHECK(void_results[1].hasValue());

        // Test collectAny with void futures
        std::vector<Future<void>> void_futures2;
        void_futures2.push_back(FutureFactory::makeFuture());

        auto void_any_result = FutureCollector::collectAny(std::move(void_futures2));
        auto void_result = void_any_result.get();

        std::size_t index = std::get<0>(void_result);
        Try<void> try_value = std::get<1>(void_result);

        BOOST_CHECK_EQUAL(index, 0);
        BOOST_CHECK(try_value.hasValue());

        // Test collectAnyWithoutException with void futures (returns just index)
        std::vector<Future<void>> void_futures3;
        void_futures3.push_back(FutureFactory::makeFuture());

        auto void_any_success_result =
            FutureCollector::collectAnyWithoutException(std::move(void_futures3));
        auto void_index = void_any_success_result.get();

        BOOST_CHECK_EQUAL(void_index, 0);

        BOOST_TEST_MESSAGE("Void future collections work correctly");
    }
}

namespace {
// Resolves after `delay` on a background thread - used below to simulate a
// slow/partitioned peer without actually needing a network.
template<typename T>
auto make_delayed_future(T value, std::chrono::milliseconds delay) -> Future<T> {
    auto promise = std::make_shared<Promise<T>>();
    auto future = promise->getFuture();
    std::thread([promise, value = std::move(value), delay]() mutable {
        std::this_thread::sleep_for(delay);
        promise->setValue(std::move(value));
    }).detach();
    return future;
}

auto make_delayed_failed_future(std::chrono::milliseconds delay, std::string message)
    -> Future<int> {
    auto promise = std::make_shared<Promise<int>>();
    auto future = promise->getFuture();
    std::thread([promise, delay, message = std::move(message)]() mutable {
        std::this_thread::sleep_for(delay);
        promise->setException(std::make_exception_ptr(std::runtime_error(message)));
    }).detach();
    return future;
}
}  // namespace

/**
 * Test raft_future_collector<T>::collect_n_successes_with_timeout - added
 * alongside node<Types>::read_state()'s switch to it (raft.hpp) after a real
 * multi-AZ EC2 run showed every linearizable read on the leader stalling for
 * the full command timeout while a single follower was network-partitioned,
 * even though a majority of followers (the one reachable one, plus the
 * leader's own implicit vote) was already available. The prior primitives
 * (collect_all_with_timeout, collect_majority) both go through collectAll
 * internally and therefore always wait for every future to individually
 * settle before checking the count, regardless of how early the needed
 * count is actually reached - this covers the specific behavior that fixes.
 */
BOOST_AUTO_TEST_CASE(collect_n_successes_with_timeout_test, *boost::unit_test::timeout(30)) {
    // Basic correctness: exactly required_successes successful values, drawn
    // from whichever futures actually succeeded.
    {
        std::vector<Future<int>> futures;
        futures.push_back(FutureFactory::makeFuture(1));
        futures.push_back(FutureFactory::makeFuture(2));
        futures.push_back(FutureFactory::makeFuture(3));

        auto result = raft_future_collector<int>::collect_n_successes_with_timeout(
                          std::move(futures), 2, long_timeout)
                          .get();
        BOOST_CHECK_EQUAL(result.size(), 2u);
    }

    // Early resolution: 2 of 3 futures succeed near-instantly, the third is
    // deliberately slow. Only 2 successes are required (majority of 3), so
    // this must resolve in well under the slow future's own delay - the
    // exact behavior a partitioned Raft follower's hung connect attempt
    // needs from the leader's read_state() heartbeat collection.
    {
        constexpr auto slow_delay = std::chrono::milliseconds(2000);
        std::vector<Future<int>> futures;
        futures.push_back(FutureFactory::makeFuture(10));
        futures.push_back(FutureFactory::makeFuture(20));
        futures.push_back(make_delayed_future(30, slow_delay));

        auto start = std::chrono::steady_clock::now();
        auto result = raft_future_collector<int>::collect_n_successes_with_timeout(
                          std::move(futures), 2, std::chrono::milliseconds(10000))
                          .get();
        auto elapsed = std::chrono::steady_clock::now() - start;

        BOOST_CHECK_EQUAL(result.size(), 2u);
        BOOST_CHECK_MESSAGE(
            elapsed < slow_delay / 2,
            "expected early resolution well before the slow future's own "
            "delay; took "
                << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms");
    }

    // Early failure: requiring all 3 successes, one future fails
    // immediately and the other two are slow. Success is already
    // mathematically impossible the instant the fast failure lands (0
    // successes + 2 still-pending < 3 required), so this must throw well
    // before the slow futures' own delay - not silently wait out the full
    // timeout only to fail once everything has settled.
    {
        constexpr auto slow_delay = std::chrono::milliseconds(2000);
        std::vector<Future<int>> futures;
        futures.push_back(FutureFactory::makeExceptionalFuture<int>(folly::exception_wrapper(
            std::runtime_error("collect_n_successes_with_timeout_test: immediate failure"))));
        futures.push_back(make_delayed_future(40, slow_delay));
        futures.push_back(make_delayed_future(50, slow_delay));

        auto start = std::chrono::steady_clock::now();
        auto collected = raft_future_collector<int>::collect_n_successes_with_timeout(
            std::move(futures), 3, std::chrono::milliseconds(10000));
        BOOST_CHECK_THROW(collected.get(), std::exception);
        auto elapsed = std::chrono::steady_clock::now() - start;

        BOOST_CHECK_MESSAGE(
            elapsed < slow_delay / 2,
            "expected early failure well before the slow futures' own delay; "
            "took "
                << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms");
    }

    // Timeout still applies when required_successes is never reached and
    // nothing ever definitively fails (a hung, never-settling future -
    // there's no earlier point at which "impossible" could be detected, so
    // this must fall back to the per-future timeout like the other
    // primitives).
    {
        std::vector<Future<int>> futures;
        futures.push_back(make_delayed_future(60, std::chrono::milliseconds(500)));
        auto promise = std::make_shared<Promise<int>>();
        futures.push_back(promise->getFuture());  // never settles

        BOOST_CHECK_THROW(raft_future_collector<int>::collect_n_successes_with_timeout(
                              std::move(futures), 2, short_timeout)
                              .get(),
                          std::exception);
    }

    BOOST_TEST_MESSAGE(
        "collect_n_successes_with_timeout resolves early on majority and on "
        "provable impossibility, and still respects its timeout otherwise");
}

/**
 * Test collection operation performance and scalability
 */
BOOST_AUTO_TEST_CASE(collection_operations_performance_test, *boost::unit_test::timeout(60)) {
    constexpr std::size_t large_collection_size = 100;

    // Test collectAll with large collection
    {
        std::vector<Future<int>> large_futures;
        for (std::size_t i = 0; i < large_collection_size; ++i) {
            int value = test_value_base + static_cast<int>(i);
            large_futures.push_back(FutureFactory::makeFuture(value));
        }

        auto start_time = std::chrono::steady_clock::now();
        auto all_result = FutureCollector::collectAll(std::move(large_futures));
        auto results = all_result.get();
        auto end_time = std::chrono::steady_clock::now();

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        BOOST_CHECK_EQUAL(results.size(), large_collection_size);
        BOOST_CHECK(duration < std::chrono::milliseconds(1000));  // Should complete quickly

        // Verify all results are correct
        for (std::size_t i = 0; i < results.size(); ++i) {
            BOOST_CHECK(results[i].hasValue());
            BOOST_CHECK_EQUAL(results[i].value(), test_value_base + static_cast<int>(i));
        }
    }

    // Test collectN with large collection
    {
        std::vector<Future<int>> large_futures;
        for (std::size_t i = 0; i < large_collection_size; ++i) {
            int value = test_value_base + static_cast<int>(i);
            large_futures.push_back(FutureFactory::makeFuture(value));
        }

        std::size_t n = large_collection_size / 2;
        auto start_time = std::chrono::steady_clock::now();
        auto n_result = FutureCollector::collectN(std::move(large_futures), n);
        auto results = n_result.get();
        auto end_time = std::chrono::steady_clock::now();

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        BOOST_CHECK_EQUAL(results.size(), n);
        BOOST_CHECK(duration < std::chrono::milliseconds(1000));  // Should complete quickly
    }

    BOOST_TEST_MESSAGE("Collection operations perform well with large collections");
}

/**
 * Test collection operation thread safety
 */
BOOST_AUTO_TEST_CASE(collection_operations_thread_safety_test, *boost::unit_test::timeout(60)) {
    constexpr std::size_t num_threads = 4;
    constexpr std::size_t operations_per_thread = 10;

    std::vector<std::thread> threads;
    std::atomic<std::size_t> successful_operations{0};

    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (std::size_t op = 0; op < operations_per_thread; ++op) {
                try {
                    // Create futures for this operation
                    std::vector<Future<int>> futures;
                    for (std::size_t i = 0; i < 5; ++i) {
                        int value = test_value_base + static_cast<int>(t * 1000 + op * 10 + i);
                        futures.push_back(FutureFactory::makeFuture(value));
                    }

                    // Test collectAll
                    auto all_result = FutureCollector::collectAll(std::move(futures));
                    auto results = all_result.get();

                    if (results.size() == 5) {
                        bool all_valid = true;
                        for (std::size_t i = 0; i < results.size(); ++i) {
                            if (!results[i].hasValue()) {
                                all_valid = false;
                                break;
                            }
                            int expected =
                                test_value_base + static_cast<int>(t * 1000 + op * 10 + i);
                            if (results[i].value() != expected) {
                                all_valid = false;
                                break;
                            }
                        }
                        if (all_valid) {
                            successful_operations.fetch_add(1);
                        }
                    }
                } catch (...) {
                    // Ignore exceptions for this test
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // All operations should have succeeded
    BOOST_CHECK_EQUAL(successful_operations.load(), num_threads * operations_per_thread);

    BOOST_TEST_MESSAGE("Collection operations are thread-safe");
}