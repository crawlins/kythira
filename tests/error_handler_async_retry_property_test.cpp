/**
 * @file error_handler_async_retry_property_test.cpp
 * @brief Property-based tests for async retry without blocking
 * 
 * **Feature: folly-concept-wrappers, Property 27: Async Retry Without Blocking**
 * 
 * This test validates that retry operations use Future.delay() and Future-returning
 * callbacks instead of blocking sleep, ensuring no threads are blocked during retry
 * backoff periods.
 * 
 * **Validates: Requirements 32.1, 32.2, 32.3, 32.4, 32.5**
 */

#define BOOST_TEST_MODULE error_handler_async_retry_property_test
#include <boost/test/unit_test.hpp>
#include <raft/error_handler.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <chrono>
#include <random>
#include <atomic>
#include <thread>

using namespace kythira;

namespace {
    constexpr std::size_t num_property_iterations = 50;
    constexpr std::chrono::milliseconds short_delay{50};
    constexpr std::chrono::milliseconds medium_delay{100};
    constexpr std::chrono::milliseconds long_delay{200};
    
    // Random number generator for property tests
    std::random_device rd;
    std::mt19937 gen(rd());
}

// Global fixture to initialize Folly once for all tests
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("error_handler_async_retry_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(error_handler_async_retry_property_tests)

/**
 * Property 1: Retry delays should use Future.delay() instead of std::this_thread::sleep_for
 * 
 * For any retry operation with delay, the system should use Future.delay() which returns
 * a Future that completes after the delay, rather than blocking the current thread.
 * 
 * **Validates: Requirement 32.1**
 */
BOOST_AUTO_TEST_CASE(property_retry_uses_future_delay, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        error_handler<int> handler;
        
        // Configure with short delays for faster testing
        typename error_handler<int>::retry_policy policy{
            .initial_delay = short_delay,
            .max_delay = medium_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,
            .max_attempts = 3
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> attempt_count{0};
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int expected_value = value_dist(gen);
        
        auto operation = [&attempt_count, expected_value]() -> Future<int> {
            std::size_t current_attempt = ++attempt_count;
            
            // Fail first 2 attempts, succeed on 3rd
            if (current_attempt < 3) {
                return FutureFactory::makeExceptionalFuture<int>(
                    std::runtime_error("Temporary failure"));
            }
            
            return FutureFactory::makeFuture(expected_value);
        };
        
        // Execute with retry - should complete without blocking threads
        auto result = handler.execute_with_retry("test_operation", operation)
            .via(&executor)
            .get();
        
        BOOST_CHECK_EQUAL(result, expected_value);
        BOOST_CHECK_EQUAL(attempt_count.load(), 3);
    }
}

/**
 * Property 2: Async retry should use thenTry with Future-returning callbacks
 * 
 * For any retry operation, the system should chain operations using thenTry with
 * callbacks that return Future<T>, enabling non-blocking async chains.
 * 
 * **Validates: Requirement 32.2**
 */
BOOST_AUTO_TEST_CASE(property_retry_uses_then_try_with_future_callbacks, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        error_handler<int> handler;
        
        typename error_handler<int>::retry_policy policy{
            .initial_delay = short_delay,
            .max_delay = long_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 4
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> attempt_count{0};
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::mutex times_mutex;
        
        auto operation = [&attempt_count, &attempt_times, &times_mutex]() -> Future<int> {
            {
                std::lock_guard<std::mutex> lock(times_mutex);
                attempt_times.push_back(std::chrono::steady_clock::now());
            }
            
            std::size_t current_attempt = ++attempt_count;
            
            // Fail first 3 attempts
            if (current_attempt < 4) {
                return FutureFactory::makeExceptionalFuture<int>(
                    std::runtime_error("Network timeout"));
            }
            
            return FutureFactory::makeFuture(42);
        };
        
        auto result = handler.execute_with_retry("test_operation", operation)
            .via(&executor)
            .get();
        
        BOOST_CHECK_EQUAL(result, 42);
        BOOST_CHECK_EQUAL(attempt_count.load(), 4);
        
        // Verify delays were applied between attempts
        BOOST_REQUIRE_GE(attempt_times.size(), 2);
        
        for (std::size_t j = 1; j < attempt_times.size(); ++j) {
            auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                attempt_times[j] - attempt_times[j-1]);
            
            // Should have some delay (at least 80% of expected to account for jitter)
            BOOST_CHECK_GE(delay.count(), short_delay.count() * 0.8);
        }
    }
}

