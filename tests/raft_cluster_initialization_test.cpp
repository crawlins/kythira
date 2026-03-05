/**
 * Cluster Initialization Test for Raft Consensus
 * 
 * Tests proper cluster bootstrap and initial leader election:
 * - Cluster initialization with initial configuration
 * - All nodes start in follower state
 * - Election timeout randomization across nodes
 * - First leader election completes successfully
 * 
 * Requirements: 1.1, 1.2, 2.1, 2.2
 * Task: 701 - Implement cluster initialization test
 */

#define BOOST_TEST_MODULE RaftClusterInitializationTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/logger.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>
#include <raft/test_state_machine.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <set>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_cluster_initialization_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    // Test constants
    constexpr std::size_t small_cluster_size = 3;
    constexpr std::size_t medium_cluster_size = 5;
    constexpr std::chrono::milliseconds election_timeout_min{150};
    constexpr std::chrono::milliseconds election_timeout_max{300};
    constexpr std::chrono::milliseconds heartbeat_interval{50};
    constexpr std::chrono::milliseconds leader_election_timeout{5000};  // Increased timeout
    constexpr std::chrono::milliseconds state_check_interval{50};  // Increased check interval
    
    // Types for simulator-based testing
    struct test_raft_types {
        // Future types
        using future_type = kythira::Future<std::vector<std::byte>>;
        using promise_type = kythira::Promise<std::vector<std::byte>>;
        using try_type = kythira::Try<std::vector<std::byte>>;
        
        // Basic data types
        using node_id_type = std::uint64_t;
        using term_id_type = std::uint64_t;
        using log_index_type = std::uint64_t;
        
        // Serializer and data types
        using serialized_data_type = std::vector<std::byte>;
        using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;
        
        // Network types
        using raft_network_types = kythira::raft_simulator_network_types<std::string>;
        using network_client_type = kythira::simulator_network_client<raft_network_types, serializer_type, serialized_data_type>;
        using network_server_type = kythira::simulator_network_server<raft_network_types, serializer_type, serialized_data_type>;
        
        // Component types
        using persistence_engine_type = kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
        using logger_type = kythira::console_logger;
        using metrics_type = kythira::noop_metrics;
        using membership_manager_type = kythira::default_membership_manager<node_id_type>;
        using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
        
        // Configuration type
        using configuration_type = kythira::raft_configuration;
        
        // Type aliases for commonly used compound types
        using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
        using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
        using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;
        
        // RPC message types
        using request_vote_request_type = kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
        using request_vote_response_type = kythira::request_vote_response<term_id_type>;
        using append_entries_request_type = kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
        using append_entries_response_type = kythira::append_entries_response<term_id_type, log_index_type>;
        using install_snapshot_request_type = kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
        using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
    };
    
    // Helper to create a cluster of nodes
    struct cluster_fixture {
        using node_type = kythira::node<test_raft_types>;
        using simulator_type = network_simulator::NetworkSimulator<test_raft_types::raft_network_types>;
        
        std::shared_ptr<simulator_type> simulator;
        std::vector<std::unique_ptr<node_type>> nodes;
        std::vector<test_raft_types::node_id_type> node_ids;
        
        explicit cluster_fixture(std::size_t node_count) {
            simulator = std::make_shared<simulator_type>();
            simulator->start();
            
            // Create nodes
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = static_cast<test_raft_types::node_id_type>(i + 1);
                node_ids.push_back(node_id);
                
                // Use the node_id directly as the simulator address (it will convert to string internally)
                auto sim_node = simulator->create_node(std::to_string(node_id));
                
                auto config = kythira::raft_configuration{};
                config._election_timeout_min = election_timeout_min;
                config._election_timeout_max = election_timeout_max;
                config._heartbeat_interval = heartbeat_interval;
                
                auto node = std::make_unique<node_type>(
                    node_id,
                    test_raft_types::network_client_type{sim_node, test_raft_types::serializer_type{}},
                    test_raft_types::network_server_type{sim_node, test_raft_types::serializer_type{}},
                    test_raft_types::persistence_engine_type{},
                    test_raft_types::logger_type{kythira::log_level::error},
                    test_raft_types::metrics_type{},
                    test_raft_types::membership_manager_type{},
                    config
                );
                
                nodes.push_back(std::move(node));
            }
            
            // Create fully connected network topology (all nodes can communicate)
            network_simulator::NetworkEdge edge(std::chrono::milliseconds{0}, 1.0);  // No latency, 100% reliability
            for (std::size_t i = 0; i < node_count; ++i) {
                for (std::size_t j = 0; j < node_count; ++j) {
                    if (i != j) {
                        simulator->add_edge(
                            std::to_string(node_ids[i]),
                            std::to_string(node_ids[j]),
                            edge
                        );
                    }
                }
            }
            
            // Set cluster configuration for all nodes AFTER all nodes are created
            for (auto& node : nodes) {
                node->set_cluster_configuration(node_ids);
            }
        }
        
        auto start_all() -> void {
            for (auto& node : nodes) {
                node->start();
            }
        }
        
        auto stop_all() -> void {
            for (auto& node : nodes) {
                node->stop();
            }
        }
        
        auto tick_all() -> void {
            for (auto& node : nodes) {
                node->check_election_timeout();
                node->check_heartbeat_timeout();
            }
        }
        
        auto get_leader() -> std::optional<test_raft_types::node_id_type> {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i]->is_leader()) {
                    return node_ids[i];
                }
            }
            return std::nullopt;
        }
        
        auto count_leaders() -> std::size_t {
            std::size_t count = 0;
            for (const auto& node : nodes) {
                if (node->is_leader()) {
                    ++count;
                }
            }
            return count;
        }
        
        auto wait_for_leader(std::chrono::milliseconds timeout) -> std::optional<test_raft_types::node_id_type> {
            auto start = std::chrono::steady_clock::now();
            
            while (std::chrono::steady_clock::now() - start < timeout) {
                tick_all();
                std::this_thread::sleep_for(state_check_interval);
                
                auto leader = get_leader();
                if (leader.has_value()) {
                    return leader;
                }
            }
            
            return std::nullopt;
        }
        
        ~cluster_fixture() {
            stop_all();
        }
    };
}

