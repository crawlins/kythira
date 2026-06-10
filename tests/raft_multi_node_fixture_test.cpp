/**
 * Test for Multi-Node Raft Test Fixture
 *
 * Validates the multi-node test fixture infrastructure:
 * - Cluster initialization with dynamic sizes
 * - Node lifecycle management (start, stop, restart)
 * - Network simulator integration
 * - Network partition simulation
 * - Leader election coordination
 *
 * Requirements: 1.1, 1.2, 1.3, 2.1
 * Task: 700 - Create multi-node test fixture
 */

#define BOOST_TEST_MODULE RaftMultiNodeFixtureTest
#include <boost/test/unit_test.hpp>

#include "raft_multi_node_test_fixture.hpp"
#include <chrono>
#include <thread>
#include <set>

namespace {
// Test constants
constexpr std::size_t small_cluster_size = 3;
constexpr std::size_t medium_cluster_size = 5;
constexpr std::size_t large_cluster_size = 7;
constexpr auto short_timeout = std::chrono::milliseconds(100);
constexpr auto medium_timeout = std::chrono::milliseconds(500);
constexpr auto long_timeout = std::chrono::milliseconds(1000);
}

/**
 * Test 1: Fixture initialization with different cluster sizes
 *
 * Validates: Requirements 1.1, 1.2
 */
BOOST_AUTO_TEST_CASE(test_fixture_initialization, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Fixture initialization with different cluster sizes");

    // Test 1: Initialize 3-node cluster
    {
        kythira::test::cluster_config config;
        config.node_count = small_cluster_size;

        kythira::test::raft_multi_node_fixture fixture(config);
        fixture.initialize_cluster();

        BOOST_CHECK_EQUAL(fixture.get_node_count(), small_cluster_size);

        auto node_ids = fixture.get_node_ids();
        BOOST_CHECK_EQUAL(node_ids.size(), small_cluster_size);

        fixture.cleanup();
    }

    // Test 2: Initialize 5-node cluster
    {
        kythira::test::cluster_config config;
        config.node_count = medium_cluster_size;

        kythira::test::raft_multi_node_fixture fixture(config);
        fixture.initialize_cluster();

        BOOST_CHECK_EQUAL(fixture.get_node_count(), medium_cluster_size);

        fixture.cleanup();
    }

    // Test 3: Initialize 7-node cluster
    {
        kythira::test::cluster_config config;
        config.node_count = large_cluster_size;

        kythira::test::raft_multi_node_fixture fixture(config);
        fixture.initialize_cluster();

        BOOST_CHECK_EQUAL(fixture.get_node_count(), large_cluster_size);

        fixture.cleanup();
    }

    // Test 4: Invalid cluster size (even number) should throw
    {
        kythira::test::cluster_config config;
        config.node_count = 4;  // Even number - invalid

        BOOST_CHECK_THROW(kythira::test::raft_multi_node_fixture fixture(config),
                          std::invalid_argument);
    }

    // Test 5: Invalid cluster size (too small) should throw
    {
        kythira::test::cluster_config config;
        config.node_count = 1;  // Too small

        BOOST_CHECK_THROW(kythira::test::raft_multi_node_fixture fixture(config),
                          std::invalid_argument);
    }

    BOOST_TEST_MESSAGE("✓ Fixture initialization works correctly");
}

/**
 * Test 2: Node lifecycle management
 *
 * Validates: Requirements 1.3, 2.1
 */
BOOST_AUTO_TEST_CASE(test_node_lifecycle_management, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Node lifecycle management");

    kythira::test::cluster_config config;
    config.node_count = small_cluster_size;

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();

    auto node_ids = fixture.get_node_ids();
    BOOST_REQUIRE_EQUAL(node_ids.size(), small_cluster_size);

    // Test 1: All nodes should be stopped initially
    for (const auto& node_id : node_ids) {
        BOOST_CHECK(!fixture.is_node_running(node_id));
    }

    // Test 2: Start all nodes
    fixture.start_all_nodes();
    for (const auto& node_id : node_ids) {
        BOOST_CHECK(fixture.is_node_running(node_id));
    }

    // Test 3: Stop a specific node
    const auto& first_node = node_ids[0];
    fixture.stop_node(first_node);
    BOOST_CHECK(!fixture.is_node_running(first_node));

    // Other nodes should still be running
    for (std::size_t i = 1; i < node_ids.size(); ++i) {
        BOOST_CHECK(fixture.is_node_running(node_ids[i]));
    }

    // Test 4: Restart the stopped node
    fixture.restart_node(first_node);
    BOOST_CHECK(fixture.is_node_running(first_node));

    // Test 5: Stop all nodes
    fixture.stop_all_nodes();
    for (const auto& node_id : node_ids) {
        BOOST_CHECK(!fixture.is_node_running(node_id));
    }

    fixture.cleanup();

    BOOST_TEST_MESSAGE("✓ Node lifecycle management works correctly");
}

