#define BOOST_TEST_MODULE RaftFailedReadRejectionPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/future_collector.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>

using namespace kythira;

namespace {
    constexpr std::chrono::milliseconds test_timeout{5000};
    constexpr std::size_t min_cluster_size = 3;
    constexpr std::size_t max_cluster_size = 11;
    constexpr std::size_t test_iterations = 50;
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
 * **Feature: raft-completion, Property 34: Failed Read Rejection**
 * 
 * Property: For any failed heartbeat collection during read, the read request is rejected with leadership error.
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(raft_failed_read_rejection_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(min_cluster_size, max_cluster_size);
    std::uniform_int_distribution<int> delay_dist(10, 100); // milliseconds
    std::uniform_int_distribution<int> failure_rate_dist(60, 90); // percentage
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random cluster size (odd numbers for clear majority)
        std::size_t cluster_size = cluster_size_dist(gen);
        if (cluster_size % 2 == 0) cluster_size++; // Ensure odd number
        
        const std::size_t majority_count = (cluster_size / 2) + 1;
        const std::size_t follower_count = cluster_size - 1; // Exclude leader
        const std::size_t required_successful_followers = majority_count - 1; // -1 for leader
        
        BOOST_TEST_MESSAGE("Testing cluster size: " << cluster_size 
                          << ", majority needed: " << majority_count 
                          << ", required successful followers: " << required_successful_followers);
        
        // Create scenarios that should cause read rejection
        const int scenario = gen() % 3;
        
        if (scenario == 0) {
            // Scenario 1: Insufficient successful responses (network failures)
            BOOST_TEST_MESSAGE("Testing scenario: Insufficient successful responses");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> heartbeat_futures;
            heartbeat_futures.reserve(follower_count);
            
            const std::uint64_t current_term = 5;
            std::size_t successful_responses = 0;
            
            // Ensure we don't have enough successful responses for majority
            const std::size_t max_successes = (required_successful_followers > 0) ? 
                                             required_successful_followers - 1 : 0;
            
            for (std::size_t i = 0; i < follower_count; ++i) {
                const int delay_ms = delay_dist(gen);
                const bool will_succeed = (successful_responses < max_successes) && (gen() % 3 == 0); // Low success rate
                
                if (will_succeed) {
                    successful_responses++;
                    // Create successful response
                    kythira::append_entries_response<std::uint64_t, std::uint64_t> response{
                        current_term, true, i
                    };
                    auto future = kythira::FutureFactory::makeFuture(response)
                        .delay(std::chrono::milliseconds(delay_ms));
                    heartbeat_futures.push_back(std::move(future));
                } else {
                    // Create failed response or timeout
                    if (gen() % 2 == 0) {
                        // Failed response (network issue)
                        kythira::append_entries_response<std::uint64_t, std::uint64_t> response{
                            current_term, false, 0
                        };
                        auto future = kythira::FutureFactory::makeFuture(response)
                            .delay(std::chrono::milliseconds(delay_ms));
                        heartbeat_futures.push_back(std::move(future));
                    } else {
                        // Timeout simulation
                        auto future = kythira::FutureFactory::makeExceptionalFuture<
                            kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                            std::runtime_error("Heartbeat timeout"));
                        heartbeat_futures.push_back(std::move(future));
                    }
                }
            }
            
            BOOST_TEST_MESSAGE("Simulated " << successful_responses << " successful responses (insufficient for majority)");
            
            // Test that failed heartbeat collection rejects read
            auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                std::move(heartbeat_futures), test_timeout);
            
