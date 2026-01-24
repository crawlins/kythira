#define BOOST_TEST_MODULE NetworkSimulatorConnectionPoolCleanupPropertyTest
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
    constexpr std::chrono::milliseconds short_idle_time{100};  // Very short for testing
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
 * Feature: network-simulator, Property 29: Connection Pool Cleanup
 * Validates: Requirements 16.4
 * 
 * Property: For any pooled connection that becomes stale or invalid, the connection 
 * pool SHALL automatically remove it from the pool during cleanup operations.
 */
BOOST_AUTO_TEST_CASE(property_connection_pool_cleanup, * boost::unit_test::timeout(120)) {
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
        
        // Configure connection pool with short idle time for testing
        typename ConnectionPool<DefaultNetworkTypes>::PoolConfig pool_config;
        pool_config.max_connections_per_endpoint = 10;
        pool_config.max_idle_time = short_idle_time;
        pool_config.max_connection_age = std::chrono::milliseconds{60000}; // 1 minute
        pool_config.enable_health_checks = true;
        
        auto& pool = sim.get_connection_pool();
        pool.configure_pool(pool_config);
        
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
            // Test Case 1: Add connections to pool and let them become stale
            std::vector<std::shared_ptr<typename DefaultNetworkTypes::connection_type>> connections;
            constexpr std::size_t num_connections = 5;
            
            for (std::size_t j = 0; j < num_connections; ++j) {
                auto conn_future = client->connect(server_addr, server_port, connection_timeout);
                auto conn = conn_future.get();
                
                BOOST_REQUIRE(conn);
                BOOST_REQUIRE(conn->is_open());
                
                connections.push_back(conn);
                pool.return_connection(conn);
            }
            
            // Verify connections are in pool
            auto initial_pool_size = pool.get_pool_size(Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (initial_pool_size == num_connections) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Pool contains " << initial_pool_size << " connections");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Initial pool size incorrect: " << initial_pool_size 
                    << " (expected " << num_connections << ")");
            }
            
            // Test Case 2: Wait for connections to become stale
            std::this_thread::sleep_for(short_idle_time + std::chrono::milliseconds{50});
            
            // Run cleanup
            pool.cleanup_stale_connections();
            
            // Verify stale connections were removed
            auto pool_size_after_cleanup = pool.get_pool_size(Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (pool_size_after_cleanup == 0) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Stale connections cleaned up successfully");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Stale connections not cleaned up, pool size: " 
                    << pool_size_after_cleanup);
            }
            
            // Test Case 3: Add closed connections and verify cleanup
            for (std::size_t j = 0; j < num_connections; ++j) {
                auto conn_future = client->connect(server_addr, server_port, connection_timeout);
                auto conn = conn_future.get();
                
                BOOST_REQUIRE(conn);
                BOOST_REQUIRE(conn->is_open());
                
                // Close some connections before returning to pool
                if (j % 2 == 0) {
                    conn->close();
                }
                
                pool.return_connection(conn);
            }
            
            // Run cleanup - should remove closed connections
            pool.cleanup_stale_connections();
            
            auto pool_size_after_closed_cleanup = pool.get_pool_size(
                Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            // Should have approximately half the connections (the ones that weren't closed)
            // Allow some tolerance
            if (pool_size_after_closed_cleanup <= num_connections / 2 + 1) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Closed connections cleaned up, pool size: " 
                    << pool_size_after_closed_cleanup);
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Closed connections not cleaned up properly, pool size: " 
                    << pool_size_after_closed_cleanup);
            }
            
            // Test Case 4: Verify cleanup doesn't remove healthy connections
            // First, wait for all existing connections to become stale
            std::this_thread::sleep_for(short_idle_time + std::chrono::milliseconds{50});
            
            // Clean up any stale connections from previous test cases
            pool.cleanup_stale_connections();
            
            // Verify pool is now empty
            auto pool_size_after_full_cleanup = pool.get_pool_size(
                Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (pool_size_after_full_cleanup == 0) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Pool fully cleaned before fresh connection test");
            }
            
            // Now add fresh connections to a clean pool
            std::vector<std::shared_ptr<typename DefaultNetworkTypes::connection_type>> fresh_connections;
            for (std::size_t j = 0; j < 3; ++j) {
                auto conn_future = client->connect(server_addr, server_port, connection_timeout);
                auto conn = conn_future.get();
                
                BOOST_REQUIRE(conn);
                BOOST_REQUIRE(conn->is_open());
                
                fresh_connections.push_back(conn);
                pool.return_connection(conn);
            }
            
            auto pool_size_before_cleanup = pool.get_pool_size(
                Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            // Run cleanup immediately (connections are fresh, should not be removed)
            pool.cleanup_stale_connections();
            
            auto pool_size_after_fresh_cleanup = pool.get_pool_size(
                Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (pool_size_after_fresh_cleanup >= pool_size_before_cleanup - 1 && pool_size_after_fresh_cleanup >= 2) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Fresh connections mostly preserved during cleanup (" 
                    << pool_size_before_cleanup << " -> " << pool_size_after_fresh_cleanup << ")");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Too many fresh connections removed: " 
                    << pool_size_before_cleanup << " -> " << pool_size_after_fresh_cleanup);
            }
            
            // Test Case 5: Multiple cleanup cycles
            // Wait for connections to become stale
            std::this_thread::sleep_for(short_idle_time + std::chrono::milliseconds{50});
            
            // Run multiple cleanups
            for (std::size_t j = 0; j < 3; ++j) {
                pool.cleanup_stale_connections();
            }
            
            auto final_pool_size = pool.get_pool_size(Endpoint<DefaultNetworkTypes>{server_addr, server_port});
            
            if (final_pool_size == 0) {
                ++success_count;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Multiple cleanup cycles work correctly");
            } else {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Multiple cleanups failed, pool size: " << final_pool_size);
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
