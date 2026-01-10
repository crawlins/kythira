#define BOOST_TEST_MODULE RaftHeartbeatRetryBackoffPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/error_handler.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>

using namespace kythira;

namespace {
    constexpr std::chrono::milliseconds base_delay{100};
    constexpr std::chrono::milliseconds max_delay{5000};
    constexpr double backoff_multiplier = 2.0;
    constexpr std::size_t max_attempts = 5;
    constexpr std::size_t test_iterations = 20;
}

// Global fixture to initialize Folly
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
 * **Feature: raft-completion, Property 16: Heartbeat Retry with Backoff**
 * 
 * Property: For any heartbeat RPC failure, the system retries with exponential backoff up to configured limits.
 * **Validates: Requirements 4.1**
 */
BOOST_AUTO_TEST_CASE(raft_heartbeat_retry_backoff_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> failure_count_dist(1, 4); // Number of failures before success
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Create error handler with heartbeat-specific retry policy
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy heartbeat_policy{
            .initial_delay = base_delay,
            .max_delay = max_delay,
            .backoff_multiplier = backoff_multiplier,
            .jitter_factor = 0.1,
            .max_attempts = max_attempts
        };
        
        handler.set_retry_policy("heartbeat", heartbeat_policy);
        
        const int failures_before_success = failure_count_dist(gen);
        BOOST_TEST_MESSAGE("Testing with " << failures_before_success << " failures before success");
        
        // Track retry attempts and timing
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::atomic<int> attempt_count{0};
        
        // Create operation that fails a specific number of times then succeeds
        auto heartbeat_operation = [&attempt_count, failures_before_success, &attempt_times]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            attempt_times.push_back(std::chrono::steady_clock::now());
            int current_attempt = ++attempt_count;
            
            if (current_attempt <= failures_before_success) {
                // Simulate different types of network failures
                std::vector<std::string> failure_messages = {
                    "Network timeout occurred",
                    "Connection refused by target",
                    "Network is unreachable",
                    "Temporary failure, try again"
                };
                
                std::random_device rd;
                std::mt19937 rng(rd());
                std::uniform_int_distribution<std::size_t> msg_dist(0, failure_messages.size() - 1);
                
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error(failure_messages[msg_dist(rng)]));
            } else {
                // Success case
                kythira::append_entries_response<std::uint64_t, std::uint64_t> success_response{
                    1, // term
                    true, // success
                    std::nullopt, // conflict_term
                    std::nullopt  // conflict_index
                };
                return kythira::FutureFactory::makeFuture(success_response);
            }
        };
        
        // Execute with retry
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            auto result = handler.execute_with_retry("heartbeat", heartbeat_operation).get();
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Should eventually succeed after retries
            BOOST_CHECK(result.success());
            BOOST_TEST_MESSAGE("✓ Operation succeeded after " << attempt_count.load() << " attempts in " 
                              << total_elapsed.count() << "ms");
            
            // Property: Should make exactly failures_before_success + 1 attempts
            BOOST_CHECK_EQUAL(attempt_count.load(), failures_before_success + 1);
            
            // Property: Should respect exponential backoff timing
            if (attempt_times.size() > 1) {
                for (std::size_t i = 1; i < attempt_times.size(); ++i) {
                    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        attempt_times[i] - attempt_times[i-1]);
                    
                    // Calculate expected delay for this retry (with some tolerance for jitter and timing)
                    auto expected_base_delay = base_delay;
                    for (std::size_t j = 1; j < i; ++j) {
                        expected_base_delay = std::chrono::milliseconds{
                            static_cast<long long>(expected_base_delay.count() * backoff_multiplier)
                        };
                    }
                    expected_base_delay = std::min(expected_base_delay, max_delay);
                    
                    // Allow for jitter (±20%) and timing overhead
                    auto min_expected = std::chrono::milliseconds{
                        static_cast<long long>(expected_base_delay.count() * 0.7)
                    };
                    auto max_expected = std::chrono::milliseconds{
                        static_cast<long long>(expected_base_delay.count() * 1.5)
                    };
                    
                    BOOST_TEST_MESSAGE("Retry " << i << ": delay=" << delay.count() 
                                      << "ms, expected range=[" << min_expected.count() 
                                      << "," << max_expected.count() << "]ms");
                    
                    // Property: Delays should follow exponential backoff pattern
                    BOOST_CHECK_GE(delay.count(), min_expected.count());
                    BOOST_CHECK_LE(delay.count(), max_expected.count());
                }
            }
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("Operation failed after " << attempt_count.load() << " attempts in " 
                              << total_elapsed.count() << "ms: " << e.what());
            
            // If we expected success but got failure, this might be due to max attempts exceeded
            if (failures_before_success < max_attempts) {
                BOOST_FAIL("Expected success but got failure: " << e.what());
            } else {
                // Property: Should respect max attempts limit
                BOOST_CHECK_LE(attempt_count.load(), max_attempts);
                BOOST_TEST_MESSAGE("✓ Correctly failed after reaching max attempts");
            }
        }
    }
    
    // Test specific backoff scenarios
    BOOST_TEST_MESSAGE("Testing specific backoff scenarios...");
    
    // Test 1: Max attempts exceeded
    {
        BOOST_TEST_MESSAGE("Test 1: Max attempts exceeded");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy strict_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{200},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0, // No jitter for predictable timing
            .max_attempts = 3
        };
        
        handler.set_retry_policy("heartbeat", strict_policy);
        
        std::atomic<int> attempt_count{0};
        auto always_fail_operation = [&attempt_count]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            ++attempt_count;
            return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                std::runtime_error("Network timeout occurred"));
        };
        
        auto start_time = std::chrono::steady_clock::now();
        BOOST_CHECK_THROW(
            handler.execute_with_retry("heartbeat", always_fail_operation).get(),
            std::exception
        );
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Property: Should make exactly max_attempts attempts
        BOOST_CHECK_EQUAL(attempt_count.load(), 3);
        
        // Property: Should complete in reasonable time based on backoff
        // Expected delays: 0, 50ms, 100ms = ~150ms + overhead
        BOOST_CHECK_GE(elapsed.count(), 140); // Allow some timing variance
        BOOST_CHECK_LE(elapsed.count(), 400); // Allow overhead
        
        BOOST_TEST_MESSAGE("✓ Max attempts test: " << attempt_count.load() 
                          << " attempts in " << elapsed.count() << "ms");
    }
    
    // Test 2: Different error types
    {
        BOOST_TEST_MESSAGE("Test 2: Different error types");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test different error classifications
        std::vector<std::string> error_messages = {
            "Network timeout occurred",           // Should retry
            "Connection refused",                 // Should retry
            "Network is unreachable",            // Should retry
            "serialization error",               // Should not retry
            "protocol violation"                 // Should not retry
        };
        
        for (const auto& error_msg : error_messages) {
            BOOST_TEST_MESSAGE("Testing error: " << error_msg);
            
            std::atomic<int> attempt_count{0};
            auto error_operation = [&attempt_count, error_msg]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
                ++attempt_count;
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error(error_msg));
            };
            
            try {
                handler.execute_with_retry("heartbeat", error_operation).get();
                BOOST_FAIL("Expected exception for error: " << error_msg);
            } catch (const std::exception& e) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                
                if (classification.should_retry) {
                    // Property: Retryable errors should make multiple attempts
                    BOOST_CHECK_GT(attempt_count.load(), 1);
                    BOOST_TEST_MESSAGE("✓ Retryable error made " << attempt_count.load() << " attempts");
                } else {
                    // Property: Non-retryable errors should fail immediately
                    BOOST_CHECK_EQUAL(attempt_count.load(), 1);
                    BOOST_TEST_MESSAGE("✓ Non-retryable error failed immediately");
                }
            }
        }
    }
    
    // Test 3: Jitter effectiveness
    {
        BOOST_TEST_MESSAGE("Test 3: Jitter effectiveness");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy jitter_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{1000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.2, // 20% jitter
            .max_attempts = 3
        };
        
        handler.set_retry_policy("heartbeat", jitter_policy);
        
        // Run multiple operations to see jitter variation
        std::vector<std::chrono::milliseconds> total_times;
        
        for (int run = 0; run < 5; ++run) {
            std::atomic<int> attempt_count{0};
            auto fail_twice_operation = [&attempt_count]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
                int current_attempt = ++attempt_count;
                if (current_attempt <= 2) {
                    return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                        std::runtime_error("Network timeout occurred"));
                } else {
                    kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
                    return kythira::FutureFactory::makeFuture(response);
                }
            };
            
            auto start_time = std::chrono::steady_clock::now();
            try {
                auto result = handler.execute_with_retry("heartbeat", fail_twice_operation).get();
                auto end_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                total_times.push_back(elapsed);
                
                BOOST_CHECK(result.success());
                BOOST_TEST_MESSAGE("Run " << run + 1 << ": " << elapsed.count() << "ms");
            } catch (const std::exception& e) {
                BOOST_FAIL("Unexpected failure in jitter test: " << e.what());
            }
        }
        
        // Property: Jitter should cause variation in timing
        if (total_times.size() > 1) {
            auto min_time = *std::min_element(total_times.begin(), total_times.end());
            auto max_time = *std::max_element(total_times.begin(), total_times.end());
            auto time_variation = max_time - min_time;
            
            BOOST_TEST_MESSAGE("Time variation: " << time_variation.count() << "ms (min=" 
                              << min_time.count() << "ms, max=" << max_time.count() << "ms)");
            
            // Should have some variation due to jitter (at least 10ms difference)
            BOOST_CHECK_GE(time_variation.count(), 10);
        }
    }
    
    BOOST_TEST_MESSAGE("All heartbeat retry with backoff property tests passed!");
}