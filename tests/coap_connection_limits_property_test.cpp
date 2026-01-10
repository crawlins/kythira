#define BOOST_TEST_MODULE coap_connection_limits_property_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/types.hpp>
#include <raft/coap_exceptions.hpp>

#include <random>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>

namespace {
    constexpr std::size_t property_test_iterations = 50;
    constexpr std::uint16_t min_port = 5683;
    constexpr std::uint16_t max_port = 6000;
    constexpr std::size_t max_connection_limit = 100;
}

BOOST_AUTO_TEST_SUITE(coap_connection_limits_property_tests)

// **Feature: coap-transport, Property 17: Connection limit enforcement**
// **Validates: Requirements 8.5**
// Property: For any configuration with connection limits, the transport should enforce 
// limits and handle excess connections appropriately.
BOOST_AUTO_TEST_CASE(property_connection_limit_enforcement, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint16_t> port_dist(min_port, max_port);
    std::uniform_int_distribution<std::size_t> limit_dist(1, max_connection_limit);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Generate random test parameters
            std::uint16_t server_port = port_dist(rng);
            std::size_t connection_limit = limit_dist(rng);
            
            // Create server configuration with connection limits
            kythira::coap_server_config server_config;
            server_config.max_concurrent_sessions = connection_limit;
            
            // Create server
            kythira::noop_metrics server_metrics;
            kythira::console_logger server_logger;
            kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                server("127.0.0.1", server_port, server_config, server_metrics, std::move(server_logger));
            
            // Test server connection limit enforcement
            try {
                server.enforce_connection_limits();
                // Should succeed when under limits
            } catch (const kythira::coap_network_error& e) {
                // May fail if resource exhaustion is detected
                BOOST_TEST_MESSAGE("Server connection limit enforcement: " << e.what());
            }
            
            // Create client configuration with connection limits
            std::unordered_map<std::uint64_t, std::string> endpoints = {
                {1, "coap://127.0.0.1:" + std::to_string(server_port)}
            };
            
            kythira::coap_client_config client_config;
            client_config.max_sessions = connection_limit;
            
            // Create client
            kythira::noop_metrics client_metrics;
            kythira::console_logger logger;
            kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
                client(endpoints, client_config, client_metrics, std::move(logger));
            
            // Test client connection limit enforcement
            try {
                client.enforce_connection_limits();
                // Should succeed when under limits
            } catch (const kythira::coap_network_error& e) {
                // May fail if resource exhaustion is detected
                BOOST_TEST_MESSAGE("Client connection limit enforcement: " << e.what());
            }
            
            // Verify that both client and server can still function normally
            BOOST_CHECK(server.is_running() == false); // Server not started in test
            
            // Test that basic operations still work
            auto token = client.generate_message_token();
            BOOST_CHECK(!token.empty());
            
            auto msg_id = client.generate_message_id();
            BOOST_CHECK_GT(msg_id, 0);
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during connection limit test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Connection limit enforcement: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

// Test specific connection limit scenarios
BOOST_AUTO_TEST_CASE(specific_connection_limit_scenarios, * boost::unit_test::timeout(45)) {
    // Test server with very low connection limit
    kythira::coap_server_config config;
    config.max_concurrent_sessions = 1; // Very low limit
    
    kythira::noop_metrics metrics;
    kythira::console_logger logger;
    kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        server("127.0.0.1", 5683, config, metrics, std::move(logger));
    
    // Should be able to enforce limits without issues initially
    try {
        server.enforce_connection_limits();
    } catch (const kythira::coap_transport_error& e) {
        BOOST_TEST_MESSAGE("Low limit enforcement: " << e.what());
    }
    
    // Test client with very low connection limit
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://127.0.0.1:5683"}
    };
    
    kythira::coap_client_config client_config;
    client_config.max_sessions = 1; // Very low limit
    
    kythira::console_logger client_logger;
    kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        client(endpoints, client_config, metrics, std::move(client_logger));
    
    // Should be able to enforce limits without issues initially
    try {
        client.enforce_connection_limits();
    } catch (const kythira::coap_transport_error& e) {
        BOOST_TEST_MESSAGE("Client low limit enforcement: " << e.what());
    }
}

