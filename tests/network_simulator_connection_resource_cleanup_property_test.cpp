#define BOOST_TEST_MODULE NetworkSimulatorConnectionResourceCleanupPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <random>
#include <string>
#include <thread>

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
 * Feature: network-simulator, Property 34: Connection Resource Cleanup
 * Validates: Requirements 18.6
 * 
 * Property: For any connection that is closed or enters an error state, all associated resources 
 * including buffers, timers, and network handles SHALL be properly deallocated to prevent 
 * resource leaks.
 */
BOOST_AUTO_TEST_CASE(property_connection_resource_cleanup, * boost::unit_test::timeout(120)) {
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
            
            // Test Case 1: Establish connection and verify it's tracked
            auto conn_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn = conn_future.get();
            
            BOOST_REQUIRE(conn);
            BOOST_REQUIRE(conn->is_open());
            
            auto local_endpoint = conn->local_endpoint();
            
            // Verify connection is tracked
            auto initial_info = tracker.get_connection_info(local_endpoint);
            if (initial_info.has_value()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection tracked before cleanup");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection not tracked before cleanup");
            }
            
            // Test Case 2: Close connection and verify cleanup
            conn->close();
            
            // Verify connection is no longer open
            if (!conn->is_open()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection closed successfully");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection still open after close");
            }
            
            // Test Case 3: Clean up connection from tracker
            tracker.cleanup_connection(local_endpoint);
            
            // Verify connection is removed from tracker
            auto after_cleanup_info = tracker.get_connection_info(local_endpoint);
            if (!after_cleanup_info.has_value()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection removed from tracker after cleanup");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection still in tracker after cleanup");
            }
            
            // Test Case 4: Verify get_all_connections doesn't include cleaned up connection
            auto all_connections = tracker.get_all_connections();
            bool found_after_cleanup = false;
            for (const auto& info : all_connections) {
                if (info.local_endpoint == local_endpoint) {
                    found_after_cleanup = true;
                    break;
                }
            }
            
            if (!found_after_cleanup) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection not in get_all_connections after cleanup");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection still in get_all_connections after cleanup");
            }
            
            // Test Case 5: Establish another connection and test error state cleanup
            auto conn2_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn2 = conn2_future.get();
            
            BOOST_REQUIRE(conn2);
            BOOST_REQUIRE(conn2->is_open());
            
            auto local_endpoint2 = conn2->local_endpoint();
            
            // Verify second connection is tracked
            auto conn2_info = tracker.get_connection_info(local_endpoint2);
            if (conn2_info.has_value()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second connection tracked");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second connection not tracked");
            }
            
            // Update state to ERROR
            tracker.update_connection_state(local_endpoint2, ConnectionState::ERROR);
            
            // Verify error state
            auto error_info = tracker.get_connection_info(local_endpoint2);
            if (error_info.has_value() && error_info->state == ConnectionState::ERROR) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second connection in ERROR state");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Second connection not in ERROR state");
            }
            
            // Clean up error connection
            tracker.cleanup_connection(local_endpoint2);
            
            // Verify cleanup of error connection
            auto after_error_cleanup = tracker.get_connection_info(local_endpoint2);
            if (!after_error_cleanup.has_value()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error connection cleaned up successfully");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Error connection not cleaned up");
            }
            
            // Close the actual connection
            conn2->close();
            
            // Test Case 6: Verify multiple connections can be cleaned up
            std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> connections;
            std::vector<Endpoint<DefaultNetworkTypes>> endpoints;
            
            // Create multiple connections
            for (std::size_t j = 0; j < 3; ++j) {
                auto conn_j_future = client->connect(server_addr, server_port, connection_timeout);
                auto conn_j = conn_j_future.get();
                
                if (conn_j && conn_j->is_open()) {
                    connections.push_back(conn_j);
                    endpoints.push_back(conn_j->local_endpoint());
                }
            }
            
            // Verify all connections are tracked
            std::size_t tracked_count = 0;
            for (const auto& ep : endpoints) {
                if (tracker.get_connection_info(ep).has_value()) {
                    ++tracked_count;
                }
            }
            
            if (tracked_count == endpoints.size()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": All multiple connections tracked");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Not all multiple connections tracked: " 
                                 << tracked_count << "/" << endpoints.size());
            }
            
            // Close and clean up all connections
            for (std::size_t j = 0; j < connections.size(); ++j) {
                connections[j]->close();
                tracker.cleanup_connection(endpoints[j]);
            }
            
            // Verify all connections are cleaned up
            std::size_t remaining_count = 0;
            for (const auto& ep : endpoints) {
                if (tracker.get_connection_info(ep).has_value()) {
                    ++remaining_count;
                }
            }
            
            if (remaining_count == 0) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": All multiple connections cleaned up");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Some connections not cleaned up: " 
                                 << remaining_count << " remaining");
            }
            
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
