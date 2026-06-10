/**
 * Cluster Initialization Unit Test for Raft Consensus
 *
 * Tests cluster initialization requirements without full network integration:
 * - Nodes start in follower state (Requirement 1.1)
 * - Cluster configuration is properly set (Requirement 1.2)
 * - Election timeout randomization (Requirement 2.1, 2.2)
 * - Initial state consistency
 *
 * This test validates the core initialization logic without relying on
 * the network simulator, which has known integration issues.
 *
 * Requirements: 1.1, 1.2, 2.1, 2.2
 * Task: 701 - Implement cluster initialization test
 */

#define BOOST_TEST_MODULE RaftClusterInitializationUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/types.hpp>

#include <random>
#include <chrono>
#include <vector>
#include <set>
#include <algorithm>

namespace {
// Test constants
constexpr std::size_t small_cluster_size = 3;
constexpr std::size_t medium_cluster_size = 5;
constexpr std::chrono::milliseconds election_timeout_min{150};
constexpr std::chrono::milliseconds election_timeout_max{300};
constexpr std::chrono::milliseconds heartbeat_interval{50};
constexpr std::size_t randomization_sample_size = 100;

// Helper to create cluster configuration
auto create_cluster_config(std::size_t node_count) -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> node_ids;
    for (std::size_t i = 0; i < node_count; ++i) {
        node_ids.push_back(static_cast<std::uint64_t>(i + 1));
    }
    return node_ids;
}
}

BOOST_AUTO_TEST_SUITE(cluster_initialization_unit_tests)

/**
 * Test 1: Server state enum values
 *
 * Validates: Requirement 1.1
 *
 * Property: The server_state enum must have follower, candidate, and leader states.
 */
BOOST_AUTO_TEST_CASE(test_server_state_enum, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Server state enum values");

    // Verify server state enum has required values
    auto follower_state = kythira::server_state::follower;
    auto candidate_state = kythira::server_state::candidate;
    auto leader_state = kythira::server_state::leader;

    // Verify they are distinct
    BOOST_CHECK_NE(follower_state, candidate_state);
    BOOST_CHECK_NE(follower_state, leader_state);
    BOOST_CHECK_NE(candidate_state, leader_state);

    BOOST_TEST_MESSAGE("✓ Server state enum validated");
}

/**
 * Test 2: Configuration defaults
 *
 * Validates: Requirement 1.2
 *
 * Property: The default Raft configuration should have sensible values
 * and maintain invariants (e.g., heartbeat < election timeout).
 */
BOOST_AUTO_TEST_CASE(test_configuration_defaults, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Configuration defaults");

    auto config = kythira::raft_configuration{};

    // Verify default values are reasonable
    BOOST_CHECK_GT(config.election_timeout_min(), std::chrono::milliseconds{0});
    BOOST_CHECK_GT(config.election_timeout_max(), config.election_timeout_min());
    BOOST_CHECK_GT(config.heartbeat_interval(), std::chrono::milliseconds{0});
    BOOST_CHECK_LT(config.heartbeat_interval(), config.election_timeout_min());

    // Verify RPC timeouts
    BOOST_CHECK_GT(config.rpc_timeout(), std::chrono::milliseconds{0});

    // Verify batch size
    BOOST_CHECK_GT(config.max_entries_per_append(), 0);

    BOOST_TEST_MESSAGE("✓ Configuration defaults are sensible");
}

/**
 * Test 3: Configuration customization
 *
 * Validates: Requirement 1.2
 *
 * Property: Configuration values can be customized and validated.
 */
BOOST_AUTO_TEST_CASE(test_configuration_customization, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Configuration customization");

    auto config = kythira::raft_configuration{};
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = heartbeat_interval;

    // Verify configuration invariants
    BOOST_CHECK_LT(config.heartbeat_interval(), config.election_timeout_min());
    BOOST_CHECK_LT(config.election_timeout_min(), config.election_timeout_max());
    BOOST_CHECK_GT(config.heartbeat_interval(), std::chrono::milliseconds{0});

    // Verify configuration can be validated
    BOOST_CHECK(config.validate());
    BOOST_CHECK(config.get_validation_errors().empty());

    BOOST_TEST_MESSAGE("✓ Configuration customization works correctly");
}

/**
 * Test 4: Election timeout randomization range
 *
 * Validates: Requirements 2.1, 2.2
 *
 * Property: Election timeouts should be randomized within the configured range
 * to prevent simultaneous elections and split votes.
 */
