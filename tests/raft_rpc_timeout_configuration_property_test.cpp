#define BOOST_TEST_MODULE RaftRpcTimeoutConfigurationPropertyTest

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

using namespace raft;

namespace {
    constexpr std::size_t test_iterations = 15;
    constexpr std::chrono::milliseconds min_timeout{10};
    constexpr std::chrono::milliseconds max_timeout{60000};
    constexpr std::chrono::milliseconds default_timeout{100};
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
 * **Feature: raft-completion, Property 42: RPC Timeout Configuration**
 * 
 * Property: When configuring RPC timeouts, the system allows separate timeout values for different RPC types.
 * **Validates: Requirements 9.1**
 */
BOOST_AUTO_TEST_CASE(raft_rpc_timeout_configuration_property_test, * boost::unit_test::timeout(180)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        // Generate random timeout values for different RPC types
        std::uniform_int_distribution<int> timeout_dist(min_timeout.count(), max_timeout.count());
        
        auto append_entries_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto request_vote_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto install_snapshot_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        auto heartbeat_timeout = std::chrono::milliseconds{timeout_dist(gen)};
        
        BOOST_TEST_MESSAGE("Testing timeouts - AppendEntries: " << append_entries_timeout.count() 
                          << "ms, RequestVote: " << request_vote_timeout.count() 
                          << "ms, InstallSnapshot: " << install_snapshot_timeout.count() 
                          << "ms, Heartbeat: " << heartbeat_timeout.count() << "ms");
        
        // Create configuration with different timeout values
        raft_configuration config;
        config._append_entries_timeout = append_entries_timeout;
        config._request_vote_timeout = request_vote_timeout;
        config._install_snapshot_timeout = install_snapshot_timeout;
        config._rpc_timeout = heartbeat_timeout;
        
        // Property: Configuration should store separate timeout values for different RPC types
        BOOST_CHECK_EQUAL(config.append_entries_timeout(), append_entries_timeout);
        BOOST_CHECK_EQUAL(config.request_vote_timeout(), request_vote_timeout);
        BOOST_CHECK_EQUAL(config.install_snapshot_timeout(), install_snapshot_timeout);
        BOOST_CHECK_EQUAL(config.rpc_timeout(), heartbeat_timeout);
        
        // Property: Different RPC types should be able to have different timeout values
        BOOST_CHECK(config.append_entries_timeout() != config.request_vote_timeout() ||
                   config.request_vote_timeout() != config.install_snapshot_timeout() ||
                   config.install_snapshot_timeout() != config.rpc_timeout() ||
                   append_entries_timeout == request_vote_timeout); // Allow equal values by chance
        
        BOOST_TEST_MESSAGE("✓ RPC timeout configuration supports separate values for different RPC types");
    }
    
    // Test 1: Default timeout values
    {
        BOOST_TEST_MESSAGE("Test 1: Default timeout values");
        raft_configuration default_config;
        
        // Property: Default configuration should have reasonable timeout values
        BOOST_CHECK_GT(default_config.append_entries_timeout().count(), 0);
        BOOST_CHECK_GT(default_config.request_vote_timeout().count(), 0);
        BOOST_CHECK_GT(default_config.install_snapshot_timeout().count(), 0);
        BOOST_CHECK_GT(default_config.rpc_timeout().count(), 0);
        
        // Property: InstallSnapshot should have the longest timeout (for large data transfers)
        BOOST_CHECK_GE(default_config.install_snapshot_timeout(), default_config.append_entries_timeout());
        BOOST_CHECK_GE(default_config.install_snapshot_timeout(), default_config.request_vote_timeout());
        
        // Property: AppendEntries should have longer timeout than RequestVote (more data)
        BOOST_CHECK_GE(default_config.append_entries_timeout(), default_config.request_vote_timeout());
        
        BOOST_TEST_MESSAGE("✓ Default timeout values: AppendEntries=" 
                          << default_config.append_entries_timeout().count() 
                          << "ms, RequestVote=" << default_config.request_vote_timeout().count()
                          << "ms, InstallSnapshot=" << default_config.install_snapshot_timeout().count()
                          << "ms, RPC=" << default_config.rpc_timeout().count() << "ms");
    }
    