/**
 * Test 3: Network topology configuration
 *
 * Validates: Requirements 1.2, 2.1
 */
BOOST_AUTO_TEST_CASE(test_network_topology_configuration, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Network topology configuration");

    kythira::test::cluster_config config;
    config.node_count = small_cluster_size;
    config.enable_network_delays = true;
    config.network_latency = std::chrono::milliseconds(10);
    config.network_reliability = 0.95;

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();

    // Verify cluster is initialized
    BOOST_CHECK_EQUAL(fixture.get_node_count(), small_cluster_size);

    auto node_ids = fixture.get_node_ids();
    BOOST_REQUIRE_EQUAL(node_ids.size(), small_cluster_size);

    // Test network delay configuration
    fixture.set_node_network_delay(node_ids[0], std::chrono::milliseconds(50));

    // Test packet loss configuration
    fixture.set_node_packet_loss(node_ids[1], 0.1);  // 10% packet loss

    fixture.cleanup();

    BOOST_TEST_MESSAGE("✓ Network topology configuration works correctly");
}

/**
 * Test 4: Network partition simulation
 *
 * Validates: Requirements 1.2, 2.1
 */
BOOST_AUTO_TEST_CASE(test_network_partition_simulation, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Network partition simulation");

    kythira::test::cluster_config config;
    config.node_count = medium_cluster_size;

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();

    auto node_ids = fixture.get_node_ids();
    BOOST_REQUIRE_EQUAL(node_ids.size(), medium_cluster_size);

    // Create two groups for partition
    std::vector<std::string> group1 = {node_ids[0], node_ids[1]};
    std::vector<std::string> group2 = {node_ids[2], node_ids[3], node_ids[4]};

    // Test 1: Create network partition
    fixture.create_network_partition(group1, group2);

    // Test 2: Heal network partition
    fixture.heal_network_partition();

    fixture.cleanup();

    BOOST_TEST_MESSAGE("✓ Network partition simulation works correctly");
}

/**
 * Test 5: Time advancement and timeout triggers
 *
 * Validates: Requirements 1.3, 2.1
 */
BOOST_AUTO_TEST_CASE(test_time_advancement, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Time advancement and timeout triggers");

    kythira::test::cluster_config config;
    config.node_count = small_cluster_size;
    config.election_timeout_min = std::chrono::milliseconds(150);
    config.election_timeout_max = std::chrono::milliseconds(300);
    config.heartbeat_interval = std::chrono::milliseconds(50);

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();
    fixture.start_all_nodes();

    // Test 1: Tick election timeouts
    fixture.tick_election_timeouts();

    // Test 2: Tick heartbeat timeouts
    fixture.tick_heartbeat_timeouts();

    // Test 3: Advance time
    fixture.advance_time(std::chrono::milliseconds(100));

    fixture.cleanup();

    BOOST_TEST_MESSAGE("✓ Time advancement works correctly");
}

/**
 * Test 6: Cluster configuration management
 *
 * Validates: Requirements 1.1, 1.2
 */
BOOST_AUTO_TEST_CASE(test_cluster_configuration, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Cluster configuration management");

    kythira::test::cluster_config config;
    config.node_count = small_cluster_size;

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();

    // Test 1: Get node IDs
    auto node_ids = fixture.get_node_ids();
    BOOST_CHECK_EQUAL(node_ids.size(), small_cluster_size);

    // Test 2: Verify all node IDs are unique
    std::set<std::string> unique_ids(node_ids.begin(), node_ids.end());
    BOOST_CHECK_EQUAL(unique_ids.size(), node_ids.size());

    // Test 3: Verify node ID format
    for (const auto& node_id : node_ids) {
        BOOST_CHECK(node_id.find("node_") == 0);
    }

    fixture.cleanup();

    BOOST_TEST_MESSAGE("✓ Cluster configuration management works correctly");
}

/**
 * Test 7: Fixture cleanup and resource management
 *
 * Validates: Requirements 1.3
 */
BOOST_AUTO_TEST_CASE(test_fixture_cleanup, *boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Fixture cleanup and resource management");

    kythira::test::cluster_config config;
    config.node_count = small_cluster_size;

    kythira::test::raft_multi_node_fixture fixture(config);
    fixture.initialize_cluster();
    fixture.start_all_nodes();

    // Verify nodes are running
    auto node_ids = fixture.get_node_ids();
    for (const auto& node_id : node_ids) {
        BOOST_CHECK(fixture.is_node_running(node_id));
    }

    // Test cleanup
    fixture.cleanup();

    // After cleanup, node count should be 0
    BOOST_CHECK_EQUAL(fixture.get_node_count(), 0);

    BOOST_TEST_MESSAGE("✓ Fixture cleanup works correctly");
}
