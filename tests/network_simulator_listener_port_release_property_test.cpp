#define BOOST_TEST_MODULE NetworkSimulatorListenerPortReleasePropertyTest
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
 * Feature: network-simulator, Property 31: Listener Port Release
 * Validates: Requirements 17.6
 * 
 * Property: For any listener that is closed, the bound port SHALL be immediately 
 * released and made available for new listeners to bind to.
 */
BOOST_AUTO_TEST_CASE(property_listener_port_immediate_release, * boost::unit_test::timeout(120)) {
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
        
        // Get listener manager reference
        auto& listener_manager = sim.get_listener_manager();
        
        // Verify port is initially available
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        // Bind a listener on the server
        auto listener_future = server->bind(server_port);
        auto listener = listener_future.get();
        
        BOOST_REQUIRE(listener != nullptr);
        BOOST_REQUIRE(listener->is_listening());
        
        // Verify port is no longer available
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        // Close the listener through ListenerManager
        listener_manager.close_listener(Endpoint<DefaultNetworkTypes>(server_addr, server_port));
        
        // Verify port is immediately available after close
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        // Verify we can immediately bind to the same port again
        auto listener_future2 = server->bind(server_port);
        auto listener2 = listener_future2.get();
        
        BOOST_REQUIRE(listener2 != nullptr);
        BOOST_REQUIRE(listener2->is_listening());
        
        // Verify port is allocated again
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: network-simulator, Property 31: Multiple Port Release
 * Validates: Requirements 17.6
 * 
 * Property: For any set of listeners that are closed, all bound ports SHALL be 
 * immediately released and made available for reuse.
 */
BOOST_AUTO_TEST_CASE(property_multiple_listener_port_release, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::size_t success_count = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses and ports (unique per iteration)
        auto server_addr = generate_random_address(rng, i * 2);
        auto base_port = generate_random_port(rng, i * 100);
        
        // Create simulator with network topology
        TestNetworkSimulator sim;
        sim.seed_rng(static_cast<std::uint32_t>(i));
        sim.start();
        
        // Add node to topology
        sim.add_node(server_addr);
        
        // Create server node
        auto server = sim.create_node(server_addr);
        
        // Get listener manager reference
        auto& listener_manager = sim.get_listener_manager();
        
        // Create multiple listeners on different ports
        constexpr std::size_t num_listeners = 5;
        std::vector<unsigned short> ports;
        std::vector<std::shared_ptr<typename DefaultNetworkTypes::listener_type>> listeners;
        
        for (std::size_t j = 0; j < num_listeners; ++j) {
            auto port = base_port + static_cast<unsigned short>(j);
            ports.push_back(port);
            
            // Verify port is available before binding
            BOOST_CHECK(listener_manager.is_port_available(port));
            
            auto listener_future = server->bind(port);
            auto listener = listener_future.get();
            
            BOOST_REQUIRE(listener != nullptr);
            BOOST_REQUIRE(listener->is_listening());
            listeners.push_back(listener);
            
            // Verify port is no longer available
            BOOST_CHECK(!listener_manager.is_port_available(port));
        }
        
        // Close all listeners
        for (std::size_t j = 0; j < num_listeners; ++j) {
            listener_manager.close_listener(Endpoint<DefaultNetworkTypes>(server_addr, ports[j]));
        }
        
        // Verify all ports are immediately available after close
        for (const auto& port : ports) {
            BOOST_CHECK(listener_manager.is_port_available(port));
        }
        
        // Verify we can bind to all ports again
        for (const auto& port : ports) {
            auto listener_future = server->bind(port);
            auto listener = listener_future.get();
            
            BOOST_REQUIRE(listener != nullptr);
            BOOST_REQUIRE(listener->is_listening());
            
            // Verify port is allocated again
            BOOST_CHECK(!listener_manager.is_port_available(port));
        }
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}

/**
 * Feature: network-simulator, Property 31: Port Release on Simulator Stop
 * Validates: Requirements 17.6
 * 
 * Property: For any listener when the simulator is stopped, the bound port SHALL 
 * be released and made available for reuse after restart.
 */
BOOST_AUTO_TEST_CASE(property_port_release_on_stop, * boost::unit_test::timeout(120)) {
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
        
        // Get listener manager reference
        auto& listener_manager = sim.get_listener_manager();
        
        // Bind a listener on the server
        auto listener_future = server->bind(server_port);
        auto listener = listener_future.get();
        
        BOOST_REQUIRE(listener != nullptr);
        BOOST_REQUIRE(listener->is_listening());
        
        // Verify port is allocated
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        // Stop the simulator
        sim.stop();
        
        // Verify port is released after stop
        BOOST_CHECK(listener_manager.is_port_available(server_port));
        
        // Restart the simulator
        sim.start();
        
        // Verify we can bind to the same port after restart
        auto listener_future2 = server->bind(server_port);
        auto listener2 = listener_future2.get();
        
        BOOST_REQUIRE(listener2 != nullptr);
        BOOST_REQUIRE(listener2->is_listening());
        
        // Verify port is allocated again
        BOOST_CHECK(!listener_manager.is_port_available(server_port));
        
        success_count++;
    }
    
    BOOST_TEST_MESSAGE("Property test completed: " << success_count << " iterations succeeded, " 
                      << failures << " iterations failed");
    BOOST_CHECK_EQUAL(failures, 0);
}
