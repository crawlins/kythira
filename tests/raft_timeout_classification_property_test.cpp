#define BOOST_TEST_MODULE RaftTimeoutClassificationPropertyTest

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
#include <string>

using namespace kythira;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{1000};
    constexpr std::chrono::milliseconds long_timeout{5000};
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
 * **Feature: raft-completion, Property 21: Timeout Classification**
 * 
 * Property: When RPC timeouts occur, the system distinguishes between network delays and actual failures.
 * **Validates: Requirements 4.6**
 */
BOOST_AUTO_TEST_CASE(raft_timeout_classification_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Create error handler for timeout classification
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test different timeout scenarios
        std::vector<std::string> timeout_messages = {
            "Network timeout occurred",
            "RPC timeout after 5000ms",
            "Operation timed out",
            "Request timeout",
            "Connection timeout",
            "Timeout waiting for response",
            "Network operation timeout",
            "RPC call timeout"
        };
        
        std::uniform_int_distribution<std::size_t> msg_dist(0, timeout_messages.size() - 1);
        auto selected_timeout = timeout_messages[msg_dist(gen)];
        
        BOOST_TEST_MESSAGE("Testing timeout message: " << selected_timeout);
        
        // Property: All timeout messages should be classified as network timeouts
        auto classification = handler.classify_error(std::runtime_error(selected_timeout));
        
        BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
        BOOST_CHECK(classification.should_retry);
        BOOST_CHECK(!classification.description.empty());
        
        BOOST_TEST_MESSAGE("✓ Timeout classified correctly: type=" << static_cast<int>(classification.type) 
                          << ", should_retry=" << classification.should_retry 
                          << ", description=" << classification.description);
    }
    
    // Test specific timeout classification scenarios
    BOOST_TEST_MESSAGE("Testing specific timeout classification scenarios...");
    
    // Test 1: Timeout vs other network errors
    {
        BOOST_TEST_MESSAGE("Test 1: Timeout vs other network errors");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<std::pair<std::string, typename error_handler<int>::error_type>> error_scenarios = {
            {"Network timeout occurred", error_handler<int>::error_type::network_timeout},
            {"Connection timeout", error_handler<int>::error_type::network_timeout},
            {"RPC timeout after 1000ms", error_handler<int>::error_type::network_timeout},
            {"Operation timed out", error_handler<int>::error_type::network_timeout},
            {"Connection refused", error_handler<int>::error_type::connection_refused},
            {"Network is unreachable", error_handler<int>::error_type::network_unreachable},
            {"No route to host", error_handler<int>::error_type::network_unreachable},
            {"Temporary failure", error_handler<int>::error_type::temporary_failure}
        };
        
        for (const auto& [error_msg, expected_type] : error_scenarios) {
            auto classification = handler.classify_error(std::runtime_error(error_msg));
            
            // Property: Error classification should match expected type
            BOOST_CHECK_EQUAL(classification.type, expected_type);
            
            // Property: All network-related errors should be retryable
            BOOST_CHECK(classification.should_retry);
            
            BOOST_TEST_MESSAGE("✓ " << error_msg << " -> " << static_cast<int>(classification.type));
        }
    }
    
    // Test 2: Timeout duration inference
    {
        BOOST_TEST_MESSAGE("Test 2: Timeout duration inference");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<std::string> timeout_with_duration = {
            "RPC timeout after 100ms",
            "Network timeout occurred after 500ms",
            "Operation timed out (1000ms)",
            "Request timeout: 2000ms elapsed",
            "Connection timeout after 5000ms",
            "Timeout waiting for response (10000ms)"
        };
        
        for (const auto& timeout_msg : timeout_with_duration) {
            auto classification = handler.classify_error(std::runtime_error(timeout_msg));
            
            // Property: Timeouts with duration info should still be classified as timeouts
            BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
            BOOST_CHECK(classification.should_retry);
            
            // Property: Description should contain relevant information
            BOOST_CHECK(classification.description.find("timeout") != std::string::npos ||
                       classification.description.find("Timeout") != std::string::npos);
            
            BOOST_TEST_MESSAGE("✓ Duration-specific timeout: " << timeout_msg);
        }
    }
    
    // Test 3: Timeout vs permanent failures
    {
        BOOST_TEST_MESSAGE("Test 3: Timeout vs permanent failures");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<std::pair<std::string, bool>> timeout_vs_permanent = {
            {"Network timeout occurred", true},              // Timeout - should retry
            {"Connection timeout", true},                    // Timeout - should retry
            {"RPC timeout", true},                          // Timeout - should retry
            {"serialization error", false},                 // Permanent - should not retry
            {"protocol violation", false},                  // Permanent - should not retry
            {"invalid format", false},                      // Permanent - should not retry
            {"authentication failed", false},               // Permanent - should not retry
            {"permission denied", false}                    // Permanent - should not retry
        };
        
        for (const auto& [error_msg, is_timeout] : timeout_vs_permanent) {
            auto classification = handler.classify_error(std::runtime_error(error_msg));
            
            if (is_timeout) {
                // Property: Timeouts should be retryable
                BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
                BOOST_CHECK(classification.should_retry);
            } else {
                // Property: Permanent failures should not be retryable
                BOOST_CHECK_NE(classification.type, error_handler<int>::error_type::network_timeout);
                BOOST_CHECK(!classification.should_retry);
            }
            
            BOOST_TEST_MESSAGE("✓ " << error_msg << " -> timeout=" << is_timeout 
                              << ", should_retry=" << classification.should_retry);
        }
    }
    
    // Test 4: Timeout retry behavior
    {
        BOOST_TEST_MESSAGE("Test 4: Timeout retry behavior");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        typename error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy timeout_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{800},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0, // No jitter for predictable timing
            .max_attempts = 4
        };
        
        handler.set_retry_policy("append_entries", timeout_policy);
        
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::atomic<int> attempt_count{0};
        
        auto timeout_retry_operation = [&attempt_count, &attempt_times]() -> kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>> {
            attempt_times.push_back(std::chrono::steady_clock::now());
            int current_attempt = ++attempt_count;
            
            if (current_attempt < 3) {
                // Simulate different timeout scenarios
                std::vector<std::string> timeout_errors = {
                    "Network timeout occurred",
                    "RPC timeout after 1000ms",
                    "Connection timeout"
                };
                
                std::random_device rd;
                std::mt19937 rng(rd());
                std::uniform_int_distribution<std::size_t> error_dist(0, timeout_errors.size() - 1);
                
                return kythira::FutureFactory::makeExceptionalFuture<kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error(timeout_errors[error_dist(rng)]));
            } else {
                kythira::append_entries_response<std::uint64_t, std::uint64_t> success_response{
                    1, true, std::nullopt, std::nullopt
                };
                return kythira::FutureFactory::makeFuture(success_response);
            }
        };
        
        try {
            auto result = handler.execute_with_retry("append_entries", timeout_retry_operation).get();
            
            BOOST_CHECK(result.success());
            BOOST_CHECK_EQUAL(attempt_count.load(), 3);
            
            // Property: Timeout retries should follow exponential backoff
            if (attempt_times.size() >= 3) {
                auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[1] - attempt_times[0]);
                auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[2] - attempt_times[1]);
                
                BOOST_TEST_MESSAGE("Timeout retry delays: " << delay1.count() << "ms, " << delay2.count() << "ms");
                
                // Expected: 50ms, 100ms (with timing tolerance)
                BOOST_CHECK_GE(delay1.count(), 30);
                BOOST_CHECK_LE(delay1.count(), 70);
                BOOST_CHECK_GE(delay2.count(), 80);
                BOOST_CHECK_LE(delay2.count(), 120);
                
                BOOST_TEST_MESSAGE("✓ Timeout retry backoff pattern verified");
            }
        } catch (const std::exception& e) {
            BOOST_FAIL("Timeout retry test should succeed: " << e.what());
        }
    }
    
    // Test 5: Context-specific timeout handling
    {
        BOOST_TEST_MESSAGE("Test 5: Context-specific timeout handling");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test different RPC contexts with timeouts
        std::vector<std::pair<std::string, std::string>> context_timeouts = {
            {"heartbeat", "Heartbeat timeout occurred"},
            {"append_entries", "AppendEntries RPC timeout"},
            {"request_vote", "Vote request timeout"},
            {"install_snapshot", "Snapshot transfer timeout"}
        };
        
        for (const auto& [context, timeout_msg] : context_timeouts) {
            auto classification = handler.classify_error(std::runtime_error(timeout_msg));
            
            // Property: Context-specific timeouts should be classified consistently
            BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
            BOOST_CHECK(classification.should_retry);
            
            BOOST_TEST_MESSAGE("✓ " << context << " timeout classified correctly");
        }
    }
    
    // Test 6: Timeout pattern recognition
    {
        BOOST_TEST_MESSAGE("Test 6: Timeout pattern recognition");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test various timeout message patterns
        std::vector<std::string> timeout_patterns = {
            "timeout",                          // Simple timeout
            "TIMEOUT",                          // Uppercase
            "Timeout",                          // Capitalized
            "timed out",                        // Past tense
            "time out",                         // Separated words
            "operation timeout",                // With context
            "network timeout occurred",         // Full sentence
            "RPC call timed out after 5s",    // With duration
            "Connection timeout (10000ms)",     // With parentheses
            "Request timeout: operation failed" // With colon
        };
        
        for (const auto& pattern : timeout_patterns) {
            auto classification = handler.classify_error(std::runtime_error(pattern));
            
            // Property: All timeout patterns should be recognized
            BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
            BOOST_CHECK(classification.should_retry);
            
            BOOST_TEST_MESSAGE("✓ Pattern recognized: " << pattern);
        }
    }
    
    // Test 7: Non-timeout error classification
    {
        BOOST_TEST_MESSAGE("Test 7: Non-timeout error classification");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test errors that might be confused with timeouts but aren't
        std::vector<std::pair<std::string, typename error_handler<int>::error_type>> non_timeout_errors = {
            {"Connection refused", error_handler<int>::error_type::connection_refused},
            {"Network is unreachable", error_handler<int>::error_type::network_unreachable},
            {"serialization error", error_handler<int>::error_type::serialization_error},
            {"protocol violation", error_handler<int>::error_type::protocol_error},
            {"invalid format", error_handler<int>::error_type::serialization_error},
            {"parse error", error_handler<int>::error_type::serialization_error},
            {"Temporary failure", error_handler<int>::error_type::temporary_failure},
            {"try again later", error_handler<int>::error_type::temporary_failure}
        };
        
        for (const auto& [error_msg, expected_type] : non_timeout_errors) {
            auto classification = handler.classify_error(std::runtime_error(error_msg));
            
            // Property: Non-timeout errors should not be classified as timeouts
            BOOST_CHECK_NE(classification.type, error_handler<int>::error_type::network_timeout);
            BOOST_CHECK_EQUAL(classification.type, expected_type);
            
            BOOST_TEST_MESSAGE("✓ Non-timeout error: " << error_msg 
                              << " -> " << static_cast<int>(classification.type));
        }
    }
    
    // Test 8: Timeout classification consistency
    {
        BOOST_TEST_MESSAGE("Test 8: Timeout classification consistency");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        // Test that the same timeout message is classified consistently
        std::string consistent_timeout = "Network timeout occurred";
        
        for (int i = 0; i < 10; ++i) {
            auto classification = handler.classify_error(std::runtime_error(consistent_timeout));
            
            // Property: Classification should be consistent across calls
            BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
            BOOST_CHECK(classification.should_retry);
            BOOST_CHECK_EQUAL(classification.description, "Network operation timed out");
        }
        
        BOOST_TEST_MESSAGE("✓ Timeout classification is consistent");
    }
    
    // Test 9: Edge cases in timeout detection
    {
        BOOST_TEST_MESSAGE("Test 9: Edge cases in timeout detection");
        error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> handler;
        
        std::vector<std::pair<std::string, bool>> edge_cases = {
            {"timeout", true},                              // Just the word
            {"TIMEOUT", true},                              // All caps
            {"TimeOut", true},                              // Mixed case
            {"time-out", true},                             // Hyphenated
            {"time_out", true},                             // Underscore
            {"timed-out", true},                            // Past tense hyphenated
            {"timing out", false},                          // Present continuous (not a timeout)
            {"timeout value", false},                       // Configuration context
            {"set timeout", false},                         // Command context
            {"timeout parameter", false},                   // Parameter context
            {"network timeout", true},                      // With qualifier
            {"operation timeout", true},                    // With operation
            {"timeout error", true},                        // With error
            {"timeout occurred", true},                     // With occurrence
            {"timeout detected", true}                      // With detection
        };
        
        for (const auto& [error_msg, should_be_timeout] : edge_cases) {
            auto classification = handler.classify_error(std::runtime_error(error_msg));
            
            if (should_be_timeout) {
                // Property: Should be classified as timeout
                BOOST_CHECK_EQUAL(classification.type, error_handler<int>::error_type::network_timeout);
                BOOST_CHECK(classification.should_retry);
            } else {
                // Property: Should not be classified as timeout
                BOOST_CHECK_NE(classification.type, error_handler<int>::error_type::network_timeout);
            }
            
            BOOST_TEST_MESSAGE("Edge case: " << error_msg 
                              << " -> timeout=" << (classification.type == error_handler<int>::error_type::network_timeout));
        }
    }
    
    BOOST_TEST_MESSAGE("All timeout classification property tests passed!");
}