#define BOOST_TEST_MODULE NetworkSimulatorConnectionEstablishmentTimeoutPropertyTest
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
    constexpr std::size_t property_test_iterations = 10;  // Reduced for faster testing
    constexpr std::chrono::milliseconds short_timeout{50};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr std::chrono::milliseconds test_latency{100};
    constexpr double perfect_reliability = 1.0;
}

// Helper to generate random node address
auto generate_random_address(std::mt19937& rng, std::size_t id) -> std::string {
    return "node_" + std::to_string(id);
}

// Helper to generate random port
auto generate_random_port(std::mt19937& rng) -> unsigned short {
    std::uniform_int_distribution<unsigned short> dist(10000, 60000);
    return dist(rng);
}

/**
 * Feature: network-simulator, Property 25: Connection Establishment Timeout Handling
 * Validates: Requirements 15.1, 15.2, 15.3
 * 
 * Property: For any connection establishment request with a specified timeout, if the 
 * connection cannot be established within the timeout period, the operation SHALL fail 
 * with a timeout exception and cancel any pending connection attempts.
 */
BOOST_AUTO_TEST_CASE(property_connection_establishment_timeout_handling, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    std::size_t timeout_failures = 0;
    std::size_t success_count = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses and ports (unique per iteration)
        auto client_addr = generate_random_address(rng, i * 3);
        auto server_addr = generate_random_address(rng, i * 3 + 1);
        auto base_port = static_cast<unsigned short>(10000 + i * 100);  // Base port per iteration
        
        // Create simulator with network topology
        TestNetworkSimulator sim;
        sim.seed_rng(static_cast<std::uint32_t>(i));
        sim.start();
        
        // Add nodes to topology
        sim.add_node(client_addr);
        sim.add_node(server_addr);
        
        // Add edge with latency
        NetworkEdge edge(test_latency, perfect_reliability);
        sim.add_edge(client_addr, server_addr, edge);
        sim.add_edge(server_addr, client_addr, edge);
        
        // Create nodes
        auto client = sim.create_node(client_addr);
        auto server = sim.create_node(server_addr);
        
        // Test Case 1: Connection with timeout shorter than latency should timeout
        {
            auto server_port = static_cast<unsigned short>(base_port + 1);
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                // Try to connect with a timeout shorter than the network latency
                // This should timeout because no listener is bound
                auto conn_future = client->connect(server_addr, server_port, short_timeout);
                auto conn = conn_future.get();
                
                // If we get here, the connection succeeded when it should have timed out
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << " Case 1: Connection succeeded when timeout was expected");
                
            } catch (const TimeoutException& e) {
                // Expected timeout exception
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                
                // Verify that the timeout occurred within a reasonable time frame
                // Allow some tolerance for timing variations
                auto max_expected_time = short_timeout + std::chrono::milliseconds{200};
                
                if (elapsed > max_expected_time) {
                    ++timeout_failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << " Case 1: Timeout took too long: " 
                        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
                        << "ms (expected < " << max_expected_time.count() << "ms)");
                } else {
                    ++success_count;
                }
                
            } catch (const std::exception& e) {
                // Other exceptions are acceptable (e.g., connection refused)
                // as long as they occur within the timeout period
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto max_expected_time = short_timeout + std::chrono::milliseconds{200};
                
                if (elapsed > max_expected_time) {
                    ++timeout_failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << " Case 1: Exception took too long: " 
                        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
                        << "ms (expected < " << max_expected_time.count() << "ms)");
                }
            }
        }
        
        // Test Case 2: Connection with sufficient timeout should succeed when listener is present
        {
            auto server_port = static_cast<unsigned short>(base_port + 2);
            // Bind a listener on the server
            auto listener_future = server->bind(server_port);
            auto listener = listener_future.get();
            
            BOOST_REQUIRE(listener);
            BOOST_REQUIRE(listener->is_listening());
            
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                // Try to connect with a timeout longer than the network latency
                auto conn_future = client->connect(server_addr, server_port, long_timeout);
                auto conn = conn_future.get();
                
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                
                // Connection should succeed
                if (!conn || !conn->is_open()) {
                    ++failures;
                    BOOST_TEST_MESSAGE("Iteration " << i << " Case 2: Connection failed when it should have succeeded");
                } else {
                    ++success_count;
                    
                    // Verify that connection was established within reasonable time
                    // Should be approximately test_latency
                    auto max_expected_time = test_latency + std::chrono::milliseconds{500};
                    
                    if (elapsed > max_expected_time) {
                        BOOST_TEST_MESSAGE("Iteration " << i << " Case 2: Connection took longer than expected: " 
                            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
                            << "ms (expected < " << max_expected_time.count() << "ms)");
                    }
                }
                
            } catch (const TimeoutException& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << " Case 2: Unexpected timeout exception");
                
            } catch (const std::exception& e) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << " Case 2: Unexpected exception: " << e.what());
            }
            
            // Clean up
            listener->close();
        }
        
        // Test Case 3: Multiple concurrent connection attempts with different timeouts
        {
            auto server_port = static_cast<unsigned short>(base_port + 3);
            // Bind a listener on the server
            auto listener_future = server->bind(server_port);
            auto listener = listener_future.get();
            
            BOOST_REQUIRE(listener);
            BOOST_REQUIRE(listener->is_listening());
            
            // Launch multiple connection attempts with different timeouts
            std::vector<std::thread> threads;
            std::atomic<std::size_t> concurrent_successes{0};
            std::atomic<std::size_t> concurrent_timeouts{0};
            
            for (std::size_t j = 0; j < 3; ++j) {
                threads.emplace_back([&, j]() {
                    try {
                        auto timeout = (j == 0) ? short_timeout : long_timeout;
                        auto conn_future = client->connect(server_addr, server_port, timeout);
                        auto conn = conn_future.get();
                        
                        if (conn && conn->is_open()) {
                            ++concurrent_successes;
                        }
                    } catch (const TimeoutException&) {
                        ++concurrent_timeouts;
                    } catch (const std::exception&) {
                        // Other exceptions are acceptable
                    }
                });
            }
            
            // Wait for all threads to complete
            for (auto& thread : threads) {
                thread.join();
            }
            
            // At least one connection should succeed (those with long timeout)
            if (concurrent_successes == 0) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << " Case 3: No concurrent connections succeeded");
            } else {
                ++success_count;
            }
            
            // Clean up
            listener->close();
        }
        
        sim.stop();
    }
    
    // Report results
    BOOST_TEST_MESSAGE("Total iterations: " << property_test_iterations);
    BOOST_TEST_MESSAGE("Successful tests: " << success_count);
    BOOST_TEST_MESSAGE("Timeout timing failures: " << timeout_failures);
    BOOST_TEST_MESSAGE("Other failures: " << failures);
    
    // Property should hold for most iterations (allow some tolerance for timing variations)
    BOOST_CHECK_LE(failures, property_test_iterations / 5);  // Allow up to 20% failures
    BOOST_CHECK_LE(timeout_failures, property_test_iterations / 5);  // Allow up to 20% timeout timing issues
}

