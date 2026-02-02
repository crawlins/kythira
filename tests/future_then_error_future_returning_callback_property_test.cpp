/**
 * @file future_then_error_future_returning_callback_property_test.cpp
 * @brief Property-based tests for thenError with Future-returning callbacks
 * 
 * **Feature: folly-concept-wrappers, Property 26: Future-Returning Callback Support in thenError**
 * 
 * This test validates that thenError supports callbacks that return Future<T> with automatic
 * flattening, enabling non-blocking async retry patterns with error recovery.
 * 
 * **Validates: Requirements 31.1, 31.2, 31.3, 31.4, 31.5**
 */

#define BOOST_TEST_MODULE future_then_error_future_returning_callback_property_test
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
        char* argv_data[] = {const_cast<char*>("future_then_error_future_returning_callback_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(future_then_error_future_returning_callback_property_tests)

/**
 * Property 1: thenError with Future-returning callback should return Future<T>, not Future<Future<T>>
 * 
 * For any callback that returns Future<T>, thenError should automatically flatten the result
 * to Future<T> instead of Future<Future<T>>.
 * 
 * **Validates: Requirement 31.1**
 */
BOOST_AUTO_TEST_CASE(property_then_error_automatic_flattening, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int test_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Create a failing future and chain with thenError that returns Future<int>
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&executor, test_value](folly::exception_wrapper ex) -> Future<int> {
                // This callback returns Future<int>, not int
                // Recover from error by returning a successful future
                return FutureFactory::makeFuture(test_value);
            })
            .via(&executor)
            .get();
        
        // Verify the result is int, not Future<int>
        BOOST_CHECK_EQUAL(result, test_value);
    }
}

/**
 * Property 2: thenError with Future-returning callback should support error recovery
 * 
 * For any exceptional future, thenError should be able to recover by returning a
 * successful Future<T> with a default or recovered value.
 * 
 * **Validates: Requirement 31.3**
 */
BOOST_AUTO_TEST_CASE(property_then_error_supports_error_recovery, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int default_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Create a failing future and recover with thenError
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&executor, default_value](folly::exception_wrapper ex) -> Future<int> {
                // Recover from error by returning default value
                return FutureFactory::makeFuture(default_value);
            })
            .via(&executor)
            .get();
        
        // Verify the result is the default value
        BOOST_CHECK_EQUAL(result, default_value);
    }
}

/**
 * Property 3: thenError with Future-returning callback should support async operations
 * 
 * For any callback that returns Future<T> with async operations (like delay),
 * the system should properly chain the operations without blocking.
 * 
 * **Validates: Requirements 31.2, 31.4**
 */
BOOST_AUTO_TEST_CASE(property_then_error_supports_async_operations, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int recovery_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Create a failing future and recover with async delay
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&executor, recovery_value](folly::exception_wrapper ex) -> Future<int> {
                // Return a future with async delay
                return FutureFactory::makeFuture(folly::Unit{})
                    .delay(short_delay)
                    .thenValue([recovery_value]() {
                        return recovery_value;
                    });
            })
            .via(&executor)
            .get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, recovery_value);
        
        // Verify that delay was applied (should be at least short_delay)
        BOOST_CHECK_GE(elapsed.count(), short_delay.count());
    }
}

/**
 * Property 4: thenError with Future-returning callback should work with void futures
 * 
 * For any callback that returns Future<void>, the system should properly handle
 * void/Unit conversions and chain operations correctly.
 * 
 * **Validates: Requirement 31.5**
 */
BOOST_AUTO_TEST_CASE(property_then_error_handles_void_futures, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::atomic<int> counter{0};
        std::string error_message = "Test error " + std::to_string(i);
        
        // Create a failing void future and recover with thenError that returns Future<void>
        FutureFactory::makeExceptionalFuture<void>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&counter, &executor](folly::exception_wrapper ex) -> Future<void> {
                counter++;
                return FutureFactory::makeFuture();
            })
            .via(&executor)
            .get();
        
        // Verify the callback was executed
        BOOST_CHECK_EQUAL(counter.load(), 1);
    }
}

/**
 * Property 5: thenError with Future-returning callback should support chaining multiple operations
 * 
 * For any sequence of thenError operations with Future-returning callbacks,
 * the system should properly chain all operations and flatten nested futures.
 * 
 * **Validates: Requirements 31.2, 31.3**
 */
