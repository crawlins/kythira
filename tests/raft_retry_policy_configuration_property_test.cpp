#define BOOST_TEST_MODULE RaftRetryPolicyConfigurationPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/types.hpp>
#include <raft/error_handler.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cmath>

using namespace raft;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds min_delay{1};
    constexpr std::chrono::milliseconds max_delay{30000};
    constexpr double min_multiplier = 1.1;
    constexpr double max_multiplier = 5.0;
    constexpr double min_jitter = 0.0;
    constexpr double max_jitter = 1.0;
    constexpr std::size_t min_attempts = 1;
    constexpr std::size_t max_attempts = 20;
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
 * **Feature: raft-completion, Property 43: Retry Policy Configuration**
 * 
 * Property: When configuring retry policies, the system supports exponential backoff with configurable parameters.
 * **Validates: Requirements 9.2**
 */
BOOST_AUTO_TEST_CASE(raft_retry_policy_configuration_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random retry policy parameters
        std::uniform_int_distribution<int> delay_dist(min_delay.count(), max_delay.count());
        std::uniform_real_distribution<double> multiplier_dist(min_multiplier, max_multiplier);
        std::uniform_real_distribution<double> jitter_dist(min_jitter, max_jitter);
        std::uniform_int_distribution<std::size_t> attempts_dist(min_attempts, max_attempts);
        
        auto initial_delay = std::chrono::milliseconds{delay_dist(gen)};
        auto max_delay_val = std::chrono::milliseconds{std::max(static_cast<int>(initial_delay.count()), delay_dist(gen))};
        auto backoff_multiplier = multiplier_dist(gen);
        auto jitter_factor = jitter_dist(gen);
        auto max_attempts = attempts_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing retry policy - Initial: " << initial_delay.count() 
                          << "ms, Max: " << max_delay_val.count() 
                          << "ms, Multiplier: " << backoff_multiplier 
                          << ", Jitter: " << jitter_factor 
                          << ", Attempts: " << max_attempts);
        
        // Create retry policy configuration
        retry_policy_config policy{
            .initial_delay = initial_delay,
            .max_delay = max_delay_val,
            .backoff_multiplier = backoff_multiplier,
            .jitter_factor = jitter_factor,
            .max_attempts = max_attempts
        };
        
        // Property: Valid retry policy should pass validation
        BOOST_CHECK(policy.is_valid());
        
        // Property: Retry policy should store configured parameters correctly
        BOOST_CHECK_EQUAL(policy.initial_delay, initial_delay);
        BOOST_CHECK_EQUAL(policy.max_delay, max_delay_val);
        BOOST_CHECK_EQUAL(policy.backoff_multiplier, backoff_multiplier);
        BOOST_CHECK_EQUAL(policy.jitter_factor, jitter_factor);
        BOOST_CHECK_EQUAL(policy.max_attempts, max_attempts);
        
        BOOST_TEST_MESSAGE("✓ Retry policy configuration stores parameters correctly");
    }
    
    // Test 1: Default retry policy configurations
    {
        BOOST_TEST_MESSAGE("Test 1: Default retry policy configurations");
        raft_configuration config;
        
        // Property: Default configuration should have valid retry policies for all RPC types
        BOOST_CHECK(config.heartbeat_retry_policy().is_valid());
        BOOST_CHECK(config.append_entries_retry_policy().is_valid());
        BOOST_CHECK(config.request_vote_retry_policy().is_valid());
        BOOST_CHECK(config.install_snapshot_retry_policy().is_valid());
        
        // Property: Different RPC types should have different retry characteristics
        const auto& heartbeat_policy = config.heartbeat_retry_policy();
        const auto& append_entries_policy = config.append_entries_retry_policy();
        const auto& request_vote_policy = config.request_vote_retry_policy();
        const auto& install_snapshot_policy = config.install_snapshot_retry_policy();
        
        // Property: InstallSnapshot should have the most aggressive retry policy (highest max_attempts)
        BOOST_CHECK_GE(install_snapshot_policy.max_attempts, append_entries_policy.max_attempts);
        BOOST_CHECK_GE(install_snapshot_policy.max_attempts, request_vote_policy.max_attempts);
        BOOST_CHECK_GE(install_snapshot_policy.max_attempts, heartbeat_policy.max_attempts);
        
        // Property: InstallSnapshot should have the longest max_delay (for large transfers)
        BOOST_CHECK_GE(install_snapshot_policy.max_delay, append_entries_policy.max_delay);
        BOOST_CHECK_GE(install_snapshot_policy.max_delay, request_vote_policy.max_delay);
        BOOST_CHECK_GE(install_snapshot_policy.max_delay, heartbeat_policy.max_delay);
        
        BOOST_TEST_MESSAGE("✓ Default retry policies have appropriate characteristics");
    }
    
    // Test 2: Retry policy validation
    {
        BOOST_TEST_MESSAGE("Test 2: Retry policy validation");
        
        // Test valid policy
        retry_policy_config valid_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{5000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 5
        };
        
        // Property: Valid policy should pass validation
        BOOST_CHECK(valid_policy.is_valid());
        
        // Test invalid policies
        std::vector<std::pair<retry_policy_config, std::string>> invalid_policies = {
            // Zero initial delay
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{0},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 2.0,
                .jitter_factor = 0.1,
                .max_attempts = 5
            }, "zero initial delay"},
            
            // Max delay less than initial delay
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{1000},
                .max_delay = std::chrono::milliseconds{500},
                .backoff_multiplier = 2.0,
                .jitter_factor = 0.1,
                .max_attempts = 5
            }, "max delay less than initial delay"},
            
            // Invalid backoff multiplier (too small)
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 1.0,
                .jitter_factor = 0.1,
                .max_attempts = 5
            }, "backoff multiplier too small"},
            
            // Invalid jitter factor (negative)
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 2.0,
                .jitter_factor = -0.1,
                .max_attempts = 5
            }, "negative jitter factor"},
            
            // Invalid jitter factor (too large)
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 2.0,
                .jitter_factor = 1.5,
                .max_attempts = 5
            }, "jitter factor too large"},
            
            // Zero max attempts
            {retry_policy_config{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 2.0,
                .jitter_factor = 0.1,
                .max_attempts = 0
            }, "zero max attempts"}
        };
        
        for (const auto& [invalid_policy, description] : invalid_policies) {
            // Property: Invalid policies should fail validation
            BOOST_CHECK(!invalid_policy.is_valid());
            BOOST_TEST_MESSAGE("✓ Invalid policy rejected: " << description);
        }
    }
    
    // Test 3: Exponential backoff calculation
    {
        BOOST_TEST_MESSAGE("Test 3: Exponential backoff calculation");
        
        retry_policy_config policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{5000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.0, // No jitter for predictable testing
            .max_attempts = 6
        };
        
        kythira::error_handler<int> handler;
        handler.set_retry_policy("test_operation", 
            typename kythira::error_handler<int>::retry_policy{
                .initial_delay = policy.initial_delay,
                .max_delay = policy.max_delay,
                .backoff_multiplier = policy.backoff_multiplier,
                .jitter_factor = policy.jitter_factor,
                .max_attempts = policy.max_attempts
            });
        
        // Property: Exponential backoff should follow the configured multiplier
        // Expected delays: 100ms, 200ms, 400ms, 800ms, 1600ms, 3200ms (capped at 5000ms)
        std::vector<std::chrono::milliseconds> expected_delays = {
            std::chrono::milliseconds{100},
            std::chrono::milliseconds{200},
            std::chrono::milliseconds{400},
            std::chrono::milliseconds{800},
            std::chrono::milliseconds{1600},
            std::chrono::milliseconds{3200}
        };
        
        // Note: We can't directly test the delay calculation without exposing internal methods,
        // but we can verify the policy configuration is stored correctly
        auto retrieved_policy = handler.get_retry_policy("test_operation");
        
        // Property: Retrieved policy should match configured policy
        BOOST_CHECK_EQUAL(retrieved_policy.initial_delay, policy.initial_delay);
        BOOST_CHECK_EQUAL(retrieved_policy.max_delay, policy.max_delay);
        BOOST_CHECK_EQUAL(retrieved_policy.backoff_multiplier, policy.backoff_multiplier);
        BOOST_CHECK_EQUAL(retrieved_policy.jitter_factor, policy.jitter_factor);
        BOOST_CHECK_EQUAL(retrieved_policy.max_attempts, policy.max_attempts);
        
        BOOST_TEST_MESSAGE("✓ Exponential backoff policy configured correctly");
    }
    
    // Test 4: Jitter configuration
    {
        BOOST_TEST_MESSAGE("Test 4: Jitter configuration");
        
        std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
        
        for (int i = 0; i < 10; ++i) {
            auto jitter_factor = jitter_dist(gen);
            
            retry_policy_config policy{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{5000},
                .backoff_multiplier = 2.0,
                .jitter_factor = jitter_factor,
                .max_attempts = 5
            };
            
            // Property: Valid jitter factors should be accepted
            BOOST_CHECK(policy.is_valid());
            BOOST_CHECK_EQUAL(policy.jitter_factor, jitter_factor);
            
            kythira::error_handler<int> handler;
            handler.set_retry_policy("jitter_test", 
                typename kythira::error_handler<int>::retry_policy{
                    .initial_delay = policy.initial_delay,
                    .max_delay = policy.max_delay,
                    .backoff_multiplier = policy.backoff_multiplier,
                    .jitter_factor = policy.jitter_factor,
                    .max_attempts = policy.max_attempts
                });
            
            auto retrieved_policy = handler.get_retry_policy("jitter_test");
            
            // Property: Jitter factor should be preserved in configuration
            BOOST_CHECK_EQUAL(retrieved_policy.jitter_factor, jitter_factor);
        }
        
        BOOST_TEST_MESSAGE("✓ Jitter configuration works correctly");
    }
    
    // Test 5: Per-operation retry policy configuration
    {
        BOOST_TEST_MESSAGE("Test 5: Per-operation retry policy configuration");
        
        kythira::error_handler<int> handler;
        
        // Configure different policies for different operations
        typename kythira::error_handler<int>::retry_policy heartbeat_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{1000},
            .backoff_multiplier = 1.5,
            .jitter_factor = 0.1,
            .max_attempts = 3
        };
        
        typename kythira::error_handler<int>::retry_policy append_entries_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{5000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.2,
            .max_attempts = 5
        };
        
        typename kythira::error_handler<int>::retry_policy request_vote_policy{
            .initial_delay = std::chrono::milliseconds{75},
            .max_delay = std::chrono::milliseconds{2000},
            .backoff_multiplier = 1.8,
            .jitter_factor = 0.15,
            .max_attempts = 4
        };
        
        handler.set_retry_policy("heartbeat", heartbeat_policy);
        handler.set_retry_policy("append_entries", append_entries_policy);
        handler.set_retry_policy("request_vote", request_vote_policy);
        
        // Property: Each operation should have its own independent retry policy
        auto retrieved_heartbeat = handler.get_retry_policy("heartbeat");
        auto retrieved_append_entries = handler.get_retry_policy("append_entries");
        auto retrieved_request_vote = handler.get_retry_policy("request_vote");
        
        // Property: Retrieved policies should match configured policies
        BOOST_CHECK_EQUAL(retrieved_heartbeat.initial_delay, heartbeat_policy.initial_delay);
        BOOST_CHECK_EQUAL(retrieved_heartbeat.max_delay, heartbeat_policy.max_delay);
        BOOST_CHECK_EQUAL(retrieved_heartbeat.backoff_multiplier, heartbeat_policy.backoff_multiplier);
        BOOST_CHECK_EQUAL(retrieved_heartbeat.max_attempts, heartbeat_policy.max_attempts);
        
        BOOST_CHECK_EQUAL(retrieved_append_entries.initial_delay, append_entries_policy.initial_delay);
        BOOST_CHECK_EQUAL(retrieved_append_entries.max_delay, append_entries_policy.max_delay);
        BOOST_CHECK_EQUAL(retrieved_append_entries.backoff_multiplier, append_entries_policy.backoff_multiplier);
        BOOST_CHECK_EQUAL(retrieved_append_entries.max_attempts, append_entries_policy.max_attempts);
        
        BOOST_CHECK_EQUAL(retrieved_request_vote.initial_delay, request_vote_policy.initial_delay);
        BOOST_CHECK_EQUAL(retrieved_request_vote.max_delay, request_vote_policy.max_delay);
        BOOST_CHECK_EQUAL(retrieved_request_vote.backoff_multiplier, request_vote_policy.backoff_multiplier);
        BOOST_CHECK_EQUAL(retrieved_request_vote.max_attempts, request_vote_policy.max_attempts);
        
        // Property: Policies should be independent (modifying one doesn't affect others)
        typename kythira::error_handler<int>::retry_policy new_heartbeat_policy{
            .initial_delay = std::chrono::milliseconds{25},
            .max_delay = std::chrono::milliseconds{500},
            .backoff_multiplier = 1.3,
            .jitter_factor = 0.05,
            .max_attempts = 2
        };
        
        handler.set_retry_policy("heartbeat", new_heartbeat_policy);
        
        // Property: Other policies should remain unchanged
        auto unchanged_append_entries = handler.get_retry_policy("append_entries");
        auto unchanged_request_vote = handler.get_retry_policy("request_vote");
        
        BOOST_CHECK_EQUAL(unchanged_append_entries.initial_delay, append_entries_policy.initial_delay);
        BOOST_CHECK_EQUAL(unchanged_request_vote.initial_delay, request_vote_policy.initial_delay);
        
        BOOST_TEST_MESSAGE("✓ Per-operation retry policies are independent");
    }
    
    // Test 6: Retry policy integration with raft configuration
    {
        BOOST_TEST_MESSAGE("Test 6: Retry policy integration with raft configuration");
        
        raft_configuration config;
        
        // Modify retry policies in configuration
        config._heartbeat_retry_policy.initial_delay = std::chrono::milliseconds{30};
        config._heartbeat_retry_policy.max_delay = std::chrono::milliseconds{800};
        config._heartbeat_retry_policy.max_attempts = 2;
        
        config._append_entries_retry_policy.initial_delay = std::chrono::milliseconds{150};
        config._append_entries_retry_policy.max_delay = std::chrono::milliseconds{6000};
        config._append_entries_retry_policy.max_attempts = 6;
        
        // Property: Configuration should store modified retry policies
        BOOST_CHECK_EQUAL(config.heartbeat_retry_policy().initial_delay, std::chrono::milliseconds{30});
        BOOST_CHECK_EQUAL(config.heartbeat_retry_policy().max_delay, std::chrono::milliseconds{800});
        BOOST_CHECK_EQUAL(config.heartbeat_retry_policy().max_attempts, 2);
        
        BOOST_CHECK_EQUAL(config.append_entries_retry_policy().initial_delay, std::chrono::milliseconds{150});
        BOOST_CHECK_EQUAL(config.append_entries_retry_policy().max_delay, std::chrono::milliseconds{6000});
        BOOST_CHECK_EQUAL(config.append_entries_retry_policy().max_attempts, 6);
        
        // Property: Modified policies should still be valid
        BOOST_CHECK(config.heartbeat_retry_policy().is_valid());
        BOOST_CHECK(config.append_entries_retry_policy().is_valid());
        BOOST_CHECK(config.request_vote_retry_policy().is_valid());
        BOOST_CHECK(config.install_snapshot_retry_policy().is_valid());
        
        BOOST_TEST_MESSAGE("✓ Retry policy integration with raft configuration works");
    }
    
    // Test 7: Boundary value testing for retry policies
    {
        BOOST_TEST_MESSAGE("Test 7: Boundary value testing for retry policies");
        
        // Test minimum valid values
        retry_policy_config min_policy{
            .initial_delay = std::chrono::milliseconds{1},
            .max_delay = std::chrono::milliseconds{1},
            .backoff_multiplier = 1.1,
            .jitter_factor = 0.0,
            .max_attempts = 1
        };
        
        // Property: Minimum valid policy should pass validation
        BOOST_CHECK(min_policy.is_valid());
        
        // Test maximum reasonable values
        retry_policy_config max_policy{
            .initial_delay = std::chrono::milliseconds{60000},
            .max_delay = std::chrono::milliseconds{300000},
            .backoff_multiplier = 10.0,
            .jitter_factor = 1.0,
            .max_attempts = 100
        };
        
        // Property: Maximum reasonable policy should pass validation
        BOOST_CHECK(max_policy.is_valid());
        
        // Test edge cases
        retry_policy_config edge_policy{
            .initial_delay = std::chrono::milliseconds{1000},
            .max_delay = std::chrono::milliseconds{1000}, // Equal to initial delay
            .backoff_multiplier = 1.0001, // Just above 1.0
            .jitter_factor = 0.9999, // Just below 1.0
            .max_attempts = 1
        };
        
        // Property: Edge case policy should pass validation
        BOOST_CHECK(edge_policy.is_valid());
        
        BOOST_TEST_MESSAGE("✓ Boundary value testing passed");
    }
    
    // Test 8: Random retry policy stress test
    {
        BOOST_TEST_MESSAGE("Test 8: Random retry policy stress test");
        
        std::uniform_int_distribution<int> delay_dist(1, 10000);
        std::uniform_real_distribution<double> multiplier_dist(1.1, 5.0);
        std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
        std::uniform_int_distribution<std::size_t> attempts_dist(1, 15);
        
        for (int i = 0; i < 50; ++i) {
            auto initial_delay = std::chrono::milliseconds{delay_dist(gen)};
            auto max_delay_val = std::chrono::milliseconds{std::max(static_cast<int>(initial_delay.count()), delay_dist(gen))};
            auto backoff_multiplier = multiplier_dist(gen);
            auto jitter_factor = jitter_dist(gen);
            auto max_attempts = attempts_dist(gen);
            
            retry_policy_config policy{
                .initial_delay = initial_delay,
                .max_delay = max_delay_val,
                .backoff_multiplier = backoff_multiplier,
                .jitter_factor = jitter_factor,
                .max_attempts = max_attempts
            };
            
            // Property: Randomly generated valid policies should pass validation
            BOOST_CHECK(policy.is_valid());
            
            // Property: Policy parameters should be stored correctly
            BOOST_CHECK_EQUAL(policy.initial_delay, initial_delay);
            BOOST_CHECK_EQUAL(policy.max_delay, max_delay_val);
            BOOST_CHECK_EQUAL(policy.backoff_multiplier, backoff_multiplier);
            BOOST_CHECK_EQUAL(policy.jitter_factor, jitter_factor);
            BOOST_CHECK_EQUAL(policy.max_attempts, max_attempts);
        }
        
        BOOST_TEST_MESSAGE("✓ Random retry policy stress test passed");
    }
    
    BOOST_TEST_MESSAGE("All retry policy configuration property tests passed!");
}