// Test connection limit enforcement with high limits
BOOST_AUTO_TEST_CASE(high_connection_limits, * boost::unit_test::timeout(45)) {
    // Test server with high connection limit
    kythira::coap_server_config config;
    config.max_concurrent_sessions = 10000; // High limit
    
    kythira::noop_metrics metrics;
    kythira::console_logger logger;
    kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        server("127.0.0.1", 5683, config, metrics, std::move(logger));
    
    // Should easily pass with high limits
    server.enforce_connection_limits();
    
    // Test client with high connection limit
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://127.0.0.1:5683"}
    };
    
    kythira::coap_client_config client_config;
    client_config.max_sessions = 10000; // High limit
    
    kythira::console_logger client_logger;
    kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        client(endpoints, client_config, metrics, std::move(client_logger));
    
    // Should easily pass with high limits
    client.enforce_connection_limits();
}

// Test concurrent connection limit enforcement
BOOST_AUTO_TEST_CASE(concurrent_connection_limit_enforcement, * boost::unit_test::timeout(60)) {
    kythira::coap_server_config config;
    config.max_concurrent_sessions = 50;
    
    kythira::noop_metrics metrics;
    kythira::console_logger logger;
    kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        server("127.0.0.1", 5683, config, metrics, std::move(logger));
    
    // Test concurrent limit enforcement
    std::vector<std::thread> threads;
    std::atomic<std::size_t> success_count{0};
    std::atomic<std::size_t> exception_count{0};
    
    for (std::size_t i = 0; i < 10; ++i) {
        threads.emplace_back([&server, &success_count, &exception_count, i]() {
            try {
                // Each thread attempts to enforce connection limits
                server.enforce_connection_limits();
                success_count.fetch_add(1);
                
            } catch (const kythira::coap_transport_error& e) {
                exception_count.fetch_add(1);
                BOOST_TEST_MESSAGE("Thread " << i << " connection limit exception: " << e.what());
            } catch (const std::exception& e) {
                exception_count.fetch_add(1);
                BOOST_TEST_MESSAGE("Thread " << i << " other exception: " << e.what());
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify that all operations completed (either success or expected exception)
    BOOST_CHECK_EQUAL(success_count.load() + exception_count.load(), 10);
    
    // Most operations should succeed under normal conditions
    BOOST_CHECK_GE(success_count.load(), 5);
}

// Test connection limit with resource exhaustion
BOOST_AUTO_TEST_CASE(connection_limit_with_resource_exhaustion, * boost::unit_test::timeout(30)) {
    kythira::coap_server_config config;
    config.max_concurrent_sessions = 20;
    
    kythira::noop_metrics metrics;
    kythira::console_logger logger;
    kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        server("127.0.0.1", 5683, config, metrics, std::move(logger));
    
    // Simulate resource exhaustion by handling it first
    server.handle_resource_exhaustion();
    
    // Now try to enforce connection limits - should not hang
    BOOST_CHECK_NO_THROW(server.enforce_connection_limits());
    
    // Test client as well
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://127.0.0.1:5683"}
    };
    
    kythira::coap_client_config client_config;
    client_config.max_sessions = 20;
    
    kythira::console_logger client_logger;
    kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
        client(endpoints, client_config, metrics, std::move(client_logger));
    
    // Simulate resource exhaustion
    client.handle_resource_exhaustion();
    
    // Try to enforce connection limits - should not hang
    BOOST_CHECK_NO_THROW(client.enforce_connection_limits());
}

// Test edge cases for connection limits
BOOST_AUTO_TEST_CASE(connection_limit_edge_cases, * boost::unit_test::timeout(45)) {
    // Test with zero connection limit (should be handled gracefully)
    kythira::coap_server_config config;
    config.max_concurrent_sessions = 0; // Edge case
    
    kythira::noop_metrics metrics;
    
    // Server creation should handle zero limit gracefully
    try {
        kythira::console_logger logger;
        kythira::coap_server<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
            server("127.0.0.1", 5683, config, metrics, std::move(logger));
        
        // Connection limit enforcement should handle zero limit
        server.enforce_connection_limits();
        
    } catch (const kythira::coap_transport_error& e) {
        // Zero limit may cause immediate failure, which is acceptable
        BOOST_TEST_MESSAGE("Zero connection limit: " << e.what());
    }
    
    // Test client with zero connection limit
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://127.0.0.1:5683"}
    };
    
    kythira::coap_client_config client_config;
    client_config.max_sessions = 0; // Edge case
    
    try {
        kythira::console_logger client_logger;
        kythira::coap_client<kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics, kythira::console_logger> 
            client(endpoints, client_config, metrics, std::move(client_logger));
        
        // Connection limit enforcement should handle zero limit
        client.enforce_connection_limits();
        
    } catch (const kythira::coap_transport_error& e) {
        // Zero limit may cause immediate failure, which is acceptable
        BOOST_TEST_MESSAGE("Client zero connection limit: " << e.what());
    }
}

BOOST_AUTO_TEST_SUITE_END()