/**
 * Property 3: No threads should be blocked during retry delays
 * 
 * For any retry operation with delay, the system should not block threads during
 * the delay period. This can be verified by ensuring the executor can process
 * other work during retry delays.
 * 
 * **Validates: Requirement 32.3**
 */
BOOST_AUTO_TEST_CASE(property_no_threads_blocked_during_delay, * boost::unit_test::timeout(90)) {
    // Use a small thread pool to make blocking more obvious
    folly::CPUThreadPoolExecutor executor(2);
    
    for (std::size_t i = 0; i < 10; ++i) {  // Fewer iterations for this expensive test
        error_handler<int> handler;
        
        typename error_handler<int>::retry_policy policy{
            .initial_delay = medium_delay,
            .max_delay = long_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,
            .max_attempts = 3
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> retry_attempt_count{0};
        std::atomic<std::size_t> other_work_count{0};
        
        // Operation that will retry
        auto retry_operation = [&retry_attempt_count]() -> Future<int> {
            std::size_t current_attempt = ++retry_attempt_count;
            
            if (current_attempt < 3) {
                return FutureFactory::makeExceptionalFuture<int>(
                    std::runtime_error("Temporary failure"));
            }
            
            return FutureFactory::makeFuture(100);
        };
        
        // Start retry operation
        auto retry_future = handler.execute_with_retry("test_operation", retry_operation)
            .via(&executor);
        
        // Submit other work to the executor while retry is happening
        std::vector<Future<int>> other_futures;
        for (int j = 0; j < 10; ++j) {
            auto other_future = FutureFactory::makeFuture(folly::Unit{})
                .via(&executor)
                .thenValue([&other_work_count, j]() {
                    other_work_count++;
                    return j;
                });
            other_futures.push_back(std::move(other_future));
        }
        
        // Wait for all work to complete
        auto retry_result = std::move(retry_future).get();
        
        for (auto& f : other_futures) {
            std::move(f).get();
        }
        
        // Verify retry completed
        BOOST_CHECK_EQUAL(retry_result, 100);
        BOOST_CHECK_EQUAL(retry_attempt_count.load(), 3);
        
        // Verify other work was processed (threads were not blocked)
        // If threads were blocked, other work would not complete
        BOOST_CHECK_EQUAL(other_work_count.load(), 10);
    }
}

/**
 * Property 4: Exception propagation should work correctly through async chains
 * 
 * For any retry operation that exhausts all attempts, exceptions should propagate
 * correctly through the async chain without being lost or corrupted.
 * 
 * **Validates: Requirement 32.4**
 */
BOOST_AUTO_TEST_CASE(property_exception_propagation_through_async_chains, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        error_handler<int> handler;
        
        typename error_handler<int>::retry_policy policy{
            .initial_delay = short_delay,
            .max_delay = medium_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,
            .max_attempts = 3
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> attempt_count{0};
        std::string error_message = "Persistent network failure " + std::to_string(i);
        
        auto operation = [&attempt_count, error_message]() -> Future<int> {
            ++attempt_count;
            
            // Always fail
            return FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error(error_message));
        };
        
        // Execute with retry - should throw after all attempts
        try {
            auto result = handler.execute_with_retry("test_operation", operation)
                .via(&executor)
                .get();
            
            BOOST_FAIL("Expected exception after exhausting retries");
        } catch (const std::runtime_error& e) {
            // Verify exception message is preserved
            BOOST_CHECK_EQUAL(std::string(e.what()), error_message);
            BOOST_CHECK_EQUAL(attempt_count.load(), 3);
        } catch (...) {
            BOOST_FAIL("Unexpected exception type");
        }
    }
}

/**
 * Property 5: Async retry should return results asynchronously without blocking
 * 
 * For any retry operation, the system should return a Future immediately and
 * complete it asynchronously, allowing the caller to continue other work.
 * 
 * **Validates: Requirement 32.5**
 */
