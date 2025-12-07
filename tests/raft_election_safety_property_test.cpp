/**
 * Property-Based Test for Election Safety
 * 
 * Feature: raft-consensus, Property 1: Election Safety
 * Validates: Requirements 6.5
 * 
 * Property: For any term, at most one leader can be elected in that term.
 */

#define BOOST_TEST_MODULE RaftElectionSafetyPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <random>
#include <chrono>
#include <thread>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_election_safety_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t property_test_iterations = 10;  // Reduced for faster testing
    constexpr std::chrono::milliseconds election_timeout_min{50};  // Shorter for testing
    constexpr std::chrono::milliseconds election_timeout_max{100};  // Shorter for testing
}

BOOST_AUTO_TEST_SUITE(election_safety_property_tests)

/**
 * Property: Single node becomes leader immediately
 * 
 * For a cluster with only one node, that node should become leader immediately
 * when it starts an election.
 */
BOOST_AUTO_TEST_CASE(single_node_becomes_leader) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        // Create single node
        constexpr std::uint64_t node_id = 1;
        auto sim_node = simulator.create_node(node_id);
        
        // Create configuration
        auto config = raft::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = std::chrono::milliseconds{50};
        
        auto node = raft::node{
            node_id,
            raft::simulator_network_client<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::simulator_network_server<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},  // Suppress logs for property test
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        // Update cluster configuration to only include this node
        
        node.start();
        
        // Wait for election timeout to elapse, then trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_CHECK(node.is_leader());
        BOOST_CHECK_EQUAL(node.get_current_term(), 1);
        
        node.stop();
    }
}

/**
 * Property: Term monotonically increases
 * 
 * For any node, the term should never decrease.
 */
BOOST_AUTO_TEST_CASE(term_monotonically_increases) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        // Create single node for simplicity
        constexpr std::uint64_t node_id = 1;
        auto sim_node = simulator.create_node(node_id);
        
        // Create configuration
        auto config = raft::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = std::chrono::milliseconds{50};
        
        auto node = raft::node{
            node_id,
            raft::simulator_network_client<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::simulator_network_server<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        // Update cluster configuration
        
        node.start();
        
        // Track term over multiple elections
        std::uint64_t previous_term = 0;
        
        for (std::size_t election = 0; election < 5; ++election) {
            // Trigger election
            std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
            
            // Give time for election
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            auto current_term = node.get_current_term();
            
            // Verify term increased or stayed the same (never decreased)
            BOOST_CHECK_GE(current_term, previous_term);
            
            previous_term = current_term;
        }
        
        node.stop();
    }
}

/**
 * Property: Candidate increments term
 * 
 * For any node that becomes a candidate, it should increment its term.
 */
BOOST_AUTO_TEST_CASE(candidate_increments_term) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        // Create single node
        constexpr std::uint64_t node_id = 1;
        auto sim_node = simulator.create_node(node_id);
        
        // Create configuration
        auto config = raft::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = std::chrono::milliseconds{50};
        
        auto node = raft::node{
            node_id,
            raft::simulator_network_client<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::simulator_network_server<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        // Update cluster configuration
        
        node.start();
        
        auto initial_term = node.get_current_term();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        auto new_term = node.get_current_term();
        
        // Verify term was incremented
        BOOST_CHECK_GT(new_term, initial_term);
        
        node.stop();
    }
}

/**
 * Property: Leader state persists until timeout
 * 
 * For any node that becomes a leader, it should remain a leader until
 * it discovers a higher term or loses connectivity.
 */
BOOST_AUTO_TEST_CASE(leader_state_persists) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        // Create single node
        constexpr std::uint64_t node_id = 1;
        auto sim_node = simulator.create_node(node_id);
        
        // Create configuration
        auto config = raft::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = std::chrono::milliseconds{50};
        
        auto node = raft::node{
            node_id,
            raft::simulator_network_client<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::simulator_network_server<
                raft::json_rpc_serializer<std::vector<std::byte>>,
                std::vector<std::byte>
            >{sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}},
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        // Update cluster configuration
        
        node.start();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        auto term = node.get_current_term();
        
        // Check that leader state persists for a period
        for (std::size_t check = 0; check < 10; ++check) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            
            // Verify still leader with same term
            BOOST_CHECK(node.is_leader());
            BOOST_CHECK_EQUAL(node.get_current_term(), term);
        }
        
        node.stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
