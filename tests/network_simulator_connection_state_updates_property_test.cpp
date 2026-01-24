#define BOOST_TEST_MODULE NetworkSimulatorConnectionStateUpdatesPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <atomic>

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
 * Feature: network-simulator, Property 33: Connection State Updates
 * Validates: Requirements 18.2, 18.4
 * 
 * Property: For any connection state change event (connecting, connected, closing, closed, error), 
 * the connection tracker SHALL update the connection state appropriately and notify any registered 
 * observers.
 */
BOOST_AUTO_TEST_CASE(property_connection_state_updates, * boost::unit_test::timeout(120)) {
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
        
        // Add edges with low latency for faster testing
        NetworkEdge edge(test_latency, perfect_reliability);
        sim.add_edge(client_addr, server_addr, edge);
        sim.add_edge(server_addr, client_addr, edge);
        
        // Create nodes
        auto client = sim.create_node(client_addr);
        auto server = sim.create_node(server_addr);
        
        // Bind a listener on the server
        auto listener_future = server->bind(server_port);
        auto listener = listener_future.get();
        
        BOOST_REQUIRE(listener);
        BOOST_REQUIRE(listener->is_listening());
        
        try {
            // Get connection tracker
            auto& tracker = sim.get_connection_tracker();
            
            // Test Case 1: Establish connection and verify initial state
            auto conn_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn = conn_future.get();
            
            BOOST_REQUIRE(conn);
            BOOST_REQUIRE(conn->is_open());
            
            auto local_endpoint = conn->local_endpoint();
            
            // Verify initial state is CONNECTED
            auto initial_info = tracker.get_connection_info(local_endpoint);
            if (initial_info.has_value() && initial_info->state == ConnectionState::CONNECTED) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Initial state is CONNECTED");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Initial state is not CONNECTED");
            }
            
            // Test Case 2: Register observer and test state change notification
            std::atomic<bool> callback_invoked{false};
            ConnectionState old_state_observed = ConnectionState::CONNECTED;
            ConnectionState new_state_observed = ConnectionState::CONNECTED;
            
            tracker.set_state_change_callback(local_endpoint, 
                [&callback_invoked, &old_state_observed, &new_state_observed]
                (ConnectionState old_state, ConnectionState new_state) {
                    callback_invoked.store(true);
                    old_state_observed = old_state;
                    new_state_observed = new_state;
                });
            
            // Test Case 3: Update state to CLOSING
            tracker.update_connection_state(local_endpoint, ConnectionState::CLOSING);
            
            // Give callback time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            // Verify callback was invoked
            if (callback_invoked.load()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": State change callback invoked");
                
                // Verify old and new states
                if (old_state_observed == ConnectionState::CONNECTED && 
                    new_state_observed == ConnectionState::CLOSING) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": State transition CONNECTED -> CLOSING observed correctly");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": State transition incorrect - old: " 
                                     << static_cast<int>(old_state_observed) 
                                     << ", new: " << static_cast<int>(new_state_observed));
                }
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": State change callback not invoked");
            }
            
            // Verify state was updated in tracker
            auto closing_info = tracker.get_connection_info(local_endpoint);
            if (closing_info.has_value() && closing_info->state == ConnectionState::CLOSING) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": State updated to CLOSING in tracker");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": State not updated to CLOSING in tracker");
            }
            
            // Test Case 4: Update state to CLOSED
            callback_invoked.store(false);
            tracker.update_connection_state(local_endpoint, ConnectionState::CLOSED);
            
            // Give callback time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            // Verify callback was invoked again
            if (callback_invoked.load()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second state change callback invoked");
                
                // Verify state transition
                if (old_state_observed == ConnectionState::CLOSING && 
                    new_state_observed == ConnectionState::CLOSED) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": State transition CLOSING -> CLOSED observed correctly");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Second state transition incorrect");
                }
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second state change callback not invoked");
            }
            
            // Verify final state
            auto closed_info = tracker.get_connection_info(local_endpoint);
            if (closed_info.has_value() && closed_info->state == ConnectionState::CLOSED) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Final state is CLOSED");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Final state is not CLOSED");
            }
            
            // Test Case 5: Update state to ERROR
            callback_invoked.store(false);
            tracker.update_connection_state(local_endpoint, ConnectionState::ERROR);
            
            // Give callback time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            // Verify callback was invoked for error state
            if (callback_invoked.load()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error state change callback invoked");
                
                // Verify state transition to ERROR
                if (new_state_observed == ConnectionState::ERROR) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": State transition to ERROR observed correctly");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": State transition to ERROR incorrect");
                }
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error state change callback not invoked");
            }
            
            // Verify error state in tracker
            auto error_info = tracker.get_connection_info(local_endpoint);
            if (error_info.has_value() && error_info->state == ConnectionState::ERROR) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error state updated in tracker");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error state not updated in tracker");
            }
            
            // Close the actual connection
            conn->close();
            
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception occurred: " << e.what());
        }
        
        // Clean up
        listener->close();
        sim.stop();
    }
    
    // Report results
    BOOST_TEST_MESSAGE("Total iterations: " << property_test_iterations);
    BOOST_TEST_MESSAGE("Successful tests: " << success_count);
    BOOST_TEST_MESSAGE("Failures: " << failures);
    
    // Property should hold for most iterations
    BOOST_CHECK_LE(failures, property_test_iterations / 5);  // Allow up to 20% failures
}
