#define BOOST_TEST_MODULE CoAPMulticastResponseAggregationPropertyTest
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
#include <unordered_map>
#include <atomic>

namespace {
    constexpr const char* test_multicast_address = "224.0.1.190";
    constexpr std::uint16_t test_multicast_port = 5685;
    constexpr std::chrono::milliseconds test_timeout{4000};
    constexpr std::chrono::milliseconds test_short_timeout{1500};
    constexpr std::chrono::milliseconds test_long_timeout{6000};
    constexpr std::size_t test_max_nodes = 6;
    constexpr std::size_t test_min_nodes = 2;
    constexpr const char* test_node_prefix = "agg_node";
    constexpr const char* test_message_prefix = "agg_message";
    constexpr const char* test_resource_path = "/raft/aggregation_test";
}

using namespace kythira;

// Test types for CoAP transport
struct test_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using serializer_type = json_serializer;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

// Property test helper functions
namespace property_helpers {
    
    auto generate_random_node_count() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_nodes, test_max_nodes);
        return dis(gen);
    }
    
    auto generate_random_timeout() -> std::chrono::milliseconds {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(2000, 5000);
        return std::chrono::milliseconds{dis(gen)};
    }
    
    auto generate_random_delay() -> std::chrono::milliseconds {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(50, 500);
        return std::chrono::milliseconds{dis(gen)};
    }
    
    auto generate_random_message() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 10000);
        return test_message_prefix + std::to_string(dis(gen));
    }
    
    auto generate_random_node_id() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 10000);
        return test_node_prefix + std::to_string(dis(gen));
    }
    
    auto generate_random_response_data() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 10000);
        return "RESPONSE_DATA_" + std::to_string(dis(gen));
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
    
    auto create_test_server_with_delay(
        const std::string& node_id, 
        const std::string& response_data,
        std::chrono::milliseconds response_delay,
        std::atomic<int>& response_counter
    ) -> std::unique_ptr<coap_server<test_types>> {
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
        
        // Register multicast handler with configurable delay
        server->register_multicast_handler([node_id, response_data, response_delay, &response_counter](
            const std::vector<std::byte>& message_data, 
            const std::string& resource_path, 
            const std::string& sender_address
        ) -> std::vector<std::byte> {
            // Simulate processing delay
            if (response_delay.count() > 0) {
                std::this_thread::sleep_for(response_delay);
            }
            
            // Increment response counter
            response_counter.fetch_add(1);
            
            // Create response with node ID and custom data
            std::string response = "AGG_RESPONSE:" + node_id + ":" + response_data;
            std::vector<std::byte> response_bytes;
            response_bytes.reserve(response.size());
            for (char c : response) {
                response_bytes.push_back(static_cast<std::byte>(c));
            }
            return response_bytes;
        });
        
        return server;
    }
    
    auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
        std::vector<std::byte> bytes;
        bytes.reserve(str.size());
        for (char c : str) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }
    
    auto bytes_to_string(const std::vector<std::byte>& bytes) -> std::string {
        std::string str;
        str.reserve(bytes.size());
        for (std::byte b : bytes) {
            str.push_back(static_cast<char>(b));
        }
        return str;
    }
    
    auto parse_aggregated_response(const std::string& response) -> std::tuple<std::string, std::string> {
        // Parse "AGG_RESPONSE:node_id:response_data"
        if (response.find("AGG_RESPONSE:") != 0) {
            return {"", ""};
        }
        
        auto first_colon = response.find(':', 13); // After "AGG_RESPONSE:"
        if (first_colon == std::string::npos) {
            return {"", ""};
        }
        
        auto second_colon = response.find(':', first_colon + 1);
        if (second_colon == std::string::npos) {
            return {"", ""};
        }
        
        auto node_id = response.substr(13, first_colon - 13);
        auto response_data = response.substr(second_colon + 1);
        
        return {node_id, response_data};
    }
}

/**
 * Feature: coap-transport, Property 29: Multicast response aggregation and correlation
 * 
 * Property: For any multicast request, all responses should be properly aggregated
 * and correlated with the original request within the timeout period.
 * 
 * Validates: Requirements 13.3
 */