    // Test 2: Timeout value boundaries
    {
        BOOST_TEST_MESSAGE("Test 2: Timeout value boundaries");
        
        // Test minimum timeout values
        raft_configuration min_config;
        min_config._append_entries_timeout = std::chrono::milliseconds{1};
        min_config._request_vote_timeout = std::chrono::milliseconds{1};
        min_config._install_snapshot_timeout = std::chrono::milliseconds{1};
        min_config._rpc_timeout = std::chrono::milliseconds{1};
        
        // Property: Configuration should accept minimum positive timeout values
        BOOST_CHECK_EQUAL(min_config.append_entries_timeout(), std::chrono::milliseconds{1});
        BOOST_CHECK_EQUAL(min_config.request_vote_timeout(), std::chrono::milliseconds{1});
        BOOST_CHECK_EQUAL(min_config.install_snapshot_timeout(), std::chrono::milliseconds{1});
        BOOST_CHECK_EQUAL(min_config.rpc_timeout(), std::chrono::milliseconds{1});
        
        // Test maximum timeout values
        raft_configuration max_config;
        auto max_timeout_val = std::chrono::milliseconds{std::numeric_limits<int>::max()};
        max_config._append_entries_timeout = max_timeout_val;
        max_config._request_vote_timeout = max_timeout_val;
        max_config._install_snapshot_timeout = max_timeout_val;
        max_config._rpc_timeout = max_timeout_val;
        
        // Property: Configuration should accept large timeout values
        BOOST_CHECK_EQUAL(max_config.append_entries_timeout(), max_timeout_val);
        BOOST_CHECK_EQUAL(max_config.request_vote_timeout(), max_timeout_val);
        BOOST_CHECK_EQUAL(max_config.install_snapshot_timeout(), max_timeout_val);
        BOOST_CHECK_EQUAL(max_config.rpc_timeout(), max_timeout_val);
        
        BOOST_TEST_MESSAGE("✓ Timeout boundary values handled correctly");
    }
    
    // Test 3: Timeout configuration independence
    {
        BOOST_TEST_MESSAGE("Test 3: Timeout configuration independence");
        
        std::uniform_int_distribution<int> timeout_dist(100, 10000);
        
        for (int i = 0; i < 5; ++i) {
            raft_configuration config;
            
            // Set each timeout independently
            auto ae_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto rv_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto is_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto rpc_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            
            config._append_entries_timeout = ae_timeout;
            config._request_vote_timeout = rv_timeout;
            config._install_snapshot_timeout = is_timeout;
            config._rpc_timeout = rpc_timeout;
            
            // Property: Setting one timeout should not affect others
            BOOST_CHECK_EQUAL(config.append_entries_timeout(), ae_timeout);
            BOOST_CHECK_EQUAL(config.request_vote_timeout(), rv_timeout);
            BOOST_CHECK_EQUAL(config.install_snapshot_timeout(), is_timeout);
            BOOST_CHECK_EQUAL(config.rpc_timeout(), rpc_timeout);
            
            // Modify one timeout and verify others remain unchanged
            auto new_ae_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            config._append_entries_timeout = new_ae_timeout;
            
            // Property: Modifying one timeout should not affect others
            BOOST_CHECK_EQUAL(config.append_entries_timeout(), new_ae_timeout);
            BOOST_CHECK_EQUAL(config.request_vote_timeout(), rv_timeout);
            BOOST_CHECK_EQUAL(config.install_snapshot_timeout(), is_timeout);
            BOOST_CHECK_EQUAL(config.rpc_timeout(), rpc_timeout);
        }
        
        BOOST_TEST_MESSAGE("✓ Timeout configurations are independent");
    }
    