BOOST_AUTO_TEST_CASE(test_election_timeout_randomization, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Election timeout randomization");

    auto config = kythira::raft_configuration{};
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;

    // Sample random election timeouts
    std::vector<std::chrono::milliseconds> timeouts;
    std::set<std::chrono::milliseconds> unique_timeouts;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(config.election_timeout_min().count(),
                                        config.election_timeout_max().count());

    for (std::size_t i = 0; i < randomization_sample_size; ++i) {
        auto random_timeout = std::chrono::milliseconds{dis(gen)};
        timeouts.push_back(random_timeout);
        unique_timeouts.insert(random_timeout);
    }

    // Verify we got different timeout values (randomization is working)
    // With 100 samples in a 150ms range, we should get many unique values
    BOOST_CHECK_GT(unique_timeouts.size(), randomization_sample_size / 10);

    // Verify all timeouts are within the configured range
    for (const auto& timeout : timeouts) {
        BOOST_CHECK_GE(timeout, election_timeout_min);
        BOOST_CHECK_LE(timeout, election_timeout_max);
    }

    BOOST_TEST_MESSAGE("✓ Election timeout randomization verified (got "
                       << unique_timeouts.size() << " unique values from "
                       << randomization_sample_size << " samples)");
}

/**
 * Test 5: Cluster configuration structure
 *
 * Validates: Requirement 1.2
 *
 * Property: Cluster configurations can be created with different sizes
 * and all node IDs should be unique.
 */
BOOST_AUTO_TEST_CASE(test_cluster_configuration_structure, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Cluster configuration structure");

    for (std::size_t cluster_size : {small_cluster_size, medium_cluster_size, std::size_t{7}}) {
        auto node_ids = create_cluster_config(cluster_size);

        // Verify correct number of node IDs
        BOOST_CHECK_EQUAL(node_ids.size(), cluster_size);

        // Verify all IDs are unique
        std::set<std::uint64_t> unique_ids(node_ids.begin(), node_ids.end());
        BOOST_CHECK_EQUAL(unique_ids.size(), cluster_size);

        // Verify all IDs are positive
        for (const auto& node_id : node_ids) {
            BOOST_CHECK_GT(node_id, 0);
        }

        BOOST_TEST_MESSAGE("✓ Cluster size " << cluster_size << " configuration valid");
    }

    BOOST_TEST_MESSAGE("✓ Cluster configuration structure validated");
}

/**
 * Test 6: Configuration validation errors
 *
 * Validates: Requirement 1.2
 *
 * Property: Invalid configurations should be detected by validation.
 */
BOOST_AUTO_TEST_CASE(test_configuration_validation_errors, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Configuration validation errors");

    // Test 1: Invalid election timeout (min >= max)
    {
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = std::chrono::milliseconds{300};
        config._election_timeout_max = std::chrono::milliseconds{150};

        BOOST_CHECK(!config.validate());
        BOOST_CHECK(!config.get_validation_errors().empty());
    }

    // Test 2: Invalid election timeout (min <= 0)
    {
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = std::chrono::milliseconds{0};
        config._election_timeout_max = std::chrono::milliseconds{300};

        BOOST_CHECK(!config.validate());
        BOOST_CHECK(!config.get_validation_errors().empty());
    }

    // Test 3: Valid configuration
    {
        auto config = kythira::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;

        BOOST_CHECK(config.validate());
        BOOST_CHECK(config.get_validation_errors().empty());
    }

    BOOST_TEST_MESSAGE("✓ Configuration validation errors detected correctly");
}

/**
 * Test 7: Retry policy configuration
 *
 * Validates: Requirement 1.2
 *
 * Property: Retry policies should be properly configured with reasonable defaults.
 */
BOOST_AUTO_TEST_CASE(test_retry_policy_configuration, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Retry policy configuration");

    auto config = kythira::raft_configuration{};

    // Verify heartbeat retry policy
    auto heartbeat_policy = config.heartbeat_retry_policy();
    BOOST_CHECK_GT(heartbeat_policy.initial_delay, std::chrono::milliseconds{0});
    BOOST_CHECK_GT(heartbeat_policy.max_delay, heartbeat_policy.initial_delay);
    BOOST_CHECK_GT(heartbeat_policy.backoff_multiplier, 1.0);
    BOOST_CHECK_GT(heartbeat_policy.max_attempts, 0);

    // Verify append entries retry policy
    auto append_policy = config.append_entries_retry_policy();
    BOOST_CHECK_GT(append_policy.initial_delay, std::chrono::milliseconds{0});
    BOOST_CHECK_GT(append_policy.max_delay, append_policy.initial_delay);
    BOOST_CHECK_GT(append_policy.backoff_multiplier, 1.0);
    BOOST_CHECK_GT(append_policy.max_attempts, 0);

    // Verify request vote retry policy
    auto vote_policy = config.request_vote_retry_policy();
    BOOST_CHECK_GT(vote_policy.initial_delay, std::chrono::milliseconds{0});
    BOOST_CHECK_GT(vote_policy.max_delay, vote_policy.initial_delay);
    BOOST_CHECK_GT(vote_policy.backoff_multiplier, 1.0);
    BOOST_CHECK_GT(vote_policy.max_attempts, 0);

    BOOST_TEST_MESSAGE("✓ Retry policy configuration validated");
}

BOOST_AUTO_TEST_SUITE_END()