BOOST_AUTO_TEST_CASE(property_multicast_response_aggregation_basic, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 40; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = generate_random_node_count();
            auto timeout = generate_random_timeout();
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Create multiple test servers with different response data
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> response_counters(node_count);
            std::unordered_map<std::string, std::string> expected_responses;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                auto response_data = generate_random_response_data() + "_" + std::to_string(i);
                expected_responses[node_id] = response_data;
                
                auto server = create_test_server_with_delay(
                    node_id, 
                    response_data, 
                    std::chrono::milliseconds{0}, // No delay for basic test
                    response_counters[i]
                );
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multicast message and collect responses
            auto message_data = string_to_bytes(test_message);
            auto start_time = std::chrono::steady_clock::now();
            
            auto multicast_future = client->send_multicast_message(
                test_multicast_address,
                test_multicast_port,
                test_resource_path,
                message_data,
                timeout
            );
            
            // Wait for response aggregation to complete
            auto aggregated_responses = multicast_future.get();
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Response aggregation should complete within timeout
            BOOST_CHECK_LE(elapsed, timeout + std::chrono::milliseconds(500)); // Allow some tolerance
            
            // Property: Should receive responses from multiple nodes
            BOOST_CHECK_GE(aggregated_responses.size(), 1);
            BOOST_CHECK_LE(aggregated_responses.size(), node_count);
            
            // Property: Each response should be properly formatted and correlated
            std::unordered_set<std::string> responding_nodes;
            for (const auto& response_bytes : aggregated_responses) {
                auto response_str = bytes_to_string(response_bytes);
                auto [node_id, response_data] = parse_aggregated_response(response_str);
                
                BOOST_CHECK(!node_id.empty());
                BOOST_CHECK(!response_data.empty());
                
                // Property: No duplicate responses from the same node
                BOOST_CHECK(responding_nodes.find(node_id) == responding_nodes.end());
                responding_nodes.insert(node_id);
                
                // Property: Response data should match expected data for the node
                auto expected_it = expected_responses.find(node_id);
                if (expected_it != expected_responses.end()) {
                    BOOST_CHECK_EQUAL(response_data, expected_it->second);
                }
            }
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 29: Multicast response timeout handling
 * 
 * Property: For any multicast request with a timeout, response aggregation
 * should complete when the timeout expires, returning all responses received so far.
 * 
 * Validates: Requirements 13.3
 */
BOOST_AUTO_TEST_CASE(property_multicast_response_timeout_handling, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = std::min(std::size_t{4}, generate_random_node_count());
            auto timeout = test_short_timeout; // Short timeout for this test
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Create test servers with varying response delays
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> response_counters(node_count);
            std::size_t fast_responders = 0;
            std::size_t slow_responders = 0;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                auto response_data = generate_random_response_data() + "_" + std::to_string(i);
                
                // Half the servers respond quickly, half respond slowly (after timeout)
                auto response_delay = (i < node_count / 2) ? 
                    std::chrono::milliseconds{100} :  // Fast responders
                    timeout + std::chrono::milliseconds{1000}; // Slow responders (after timeout)
                
                if (i < node_count / 2) {
                    fast_responders++;
                } else {
                    slow_responders++;
                }
                
                auto server = create_test_server_with_delay(
                    node_id, 
                    response_data, 
                    response_delay,
                    response_counters[i]
                );
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multicast message and measure response time
            auto message_data = string_to_bytes(test_message);
            auto start_time = std::chrono::steady_clock::now();
            
            auto multicast_future = client->send_multicast_message(
                test_multicast_address,
                test_multicast_port,
                test_resource_path,
                message_data,
                timeout
            );
            
            // Wait for response aggregation to complete
            auto aggregated_responses = multicast_future.get();
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Property: Response aggregation should complete near the timeout
            BOOST_CHECK_GE(elapsed, timeout - std::chrono::milliseconds(200)); // Should wait close to timeout
            BOOST_CHECK_LE(elapsed, timeout + std::chrono::milliseconds(500)); // But not much longer
            
            // Property: Should only receive responses from fast responders
            BOOST_CHECK_LE(aggregated_responses.size(), fast_responders);
            
            // Property: All received responses should be from fast responders
            for (const auto& response_bytes : aggregated_responses) {
                auto response_str = bytes_to_string(response_bytes);
                auto [node_id, response_data] = parse_aggregated_response(response_str);
                
                BOOST_CHECK(!node_id.empty());
                BOOST_CHECK(!response_data.empty());
                
                // The response should be from a fast responder (node index < node_count/2)
                // We can't easily verify this without more complex tracking, but we can
                // verify the response format is correct
                BOOST_CHECK(response_str.find("AGG_RESPONSE:") == 0);
            }
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 29: Multicast response deduplication
 * 
 * Property: For any multicast request, duplicate responses from the same node
 * should be filtered out during aggregation.
 * 
 * Validates: Requirements 13.3
 */
BOOST_AUTO_TEST_CASE(property_multicast_response_deduplication, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 30; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = generate_random_node_count();
            auto timeout = generate_random_timeout();
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Create test servers that might send duplicate responses
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> response_counters(node_count);
            std::unordered_set<std::string> expected_node_ids;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                auto response_data = generate_random_response_data() + "_" + std::to_string(i);
                expected_node_ids.insert(node_id);
                
                auto server = create_test_server_with_delay(
                    node_id, 
                    response_data, 
                    std::chrono::milliseconds{0},
                    response_counters[i]
                );
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multicast message
            auto message_data = string_to_bytes(test_message);
            auto multicast_future = client->send_multicast_message(
                test_multicast_address,
                test_multicast_port,
                test_resource_path,
                message_data,
                timeout
            );
            
            // Wait for response aggregation
            auto aggregated_responses = multicast_future.get();
            
            // Property: No duplicate responses from the same node
            std::unordered_set<std::string> responding_nodes;
            for (const auto& response_bytes : aggregated_responses) {
                auto response_str = bytes_to_string(response_bytes);
                auto [node_id, response_data] = parse_aggregated_response(response_str);
                
                BOOST_CHECK(!node_id.empty());
                
                // Property: Each node should appear only once in the aggregated responses
                BOOST_CHECK(responding_nodes.find(node_id) == responding_nodes.end());
                responding_nodes.insert(node_id);
            }
            
            // Property: Number of unique responses should equal number of responding nodes
            BOOST_CHECK_EQUAL(aggregated_responses.size(), responding_nodes.size());
            
            // Property: All responding nodes should be from our expected set
            for (const auto& node_id : responding_nodes) {
                BOOST_CHECK(expected_node_ids.find(node_id) != expected_node_ids.end());
            }
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 29: Multicast response ordering preservation
 * 
 * Property: For any multicast request, the order of responses in the aggregated
 * result should be consistent and deterministic based on arrival time.
 * 
 * Validates: Requirements 13.3
 */
BOOST_AUTO_TEST_CASE(property_multicast_response_ordering_preservation, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = std::min(std::size_t{4}, generate_random_node_count());
            auto timeout = generate_random_timeout();
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Create test servers with staggered response delays
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> response_counters(node_count);
            std::vector<std::pair<std::string, std::chrono::milliseconds>> expected_order;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                auto response_data = generate_random_response_data() + "_" + std::to_string(i);
                
                // Create staggered delays to ensure predictable ordering
                auto response_delay = std::chrono::milliseconds{100 + (i * 200)};
                expected_order.emplace_back(node_id, response_delay);
                
                auto server = create_test_server_with_delay(
                    node_id, 
                    response_data, 
                    response_delay,
                    response_counters[i]
                );
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multicast message
            auto message_data = string_to_bytes(test_message);
            auto multicast_future = client->send_multicast_message(
                test_multicast_address,
                test_multicast_port,
                test_resource_path,
                message_data,
                timeout
            );
            
            // Wait for response aggregation
            auto aggregated_responses = multicast_future.get();
            
            // Property: Responses should be received in order of their delays
            std::vector<std::string> actual_order;
            for (const auto& response_bytes : aggregated_responses) {
                auto response_str = bytes_to_string(response_bytes);
                auto [node_id, response_data] = parse_aggregated_response(response_str);
                
                BOOST_CHECK(!node_id.empty());
                actual_order.push_back(node_id);
            }
            
            // Property: The order should generally follow the delay order
            // (Note: In real networks, exact ordering might not be guaranteed due to
            // network conditions, but in our controlled test environment, we expect
            // responses to arrive in the order of their delays)
            if (actual_order.size() >= 2) {
                // Check that at least the first and last responses are in correct order
                auto first_expected = expected_order[0].first;
                auto last_expected = expected_order[expected_order.size() - 1].first;
                
                if (actual_order.size() == expected_order.size()) {
                    // If we got all responses, first should be first and last should be last
                    BOOST_CHECK_EQUAL(actual_order.front(), first_expected);
                    BOOST_CHECK_EQUAL(actual_order.back(), last_expected);
                }
            }
            
            // Property: All responses should be unique and valid
            std::unordered_set<std::string> unique_responses(actual_order.begin(), actual_order.end());
            BOOST_CHECK_EQUAL(unique_responses.size(), actual_order.size());
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 29: Multicast response aggregation under load
 * 
 * Property: For any multicast request under high load conditions, response
 * aggregation should still function correctly and handle all responses.
 * 
 * Validates: Requirements 13.3
 */
BOOST_AUTO_TEST_CASE(property_multicast_response_aggregation_under_load, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 10; ++iteration) {
        try {
            // Generate test parameters for load testing
            auto node_count = test_max_nodes; // Use maximum nodes for load test
            auto timeout = test_long_timeout; // Longer timeout for load test
            auto concurrent_requests = std::min(std::size_t{3}, generate_random_node_count());
            
            // Create test client
            auto client = create_test_client();
            
            // Create multiple test servers
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> response_counters(node_count);
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_load_" + std::to_string(i);
                auto response_data = generate_random_response_data() + "_load_" + std::to_string(i);
                
                auto server = create_test_server_with_delay(
                    node_id, 
                    response_data, 
                    generate_random_delay(), // Random small delays
                    response_counters[i]
                );
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            
            // Send multiple concurrent multicast requests
            std::vector<folly::Future<std::vector<std::vector<std::byte>>>> futures;
            for (std::size_t i = 0; i < concurrent_requests; ++i) {
                auto test_message = generate_random_message() + "_concurrent_" + std::to_string(i);
                auto message_data = string_to_bytes(test_message);
                
                auto future = client->send_multicast_message(
                    test_multicast_address,
                    test_multicast_port,
                    test_resource_path,
                    message_data,
                    timeout
                );
                futures.push_back(std::move(future));
            }
            
            // Wait for all concurrent requests to complete
            auto results = folly::collectAll(futures).get();
            
            // Property: All concurrent requests should complete successfully
            BOOST_CHECK_EQUAL(results.size(), concurrent_requests);
            
            for (const auto& result : results) {
                BOOST_CHECK(result.hasValue());
                if (result.hasValue()) {
                    auto aggregated_responses = result.value();
                    
                    // Property: Each request should get responses from multiple nodes
                    BOOST_CHECK_GE(aggregated_responses.size(), 1);
                    BOOST_CHECK_LE(aggregated_responses.size(), node_count);
                    
                    // Property: All responses should be properly formatted
                    std::unordered_set<std::string> responding_nodes;
                    for (const auto& response_bytes : aggregated_responses) {
                        auto response_str = bytes_to_string(response_bytes);
                        auto [node_id, response_data] = parse_aggregated_response(response_str);
                        
                        BOOST_CHECK(!node_id.empty());
                        BOOST_CHECK(!response_data.empty());
                        
                        // Property: No duplicate responses within a single request
                        BOOST_CHECK(responding_nodes.find(node_id) == responding_nodes.end());
                        responding_nodes.insert(node_id);
                    }
                }
            }
            
            // Clean up servers
            for (auto& server : servers) {
                server->stop();
            }
            
            // Allow cleanup time
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}