BOOST_AUTO_TEST_CASE(property_async_retry_returns_immediately, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < num_property_iterations; ++i) {
        error_handler<int> handler;
        
        typename error_handler<int>::retry_policy policy{
            .initial_delay = medium_delay,
            .max_delay = long_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,
            .max_attempts = 3
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> attempt_count{0};
        std::uniform_int_distribution<int> value_dist(1, 1000);
        int expected_value = value_dist(gen);
        
        auto operation = [&attempt_count, expected_value]() -> Future<int> {
            std::size_t current_attempt = ++attempt_count;
            
            if (current_attempt < 3) {
                return FutureFactory::makeExceptionalFuture<int>(
                    std::runtime_error("Temporary failure"));
            }
            
            return FutureFactory::makeFuture(expected_value);
        };
        
        // Measure time to get the future (should be immediate)
        auto start_time = std::chrono::steady_clock::now();
        
        auto future = handler.execute_with_retry("test_operation", operation)
            .via(&executor);
        
        auto future_creation_time = std::chrono::steady_clock::now();
        auto creation_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            future_creation_time - start_time);
        
        // Future creation should be very fast (< 10ms)
        BOOST_CHECK_LT(creation_duration.count(), 10);
        
        // Now wait for the result (this will take time due to retries)
        auto result = std::move(future).get();
        
        auto completion_time = std::chrono::steady_clock::now();
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            completion_time - start_time);
        
        // Total duration should include retry delays
        // Expected: ~150ms (50ms + 100ms delays)
        BOOST_CHECK_GE(total_duration.count(), 100);
        
        BOOST_CHECK_EQUAL(result, expected_value);
        BOOST_CHECK_EQUAL(attempt_count.load(), 3);
    }
}

/**
 * Property 6: Exponential backoff should still be applied correctly with async delays
 * 
 * For any retry operation, delays should increase exponentially even when using
 * async Future.delay() instead of blocking sleep.
 * 
 * **Validates: Requirements 32.1, 32.2**
 */
BOOST_AUTO_TEST_CASE(property_exponential_backoff_with_async_delays, * boost::unit_test::timeout(90)) {
    folly::CPUThreadPoolExecutor executor(4);
    
    for (std::size_t i = 0; i < 20; ++i) {  // Fewer iterations for timing-sensitive test
        error_handler<int> handler;
        
        typename error_handler<int>::retry_policy policy{
            .initial_delay = short_delay,
            .max_delay = long_delay,
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0,  // No jitter for predictable timing
            .max_attempts = 4
        };
        
        handler.set_retry_policy("test_operation", policy);
        
        std::atomic<std::size_t> attempt_count{0};
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::mutex times_mutex;
        
        auto operation = [&attempt_count, &attempt_times, &times_mutex]() -> Future<int> {
            {
                std::lock_guard<std::mutex> lock(times_mutex);
                attempt_times.push_back(std::chrono::steady_clock::now());
            }
            
            std::size_t current_attempt = ++attempt_count;
            
            if (current_attempt < 4) {
                return FutureFactory::makeExceptionalFuture<int>(
                    std::runtime_error("Temporary failure"));
            }
            
            return FutureFactory::makeFuture(42);
        };
        
        auto result = handler.execute_with_retry("test_operation", operation)
            .via(&executor)
            .get();
        
        BOOST_CHECK_EQUAL(result, 42);
        BOOST_REQUIRE_EQUAL(attempt_times.size(), 4);
        
        // Verify exponential backoff
        // Expected delays: 0ms, 50ms, 100ms, 200ms (capped)
        auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(
            attempt_times[1] - attempt_times[0]);
        auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            attempt_times[2] - attempt_times[1]);
        auto delay3 = std::chrono::duration_cast<std::chrono::milliseconds>(
            attempt_times[3] - attempt_times[2]);
        
        // Allow 20% tolerance for timing variations
        BOOST_CHECK_GE(delay1.count(), short_delay.count() * 0.8);
        BOOST_CHECK_LE(delay1.count(), short_delay.count() * 1.2);
        
        BOOST_CHECK_GE(delay2.count(), (short_delay.count() * 2) * 0.8);
        BOOST_CHECK_LE(delay2.count(), (short_delay.count() * 2) * 1.2);
        
        // Third delay should be capped at long_delay (200ms)
        BOOST_CHECK_GE(delay3.count(), long_delay.count() * 0.8);
        BOOST_CHECK_LE(delay3.count(), long_delay.count() * 1.2);
    }
}

BOOST_AUTO_TEST_SUITE_END()
