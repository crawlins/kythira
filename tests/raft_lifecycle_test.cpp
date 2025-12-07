#define BOOST_TEST_MODULE raft_lifecycle_test
#include <boost/test/included/unit_test.hpp>

#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/membership.hpp>

#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_lifecycle_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_log_index = 10;
}

// Test node initialization and lifecycle
BOOST_AUTO_TEST_CASE(test_node_lifecycle) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create network node
    auto sim_node = simulator.create_node(test_node_id);
    
    // Create components
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto network_client = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto network_server = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto persistence = raft::memory_persistence_engine<>{};
    auto logger = raft::console_logger{};
    auto metrics = raft::noop_metrics{};
    auto membership = raft::default_membership_manager<>{};
    
    // Create node
    auto node = raft::node{
        test_node_id,
        std::move(network_client),
        std::move(network_server),
        std::move(persistence),
        std::move(logger),
        std::move(metrics),
        std::move(membership)
    };
    
    // Initially not running
    BOOST_CHECK(!node.is_running());
    
    // Start the node
    node.start();
    BOOST_CHECK(node.is_running());
    
    // Verify initial state
    BOOST_CHECK_EQUAL(node.get_node_id(), test_node_id);
    BOOST_CHECK_EQUAL(node.get_current_term(), 0);  // Initial term is 0
    BOOST_CHECK_EQUAL(node.get_state(), raft::server_state::follower);
    BOOST_CHECK(!node.is_leader());
    
    // Stop the node
    node.stop();
    BOOST_CHECK(!node.is_running());
}

// Test state recovery from persistence
BOOST_AUTO_TEST_CASE(test_state_recovery) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create network node
    auto sim_node = simulator.create_node(test_node_id);
    
    // Create persistence engine and save some state
    auto persistence = raft::memory_persistence_engine<>{};
    persistence.save_current_term(test_term);
    persistence.save_voted_for(test_node_id);
    
    // Add some log entries
    for (std::uint64_t i = 1; i <= test_log_index; ++i) {
        auto entry = raft::log_entry<std::uint64_t, std::uint64_t>{
            test_term,
            i,
            std::vector<std::byte>{std::byte{static_cast<unsigned char>(i)}}
        };
        persistence.append_log_entry(entry);
    }
    
    // Create components
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto network_client = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto network_server = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto logger = raft::console_logger{};
    auto metrics = raft::noop_metrics{};
    auto membership = raft::default_membership_manager<>{};
    
    // Create node with persistence that has saved state
    auto node = raft::node{
        test_node_id,
        std::move(network_client),
        std::move(network_server),
        std::move(persistence),
        std::move(logger),
        std::move(metrics),
        std::move(membership)
    };
    
    // Start the node - should recover state from persistence
    node.start();
    
    // Verify recovered state
    BOOST_CHECK_EQUAL(node.get_current_term(), test_term);
    BOOST_CHECK_EQUAL(node.get_state(), raft::server_state::follower);
    
    node.stop();
}

// Test multiple start/stop cycles
BOOST_AUTO_TEST_CASE(test_multiple_start_stop_cycles) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create network node
    auto sim_node = simulator.create_node(test_node_id);
    
    // Create components
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto network_client = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto network_server = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto persistence = raft::memory_persistence_engine<>{};
    auto logger = raft::console_logger{};
    auto metrics = raft::noop_metrics{};
    auto membership = raft::default_membership_manager<>{};
    
    // Create node
    auto node = raft::node{
        test_node_id,
        std::move(network_client),
        std::move(network_server),
        std::move(persistence),
        std::move(logger),
        std::move(metrics),
        std::move(membership)
    };
    
    // Multiple start/stop cycles
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(!node.is_running());
        node.start();
        BOOST_CHECK(node.is_running());
        node.stop();
        BOOST_CHECK(!node.is_running());
    }
}

// Test idempotent start and stop
BOOST_AUTO_TEST_CASE(test_idempotent_start_stop) {
    // Create network simulator
    auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
    simulator.start();
    
    // Create network node
    auto sim_node = simulator.create_node(test_node_id);
    
    // Create components
    auto serializer = raft::json_rpc_serializer<std::vector<std::byte>>{};
    auto network_client = raft::simulator_network_client<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto network_server = raft::simulator_network_server<
        raft::json_rpc_serializer<std::vector<std::byte>>,
        std::vector<std::byte>
    >{sim_node, serializer};
    auto persistence = raft::memory_persistence_engine<>{};
    auto logger = raft::console_logger{};
    auto metrics = raft::noop_metrics{};
    auto membership = raft::default_membership_manager<>{};
    
    // Create node
    auto node = raft::node{
        test_node_id,
        std::move(network_client),
        std::move(network_server),
        std::move(persistence),
        std::move(logger),
        std::move(metrics),
        std::move(membership)
    };
    
    // Multiple start calls should be idempotent
    node.start();
    BOOST_CHECK(node.is_running());
    node.start();  // Should not cause issues
    BOOST_CHECK(node.is_running());
    
    // Multiple stop calls should be idempotent
    node.stop();
    BOOST_CHECK(!node.is_running());
    node.stop();  // Should not cause issues
    BOOST_CHECK(!node.is_running());
}
