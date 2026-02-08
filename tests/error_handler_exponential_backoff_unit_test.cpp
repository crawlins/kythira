#define BOOST_TEST_MODULE error_handler_exponential_backoff_unit_test
#include <boost/test/unit_test.hpp>
#include <folly/init/Init.h>
#include <raft/error_handler.hpp>
#include <chrono>
#include <thread>
#include <iostream>

// Global fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        static bool initialized = false;
        if (!initialized) {
            int argc = 1;
            char* argv_data[] = {const_cast<char*>("test"), nullptr};
            char** argv = argv_data;
            folly::init(&argc, &argv);
            initialized = true;
        }
    }
};

BOOST_TEST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t test_max_attempts = 5;
    constexpr std::chrono::milliseconds test_initial_delay{100};
    constexpr std::chrono::milliseconds test_max_delay{2000};
    constexpr double test_backoff_multiplier = 2.0;
    constexpr double test_jitter_factor = 0.1;
}

/**
 * @brief Test exponential backoff delay calculation
 * 
 * This test verifies that the ErrorHandler correctly calculates delays
 * with exponential backoff, capping at max_delay, and applying jitter.
 */
BOOST_AUTO_TEST_CASE(test_exponential_backoff_calculation, * boost::unit_test::timeout(30)) {
    using namespace kythira;
    
    // Create error handler with test policy
    error_handler<int> handler;
    
    typename error_handler<int>::retry_policy policy{
        .initial_delay = test_initial_delay,
        .max_delay = test_max_delay,
        .backoff_multiplier = test_backoff_multiplier,
        .jitter_factor = test_jitter_factor,
        .max_attempts = test_max_attempts
    };
    
    handler.set_retry_policy("test_operation", policy);
    
    // Test that delays increase exponentially
    std::size_t attempt_count = 0;
    std::vector<std::chrono::milliseconds> observed_delays;
    
    auto operation = [&attempt_count]() -> Future<int> {
        attempt_count++;
        // Always fail to trigger retry
        return FutureFactory::makeExceptionalFuture<int>(
            std::runtime_error("Test failure for retry"));
    };
    
    // Measure time for retries
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        auto result = handler.execute_with_retry("test_operation", operation);
        result.get();  // This should throw after all retries
        BOOST_FAIL("Expected exception after retries");
    } catch (const std::exception& e) {
        // Expected - all retries exhausted
        BOOST_CHECK_EQUAL(attempt_count, test_max_attempts);
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate expected minimum total delay (without jitter)
    // Delays: 0ms (first attempt), 100ms, 200ms, 400ms, 800ms
    // Total minimum: 1500ms
    std::chrono::milliseconds expected_min_delay{0};
    for (std::size_t i = 1; i < test_max_attempts; ++i) {
        auto delay = test_initial_delay;
        for (std::size_t j = 1; j < i; ++j) {
            delay = std::chrono::milliseconds{
                static_cast<long long>(delay.count() * test_backoff_multiplier)
            };
        }
        delay = std::min(delay, test_max_delay);
        expected_min_delay += delay;
    }
    
    std::cout << "Total time: " << total_time.count() << "ms" << std::endl;
    std::cout << "Expected minimum delay: " << expected_min_delay.count() << "ms" << std::endl;
    
    // Verify that actual time is at least the expected minimum
    // (accounting for some execution overhead, allow 90% of expected)
    BOOST_CHECK_GE(total_time.count(), expected_min_delay.count() * 0.9);
    
    // Verify delays are not zero (the bug we're fixing)
    BOOST_CHECK_GT(total_time.count(), 100);  // Should be much more than 100ms
}

/**
 * @brief Test delay capping at max_delay
 */