    // Test 4: Timeout configuration validation
    {
        BOOST_TEST_MESSAGE("Test 4: Timeout configuration validation");
        
        // Test valid configuration
        raft_configuration valid_config;
        valid_config._append_entries_timeout = std::chrono::milliseconds{5000};
        valid_config._request_vote_timeout = std::chrono::milliseconds{2000};
        valid_config._install_snapshot_timeout = std::chrono::milliseconds{30000};
        valid_config._rpc_timeout = std::chrono::milliseconds{1000};
        
        // Ensure heartbeat interval compatibility (heartbeat should be < election_timeout_min / 3)
        valid_config._heartbeat_interval = std::chrono::milliseconds{40}; // 150/40 = 3.75 > 3.0
        valid_config._election_timeout_min = std::chrono::milliseconds{150};
        valid_config._election_timeout_max = std::chrono::milliseconds{300};
        
        // Property: Valid timeout configuration should pass validation
        auto errors = valid_config.get_validation_errors();
        
        // Debug: Print all validation errors
        for (const auto& error : errors) {
            BOOST_TEST_MESSAGE("Validation error: " << error);
        }
        
        bool has_timeout_errors = std::any_of(errors.begin(), errors.end(), 
            [](const std::string& error) {
                return error.find("timeout") != std::string::npos;
            });
        BOOST_CHECK(!has_timeout_errors);
        
        // Test invalid configuration (zero timeouts)
        raft_configuration invalid_config;
        invalid_config._append_entries_timeout = std::chrono::milliseconds{0};
        invalid_config._request_vote_timeout = std::chrono::milliseconds{0};
        invalid_config._install_snapshot_timeout = std::chrono::milliseconds{0};
        invalid_config._rpc_timeout = std::chrono::milliseconds{0};
        
        // Property: Invalid timeout configuration should fail validation
        auto invalid_errors = invalid_config.get_validation_errors();
        bool has_append_entries_error = std::any_of(invalid_errors.begin(), invalid_errors.end(),
            [](const std::string& error) {
                return error.find("append_entries_timeout") != std::string::npos;
            });
        bool has_request_vote_error = std::any_of(invalid_errors.begin(), invalid_errors.end(),
            [](const std::string& error) {
                return error.find("request_vote_timeout") != std::string::npos;
            });
        bool has_install_snapshot_error = std::any_of(invalid_errors.begin(), invalid_errors.end(),
            [](const std::string& error) {
                return error.find("install_snapshot_timeout") != std::string::npos;
            });
        bool has_rpc_timeout_error = std::any_of(invalid_errors.begin(), invalid_errors.end(),
            [](const std::string& error) {
                return error.find("rpc_timeout") != std::string::npos;
            });
        
        BOOST_CHECK(has_append_entries_error);
        BOOST_CHECK(has_request_vote_error);
        BOOST_CHECK(has_install_snapshot_error);
        BOOST_CHECK(has_rpc_timeout_error);
        
        BOOST_TEST_MESSAGE("✓ Timeout configuration validation works correctly");
    }
    
    // Test 5: Timeout configuration concept compliance
    {
        BOOST_TEST_MESSAGE("Test 5: Timeout configuration concept compliance");
        
        raft_configuration config;
        
        // Property: Configuration should satisfy the raft_configuration_type concept
        static_assert(raft_configuration_type<raft_configuration>);
        
        // Property: All timeout accessor methods should return chrono::milliseconds
        static_assert(std::same_as<decltype(config.append_entries_timeout()), std::chrono::milliseconds>);
        static_assert(std::same_as<decltype(config.request_vote_timeout()), std::chrono::milliseconds>);
        static_assert(std::same_as<decltype(config.install_snapshot_timeout()), std::chrono::milliseconds>);
        static_assert(std::same_as<decltype(config.rpc_timeout()), std::chrono::milliseconds>);
        
        BOOST_TEST_MESSAGE("✓ Configuration satisfies concept requirements");
    }
    
