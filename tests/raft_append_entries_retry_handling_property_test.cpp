#define BOOST_TEST_MODULE RaftAppendEntriesRetryHandlingPropertyTest

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
    constexpr std::size_t test_iterations = 15;
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
 * **Feature: raft-completion, Property 17: AppendEntries Retry Handling**
 * 
 * Property: For any AppendEntries RPC failure, the system retries the operation and handles different failure modes appropriately.
 * **Validates: Requirements 4.2**
 */
BOOST_AUTO_TEST_CASE(raft_append_entries_retry_handling_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> failure_count_dist(1, 3); // Number of failures before success
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Create error handler with AppendEntries-specific retry policy
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy append_entries_policy{
            .initial_delay = base_delay,
            .max_delay = max_delay,
            .backoff_multiplier = backoff_multiplier,
            .jitter_factor = 0.1,
            .max_attempts = max_attempts
        };
        
        handler.set_retry_policy("append_entries", append_entries_policy);
        
        const int failures_before_success = failure_count_dist(gen);
        BOOST_TEST_MESSAGE("Testing with " << failures_before_success << " failures before success");
        
        // Track retry attempts and failure modes
        std::vector<std::string> failure_modes_encountered;
        std::atomic<int> attempt_count{0};
        
        // Create operation that fails with different modes then succeeds
        auto append_entries_operation = [&attempt_count, failures_before_success, &failure_modes_encountered]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt <= failures_before_success) {
                // Simulate different types of AppendEntries failures
                std::vector<std::string> failure_messages = {
                    "Network timeout occurred during AppendEntries",
                    "Connection refused by follower",
                    "Network is unreachable for AppendEntries",
                    "Temporary failure in log replication",
                    "RPC serialization error in AppendEntries"
                };
                
                std::random_device rd;
                std::mt19937 rng(rd());
                std::uniform_int_distribution<std::size_t> msg_dist(0, failure_messages.size() - 1);
                
                auto selected_failure = failure_messages[msg_dist(rng)];
                failure_modes_encountered.push_back(selected_failure);
                
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error(selected_failure));
            } else {
                // Success case - AppendEntries succeeded
                kythira::append_entries_response<std::uint64_t, std::uint64_t> success_response{
                    2, // term
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
            auto result = handler.execute_with_retry("append_entries", append_entries_operation).get();
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Should eventually succeed after retries
            BOOST_CHECK(result.success());
            BOOST_CHECK_EQUAL(result.term(), 2);
            BOOST_TEST_MESSAGE("✓ AppendEntries succeeded after " << attempt_count.load() << " attempts in " 
                              << total_elapsed.count() << "ms");
            
            // Property: Should make exactly failures_before_success + 1 attempts
            BOOST_CHECK_EQUAL(attempt_count.load(), failures_before_success + 1);
            
            // Property: Should handle different failure modes appropriately
            for (const auto& failure_mode : failure_modes_encountered) {
                auto classification = handler.classify_error(std::runtime_error(failure_mode));
                BOOST_TEST_MESSAGE("Failure mode: " << failure_mode << " -> should_retry=" << classification.should_retry);
                
                // Most network-related failures should be retryable
                if (failure_mode.find("timeout") != std::string::npos ||
                    failure_mode.find("refused") != std::string::npos ||
                    failure_mode.find("unreachable") != std::string::npos ||
                    failure_mode.find("Temporary") != std::string::npos) {
                    BOOST_CHECK(classification.should_retry);
                } else if (failure_mode.find("serialization") != std::string::npos) {
                    // Serialization errors should not be retryable
                    BOOST_CHECK(!classification.should_retry);
                }
            }
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("AppendEntries failed after " << attempt_count.load() << " attempts in " 
                              << total_elapsed.count() << "ms: " << e.what());
            
            // If we expected success but got failure, this might be due to max attempts exceeded
            if (failures_before_success < max_attempts) {
                // Check if failure was due to non-retryable error
                bool has_non_retryable = false;
                for (const auto& failure_mode : failure_modes_encountered) {
                    auto classification = handler.classify_error(std::runtime_error(failure_mode));
                    if (!classification.should_retry) {
                        has_non_retryable = true;
                        break;
                    }
                }
                
                if (!has_non_retryable) {
                    BOOST_FAIL("Expected success but got failure: " << e.what());
                }
            } else {
                // Property: Should respect max attempts limit
                BOOST_CHECK_LE(attempt_count.load(), max_attempts);
                BOOST_TEST_MESSAGE("✓ Correctly failed after reaching max attempts");
            }
        }
    }
    
    // Test specific AppendEntries failure scenarios
    BOOST_TEST_MESSAGE("Testing specific AppendEntries failure scenarios...");
    
    // Test 1: Log conflict handling
    {
        BOOST_TEST_MESSAGE("Test 1: Log conflict handling");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::atomic<int> attempt_count{0};
        auto log_conflict_operation = [&attempt_count]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt == 1) {
                // First attempt: log conflict (should not retry - this is protocol level)
                kythira::append_entries_response<std::uint64_t, std::uint64_t> conflict_response{
                    2, // term
                    false, // success = false (conflict)
                    std::make_optional<std::uint64_t>(1), // conflict_term
                    std::make_optional<std::uint64_t>(5)  // conflict_index
                };
                return kythira::FutureFactory::makeFuture(conflict_response);
            } else {
                // Subsequent attempts should not happen for log conflicts
                BOOST_FAIL("Should not retry on log conflict");
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error("Unexpected retry"));
            }
        };
        
        try {
            auto result = handler.execute_with_retry("append_entries", log_conflict_operation).get();
            
            // Property: Log conflicts should be returned immediately (not retried)
            BOOST_CHECK(!result.success());
            BOOST_CHECK(result.conflict_term().has_value());
            BOOST_CHECK(result.conflict_index().has_value());
            BOOST_CHECK_EQUAL(attempt_count.load(), 1);
            
            BOOST_TEST_MESSAGE("✓ Log conflict handled correctly without retry");
        } catch (const std::exception& e) {
            BOOST_FAIL("Log conflict should not throw exception: " << e.what());
        }
    }
    
    // Test 2: Term mismatch handling
    {
        BOOST_TEST_MESSAGE("Test 2: Term mismatch handling");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::atomic<int> attempt_count{0};
        auto term_mismatch_operation = [&attempt_count]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt == 1) {
                // Higher term response (should not retry - this is protocol level)
                kythira::append_entries_response<std::uint64_t, std::uint64_t> higher_term_response{
                    5, // higher term
                    false, // success = false
                    std::nullopt, // conflict_term
                    std::nullopt  // conflict_index
                };
                return kythira::FutureFactory::makeFuture(higher_term_response);
            } else {
                BOOST_FAIL("Should not retry on term mismatch");
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error("Unexpected retry"));
            }
        };
        
        try {
            auto result = handler.execute_with_retry("append_entries", term_mismatch_operation).get();
            
            // Property: Term mismatches should be returned immediately
            BOOST_CHECK(!result.success());
            BOOST_CHECK_EQUAL(result.term(), 5);
            BOOST_CHECK_EQUAL(attempt_count.load(), 1);
            
            BOOST_TEST_MESSAGE("✓ Term mismatch handled correctly without retry");
        } catch (const std::exception& e) {
            BOOST_FAIL("Term mismatch should not throw exception: " << e.what());
        }
    }
    
    // Test 3: Network vs Protocol error distinction
    {
        BOOST_TEST_MESSAGE("Test 3: Network vs Protocol error distinction");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test different error types
        std::vector<std::pair<std::string, bool>> error_scenarios = {
            {"Network timeout occurred", true},           // Should retry
            {"Connection refused", true},                 // Should retry
            {"Network is unreachable", true},            // Should retry
            {"Temporary failure", true},                  // Should retry
            {"serialization error", false},              // Should not retry
            {"protocol violation", false},               // Should not retry
            {"invalid format", false}                    // Should not retry
        };
        
        for (const auto& [error_msg, should_retry] : error_scenarios) {
            BOOST_TEST_MESSAGE("Testing error: " << error_msg << " (should_retry=" << should_retry << ")");
            
            std::atomic<int> attempt_count{0};
            auto error_operation = [&attempt_count, error_msg]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
                ++attempt_count;
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error(error_msg));
            };
            
            try {
                handler.execute_with_retry("append_entries", error_operation).get();
                BOOST_FAIL("Expected exception for error: " << error_msg);
            } catch (const std::exception& e) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                
                // Property: Error classification should match expected retry behavior
                BOOST_CHECK_EQUAL(classification.should_retry, should_retry);
                
                if (should_retry) {
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
    
    // Test 4: Backoff progression for AppendEntries
    {
        BOOST_TEST_MESSAGE("Test 4: Backoff progression for AppendEntries");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy backoff_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{400},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0, // No jitter for predictable timing
            .max_attempts = 4
        };
        
        handler.set_retry_policy("append_entries", backoff_policy);
        
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::atomic<int> attempt_count{0};
        
        auto backoff_test_operation = [&attempt_count, &attempt_times]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            attempt_times.push_back(std::chrono::steady_clock::now());
            int current_attempt = ++attempt_count;
            
            if (current_attempt < 4) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error("Network timeout occurred"));
            } else {
                kythira::append_entries_response<std::uint64_t, std::uint64_t> success_response{
                    1, true, std::nullopt, std::nullopt
                };
                return kythira::FutureFactory::makeFuture(success_response);
            }
        };
        
        try {
            auto result = handler.execute_with_retry("append_entries", backoff_test_operation).get();
            
            BOOST_CHECK(result.success());
            BOOST_CHECK_EQUAL(attempt_count.load(), 4);
            
            // Property: Should follow exponential backoff pattern
            // Expected delays: 0, 50ms, 100ms, 200ms
            if (attempt_times.size() >= 4) {
                auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[1] - attempt_times[0]);
                auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[2] - attempt_times[1]);
                auto delay3 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[3] - attempt_times[2]);
                
                BOOST_TEST_MESSAGE("Delays: " << delay1.count() << "ms, " << delay2.count() << "ms, " << delay3.count() << "ms");
                
                // Allow some timing variance (±20ms)
                BOOST_CHECK_GE(delay1.count(), 30);
                BOOST_CHECK_LE(delay1.count(), 70);
                BOOST_CHECK_GE(delay2.count(), 80);
                BOOST_CHECK_LE(delay2.count(), 120);
                BOOST_CHECK_GE(delay3.count(), 180);
                BOOST_CHECK_LE(delay3.count(), 220);
                
                BOOST_TEST_MESSAGE("✓ Exponential backoff pattern verified");
            }
        } catch (const std::exception& e) {
            BOOST_FAIL("Backoff test should succeed: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("All AppendEntries retry handling property tests passed!");
}