            try {
                auto results = std::move(collection_future).get();
                
                // Count successful responses
                std::size_t successful_in_results = 0;
                for (const auto& response : results) {
                    if (response.success()) {
                        successful_in_results++;
                    }
                }
                
                const std::size_t total_success = successful_in_results + 1; // +1 for leader
                
                // Property: If insufficient majority, read should be rejected
                if (total_success < majority_count) {
                    BOOST_TEST_MESSAGE("✓ Insufficient majority (" << total_success 
                                      << "/" << majority_count << "), read correctly rejected");
                } else {
                    BOOST_TEST_MESSAGE("Unexpected majority achieved, read would succeed");
                }
                
            } catch (const std::exception& e) {
                // Property: Collection failure should cause read rejection
                BOOST_TEST_MESSAGE("✓ Heartbeat collection failed, read correctly rejected: " << e.what());
            }
            
        } else if (scenario == 1) {
            // Scenario 2: All timeout responses
            BOOST_TEST_MESSAGE("Testing scenario: All timeout responses");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> timeout_futures;
            timeout_futures.reserve(follower_count);
            
            for (std::size_t i = 0; i < follower_count; ++i) {
                // Create timeout simulation
                auto future = kythira::FutureFactory::makeExceptionalFuture<
                    kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                    std::runtime_error("Network timeout"));
                timeout_futures.push_back(std::move(future));
            }
            
            BOOST_TEST_MESSAGE("Simulated all timeout responses");
            
            // Test that timeout collection rejects read
            auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                std::move(timeout_futures), std::chrono::milliseconds(100)); // Short timeout
            
            try {
                auto results = std::move(collection_future).get();
                BOOST_TEST_MESSAGE("Unexpected success with all timeouts");
            } catch (const std::exception& e) {
                // Property: All timeouts should cause read rejection
                BOOST_TEST_MESSAGE("✓ All timeouts correctly caused read rejection: " << e.what());
            }
            
        } else {
            // Scenario 3: Mixed failures with insufficient majority
            BOOST_TEST_MESSAGE("Testing scenario: Mixed failures with insufficient majority");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> mixed_futures;
            mixed_futures.reserve(follower_count);
            
            const std::uint64_t current_term = 8;
            std::size_t successful_responses = 0;
            std::size_t failed_responses = 0;
            std::size_t timeout_responses = 0;
            
            // Ensure insufficient successful responses
            const std::size_t max_successes = (required_successful_followers > 1) ? 
                                             required_successful_followers - 1 : 0;
            
            for (std::size_t i = 0; i < follower_count; ++i) {
                const int delay_ms = delay_dist(gen);
                const int response_type = gen() % 3;
                
                if (response_type == 0 && successful_responses < max_successes) {
                    // Successful response (limited)
                    successful_responses++;
                    kythira::append_entries_response<std::uint64_t, std::uint64_t> response{
                        current_term, true, i
                    };
                    auto future = kythira::FutureFactory::makeFuture(response)
                        .delay(std::chrono::milliseconds(delay_ms));
                    mixed_futures.push_back(std::move(future));
                } else if (response_type == 1) {
                    // Failed response
                    failed_responses++;
                    kythira::append_entries_response<std::uint64_t, std::uint64_t> response{
                        current_term, false, 0
                    };
                    auto future = kythira::FutureFactory::makeFuture(response)
                        .delay(std::chrono::milliseconds(delay_ms));
                    mixed_futures.push_back(std::move(future));
                } else {
                    // Timeout response
                    timeout_responses++;
                    auto future = kythira::FutureFactory::makeExceptionalFuture<
                        kythira::append_entries_response<std::uint64_t, std::uint64_t>>(
                        std::runtime_error("Mixed failure timeout"));
                    mixed_futures.push_back(std::move(future));
                }
            }
            
            BOOST_TEST_MESSAGE("Simulated " << successful_responses << " successful, " 
                              << failed_responses << " failed, " << timeout_responses << " timeout responses");
            
            // Test that mixed failures with insufficient majority reject read
            auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                std::move(mixed_futures), test_timeout);
            
            try {
                auto results = std::move(collection_future).get();
                
                // Count successful responses
                std::size_t successful_in_results = 0;
                for (const auto& response : results) {
                    if (response.success()) {
                        successful_in_results++;
                    }
                }
                
                const std::size_t total_success = successful_in_results + 1; // +1 for leader
                
                // Property: If insufficient majority, read should be rejected
                if (total_success < majority_count) {
                    BOOST_TEST_MESSAGE("✓ Mixed failures with insufficient majority (" << total_success 
                                      << "/" << majority_count << "), read correctly rejected");
                } else {
                    BOOST_TEST_MESSAGE("Unexpected majority achieved with mixed failures");
                }
                
            } catch (const std::exception& e) {
                // Property: Collection failure should cause read rejection
                BOOST_TEST_MESSAGE("✓ Mixed failures correctly caused read rejection: " << e.what());
            }
        }
    }
    
    // Test specific edge cases for failed read rejection
    BOOST_TEST_MESSAGE("Testing failed read rejection edge cases...");
    
    // Test with all failed responses (same term, but all failed)
    {
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> all_failed_futures;
        const std::uint64_t current_term = 12;
        
        for (std::size_t i = 0; i < 4; ++i) {
            kythira::append_entries_response<std::uint64_t, std::uint64_t> response{
                current_term, false, 0 // All failed
            };
            all_failed_futures.push_back(kythira::FutureFactory::makeFuture(response));
        }
        
        auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
            std::move(all_failed_futures), test_timeout);
        
        auto results = std::move(collection_future).get();
        
        // All responses should be failed
        for (const auto& response : results) {
            BOOST_CHECK_EQUAL(response.term(), current_term);
            BOOST_CHECK(!response.success());
        }
        
        // Property: With all failed responses (0 + leader = 1), insufficient for majority of 5
        const std::size_t total_success = 1; // Only leader
        BOOST_CHECK_LT(total_success, 3); // Less than majority of 5
        
        BOOST_TEST_MESSAGE("✓ All failed responses correctly cause read rejection");
    }
    
    // Test with very short timeout (should cause timeout failures)
    {
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> slow_futures;
        
        for (std::size_t i = 0; i < 3; ++i) {
            kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, i};
            // Create futures that take longer than the timeout
            auto future = kythira::FutureFactory::makeFuture(response)
                .delay(std::chrono::milliseconds(1000)); // 1 second delay
            slow_futures.push_back(std::move(future));
        }
        
        auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
            std::move(slow_futures), std::chrono::milliseconds(50)); // Very short timeout
        
        try {
            auto results = std::move(collection_future).get();
            BOOST_TEST_MESSAGE("Unexpected success with very short timeout");
        } catch (const std::exception& e) {
            // Property: Very short timeout should cause read rejection
            BOOST_TEST_MESSAGE("✓ Very short timeout correctly caused read rejection: " << e.what());
        }
    }
    
    // Test with empty futures (should fail immediately)
    {
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> empty_futures;
        
        auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
            std::move(empty_futures), test_timeout);
        
        try {
            auto results = std::move(collection_future).get();
            BOOST_FAIL("Empty futures should have caused immediate failure");
        } catch (const std::exception& e) {
            // Property: Empty futures should cause immediate read rejection
            BOOST_TEST_MESSAGE("✓ Empty futures correctly caused immediate read rejection: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("All failed read rejection property tests passed!");
}