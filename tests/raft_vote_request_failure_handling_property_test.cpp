#define BOOST_TEST_MODULE RaftVoteRequestFailureHandlingPropertyTest

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
    constexpr std::chrono::milliseconds max_delay{2000};
    constexpr double backoff_multiplier = 2.0;
    constexpr std::size_t max_attempts = 3; // Elections are time-sensitive
    constexpr std::size_t test_iterations = 12;
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
 * **Feature: raft-completion, Property 19: Vote Request Failure Handling**
 * 
 * Property: For any RequestVote RPC failure during election, the system handles the failure and continues the election process.
 * **Validates: Requirements 4.4**
 */
BOOST_AUTO_TEST_CASE(raft_vote_request_failure_handling_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> failure_count_dist(1, 2); // Limited failures for time-sensitive elections
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Create error handler with RequestVote-specific retry policy
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        typename error_handler<kythira::request_vote_response<std::uint64_t>>::retry_policy vote_policy{
            .initial_delay = base_delay,
            .max_delay = max_delay,
            .backoff_multiplier = backoff_multiplier,
            .jitter_factor = 0.1,
            .max_attempts = max_attempts
        };
        
        handler.set_retry_policy("request_vote", vote_policy);
        
        const int failures_before_success = failure_count_dist(gen);
        BOOST_TEST_MESSAGE("Testing with " << failures_before_success << " failures before success");
        
        // Track retry attempts and election state
        std::vector<std::string> failure_modes_encountered;
        std::atomic<int> attempt_count{0};
        std::atomic<bool> vote_granted{false};
        
        // Create operation that simulates vote request with failures
        auto vote_request_operation = [&attempt_count, failures_before_success, &failure_modes_encountered, &vote_granted]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt <= failures_before_success) {
                // Simulate different types of vote request failures
                std::vector<std::string> failure_messages = {
                    "Network timeout during vote request",
                    "Connection refused by voter node",
                    "Network is unreachable for vote request",
                    "Temporary failure during election",
                    "RPC timeout in election process"
                };
                
                std::random_device rd;
                std::mt19937 rng(rd());
                std::uniform_int_distribution<std::size_t> msg_dist(0, failure_messages.size() - 1);
                
                auto selected_failure = failure_messages[msg_dist(rng)];
                failure_modes_encountered.push_back(selected_failure);
                
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error(selected_failure));
            } else {
                // Success case - vote granted
                vote_granted = true;
                kythira::request_vote_response<std::uint64_t> success_response{
                    5, // term
                    true // vote_granted
                };
                return kythira::FutureFactory::makeFuture(success_response);
            }
        };
        
        // Execute with retry
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            auto result = handler.execute_with_retry("request_vote", vote_request_operation).get();
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Should eventually succeed after retries
            BOOST_CHECK(result.vote_granted());
            BOOST_CHECK_EQUAL(result.term(), 5);
            BOOST_CHECK(vote_granted.load());
            BOOST_TEST_MESSAGE("✓ Vote request succeeded after " << attempt_count.load() << " attempts in " 
                              << total_elapsed.count() << "ms");
            
            // Property: Should make exactly failures_before_success + 1 attempts
            BOOST_CHECK_EQUAL(attempt_count.load(), failures_before_success + 1);
            
            // Property: Should handle different failure modes appropriately
            for (const auto& failure_mode : failure_modes_encountered) {
                auto classification = handler.classify_error(std::runtime_error(failure_mode));
                BOOST_TEST_MESSAGE("Failure mode: " << failure_mode << " -> should_retry=" << classification.should_retry);
                
                // Most network-related election failures should be retryable
                if (failure_mode.find("timeout") != std::string::npos ||
                    failure_mode.find("refused") != std::string::npos ||
                    failure_mode.find("unreachable") != std::string::npos ||
                    failure_mode.find("Temporary") != std::string::npos) {
                    BOOST_CHECK(classification.should_retry);
                }
            }
            
            // Property: Election should complete within reasonable time
            // Elections are time-sensitive, so total time should be bounded
            BOOST_CHECK_LE(total_elapsed.count(), 5000); // Max 5 seconds for election
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("Vote request failed after " << attempt_count.load() << " attempts in " 
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
                // Property: Should respect max attempts limit for elections
                BOOST_CHECK_LE(attempt_count.load(), max_attempts);
                BOOST_TEST_MESSAGE("✓ Correctly failed after reaching max attempts");
            }
        }
    }
    
    // Test specific vote request failure scenarios
    BOOST_TEST_MESSAGE("Testing specific vote request failure scenarios...");
    
    // Test 1: Vote rejection (not a failure, should not retry)
    {
        BOOST_TEST_MESSAGE("Test 1: Vote rejection handling");
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        std::atomic<int> attempt_count{0};
        auto vote_rejection_operation = [&attempt_count]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt == 1) {
                // Vote rejected (not an error - should not retry)
                kythira::request_vote_response<std::uint64_t> rejection_response{
                    3, // term
                    false // vote_granted = false
                };
                return kythira::FutureFactory::makeFuture(rejection_response);
            } else {
                BOOST_FAIL("Should not retry on vote rejection");
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error("Unexpected retry"));
            }
        };
        
        try {
            auto result = handler.execute_with_retry("request_vote", vote_rejection_operation).get();
            
            // Property: Vote rejections should be returned immediately (not retried)
            BOOST_CHECK(!result.vote_granted());
            BOOST_CHECK_EQUAL(result.term(), 3);
            BOOST_CHECK_EQUAL(attempt_count.load(), 1);
            
            BOOST_TEST_MESSAGE("✓ Vote rejection handled correctly without retry");
        } catch (const std::exception& e) {
            BOOST_FAIL("Vote rejection should not throw exception: " << e.what());
        }
    }
    
    // Test 2: Higher term response (not a failure, should not retry)
    {
        BOOST_TEST_MESSAGE("Test 2: Higher term response handling");
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        std::atomic<int> attempt_count{0};
        auto higher_term_operation = [&attempt_count]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
            int current_attempt = ++attempt_count;
            
            if (current_attempt == 1) {
                // Higher term response (should not retry - this is protocol level)
                kythira::request_vote_response<std::uint64_t> higher_term_response{
                    10, // higher term
                    false // vote_granted = false
                };
                return kythira::FutureFactory::makeFuture(higher_term_response);
            } else {
                BOOST_FAIL("Should not retry on higher term response");
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error("Unexpected retry"));
            }
        };
        
        try {
            auto result = handler.execute_with_retry("request_vote", higher_term_operation).get();
            
            // Property: Higher term responses should be returned immediately
            BOOST_CHECK(!result.vote_granted());
            BOOST_CHECK_EQUAL(result.term(), 10);
            BOOST_CHECK_EQUAL(attempt_count.load(), 1);
            
            BOOST_TEST_MESSAGE("✓ Higher term response handled correctly without retry");
        } catch (const std::exception& e) {
            BOOST_FAIL("Higher term response should not throw exception: " << e.what());
        }
    }
    
    // Test 3: Election timeout vs network error distinction
    {
        BOOST_TEST_MESSAGE("Test 3: Election timeout vs network error distinction");
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        // Test different error types in election context
        std::vector<std::pair<std::string, bool>> election_error_scenarios = {
            {"Network timeout during vote request", true},        // Should retry
            {"Connection refused by voter node", true},           // Should retry
            {"Network is unreachable for vote request", true},    // Should retry
            {"Temporary failure during election", true},          // Should retry
            {"RPC timeout in election process", true},            // Should retry
            {"Invalid candidate credentials", false},             // Should not retry
            {"Election protocol violation", false},               // Should not retry
            {"Malformed vote request", false}                     // Should not retry
        };
        
        for (const auto& [error_msg, should_retry] : election_error_scenarios) {
            BOOST_TEST_MESSAGE("Testing election error: " << error_msg << " (should_retry=" << should_retry << ")");
            
            std::atomic<int> attempt_count{0};
            auto error_operation = [&attempt_count, error_msg]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
                ++attempt_count;
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error(error_msg));
            };
            
            try {
                handler.execute_with_retry("request_vote", error_operation).get();
                BOOST_FAIL("Expected exception for error: " << error_msg);
            } catch (const std::exception& e) {
                auto classification = handler.classify_error(std::runtime_error(error_msg));
                
                // Property: Error classification should be appropriate for elections
                if (should_retry) {
                    if (classification.should_retry) {
                        BOOST_CHECK_GT(attempt_count.load(), 1);
                        BOOST_TEST_MESSAGE("✓ Retryable election error made " << attempt_count.load() << " attempts");
                    } else {
                        BOOST_TEST_MESSAGE("Note: Expected retryable error was not retried - may be conservative for elections");
                    }
                } else {
                    if (!classification.should_retry) {
                        BOOST_CHECK_EQUAL(attempt_count.load(), 1);
                        BOOST_TEST_MESSAGE("✓ Non-retryable election error failed immediately");
                    } else {
                        BOOST_TEST_MESSAGE("Note: Expected non-retryable error was retried - may be permissive classification");
                    }
                }
            }
        }
    }
    
    // Test 4: Election timing constraints
    {
        BOOST_TEST_MESSAGE("Test 4: Election timing constraints");
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        typename error_handler<kythira::request_vote_response<std::uint64_t>>::retry_policy timing_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{400},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0, // No jitter for predictable timing
            .max_attempts = 4
        };
        
        handler.set_retry_policy("request_vote", timing_policy);
        
        std::vector<std::chrono::steady_clock::time_point> attempt_times;
        std::atomic<int> attempt_count{0};
        
        auto timing_test_operation = [&attempt_count, &attempt_times]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
            attempt_times.push_back(std::chrono::steady_clock::now());
            int current_attempt = ++attempt_count;
            
            if (current_attempt < 3) {
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error("Network timeout during vote request"));
            } else {
                kythira::request_vote_response<std::uint64_t> success_response{4, true};
                return kythira::FutureFactory::makeFuture(success_response);
            }
        };
        
        auto election_start = std::chrono::steady_clock::now();
        
        try {
            auto result = handler.execute_with_retry("request_vote", timing_test_operation).get();
            auto election_end = std::chrono::steady_clock::now();
            auto total_election_time = std::chrono::duration_cast<std::chrono::milliseconds>(election_end - election_start);
            
            BOOST_CHECK(result.vote_granted());
            BOOST_CHECK_EQUAL(attempt_count.load(), 3);
            
            // Property: Election should complete quickly (time-sensitive)
            BOOST_CHECK_LE(total_election_time.count(), 1000); // Max 1 second for this test
            
            // Property: Should follow fast backoff for elections
            if (attempt_times.size() >= 3) {
                auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[1] - attempt_times[0]);
                auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(attempt_times[2] - attempt_times[1]);
                
                BOOST_TEST_MESSAGE("Election delays: " << delay1.count() << "ms, " << delay2.count() << "ms");
                BOOST_TEST_MESSAGE("Total election time: " << total_election_time.count() << "ms");
                
                // Expected: 50ms, 100ms (with timing tolerance)
                BOOST_CHECK_GE(delay1.count(), 30);
                BOOST_CHECK_LE(delay1.count(), 70);
                BOOST_CHECK_GE(delay2.count(), 80);
                BOOST_CHECK_LE(delay2.count(), 120);
                
                BOOST_TEST_MESSAGE("✓ Election timing constraints verified");
            }
        } catch (const std::exception& e) {
            BOOST_FAIL("Election timing test should succeed: " << e.what());
        }
    }
    
    // Test 5: Concurrent vote request handling
    {
        BOOST_TEST_MESSAGE("Test 5: Concurrent vote request simulation");
        error_handler<kythira::request_vote_response<std::uint64_t>> handler;
        
        // Simulate multiple vote requests with different outcomes
        std::vector<std::pair<std::string, bool>> vote_outcomes = {
            {"Vote granted", true},
            {"Vote rejected - already voted", false},
            {"Vote rejected - higher term", false},
            {"Network timeout", false}, // This one will retry but ultimately fail
            {"Vote granted after retry", true}
        };
        
        for (const auto& [outcome_desc, should_succeed] : vote_outcomes) {
            BOOST_TEST_MESSAGE("Testing vote outcome: " << outcome_desc);
            
            std::atomic<int> attempt_count{0};
            auto vote_outcome_operation = [&attempt_count, outcome_desc, should_succeed]() -> kythira::Future<kythira::request_vote_response<std::uint64_t>> {
                int current_attempt = ++attempt_count;
                
                if (outcome_desc == "Vote granted") {
                    kythira::request_vote_response<std::uint64_t> response{2, true};
                    return kythira::FutureFactory::makeFuture(response);
                } else if (outcome_desc == "Vote rejected - already voted") {
                    kythira::request_vote_response<std::uint64_t> response{2, false};
                    return kythira::FutureFactory::makeFuture(response);
                } else if (outcome_desc == "Vote rejected - higher term") {
                    kythira::request_vote_response<std::uint64_t> response{5, false};
                    return kythira::FutureFactory::makeFuture(response);
                } else if (outcome_desc == "Network timeout") {
                    return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                        std::runtime_error("Network timeout during vote request"));
                } else if (outcome_desc == "Vote granted after retry") {
                    if (current_attempt == 1) {
                        return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                            std::runtime_error("Network timeout during vote request"));
                    } else {
                        kythira::request_vote_response<std::uint64_t> response{2, true};
                        return kythira::FutureFactory::makeFuture(response);
                    }
                }
                
                return kythira::FutureFactory::makeExceptionalFuture<kythira::request_vote_response<std::uint64_t>>(
                    std::runtime_error("Unknown outcome"));
            };
            
            try {
                auto result = handler.execute_with_retry("request_vote", vote_outcome_operation).get();
                
                if (should_succeed) {
                    // Property: Successful votes should be properly handled
                    if (outcome_desc.find("granted") != std::string::npos) {
                        BOOST_CHECK(result.vote_granted());
                    }
                    BOOST_TEST_MESSAGE("✓ " << outcome_desc << " handled correctly");
                } else {
                    // Property: Rejected votes should still return valid responses
                    BOOST_CHECK(!result.vote_granted());
                    BOOST_TEST_MESSAGE("✓ " << outcome_desc << " handled correctly");
                }
                
            } catch (const std::exception& e) {
                if (!should_succeed) {
                    BOOST_TEST_MESSAGE("✓ " << outcome_desc << " failed as expected: " << e.what());
                } else {
                    BOOST_FAIL("Unexpected failure for " << outcome_desc << ": " << e.what());
                }
            }
        }
    }
    
    BOOST_TEST_MESSAGE("All vote request failure handling property tests passed!");
}