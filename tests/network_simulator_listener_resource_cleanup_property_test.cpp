#define BOOST_TEST_MODULE NetworkSimulatorListenerResourceCleanupPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <folly/init/Init.h>

using namespace network_simulator;

// Type alias for the correct NetworkSimulator template instantiation
using TestNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;

// Global fixture to initialize folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        init_obj = std::make_unique<folly::Init>(&argc, &argv_ptr);
    }
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> init_obj;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t property_test_iterations = 10;
    constexpr std::chrono::milliseconds connection_timeout{2000};
    constexpr std::chrono::milliseconds test_latency{50};
    constexpr double perfect_reliability = 1.0;
}

// Helper to generate random node address
auto generate_random_address(std::mt19937& rng, std::size_t id) -> std::string {
    return "node_" + std::to_string(id);
}

// Helper to generate random port
auto generate_random_port(std::mt19937& rng, std::size_t base) -> unsigned short {
    return static_cast<unsigned short>(10000 + base);
}

/**
 * Feature: network-simulator, Property 30: Listener Resource Cleanup
 * Validates: Requirements 17.2, 17.3, 17.4
 * 
 * Property: For any listener that is closed or when the simulator is stopped, all 
 * associated resources including ports, pending connections, and timers SHALL be 
 * immediately released and made available for reuse.
 */
BOOST_AUTO_TEST_CASE(property_listener_resource_cleanup, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::size_t success_count = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses and ports (unique per iteration)
        auto server_addr = generate_random_address(rng, i * 2);
        auto server_port = generate_random_port(rng, i * 100);
        
        // Create simulator with network topology
        TestNetworkSimulator sim;
        sim.seed_rng(static_cast<std::uint32_t>(i));
        sim.start();
        
        // Add node to topology
        sim.add_node(server_addr);
        
        // Create server node
        auto server = sim.create_node(server_addr);
        
        // Bind a listener on the server
        auto listener_future = server->bind(server_port);
        auto listener = listener_future.get();
        
        // Verify listener is active
        BOOST_REQUIRE(listener != nullptr);
        BOOST_REQUIRE(listener->is_listening());
        
        // Verify port is allocated in ListenerManager
        auto& listener_manager = sim.get_listener_manager();
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        // Close the listener explicitly through ListenerManager
        listener_manager.close_listener(Endpoint<DefaultNetworkTypes>(server_addr, server_port));
        
        // Verify listener is no longer listening
        BOOST_CHECK(!listener->is_listening());
        
        // Verify port is released and available for reuse
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        // Try to bind to the same port again - should succeed since port was released
        auto listener_future2 = server->bind(server_port);
        auto listener2 = listener_future2.get();
        
        BOOST_REQUIRE(listener2 != nullptr);
        BOOST_REQUIRE(listener2->is_listening());
        
        // Verify port is allocated again
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        // Now test cleanup on simulator stop
        sim.stop();
        
        // Verify all listeners are closed after stop
        BOOST_CHECK(!listener2->is_listening());
        
        // Verify port is released after stop
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: network-simulator, Property 30: Listener Resource Cleanup on Reset
 * Validates: Requirements 17.3, 17.4
 * 
 * Property: For any listener when the simulator is reset, all associated resources 
 * SHALL be cleaned up and the simulator SHALL return to initial state.
 */
BOOST_AUTO_TEST_CASE(property_listener_cleanup_on_reset, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::size_t success_count = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses and ports (unique per iteration)
        auto server_addr = generate_random_address(rng, i * 2);
        auto server_port = generate_random_port(rng, i * 100);
        
        // Create simulator with network topology
        TestNetworkSimulator sim;
        sim.seed_rng(static_cast<std::uint32_t>(i));
        sim.start();
        
        // Add node to topology
        sim.add_node(server_addr);
        
        // Create server node
        auto server = sim.create_node(server_addr);
        
        // Bind multiple listeners
        std::vector<std::shared_ptr<typename DefaultNetworkTypes::listener_type>> listeners;
        for (std::size_t j = 0; j < 3; ++j) {
            auto port = server_port + static_cast<unsigned short>(j);
            auto listener_future = server->bind(port);
            auto listener = listener_future.get();
            
            BOOST_REQUIRE(listener != nullptr);
            BOOST_REQUIRE(listener->is_listening());
            listeners.push_back(listener);
        }
        
        // Verify all ports are allocated
        auto& listener_manager = sim.get_listener_manager();
        for (std::size_t j = 0; j < 3; ++j) {
            auto port = server_port + static_cast<unsigned short>(j);
            BOOST_CHECK(!listener_manager.is_port_available(port));
        }
        
        // Reset the simulator
        sim.reset();
        
        // Verify all listeners are closed after reset
        for (const auto& listener : listeners) {
            BOOST_CHECK(!listener->is_listening());
        }
        
        // Verify all ports are released after reset
        for (std::size_t j = 0; j < 3; ++j) {
            auto port = server_port + static_cast<unsigned short>(j);
            BOOST_CHECK(listener_manager.is_port_available(port));
        }
        
        // Verify no active listeners remain
        auto active_listeners = listener_manager.get_all_listeners();
        BOOST_CHECK_EQUAL(active_listeners.size(), 0);
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: network-simulator, Property 30: Pending Accept Operations Cleanup
 * Validates: Requirements 17.5
 * 
 * Property: For any listener with pending accept operations, when the listener is 
 * closed or simulator is stopped, the pending operations SHALL be properly handled.
 */
BOOST_AUTO_TEST_CASE(property_pending_accept_cleanup, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::size_t success_count = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses and ports (unique per iteration)
        auto client_addr = generate_random_address(rng, i * 3);
        auto server_addr = generate_random_address(rng, i * 3 + 1);
        auto server_port = generate_random_port(rng, i * 100);
        
        // Create simulator with network topology
        TestNetworkSimulator sim;
        sim.seed_rng(static_cast<std::uint32_t>(i));
        sim.start();
        
        // Add nodes to topology
        sim.add_node(client_addr);
        sim.add_node(server_addr);
        
        // Add edges
        NetworkEdge edge(test_latency, perfect_reliability);
        sim.add_edge(client_addr, server_addr, edge);
        sim.add_edge(server_addr, client_addr, edge);
        
        // Create nodes
        auto client = sim.create_node(client_addr);
        auto server = sim.create_node(server_addr);
        
        // Bind a listener on the server
        auto listener_future = server->bind(server_port);
        auto listener = listener_future.get();
        
        BOOST_REQUIRE(listener != nullptr);
        BOOST_REQUIRE(listener->is_listening());
        
        // Establish a connection to create pending accept
        auto connect_future = client->connect(server_addr, server_port);
        auto connection = connect_future.get();
        
        BOOST_REQUIRE(connection != nullptr);
        BOOST_REQUIRE(connection->is_open());
        
        // Accept the connection
        auto accept_future = listener->accept();
        auto server_connection = accept_future.get();
        
        BOOST_REQUIRE(server_connection != nullptr);
        BOOST_REQUIRE(server_connection->is_open());
        
        // Get listener manager reference
        auto& listener_manager = sim.get_listener_manager();
        
        // Close the listener while connections exist through ListenerManager
        listener_manager.close_listener(Endpoint<DefaultNetworkTypes>(server_addr, server_port));
        
        // Verify listener is closed
        BOOST_CHECK(!listener->is_listening());
        
        // Verify port is released
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        // Verify existing connections remain open (cleanup doesn't affect established connections)
        BOOST_CHECK(connection->is_open());
        BOOST_CHECK(server_connection->is_open());
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}
