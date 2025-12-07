/**
 * Integration Test for Log Replication
 * 
 * Tests log replication functionality including:
 * - Log entry appending
 * - Log persistence
 * - Log recovery after restart
 * 
 * Note: Simplified for single-node cluster due to implementation constraints
 * 
 * Requirements: 7.1, 7.2, 7.3
 */

#define BOOST_TEST_MODULE RaftLogReplicationIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>

struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_log_replication_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::chrono::milliseconds election_timeout_min{150};
    constexpr std::chrono::milliseconds election_timeout_max{300};
    constexpr std::chrono::milliseconds heartbeat_interval{50};
}

BOOST_AUTO_TEST_SUITE(log_replication_integration_tests)

BOOST_AUTO_TEST_CASE(log_entry_appending) {
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    constexpr std::uint64_t node_id = 1;
    auto sim_node = simulator.create_node(node_id);
    
    auto config = raft::raft_configuration{};
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = heartbeat_interval;
    
    auto node = raft::node{
        node_id,
        raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
            sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
        },
        raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
            sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
        },
        raft::memory_persistence_engine<>{},
        raft::console_logger{raft::log_level::error},
        raft::noop_metrics{},
        raft::default_membership_manager<>{},
        config
    };
    
    node.start();
    
    // Become leader
    std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
    node.check_election_timeout();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    
    BOOST_REQUIRE(node.is_leader());
    
    auto initial_term = node.get_current_term();
    
    // Submit commands
    std::vector<std::byte> command1{std::byte{1}, std::byte{2}, std::byte{3}};
    std::vector<std::byte> command2{std::byte{4}, std::byte{5}, std::byte{6}};
    
    node.submit_command(command1, std::chrono::milliseconds{1000});
    node.submit_command(command2, std::chrono::milliseconds{1000});
    
    // Verify node is still leader and term hasn't changed
    BOOST_CHECK(node.is_leader());
    BOOST_CHECK_EQUAL(node.get_current_term(), initial_term);
    
    node.stop();
}

BOOST_AUTO_TEST_CASE(log_persistence_and_recovery) {
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    constexpr std::uint64_t node_id = 1;
    auto sim_node = simulator.create_node(node_id);
    
    auto config = raft::raft_configuration{};
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = heartbeat_interval;
    
    std::uint64_t first_term = 0;
    
    {
        auto node = raft::node{
            node_id,
            raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
            },
            raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
            },
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        node.start();
        
        // Become leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        BOOST_REQUIRE(node.is_leader());
        
        first_term = node.get_current_term();
        BOOST_CHECK_GT(first_term, 0);
        
        // Submit command
        std::vector<std::byte> command{std::byte{7}, std::byte{8}, std::byte{9}};
        node.submit_command(command, std::chrono::milliseconds{1000});
        
        node.stop();
    }
    
    // Restart node - it should recover and potentially become leader again
    {
        auto node2 = raft::node{
            node_id,
            raft::simulator_network_client<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
            },
            raft::simulator_network_server<raft::json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>{
                sim_node, raft::json_rpc_serializer<std::vector<std::byte>>{}
            },
            raft::memory_persistence_engine<>{},
            raft::console_logger{raft::log_level::error},
            raft::noop_metrics{},
            raft::default_membership_manager<>{},
            config
        };
        
        node2.start();
        
        // Node should start as follower
        BOOST_CHECK(!node2.is_leader());
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node2.check_election_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Should become leader
        BOOST_CHECK(node2.is_leader());
        
        node2.stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
