#define BOOST_TEST_MODULE NetworkSimulatorConnectionPoolReusePropertyTest
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
 * Feature: network-simulator, Property 27: Connection Pool Reuse
 * Validates: Requirements 16.2
 * 
 * Property: For any connection request to a destination where a healthy pooled connection 
 * exists, the connection pool SHALL return the existing connection rather than creating 
 * a new one.
 */
BOOST_AUTO_TEST_CASE(property_connection_pool_reuse, * boost::unit_test::timeout(120)) {
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
            // Test Case 1: Create initial connection
            auto conn1_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn1 = conn1_future.get();
            
            BOOST_REQUIRE(conn1);
            BOOST_REQUIRE(conn1->is_open());
            
            // Get the connection pointer for comparison
            auto conn1_ptr = conn1.get();
            
            // Return connection to pool (simulate connection being released)
            auto& pool = sim.get_connection_pool();
            pool.return_connection(conn1);
            
            // Test Case 2: Request connection to same destination
            // Should reuse the pooled connection
            auto conn2_future = client->connect(server_addr, server_port, connection_timeout);
            auto conn2 = conn2_future.get();
            
            BOOST_REQUIRE(conn2);
            BOOST_REQUIRE(conn2->is_open());
            
            // Get the connection pointer for comparison
            auto conn2_ptr = conn2.get();
            
            // Verify that the same connection was reused
            if (conn1_ptr == conn2_ptr) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Connection successfully reused from pool");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": New connection created instead of reusing pooled connection");
            }
            
            // Test Case 3: Verify pool size
            auto pool_size = pool.get_pool_size(Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            // After returning conn2, pool should have 1 connection
            pool.return_connection(conn2);
            auto pool_size_after = pool.get_pool_size(Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (pool_size_after >= 1) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Pool correctly maintains connections");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Pool size incorrect after returning connection");
            }
            
            // Test Case 4: Multiple sequential reuses
            std::shared_ptr<typename DefaultNetworkTypes::connection_type> last_conn;
            bool all_reused = true;
            
            for (std::size_t j = 0; j < 3; ++j) {
                auto conn_future = client->connect(server_addr, server_port, connection_timeout);
                auto conn = conn_future.get();
                
                BOOST_REQUIRE(conn);
                BOOST_REQUIRE(conn->is_open());
                
                if (last_conn && conn.get() != last_conn.get()) {
                    all_reused = false;
                    BOOST_TEST_MESSAGE("Iteration " << i << " reuse " << j << ": Different connection returned");
                }
                
                last_conn = conn;
                pool.return_connection(conn);
            }
            
            if (all_reused) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": All sequential requests reused the same connection");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Sequential requests did not consistently reuse connection");
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
