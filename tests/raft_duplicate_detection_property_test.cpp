/**
 * Property-Based Test for Duplicate Detection
 * 
 * Feature: raft-consensus, Property 19: Duplicate Detection
 * Validates: Requirements 11.4
 * 
 * Property: For any client operation with a serial number, if the operation is retried,
 * the system detects the duplicate and returns the cached response without re-executing.
 */

#define BOOST_TEST_MODULE RaftDuplicateDetectionPropertyTest
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
        char* argv_data[] = {const_cast<char*>("raft_duplicate_detection_property_test"), nullptr};
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
    constexpr std::uint64_t test_client_id = 12345;
}

BOOST_AUTO_TEST_SUITE(duplicate_detection_property_tests)

/**
 * Property: Duplicate requests return cached response
 * 
 * For any client operation with a serial number, if the same serial number
 * is submitted again, the system returns the cached response.
 */
BOOST_AUTO_TEST_CASE(duplicate_requests_return_cached_response) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit a command with serial number 1
        std::vector<std::byte> command{std::byte{42}};
        constexpr std::uint64_t serial_number = 1;
        
        auto first_future = node.submit_command_with_session(
            test_client_id,
            serial_number,
            command,
            rpc_timeout
        );
        
        // Wait for first submission to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_REQUIRE(first_future.isReady());
        BOOST_REQUIRE(!first_future.hasException());
        
        auto first_response = first_future.value();
        
        // Submit the same command with the same serial number (duplicate)
        auto second_future = node.submit_command_with_session(
            test_client_id,
            serial_number,
            command,
            rpc_timeout
        );
        
        // Wait for second submission to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_REQUIRE(second_future.isReady());
        BOOST_REQUIRE(!second_future.hasException());
        
        auto second_response = second_future.value();
        
        // Verify that both responses are identical (cached response)
        BOOST_CHECK_EQUAL(first_response.size(), second_response.size());
        BOOST_CHECK(first_response == second_response);
        
        node.stop();
    }
}

/**
 * Property: Old serial numbers return cached response
 * 
 * For any client that has submitted requests with serial numbers 1..N,
 * resubmitting any request with serial number <= N returns the cached response.
 */