BOOST_AUTO_TEST_CASE(property_then_error_supports_chaining, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 100);
        int recovery_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Chain multiple thenError operations with Future-returning callbacks
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([recovery_value](folly::exception_wrapper ex) -> Future<int> {
                // First recovery: return recovery_value + 1
                return FutureFactory::makeFuture(recovery_value + 1);
            })
            .thenValue([](int value) -> int {
                // Transform the value
                return value * 2;
            })
            .thenError([](folly::exception_wrapper ex) -> Future<int> {
                // This shouldn't be called since previous operation succeeded
                return FutureFactory::makeFuture(-1);
            })
            .via(&executor)
            .get();
        
        // Verify the result: (recovery_value + 1) * 2
        int expected = (recovery_value + 1) * 2;
        BOOST_CHECK_EQUAL(result, expected);
    }
}

/**
 * Property 6: thenError with Future-returning callback should propagate errors correctly
 * 
 * For any callback that returns Future<T> with an exception, the error should
 * propagate through the async chain correctly.
 * 
 * **Validates: Requirement 31.3**
 */
BOOST_AUTO_TEST_CASE(property_then_error_propagates_errors, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::string first_error = "First error " + std::to_string(i);
        std::string second_error = "Second error " + std::to_string(i);
        
        // Create a failing future and chain with thenError that returns another exceptional future
        try {
            FutureFactory::makeExceptionalFuture<int>(
                    folly::exception_wrapper(std::runtime_error(first_error)))
                .thenError([&second_error](folly::exception_wrapper ex) -> Future<int> {
                    // Return another exceptional future
                    return FutureFactory::makeExceptionalFuture<int>(
                        folly::exception_wrapper(std::runtime_error(second_error)));
                })
                .via(&executor)
                .get();
            
            BOOST_FAIL("Expected exception to be thrown");
        } catch (const std::runtime_error& e) {
            BOOST_CHECK_EQUAL(std::string(e.what()), second_error);
        }
    }
}

/**
 * Property 7: thenError with Future-returning callback should work with different value types
 * 
 * For any value type T, thenError should support callbacks that return Future<T>
 * and properly handle type conversions.
 * 
 * **Validates: Requirements 31.1, 31.2**
 */
BOOST_AUTO_TEST_CASE(property_then_error_handles_type_conversions, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int recovery_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Convert error to string through Future-returning callback
        auto result = FutureFactory::makeExceptionalFuture<std::string>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([recovery_value](folly::exception_wrapper ex) -> Future<std::string> {
                return FutureFactory::makeFuture(std::to_string(recovery_value));
            })
            .via(&executor)
            .get();
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, std::to_string(recovery_value));
    }
}

/**
 * Property 8: thenError with Future-returning callback should enable async retry patterns
 * 
 * For any retry scenario with delay, thenError should support non-blocking retry
 * patterns using Future-returning callbacks.
 * 
 * **Validates: Requirements 31.1, 31.2, 31.3, 31.4, 31.5**
 */
BOOST_AUTO_TEST_CASE(property_then_error_enables_async_retry, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::atomic<int> attempt_count{0};
        std::uniform_int_distribution<int> max_attempts_dist(2, 5);
        int max_attempts = max_attempts_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Simulate async retry pattern with error recovery
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&attempt_count, max_attempts](folly::exception_wrapper ex) -> Future<int> {
                attempt_count++;
                // Non-blocking delay followed by recovery
                return FutureFactory::makeFuture(folly::Unit{})
                    .delay(short_delay)
                    .thenValue([&attempt_count, max_attempts]() {
                        return max_attempts;
                    });
            })
            .via(&executor)
            .get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, max_attempts);
        BOOST_CHECK_EQUAL(attempt_count.load(), 1);
        
        // Verify that delay was applied
        BOOST_CHECK_GE(elapsed.count(), short_delay.count());
    }
}

/**
 * Property 9: thenError with Future-returning callback should work with std::exception_ptr
 * 
 * For any callback that accepts std::exception_ptr and returns Future<T>,
 * the system should properly convert between exception types.
 * 
 * **Validates: Requirements 31.1, 31.3**
 */
BOOST_AUTO_TEST_CASE(property_then_error_handles_exception_ptr, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int recovery_value = value_dist(gen);
        std::string error_message = "Test error " + std::to_string(i);
        
        // Create a failing future and recover with thenError using std::exception_ptr
        auto result = FutureFactory::makeExceptionalFuture<int>(
                folly::exception_wrapper(std::runtime_error(error_message)))
            .thenError([&executor, recovery_value](std::exception_ptr ex) -> Future<int> {
                // Verify we received an exception
                BOOST_CHECK(ex != nullptr);
                // Recover from error
                return FutureFactory::makeFuture(recovery_value);
            })
            .via(&executor)
            .get();
        
        // Verify the result
        BOOST_CHECK_EQUAL(result, recovery_value);
    }
}

BOOST_AUTO_TEST_SUITE_END()