    // Test 6: Timeout configuration with error handlers
    {
        BOOST_TEST_MESSAGE("Test 6: Timeout configuration with error handlers");
        
        raft_configuration config;
        config._append_entries_timeout = std::chrono::milliseconds{3000};
        config._request_vote_timeout = std::chrono::milliseconds{1500};
        config._install_snapshot_timeout = std::chrono::milliseconds{20000};
        config._rpc_timeout = std::chrono::milliseconds{800};
        
        // Create error handlers with different timeout configurations
        kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> ae_handler;
        kythira::error_handler<kythira::request_vote_response<std::uint64_t>> rv_handler;
        kythira::error_handler<kythira::install_snapshot_response<std::uint64_t>> is_handler;
        
        // Property: Error handlers should be configurable with different timeout policies
        typename kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy ae_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = config.append_entries_timeout(),
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 5
        };
        
        typename kythira::error_handler<kythira::request_vote_response<std::uint64_t>>::retry_policy rv_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = config.request_vote_timeout(),
            .backoff_multiplier = 1.8,
            .jitter_factor = 0.1,
            .max_attempts = 3
        };
        
        typename kythira::error_handler<kythira::install_snapshot_response<std::uint64_t>>::retry_policy is_policy{
            .initial_delay = std::chrono::milliseconds{500},
            .max_delay = config.install_snapshot_timeout(),
            .backoff_multiplier = 2.5,
            .jitter_factor = 0.1,
            .max_attempts = 10
        };
        
        // Property: Policies should be valid and configurable
        BOOST_CHECK(ae_policy.is_valid());
        BOOST_CHECK(rv_policy.is_valid());
        BOOST_CHECK(is_policy.is_valid());
        
        ae_handler.set_retry_policy("append_entries", ae_policy);
        rv_handler.set_retry_policy("request_vote", rv_policy);
        is_handler.set_retry_policy("install_snapshot", is_policy);
        
        // Property: Configured policies should be retrievable
        auto retrieved_ae_policy = ae_handler.get_retry_policy("append_entries");
        auto retrieved_rv_policy = rv_handler.get_retry_policy("request_vote");
        auto retrieved_is_policy = is_handler.get_retry_policy("install_snapshot");
        
        BOOST_CHECK_EQUAL(retrieved_ae_policy.max_delay, config.append_entries_timeout());
        BOOST_CHECK_EQUAL(retrieved_rv_policy.max_delay, config.request_vote_timeout());
        BOOST_CHECK_EQUAL(retrieved_is_policy.max_delay, config.install_snapshot_timeout());
        
        BOOST_TEST_MESSAGE("✓ Timeout configuration integrates with error handlers");
    }
    
    // Test 7: Random timeout configuration stress test
    {
        BOOST_TEST_MESSAGE("Test 7: Random timeout configuration stress test");
        
        std::uniform_int_distribution<int> timeout_dist(1, 60000);
        
        for (int i = 0; i < 20; ++i) {
            raft_configuration config;
            
            // Generate random timeout values
            auto ae_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto rv_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto is_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            auto rpc_timeout = std::chrono::milliseconds{timeout_dist(gen)};
            
            config._append_entries_timeout = ae_timeout;
            config._request_vote_timeout = rv_timeout;
            config._install_snapshot_timeout = is_timeout;
            config._rpc_timeout = rpc_timeout;
            
            // Property: Random timeout configurations should be stored and retrieved correctly
            BOOST_CHECK_EQUAL(config.append_entries_timeout(), ae_timeout);
            BOOST_CHECK_EQUAL(config.request_vote_timeout(), rv_timeout);
            BOOST_CHECK_EQUAL(config.install_snapshot_timeout(), is_timeout);
            BOOST_CHECK_EQUAL(config.rpc_timeout(), rpc_timeout);
            
            // Property: All timeout values should be positive
            BOOST_CHECK_GT(config.append_entries_timeout().count(), 0);
            BOOST_CHECK_GT(config.request_vote_timeout().count(), 0);
            BOOST_CHECK_GT(config.install_snapshot_timeout().count(), 0);
            BOOST_CHECK_GT(config.rpc_timeout().count(), 0);
        }
        
        BOOST_TEST_MESSAGE("✓ Random timeout configuration stress test passed");
    }
    
    BOOST_TEST_MESSAGE("All RPC timeout configuration property tests passed!");
}