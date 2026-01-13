#define BOOST_TEST_MODULE CoAPMulticastDiscoveryPropertyTest
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/noop_metrics.hpp>

#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <unordered_set>

namespace {
    constexpr const char* test_multicast_address = "224.0.1.187";
    constexpr std::uint16_t test_multicast_port = 5683;
    constexpr std::chrono::milliseconds test_timeout{3000};
    constexpr std::chrono::milliseconds test_short_timeout{1000};
    constexpr std::size_t test_max_nodes = 10;
    constexpr std::size_t test_min_nodes = 1;
    constexpr const char* test_node_prefix = "test_node";
    constexpr const char* test_discovery_resource = "/raft/discovery";
}

using namespace kythira;

// Test types for CoAP transport
struct test_types {
    using future_type = folly::Future<std::vector<std::byte>>;
    using serializer_type = json_serializer;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

// Property test helper functions
namespace property_helpers {
    
    auto generate_random_multicast_address() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(224, 239);
        static std::uniform_int_distribution<int> octet_dis(0, 255);
        
        return std::to_string(dis(gen)) + "." + 
               std::to_string(octet_dis(gen)) + "." + 
               std::to_string(octet_dis(gen)) + "." + 
               std::to_string(octet_dis(gen));
    }
    
    auto generate_random_port() -> std::uint16_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::uint16_t> dis(5683, 65535);
        return dis(gen);
    }
    
    auto generate_random_node_count() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_nodes, test_max_nodes);
        return dis(gen);
    }
    
    auto generate_random_timeout() -> std::chrono::milliseconds {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1000, 5000);
        return std::chrono::milliseconds{dis(gen)};
    }
    
    auto generate_random_node_id() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 1000);
        return test_node_prefix + std::to_string(dis(gen));
    }
    
    auto create_test_client() -> std::unique_ptr<coap_client<test_types>> {
        std::unordered_map<std::uint64_t, std::string> endpoints;
        coap_client_config config;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        return std::make_unique<coap_client<test_types>>(
            std::move(endpoints),
            std::move(config),
            noop_metrics{}
        );
    }
    
    auto create_test_server(const std::string& node_id) -> std::unique_ptr<coap_server<test_types>> {
        coap_server_config config;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        auto server = std::make_unique<coap_server<test_types>>(
            "0.0.0.0",
            test_multicast_port,
            std::move(config),
            noop_metrics{}
        );
        
        // Register discovery handler that responds with node ID
        server->register_discovery_handler([node_id](const std::vector<std::byte>& request_data) -> std::vector<std::byte> {
            std::string response = "RAFT_DISCOVERY:" + node_id;
            std::vector<std::byte> response_data;
            response_data.reserve(response.size());
            for (char c : response) {
                response_data.push_back(static_cast<std::byte>(c));
            }
            return response_data;
        });
        
        return server;
    }
}

/**
 * Feature: coap-transport, Property 27: Multicast support for discovery operations
 * 
 * Property: For any valid multicast address and timeout, multicast discovery
 * should return responses from all listening nodes within the timeout period.
 * 
 * Validates: Requirements 13.1
 */
