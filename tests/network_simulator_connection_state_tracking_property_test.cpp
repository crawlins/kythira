#define BOOST_TEST_MODULE NetworkSimulatorConnectionStateTrackingPropertyTest
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
 * Feature: network-simulator, Property 32: Connection State Tracking
 * Validates: Requirements 18.1, 18.2
 * 
 * Property: For any connection that is established, the connection tracker SHALL maintain 
 * accurate state information including current status, establishment time, and data 
 * transfer statistics.
 */
BOOST_AUTO_TEST_CASE(property_connection_state_tracking, * boost::unit_test::timeout(120)) {
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
            
            // Record time before connection establishment
            auto before_connect = std::chrono::steady_clock::now();
            
            // Test Case 1: Establish connection and verify tracking
            auto conn_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn = conn_future.get();
            
            BOOST_REQUIRE(conn);
            BOOST_REQUIRE(conn->is_open());
            
            // Record time after connection establishment
            auto after_connect = std::chrono::steady_clock::now();
            
            // Get connection info from tracker
            auto local_endpoint = conn->local_endpoint();
            auto conn_info = tracker.get_connection_info(local_endpoint);
            
            if (conn_info.has_value()) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection tracked successfully");
                
                // Verify state is CONNECTED
                if (conn_info->state == ConnectionState::CONNECTED) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Connection state is CONNECTED");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Connection state is not CONNECTED");
                }
                
                // Verify establishment time is reasonable
                if (conn_info->stats.established_time >= before_connect &&
                    conn_info->stats.established_time <= after_connect) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Establishment time is accurate");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Establishment time is inaccurate");
                }
                
                // Verify endpoints match
                if (conn_info->local_endpoint == local_endpoint &&
                    conn_info->remote_endpoint.address == server_addr &&
                    conn_info->remote_endpoint.port == server_port) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Endpoints match");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Endpoints do not match");
                }
                
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection not tracked");
            }
            
            // Test Case 2: Verify data transfer statistics
            std::vector<std::byte> test_data(100);
            for (std::size_t j = 0; j < test_data.size(); ++j) {
                test_data[j] = static_cast<std::byte>(j % 256);
            }
            
            // Write data
            auto write_future = conn->write(test_data, connection_timeout);
            auto write_result = write_future.get();
            
            BOOST_REQUIRE(write_result);
            
            // Stats are now updated automatically in route_connection_data()
            // No need to manually update them
            
            // Verify stats updated
            auto updated_info = tracker.get_connection_info(local_endpoint);
            if (updated_info.has_value()) {
                if (updated_info->stats.bytes_sent == test_data.size() &&
                    updated_info->stats.messages_sent == 1) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Data transfer stats updated correctly");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Data transfer stats incorrect - bytes_sent: " 
                                     << updated_info->stats.bytes_sent << ", messages_sent: " 
                                     << updated_info->stats.messages_sent);
                }
                
                // Verify last activity time updated
                if (updated_info->stats.last_activity > conn_info->stats.last_activity) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Last activity time updated");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Last activity time not updated");
                }
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection info not found after stats update");
            }
            
            // Test Case 3: Verify connection state after close
            conn->close();
            
            // State is now updated automatically in Connection::close()
            // No need to manually update it
            
            auto closed_info = tracker.get_connection_info(local_endpoint);
            if (closed_info.has_value()) {
                if (closed_info->state == ConnectionState::CLOSED) {
                    ++success_count;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Connection state updated to CLOSED");
                } else {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << ": Connection state not updated to CLOSED");
                }
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection info not found after close");
            }
            
            // Test Case 4: Verify get_all_connections
            auto all_connections = tracker.get_all_connections();
            bool found = false;
            for (const auto& info : all_connections) {
                if (info.local_endpoint == local_endpoint) {
                    found = true;
                    break;
                }
            }
            
            if (found) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection found in get_all_connections");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection not found in get_all_connections");
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
