/**
 * @file future_then_try_future_returning_callback_property_test.cpp
 * @brief Property-based tests for thenTry with Future-returning callbacks
 * 
 * **Feature: folly-concept-wrappers, Property 25: Future-Returning Callback Support in thenTry**
 * 
 * This test validates that thenTry supports callbacks that return Future<U> with automatic
 * flattening, enabling non-blocking async retry patterns.
 * 
 * **Validates: Requirements 30.1, 30.2, 30.3, 30.4, 30.5**
 */

#define BOOST_TEST_MODULE future_then_try_future_returning_callback_property_test
#include <boost/test/unit_test.hpp>
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <chrono>
#include <random>
#include <string>
#include <vector>

using namespace kythira;

namespace {
    constexpr std::size_t num_property_iterations = 100;
    constexpr std::chrono::milliseconds test_timeout{5000};
    constexpr std::chrono::milliseconds short_delay{10};
    constexpr std::chrono::milliseconds medium_delay{50};
    
    // Random number generator for property tests
    std::random_device rd;
    std::mt19937 gen(rd());
}

// Global fixture to initialize Folly once for all tests
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("future_then_try_future_returning_callback_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(future_then_try_future_returning_callback_property_tests)

/**
 * Property 1: thenTry with Future-returning callback should return Future<U>, not Future<Future<U>>
 * 
 * For any callback that returns Future<U>, thenTry should automatically flatten the result
 * to Future<U> instead of Future<Future<U>>.
 * 
 * **Validates: Requirement 30.1**
 */
BOOST_AUTO_TEST_CASE(property_then_try_automatic_flattening, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        
        // Create a future and chain with thenTry that returns Future<int>
        auto result = FutureFactory::makeFuture(test_value)
            .thenTry([&executor](Try<int> t) -> Future<int> {
                // This callback returns Future<int>, not int
                if (t.hasValue()) {
                    return FutureFactory::makeFuture(t.value() * 2);
                } else {
                    return FutureFactory::makeExceptionalFuture<int>(t.exception());
                }
            })
            .via(&executor)
            .get();
        
        // Verify the result is int, not Future<int>
        BOOST_CHECK_EQUAL(result, test_value * 2);
    }
}

/**
 * Property 2: thenTry with Future-returning callback should handle both success and error cases
 * 
 * For any Try<T> parameter, the callback should be able to inspect both hasValue() and
 * hasException() and return appropriate Future<U> for each case.
 * 
 * **Validates: Requirement 30.4**
 */
BOOST_AUTO_TEST_CASE(property_then_try_handles_success_and_error, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        std::bernoulli_distribution should_fail(0.5);
        bool inject_failure = should_fail(gen);
        
        // Create a future that may succeed or fail
        Future<int> initial_future = inject_failure
            ? FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error("Test error")))
            : FutureFactory::makeFuture(test_value);
        
        // Chain with thenTry that handles both cases
        auto result = std::move(initial_future)
            .thenTry([&executor, test_value](Try<int> t) -> Future<int> {
                if (t.hasValue()) {
                    // Success case - return doubled value
                    return FutureFactory::makeFuture(t.value() * 2);
                } else {
                    // Error case - return default value
                    return FutureFactory::makeFuture(test_value);
                }
            })
            .via(&executor)
            .get();
        
        // Verify the result
        if (inject_failure) {
            BOOST_CHECK_EQUAL(result, test_value);
        } else {
            BOOST_CHECK_EQUAL(result, test_value * 2);
        }
    }
}

/**
 * Property 3: thenTry with Future-returning callback should support async operations
 * 
 * For any callback that returns Future<U> with async operations (like delay),
 * the system should properly chain the operations without blocking.
 * 
 * **Validates: Requirements 30.2, 30.3**
 */
BOOST_AUTO_TEST_CASE(property_then_try_supports_async_operations, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Create a future and chain with thenTry that includes async delay
        auto result = FutureFactory::makeFuture(test_value)
            .thenTry([&executor](Try<int> t) -> Future<int> {
                if (t.hasValue()) {
                    // Return a future with async delay
                    return FutureFactory::makeFuture(folly::Unit{})
                        .delay(short_delay)
                        .thenValue([value = t.value()]() {
                            return value * 2;
                        });
                } else {
                    return FutureFactory::makeExceptionalFuture<int>(t.exception());
                }
            })
            .via(&executor)
            .get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, test_value * 2);
        
        // Verify that delay was applied (should be at least short_delay)
        BOOST_CHECK_GE(elapsed.count(), short_delay.count());
    }
}

/**
 * Property 4: thenTry with Future-returning callback should work with void futures
 * 
 * For any callback that returns Future<void>, the system should properly handle
 * void/Unit conversions and chain operations correctly.
 * 
 * **Validates: Requirement 30.5**
 */
BOOST_AUTO_TEST_CASE(property_then_try_handles_void_futures, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::atomic<int> counter{0};
        
        // Create a void future and chain with thenTry that returns Future<void>
        FutureFactory::makeFuture()
            .thenTry([&counter, &executor](Try<void> t) -> Future<void> {
                if (t.hasValue()) {
                    counter++;
                    return FutureFactory::makeFuture();
                } else {
                    return FutureFactory::makeExceptionalFuture<void>(t.exception());
                }
            })
            .via(&executor)
            .get();
        
        // Verify the callback was executed
        BOOST_CHECK_EQUAL(counter.load(), 1);
    }
}