BOOST_AUTO_TEST_CASE(old_serial_numbers_return_cached_response) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit multiple commands with increasing serial numbers
        constexpr std::size_t num_commands = 5;
        std::vector<std::byte> command{std::byte{42}};
        
        for (std::uint64_t serial = 1; serial <= num_commands; ++serial) {
            auto future = node.submit_command_with_session(
                test_client_id,
                serial,
                command,
                rpc_timeout
            );
            
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            BOOST_REQUIRE(future.isReady());
            BOOST_REQUIRE(!future.hasException());
        }
        
        // Now resubmit an old serial number (e.g., serial number 3)
        constexpr std::uint64_t old_serial = 3;
        auto retry_future = node.submit_command_with_session(
            test_client_id,
            old_serial,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Should succeed and return cached response
        BOOST_CHECK(retry_future.isReady());
        BOOST_CHECK(!retry_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: New client sessions start with serial number 1
 * 
 * For any new client (one that hasn't submitted requests before),
 * the first serial number must be 1.
 */
BOOST_AUTO_TEST_CASE(new_client_sessions_start_with_serial_one) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Try to submit with serial number != 1 for a new client
        std::vector<std::byte> command{std::byte{42}};
        constexpr std::uint64_t new_client_id = 99999;
        constexpr std::uint64_t invalid_serial = 5;
        
        auto future = node.submit_command_with_session(
            new_client_id,
            invalid_serial,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Should fail because serial number must start at 1
        BOOST_CHECK(future.isReady());
        BOOST_CHECK(future.hasException());
        
        // Now try with serial number 1 - should succeed
        auto valid_future = node.submit_command_with_session(
            new_client_id,
            1,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_CHECK(valid_future.isReady());
        BOOST_CHECK(!valid_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: Serial numbers must be sequential
 * 
 * For any client session, serial numbers must increase by exactly 1.
 * Skipping serial numbers should be rejected.
 */
BOOST_AUTO_TEST_CASE(serial_numbers_must_be_sequential) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit command with serial number 1
        std::vector<std::byte> command{std::byte{42}};
        
        auto first_future = node.submit_command_with_session(
            test_client_id,
            1,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_REQUIRE(first_future.isReady());
        BOOST_REQUIRE(!first_future.hasException());
        
        // Try to skip to serial number 3 (should fail)
        auto skip_future = node.submit_command_with_session(
            test_client_id,
            3,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Should fail because we skipped serial number 2
        BOOST_CHECK(skip_future.isReady());
        BOOST_CHECK(skip_future.hasException());
        
        // Now submit with serial number 2 (should succeed)
        auto valid_future = node.submit_command_with_session(
            test_client_id,
            2,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_CHECK(valid_future.isReady());
        BOOST_CHECK(!valid_future.hasException());
        
        node.stop();
    }
}

/**
 * Property: Different clients have independent sessions
 * 
 * For any two different clients, their serial numbers are tracked independently.
 */
BOOST_AUTO_TEST_CASE(different_clients_have_independent_sessions) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit commands from client 1
        constexpr std::uint64_t client_1 = 100;
        constexpr std::uint64_t client_2 = 200;
        std::vector<std::byte> command{std::byte{42}};
        
        // Client 1 submits with serial 1
        auto client1_future1 = node.submit_command_with_session(
            client_1,
            1,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_REQUIRE(client1_future1.isReady());
        BOOST_REQUIRE(!client1_future1.hasException());
        
        // Client 2 should also be able to submit with serial 1 (independent session)
        auto client2_future1 = node.submit_command_with_session(
            client_2,
            1,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_CHECK(client2_future1.isReady());
        BOOST_CHECK(!client2_future1.hasException());
        
        // Client 1 submits with serial 2
        auto client1_future2 = node.submit_command_with_session(
            client_1,
            2,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_CHECK(client1_future2.isReady());
        BOOST_CHECK(!client1_future2.hasException());
        
        // Client 2 submits with serial 2
        auto client2_future2 = node.submit_command_with_session(
            client_2,
            2,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_CHECK(client2_future2.isReady());
        BOOST_CHECK(!client2_future2.hasException());
        
        node.stop();
    }
}

/**
 * Property: Retrying with same serial number multiple times returns same response
 * 
 * For any client operation, retrying the same serial number multiple times
 * always returns the same cached response.
 */
BOOST_AUTO_TEST_CASE(multiple_retries_return_same_response) {
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
        
        // Trigger election to make node a leader
        std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{50});
        node.check_election_timeout();
        
        // Give time for election to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify node became leader
        BOOST_REQUIRE(node.is_leader());
        
        // Submit initial command
        std::vector<std::byte> command{std::byte{42}};
        constexpr std::uint64_t serial_number = 1;
        
        auto first_future = node.submit_command_with_session(
            test_client_id,
            serial_number,
            command,
            rpc_timeout
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        BOOST_REQUIRE(first_future.isReady());
        BOOST_REQUIRE(!first_future.hasException());
        
        auto first_response = first_future.value();
        
        // Retry multiple times with the same serial number
        constexpr std::size_t num_retries = 5;
        for (std::size_t retry = 0; retry < num_retries; ++retry) {
            auto retry_future = node.submit_command_with_session(
                test_client_id,
                serial_number,
                command,
                rpc_timeout
            );
            
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            BOOST_REQUIRE(retry_future.isReady());
            BOOST_REQUIRE(!retry_future.hasException());
            
            auto retry_response = retry_future.value();
            
            // All retries should return the same response
            BOOST_CHECK_EQUAL(first_response.size(), retry_response.size());
            BOOST_CHECK(first_response == retry_response);
        }
        
        node.stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