BOOST_AUTO_TEST_CASE(test_delay_capping, * boost::unit_test::timeout(30)) {
    using namespace kythira;
    
    error_handler<int> handler;
    
    // Policy with low max_delay to test capping
    typename error_handler<int>::retry_policy policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{200},  // Cap at 200ms
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,  // No jitter for predictable testing
        .max_attempts = 5
    };
    
    handler.set_retry_policy("test_capping", policy);
    
    std::size_t attempt_count = 0;
    
    auto operation = [&attempt_count]() -> Future<int> {
        attempt_count++;
        return FutureFactory::makeExceptionalFuture<int>(
            std::runtime_error("Test failure"));
    };
    
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        auto result = handler.execute_with_retry("test_capping", operation);
        result.get();
        BOOST_FAIL("Expected exception");
    } catch (const std::exception&) {
        // Expected
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Expected delays: 0ms, 100ms, 200ms (capped), 200ms (capped), 200ms (capped)
    // Total: 700ms
    std::chrono::milliseconds expected_delay{700};
    
    std::cout << "Total time with capping: " << total_time.count() << "ms" << std::endl;
    std::cout << "Expected delay: " << expected_delay.count() << "ms" << std::endl;
    
    // Allow some tolerance for execution overhead
    BOOST_CHECK_GE(total_time.count(), expected_delay.count() * 0.9);
    BOOST_CHECK_LE(total_time.count(), expected_delay.count() * 1.5);
}

/**
 * @brief Test jitter application
 */
BOOST_AUTO_TEST_CASE(test_jitter_application, * boost::unit_test::timeout(30)) {
    using namespace kythira;
    
    error_handler<int> handler;
    
    typename error_handler<int>::retry_policy policy{
        .initial_delay = std::chrono::milliseconds{100},
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.2,  // 20% jitter
        .max_attempts = 3
    };
    
    handler.set_retry_policy("test_jitter", policy);
    
    // Run multiple times to observe jitter variation
    std::vector<std::chrono::milliseconds> total_times;
    
    for (int run = 0; run < 5; ++run) {
        std::size_t attempt_count = 0;
        
        auto operation = [&attempt_count]() -> Future<int> {
            attempt_count++;
            return FutureFactory::makeExceptionalFuture<int>(
                std::runtime_error("Test failure"));
        };
        
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            auto result = handler.execute_with_retry("test_jitter", operation);
            result.get();
        } catch (const std::exception&) {
            // Expected
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        total_times.push_back(total_time);
    }
    
    // Check that times vary (jitter is working)
    bool has_variation = false;
    for (std::size_t i = 1; i < total_times.size(); ++i) {
        if (std::abs(total_times[i].count() - total_times[0].count()) > 10) {
            has_variation = true;
            break;
        }
    }
    
    std::cout << "Jitter test times: ";
    for (const auto& time : total_times) {
        std::cout << time.count() << "ms ";
    }
    std::cout << std::endl;
    
    // With 20% jitter, we should see some variation
    BOOST_CHECK(has_variation);
}

/**
 * @brief Test that delays are actually applied (not 0ms)
 */
BOOST_AUTO_TEST_CASE(test_delays_actually_applied, * boost::unit_test::timeout(30)) {
    using namespace kythira;
    
    error_handler<int> handler;
    
    typename error_handler<int>::retry_policy policy{
        .initial_delay = std::chrono::milliseconds{200},
        .max_delay = std::chrono::milliseconds{1000},
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0,
        .max_attempts = 3
    };
    
    handler.set_retry_policy("test_applied", policy);
    
    std::size_t attempt_count = 0;
    std::vector<std::chrono::steady_clock::time_point> attempt_times;
    
    auto operation = [&attempt_count, &attempt_times]() -> Future<int> {
        attempt_times.push_back(std::chrono::steady_clock::now());
        attempt_count++;
        return FutureFactory::makeExceptionalFuture<int>(
            std::runtime_error("Test failure"));
    };
    
    try {
        auto result = handler.execute_with_retry("test_applied", operation);
        result.get();
    } catch (const std::exception&) {
        // Expected
    }
    
    BOOST_REQUIRE_EQUAL(attempt_times.size(), 3);
    
    // Check delays between attempts
    auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(
        attempt_times[1] - attempt_times[0]);
    auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        attempt_times[2] - attempt_times[1]);
    
    std::cout << "Delay between attempt 1 and 2: " << delay1.count() << "ms" << std::endl;
    std::cout << "Delay between attempt 2 and 3: " << delay2.count() << "ms" << std::endl;
    
    // First delay should be ~200ms
    BOOST_CHECK_GE(delay1.count(), 180);  // Allow 10% tolerance
    BOOST_CHECK_LE(delay1.count(), 220);
    
    // Second delay should be ~400ms
    BOOST_CHECK_GE(delay2.count(), 360);
    BOOST_CHECK_LE(delay2.count(), 440);
    
    // Verify delays are NOT zero (the bug we're fixing)
    BOOST_CHECK_GT(delay1.count(), 0);
    BOOST_CHECK_GT(delay2.count(), 0);
}