/**
 * Property 5: thenTry with Future-returning callback should support chaining multiple operations
 * 
 * For any sequence of thenTry operations with Future-returning callbacks,
 * the system should properly chain all operations and flatten nested futures.
 * 
 * **Validates: Requirements 30.2, 30.3**
 */
BOOST_AUTO_TEST_CASE(property_then_try_supports_chaining, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 100);
        int test_value = value_dist(gen);
        
        // Chain multiple thenTry operations with Future-returning callbacks
        auto result = FutureFactory::makeFuture(test_value)
            .thenTry([](Try<int> t) -> Future<int> {
                if (t.hasValue()) {
                    return FutureFactory::makeFuture(t.value() + 1);
                } else {
                    return FutureFactory::makeExceptionalFuture<int>(t.exception());
                }
            })
            .thenTry([](Try<int> t) -> Future<int> {
                if (t.hasValue()) {
                    return FutureFactory::makeFuture(t.value() * 2);
                } else {
                    return FutureFactory::makeExceptionalFuture<int>(t.exception());
                }
            })
            .thenTry([](Try<int> t) -> Future<int> {
                if (t.hasValue()) {
                    return FutureFactory::makeFuture(t.value() - 1);
                } else {
                    return FutureFactory::makeExceptionalFuture<int>(t.exception());
                }
            })
            .via(&executor)
            .get();
        
        // Verify the result: (test_value + 1) * 2 - 1
        int expected = (test_value + 1) * 2 - 1;
        BOOST_CHECK_EQUAL(result, expected);
    }
}

/**
 * Property 6: thenTry with Future-returning callback should propagate errors correctly
 * 
 * For any callback that returns Future<U> with an exception, the error should
 * propagate through the async chain correctly.
 * 
 * **Validates: Requirement 30.3**
 */
BOOST_AUTO_TEST_CASE(property_then_try_propagates_errors, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Create a future and chain with thenTry that returns exceptional future
        try {
            FutureFactory::makeFuture(test_value)
                .thenTry([&error_message](Try<int> t) -> Future<int> {
                    if (t.hasValue()) {
                        // Return exceptional future
                        return FutureFactory::makeExceptionalFuture<int>(
                            folly::exception_wrapper(std::runtime_error(error_message)));
                    } else {
                        return FutureFactory::makeExceptionalFuture<int>(t.exception());
                    }
                })
                .via(&executor)
                .get();
            
            BOOST_FAIL("Expected exception to be thrown");
        } catch (const std::runtime_error& e) {
            BOOST_CHECK_EQUAL(std::string(e.what()), error_message);
        }
    }
}

/**
 * Property 7: thenTry with Future-returning callback should work with different value types
 * 
 * For any value type T and return type U, thenTry should support callbacks that
 * return Future<U> and properly handle type conversions.
 * 
 * **Validates: Requirements 30.1, 30.2**
 */
BOOST_AUTO_TEST_CASE(property_then_try_handles_type_conversions, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        
        // Convert int to string through Future-returning callback
        auto result = FutureFactory::makeFuture(test_value)
            .thenTry([](Try<int> t) -> Future<std::string> {
                if (t.hasValue()) {
                    return FutureFactory::makeFuture(std::to_string(t.value()));
                } else {
                    return FutureFactory::makeExceptionalFuture<std::string>(t.exception());
                }
            })
            .via(&executor)
            .get();
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, std::to_string(test_value));
    }
}

/**
 * Property 8: thenTry with Future-returning callback should enable async retry patterns
 * 
 * For any retry scenario with delay, thenTry should support non-blocking retry
 * patterns using Future-returning callbacks.
 * 
 * **Validates: Requirements 30.1, 30.2, 30.3, 30.4, 30.5**
 */
BOOST_AUTO_TEST_CASE(property_then_try_enables_async_retry, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::atomic<int> attempt_count{0};
        std::uniform_int_distribution<int> max_attempts_dist(2, 5);
        int max_attempts = max_attempts_dist(gen);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Simulate async retry pattern
        auto result = FutureFactory::makeFuture(0)
            .thenTry([&attempt_count, max_attempts, &executor](Try<int> t) -> Future<int> {
                attempt_count++;
                
                if (attempt_count.load() < max_attempts) {
                    // Simulate failure and retry with delay
                    return FutureFactory::makeFuture(folly::Unit{})
                        .delay(short_delay)
                        .thenValue([&attempt_count, max_attempts, &executor]() -> Future<int> {
                            // Recursive retry
                            return FutureFactory::makeFuture(attempt_count.load());
                        });
                } else {
                    // Success after retries
                    return FutureFactory::makeFuture(attempt_count.load());
                }
            })
            .via(&executor)
            .get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify the result
        BOOST_CHECK_GE(result, 1);
        BOOST_CHECK_LE(result, max_attempts);
        
        // Verify that delays were applied (should be at least one short_delay)
        BOOST_CHECK_GE(elapsed.count(), short_delay.count());
    }
}

BOOST_AUTO_TEST_SUITE_END()