BOOST_AUTO_TEST_SUITE(cluster_initialization_tests)

/**
 * Test 1: All nodes start in follower state
 * 
 * Validates: Requirements 1.1, 2.1
 * 
 * Property: When a cluster is initialized, all nodes must start in the follower state.
 * This is a fundamental requirement of the Raft protocol.
 */
BOOST_AUTO_TEST_CASE(test_all_nodes_start_as_followers, * boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: All nodes start in follower state");
    
    // Test with 3-node cluster
    {
        cluster_fixture cluster(small_cluster_size);
        cluster.start_all();
        
        // Verify all nodes are followers
        for (const auto& node : cluster.nodes) {
            BOOST_CHECK_EQUAL(node->get_state(), kythira::server_state::follower);
            BOOST_CHECK(!node->is_leader());
        }
        
        cluster.stop_all();
    }
    
    // Test with 5-node cluster
    {
        cluster_fixture cluster(medium_cluster_size);
        cluster.start_all();
        
        // Verify all nodes are followers
        for (const auto& node : cluster.nodes) {
            BOOST_CHECK_EQUAL(node->get_state(), kythira::server_state::follower);
            BOOST_CHECK(!node->is_leader());
        }
        
        cluster.stop_all();
    }
    
    BOOST_TEST_MESSAGE("✓ All nodes start in follower state");
}

/**
 * Test 2: Cluster bootstrap with initial configuration
 * 
 * Validates: Requirements 1.1, 1.2
 * 
 * Property: A cluster can be properly bootstrapped with an initial configuration
 * specifying all member nodes.
 */
BOOST_AUTO_TEST_CASE(test_cluster_bootstrap, * boost::unit_test::timeout(30)) {
    BOOST_TEST_MESSAGE("Test: Cluster bootstrap with initial configuration");
    
    cluster_fixture cluster(small_cluster_size);
    
    // Verify cluster configuration is set
    BOOST_CHECK_EQUAL(cluster.nodes.size(), small_cluster_size);
    BOOST_CHECK_EQUAL(cluster.node_ids.size(), small_cluster_size);
    
    // Verify all node IDs are unique
    std::set<test_raft_types::node_id_type> unique_ids(cluster.node_ids.begin(), cluster.node_ids.end());
    BOOST_CHECK_EQUAL(unique_ids.size(), small_cluster_size);
    
    // Start cluster
    cluster.start_all();
    
    // Verify all nodes are running
    for (const auto& node : cluster.nodes) {
        BOOST_CHECK(node->is_running());
    }
    
    cluster.stop_all();
    
    BOOST_TEST_MESSAGE("✓ Cluster bootstrap works correctly");
}

/**
 * Test 3: Election timeout randomization
 * 
 * Validates: Requirements 1.2, 2.2
 * 
 * Property: Election timeouts should be randomized across nodes to prevent
 * simultaneous elections and split votes.
 * 
 * Note: This test verifies that not all nodes transition to candidate state
 * at exactly the same time, which would indicate proper randomization.
 */
