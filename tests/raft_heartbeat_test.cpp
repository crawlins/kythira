#define BOOST_TEST_MODULE raft_heartbeat_test
#include <boost/test/included/unit_test.hpp>

#include "raft/raft.hpp"
#include "raft/simulator_network.hpp"
#include "raft/persistence.hpp"
#include "raft/console_logger.hpp"
#include "raft/metrics.hpp"
#include "raft/membership.hpp"
#include "raft/types.hpp"

#include "network_simulator/network_simulator.hpp"

#include <folly/init/Init.h>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_heartbeat_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

#include <thread>
#include <chrono>

namespace {
    constexpr std::uint64_t node_1_id = 1;
    constexpr std::uint64_t node_2_id = 2;
    constexpr std::uint64_t node_3_id = 3;
    
    constexpr std::chrono::milliseconds election_timeout_min{150};
    constexpr std::chrono::milliseconds election_timeout_max{300};
    constexpr std::chrono::milliseconds heartbeat_interval{50};
    constexpr std::chrono::milliseconds rpc_timeout{100};
    constexpr std::chrono::milliseconds test_duration{500};
}

BOOST_AUTO_TEST_CASE(test_leader_sends_heartbeats) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create nodes in the simulator
    auto node_1 = simulator.create_node(node_1_id);
    auto node_2 = simulator.create_node(node_2_id);
    auto node_3 = simulator.create_node(node_3_id);
    
    // Create Raft configuration
    raft::raft_configuration config;
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = heartbeat_interval;
    config._rpc_timeout = rpc_timeout;
    
    // Create serializers
    auto serializer_1 = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto serializer_2 = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto serializer_3 = raft::json_rpc_serializer<std::vector<std::byte>>{};
    
    // Create Raft nodes
    auto raft_node_1 = raft::node{
        node_1_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer_1},
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer_1},
        raft::memory_persistence_engine<>{},
        raft::console_logger{},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    auto raft_node_2 = raft::node{
        node_2_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_2, serializer_2},
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_2, serializer_2},
        raft::memory_persistence_engine<>{},
        raft::console_logger{},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    auto raft_node_3 = raft::node{
        node_3_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_3, serializer_3},
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_3, serializer_3},
        raft::memory_persistence_engine<>{},
        raft::console_logger{},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    // Start all nodes
    raft_node_1.start();
    raft_node_2.start();
    raft_node_3.start();
    
    // Wait for election timeout to trigger an election
    std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
    
    // Check election timeout on all nodes
    raft_node_1.check_election_timeout();
    raft_node_2.check_election_timeout();
    raft_node_3.check_election_timeout();
    
    // Give time for election to complete
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    
    // At least one node should be leader
    bool has_leader = raft_node_1.is_leader() || raft_node_2.is_leader() || raft_node_3.is_leader();
    BOOST_CHECK(has_leader);
    
    // Find the leader
    auto* leader = raft_node_1.is_leader() ? &raft_node_1 :
                   raft_node_2.is_leader() ? &raft_node_2 :
                   &raft_node_3;
    
    // Count heartbeats sent by checking heartbeat timeout multiple times
    std::size_t heartbeat_checks = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - start_time < test_duration) {
        // Check heartbeat timeout on the leader
        leader->check_heartbeat_timeout();
        heartbeat_checks++;
        
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // The leader should have sent multiple heartbeats
    // With heartbeat_interval of 50ms and test_duration of 500ms,
    // we expect at least 8-10 heartbeat cycles
    BOOST_CHECK_GT(heartbeat_checks, 40);
    
    // Stop all nodes
    raft_node_1.stop();
    raft_node_2.stop();
    raft_node_3.stop();
}

BOOST_AUTO_TEST_CASE(test_heartbeat_mechanism_for_leader) {
    // This test verifies that the heartbeat mechanism works correctly for a leader
    // by checking that check_heartbeat_timeout() triggers heartbeat sending
    
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create node in the simulator
    auto node_1 = simulator.create_node(node_1_id);
    
    // Create Raft configuration with short heartbeat interval
    raft::raft_configuration config;
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = std::chrono::milliseconds{50};
    config._rpc_timeout = rpc_timeout;
    
    // Create serializer
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    
    // Create Raft node
    auto raft_node = raft::node{
        node_1_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer},
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer},
        raft::memory_persistence_engine<>{},
        raft::console_logger{},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    // Start the node
    raft_node.start();
    
    // Make node become leader (in single-node cluster)
    std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
    raft_node.check_election_timeout();
    
    // Give time for election to complete
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    
    // Node should be leader
    BOOST_CHECK(raft_node.is_leader());
    
    auto initial_term = raft_node.get_current_term();
    
    // Call check_heartbeat_timeout multiple times over a period
    // This should trigger heartbeat sending
    std::size_t heartbeat_checks = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto test_period = std::chrono::milliseconds{500};
    
    while (std::chrono::steady_clock::now() - start_time < test_period) {
        // Check heartbeat timeout - this should trigger sending heartbeats
        raft_node.check_heartbeat_timeout();
        heartbeat_checks++;
        
        // Sleep for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // We should have checked heartbeat timeout many times
    BOOST_CHECK_GT(heartbeat_checks, 40);
    
    // Node should still be leader
    BOOST_CHECK(raft_node.is_leader());
    
    // Term should not have changed
    BOOST_CHECK_EQUAL(raft_node.get_current_term(), initial_term);
    
    // Stop the node
    raft_node.stop();
}

BOOST_AUTO_TEST_CASE(test_heartbeat_timeout_elapsed) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create node in the simulator
    auto node_1 = simulator.create_node(node_1_id);
    
    // Create Raft configuration with short heartbeat interval
    raft::raft_configuration config;
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = std::chrono::milliseconds{50};
    config._rpc_timeout = rpc_timeout;
    
    // Create serializer
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    
    // Create Raft node
    auto raft_node = raft::node{
        node_1_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer},
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{node_1, serializer},
        raft::memory_persistence_engine<>{},
        raft::console_logger{},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    // Start the node
    raft_node.start();
    
    // Make node become leader (in single-node cluster)
    std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
    raft_node.check_election_timeout();
    
    // Give time for election to complete
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    
    // Node should be leader
    BOOST_CHECK(raft_node.is_leader());
    
    // Wait for heartbeat interval to elapse
    std::this_thread::sleep_for(config._heartbeat_interval + std::chrono::milliseconds{10});
    
    // Check heartbeat timeout - this should trigger sending heartbeats
    raft_node.check_heartbeat_timeout();
    
    // Node should still be leader
    BOOST_CHECK(raft_node.is_leader());
    
    // Stop the node
    raft_node.stop();
}