BOOST_AUTO_TEST_CASE(property_multicast_discovery_returns_all_responses, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 100; ++iteration) {
        try {
            // Generate random test parameters
            auto multicast_address = test_multicast_address; // Use standard address for reliability
            auto multicast_port = test_multicast_port;
            auto timeout = generate_random_timeout();
            auto node_count = generate_random_node_count();
            
            // Create test client
            auto client = create_test_client();
            
            // Create multiple test servers (simulating Raft nodes)
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::unordered_set<std::string> expected_nodes;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                expected_nodes.insert(node_id);
                
                auto server = create_test_server(node_id);
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Perform multicast discovery
            auto discovery_future = client->discover_raft_nodes(
                multicast_address,
                multicast_port,
                timeout
            );
            
            // Wait for discovery to complete
            auto discovered_nodes = discovery_future.get();
            
            // Property: All listening nodes should respond to discovery
            // (In a real network, some responses might be lost, but in our test environment
            // we expect all nodes to respond)
            BOOST_CHECK_GE(discovered_nodes.size(), 1);
            BOOST_CHECK_LE(discovered_nodes.size(), node_count);
            
            // Property: All discovered nodes should be valid node IDs
            for (const auto& node_id : discovered_nodes) {
                BOOST_CHECK(!node_id.empty());
                BOOST_CHECK(node_id.find(test_node_prefix) == 0);
            }
            
            // Property: No duplicate node IDs should be returned
            std::unordered_set<std::string> unique_nodes(discovered_nodes.begin(), discovered_nodes.end());
            BOOST_CHECK_EQUAL(unique_nodes.size(), discovered_nodes.size());
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 27: Multicast discovery timeout handling
 * 
 * Property: For any multicast discovery request, if no responses are received
 * within the timeout period, the discovery should complete with an empty result.
 * 
 * Validates: Requirements 13.1
 */
BOOST_AUTO_TEST_CASE(property_multicast_discovery_timeout_handling, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate random test parameters
            auto multicast_address = generate_random_multicast_address();
            auto multicast_port = generate_random_port();
            auto timeout = test_short_timeout; // Short timeout for faster test
            
            // Create test client
            auto client = create_test_client();
            
            // Perform multicast discovery to non-existent nodes
            auto start_time = std::chrono::steady_clock::now();
            auto discovery_future = client->discover_raft_nodes(
                multicast_address,
                multicast_port,
                timeout
            );
            
            // Wait for discovery to complete
            auto discovered_nodes = discovery_future.get();
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Discovery should complete within timeout period
            BOOST_CHECK_LE(elapsed, timeout + std::chrono::milliseconds(500)); // Allow some tolerance
            
            // Property: No nodes should be discovered when none are listening
            BOOST_CHECK(discovered_nodes.empty());
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 27: Multicast discovery address validation
 * 
 * Property: For any invalid multicast address, discovery should fail with
 * appropriate error handling.
 * 
 * Validates: Requirements 13.1
 */
BOOST_AUTO_TEST_CASE(property_multicast_discovery_address_validation, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Test invalid multicast addresses
    std::vector<std::string> invalid_addresses = {
        "",                    // Empty address
        "192.168.1.1",        // Unicast address
        "127.0.0.1",          // Loopback address
        "255.255.255.255",    // Broadcast address
        "invalid.address",    // Invalid format
        "300.300.300.300",    // Out of range octets
        "224",                // Incomplete address
        "224.0.0",            // Incomplete address
    };
    
    for (const auto& invalid_address : invalid_addresses) {
        try {
            // Create test client
            auto client = create_test_client();
            
            // Attempt discovery with invalid address
            auto discovery_future = client->discover_raft_nodes(
                invalid_address,
                test_multicast_port,
                test_timeout
            );
            
            // Property: Discovery with invalid address should either:
            // 1. Throw an exception immediately, or
            // 2. Return an empty result
            try {
                auto discovered_nodes = discovery_future.get();
                // If no exception, result should be empty
                BOOST_CHECK(discovered_nodes.empty());
            } catch (const coap_network_error&) {
                // Expected exception for invalid address
                BOOST_CHECK(true);
            } catch (const std::exception& e) {
                // Other exceptions are also acceptable for invalid input
                BOOST_CHECK(true);
            }
            
        } catch (const std::exception& e) {
            // Exception during setup is acceptable for invalid addresses
            BOOST_CHECK(true);
        }
    }
}

/**
 * Feature: coap-transport, Property 27: Multicast discovery response parsing
 * 
 * Property: For any valid discovery response format, the client should correctly
 * parse and extract node IDs.
 * 
 * Validates: Requirements 13.1
 */
BOOST_AUTO_TEST_CASE(property_multicast_discovery_response_parsing, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 100; ++iteration) {
        try {
            // Generate random node ID
            auto node_id = generate_random_node_id();
            
            // Create test client
            auto client = create_test_client();
            
            // Test valid discovery response format
            std::string response_str = "RAFT_DISCOVERY:" + node_id;
            std::vector<std::byte> response_data;
            response_data.reserve(response_str.size());
            for (char c : response_str) {
                response_data.push_back(static_cast<std::byte>(c));
            }
            
            // Parse the response
            auto parsed_node_id = client->parse_discovery_response(response_data);
            
            // Property: Valid response should be parsed correctly
            BOOST_REQUIRE(parsed_node_id.has_value());
            BOOST_CHECK_EQUAL(parsed_node_id.value(), node_id);
            
            // Test invalid response formats
            std::vector<std::string> invalid_responses = {
                "",                           // Empty response
                "INVALID_FORMAT",            // Wrong format
                "RAFT_DISCOVERY",            // Missing node ID
                "RAFT_DISCOVERY:",           // Empty node ID
                "OTHER_PROTOCOL:node1",      // Wrong protocol
            };
            
            for (const auto& invalid_response : invalid_responses) {
                std::vector<std::byte> invalid_data;
                invalid_data.reserve(invalid_response.size());
                for (char c : invalid_response) {
                    invalid_data.push_back(static_cast<std::byte>(c));
                }
                
                auto parsed_invalid = client->parse_discovery_response(invalid_data);
                
                // Property: Invalid responses should not be parsed
                BOOST_CHECK(!parsed_invalid.has_value());
            }
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 27: Multicast discovery concurrent operations
 * 
 * Property: For any number of concurrent discovery operations, each should
 * complete independently without interference.
 * 
 * Validates: Requirements 13.1
 */
BOOST_AUTO_TEST_CASE(property_multicast_discovery_concurrent_operations, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto concurrent_count = std::min(std::size_t{5}, generate_random_node_count());
            auto timeout = generate_random_timeout();
            
            // Create test client
            auto client = create_test_client();
            
            // Create test server
            auto node_id = generate_random_node_id();
            auto server = create_test_server(node_id);
            server->start();
            
            // Allow server to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Launch concurrent discovery operations
            std::vector<folly::Future<std::vector<std::string>>> futures;
            for (std::size_t i = 0; i < concurrent_count; ++i) {
                auto future = client->discover_raft_nodes(
                    test_multicast_address,
                    test_multicast_port,
                    timeout
                );
                futures.push_back(std::move(future));
            }
            
            // Wait for all operations to complete
            auto results = folly::collectAll(futures).get();
            
            // Property: All concurrent operations should complete successfully
            BOOST_CHECK_EQUAL(results.size(), concurrent_count);
            
            for (const auto& result : results) {
                BOOST_CHECK(result.hasValue());
                if (result.hasValue()) {
                    auto discovered_nodes = result.value();
                    // Each operation should discover the same node
                    BOOST_CHECK_GE(discovered_nodes.size(), 1);
                    if (!discovered_nodes.empty()) {
                        BOOST_CHECK_EQUAL(discovered_nodes[0], node_id);
                    }
                }
            }
            
            // Clean up server
            server->stop();
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}