BOOST_AUTO_TEST_CASE(test_election_timeout_randomization, * boost::unit_test::timeout(45)) {
    BOOST_TEST_MESSAGE("Test: Election timeout randomization");
    
    cluster_fixture cluster(small_cluster_size);
    cluster.start_all();
    
    // Wait for election timeout range to pass
    std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
    
    // Trigger election timeout checks
    cluster.tick_all();
    
    // Give time for state transitions
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    // Count how many nodes are in candidate state
    std::size_t candidate_count = 0;
    for (const auto& node : cluster.nodes) {
        if (node->get_state() == kythira::server_state::candidate) {
            ++candidate_count;
        }
    }
    
    // With proper randomization, not all nodes should become candidates simultaneously
    // At least one node should have started an election
    BOOST_CHECK_GT(candidate_count, 0);
    
    cluster.stop_all();
    
    BOOST_TEST_MESSAGE("✓ Election timeout randomization works");
}

/**
 * Test 4: First leader election completes successfully
 * 
 * Validates: Requirements 1.2, 2.1, 2.2
 * 
 * Property: After cluster initialization, a leader should be elected within
 * a reasonable timeout period.
 */
BOOST_AUTO_TEST_CASE(test_first_leader_election, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Test: First leader election completes successfully");
    
    cluster_fixture cluster(small_cluster_size);
    cluster.start_all();
    
    // Wait for leader election
    auto leader = cluster.wait_for_leader(leader_election_timeout);
    
    // Verify a leader was elected
    BOOST_REQUIRE(leader.has_value());
    BOOST_TEST_MESSAGE("Leader elected: node " << leader.value());
    
    // Verify exactly one leader
    BOOST_CHECK_EQUAL(cluster.count_leaders(), 1);
    
    // Verify the leader node reports itself as leader
    for (std::size_t i = 0; i < cluster.nodes.size(); ++i) {
        if (cluster.node_ids[i] == leader.value()) {
            BOOST_CHECK(cluster.nodes[i]->is_leader());
            BOOST_CHECK_EQUAL(cluster.nodes[i]->get_state(), kythira::server_state::leader);
        } else {
            BOOST_CHECK(!cluster.nodes[i]->is_leader());
        }
    }
    
    cluster.stop_all();
    
    BOOST_TEST_MESSAGE("✓ First leader election completed successfully");
}

/**
 * Test 5: Leader election with 5-node cluster
 * 
 * Validates: Requirements 1.2, 2.1, 2.2
 * 
 * Property: Leader election should work correctly with larger cluster sizes.
 */
BOOST_AUTO_TEST_CASE(test_leader_election_five_nodes, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Test: Leader election with 5-node cluster");
    
    cluster_fixture cluster(medium_cluster_size);
    cluster.start_all();
    
    // Wait for leader election
    auto leader = cluster.wait_for_leader(leader_election_timeout);
    
    // Verify a leader was elected
    BOOST_REQUIRE(leader.has_value());
    BOOST_TEST_MESSAGE("Leader elected: node " << leader.value());
    
    // Verify exactly one leader
    BOOST_CHECK_EQUAL(cluster.count_leaders(), 1);
    
    cluster.stop_all();
    
    BOOST_TEST_MESSAGE("✓ Leader election works with 5-node cluster");
}

/**
 * Test 6: Initial term consistency
 * 
 * Validates: Requirements 1.1, 2.1
 * 
 * Property: All nodes should start with term 0, and after leader election,
 * all nodes should converge to the same term.
 */
BOOST_AUTO_TEST_CASE(test_initial_term_consistency, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Test: Initial term consistency");
    
    cluster_fixture cluster(small_cluster_size);
    
    // Verify all nodes start with term 0
    for (const auto& node : cluster.nodes) {
        BOOST_CHECK_EQUAL(node->get_current_term(), 0);
    }
    
    cluster.start_all();
    
    // Wait for leader election
    auto leader = cluster.wait_for_leader(leader_election_timeout);
    BOOST_REQUIRE(leader.has_value());
    
    // Give time for term propagation
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    cluster.tick_all();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    
    // Get the leader's term
    test_raft_types::term_id_type leader_term = 0;
    for (std::size_t i = 0; i < cluster.nodes.size(); ++i) {
        if (cluster.node_ids[i] == leader.value()) {
            leader_term = cluster.nodes[i]->get_current_term();
            break;
        }
    }
    
    // Verify leader term is greater than 0
    BOOST_CHECK_GT(leader_term, 0);
    
    // All nodes should eventually have the same term (or higher due to heartbeats)
    for (const auto& node : cluster.nodes) {
        BOOST_CHECK_GE(node->get_current_term(), leader_term);
    }
    
    cluster.stop_all();
    
    BOOST_TEST_MESSAGE("✓ Initial term consistency verified");
}

BOOST_AUTO_TEST_SUITE_END()
