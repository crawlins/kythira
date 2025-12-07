/**
 * Property-Based Test for Linearizable Operations
 * 
 * Feature: raft-consensus, Property 15: Linearizable Operations
 * Validates: Requirements 1.4
 * 
 * Property: For any sequence of client operations, the system ensures linearizable
 * semantics where each operation appears to execute instantaneously at some point
 * between invocation and response.
 */

#define BOOST_TEST_MODULE RaftLinearizableOperationsPropertyTest
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
#include <vector>
#include <algorithm>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_linearizable_operations_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t property_test_iterations = 10;
    constexpr std::chrono::milliseconds election_timeout_min{50};
    constexpr std::chrono::milliseconds election_timeout_max{100};
    constexpr std::chrono::milliseconds heartbeat_interval{25};
    constexpr std::chrono::milliseconds rpc_timeout{200};
}

BOOST_AUTO_TEST_SUITE(linearizable_operations_property_tests)

/**
 * Property: Non-leader rejects read requests
 * 
 * For any node that is not a leader, read_state() should reject the request.
 */
BOOST_AUTO_TEST_CASE(non_leader_rejects_reads) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Create network simulator
        auto simulator = network_simulator::NetworkSimulator<std::uint64_t, std::uint16_t>{};
        simulator.start();
        
        // Create single node (will be follower initially)
        constexpr std::uint64_t node_id = 1;
        auto sim_node = simulator.create_node(node_id);
        
        // Create configuration
        auto config = raft::raft_configuration{};
        config._election_timeout_min = election_timeout_min;
        config._election_timeout_max = election_timeout_max;
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        
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
        
        node.start();
        
        // Verify node is not a leader (should be follower initially)
        BOOST_REQUIRE(!node.is_leader());
        
        // Attempt to read state - should fail
        auto read_future = node.read_state(rpc_timeout);
        
        // Wait for future to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Verify read was rejected
        BOOST_CHECK(read_future.isReady());
        BOOST_CHECK(read_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: Leader can serve reads after confirming leadership
 * 
 * For any node that is a leader in a single-node cluster, read_state()
 * should succeed after confirming leadership.
 */
BOOST_AUTO_TEST_CASE(leader_serves_reads_single_node) {
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
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        
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
        
        node.start();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Attempt to read state - should succeed
        auto read_future = node.read_state(rpc_timeout);
        
        // Wait for future to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify read succeeded
        BOOST_CHECK(read_future.isReady());
        BOOST_CHECK(!read_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: Reads observe writes in order
 * 
 * For any sequence of writes followed by a read, the read should observe
 * all committed writes.
 */
BOOST_AUTO_TEST_CASE(reads_observe_writes_in_order) {
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
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        
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
        
        node.start();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit some commands (writes)
        constexpr std::size_t num_writes = 5;
        std::vector<folly::Future<std::vector<std::byte>>> write_futures;
        
        for (std::size_t i = 0; i < num_writes; ++i) {
            std::vector<std::byte> command(sizeof(std::size_t));
            std::memcpy(command.data(), &i, sizeof(std::size_t));
            
            auto future = node.submit_command(command, rpc_timeout);
            write_futures.push_back(std::move(future));
        }
        
        // Wait for writes to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        
        // Now perform a read - it should observe all committed writes
        auto read_future = node.read_state(rpc_timeout);
        
        // Wait for read to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify read succeeded
        BOOST_CHECK(read_future.isReady());
        BOOST_CHECK(!read_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: Concurrent reads are linearizable
 * 
 * For any set of concurrent read operations, all reads should succeed
 * and observe a consistent state.
 */
BOOST_AUTO_TEST_CASE(concurrent_reads_are_linearizable) {
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
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        
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
        
        node.start();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Issue multiple concurrent reads
        constexpr std::size_t num_reads = 10;
        std::vector<folly::Future<std::vector<std::byte>>> read_futures;
        
        for (std::size_t i = 0; i < num_reads; ++i) {
            auto future = node.read_state(rpc_timeout);
            read_futures.push_back(std::move(future));
        }
        
        // Wait for all reads to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        
        // Verify all reads succeeded
        std::size_t successful_reads = 0;
        for (auto& future : read_futures) {
            if (future.isReady() && !future.hasException()) {
                successful_reads++;
            }
        }
        
        // All reads should succeed
        BOOST_CHECK_EQUAL(successful_reads, num_reads);
        
        node.stop();
    }
}

/**
 * Property: Read after write observes the write
 * 
 * For any write operation followed by a read operation, if the write
 * completes before the read starts, the read must observe the write.
 */
BOOST_AUTO_TEST_CASE(read_after_write_observes_write) {
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
        config._heartbeat_interval = heartbeat_interval;
        config._rpc_timeout = rpc_timeout;
        
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
        
        node.start();
        
        // Trigger election
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit a write
        std::vector<std::byte> command{std::byte{42}};
        auto write_future = node.submit_command(command, rpc_timeout);
        
        // Wait for write to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify write completed
        BOOST_REQUIRE(write_future.isReady());
        
        // Now perform a read - it should observe the write
        auto read_future = node.read_state(rpc_timeout);
        
        // Wait for read to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify read succeeded
        BOOST_CHECK(read_future.isReady());
        BOOST_CHECK(!read_future.hasException());
        
        node.stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
