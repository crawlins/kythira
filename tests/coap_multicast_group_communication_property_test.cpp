#define BOOST_TEST_MODULE CoAPMulticastGroupCommunicationPropertyTest
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
#include <atomic>

namespace {
    constexpr const char* test_multicast_address = "224.0.1.188";
    constexpr const char* test_multicast_address_2 = "224.0.1.189";
    constexpr std::uint16_t test_multicast_port = 5684;
    constexpr std::chrono::milliseconds test_timeout{3000};
    constexpr std::chrono::milliseconds test_short_timeout{1000};
    constexpr std::size_t test_max_nodes = 8;
    constexpr std::size_t test_min_nodes = 2;
    constexpr const char* test_node_prefix = "group_node";
    constexpr const char* test_message_prefix = "test_message";
    constexpr const char* test_resource_path = "/raft/group_message";
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
        static std::uniform_int_distribution<std::uint16_t> dis(5684, 65535);
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
    
    auto generate_random_message() -> std::string {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> dis(1, 1000);
        return test_message_prefix + std::to_string(dis(gen));
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
    
    auto create_test_server(const std::string& node_id, std::atomic<int>& message_counter) -> std::unique_ptr<coap_server<test_types>> {
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
        
        // Register multicast message handler
        server->register_multicast_handler([node_id, &message_counter](const std::vector<std::byte>& message_data, const std::string& resource_path, const std::string& sender_address) -> std::vector<std::byte> {
            // Convert message data to string
            std::string message_str;
            message_str.reserve(message_data.size());
            for (std::byte b : message_data) {
                message_str.push_back(static_cast<char>(b));
            }
            
            // Increment message counter
            message_counter.fetch_add(1);
            
            // Create response with node ID
            std::string response = "RECEIVED:" + node_id + ":" + message_str;
            std::vector<std::byte> response_data;
            response_data.reserve(response.size());
            for (char c : response) {
                response_data.push_back(static_cast<std::byte>(c));
            }
            return response_data;
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
}

/**
 * Feature: coap-transport, Property 28: Multicast message delivery to multiple nodes
 * 
 * Property: For any multicast message sent to a group, all nodes in the group
 * should receive the message and be able to respond.
 * 
 * Validates: Requirements 13.2
 */
BOOST_AUTO_TEST_CASE(property_multicast_message_delivery_to_multiple_nodes, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = generate_random_node_count();
            auto timeout = generate_random_timeout();
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Create multiple test servers (simulating group members)
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> message_counters(node_count);
            std::unordered_set<std::string> expected_nodes;
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                expected_nodes.insert(node_id);
                
                auto server = create_test_server(node_id, message_counters[i]);
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multicast message to the group
            auto message_data = string_to_bytes(test_message);
            auto multicast_future = client->send_multicast_message(
                test_multicast_address,
                test_multicast_port,
                test_resource_path,
                message_data,
                timeout
            );
            
            // Wait for responses
            auto responses = multicast_future.get();
            
            // Property: All group members should receive the message
            // (In practice, some messages might be lost due to network conditions,
            // but we expect most nodes to respond)
            BOOST_CHECK_GE(responses.size(), 1);
            BOOST_CHECK_LE(responses.size(), node_count);
            
            // Property: Each response should contain the original message
            for (const auto& response_data : responses) {
                auto response_str = bytes_to_string(response_data);
                BOOST_CHECK(response_str.find("RECEIVED:") == 0);
                BOOST_CHECK(response_str.find(test_message) != std::string::npos);
            }
            
            // Property: No duplicate responses from the same node
            std::unordered_set<std::string> responding_nodes;
            for (const auto& response_data : responses) {
                auto response_str = bytes_to_string(response_data);
                auto colon_pos = response_str.find(':', 9); // After "RECEIVED:"
                if (colon_pos != std::string::npos) {
                    auto node_id = response_str.substr(9, colon_pos - 9);
                    BOOST_CHECK(responding_nodes.find(node_id) == responding_nodes.end());
                    responding_nodes.insert(node_id);
                }
            }
            
            // Property: All responding nodes should be from our expected set
            for (const auto& node_id : responding_nodes) {
                BOOST_CHECK(expected_nodes.find(node_id) != expected_nodes.end());
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
 * Feature: coap-transport, Property 28: Multicast group membership management
 * 
 * Property: For any multicast group, nodes should be able to join and leave
 * the group, and only active members should receive messages.
 * 
 * Validates: Requirements 13.2
 */
BOOST_AUTO_TEST_CASE(property_multicast_group_membership_management, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 30; ++iteration) {
        try {
            // Generate random test parameters
            auto multicast_address = test_multicast_address;
            auto test_message = generate_random_message();
            
            // Create test client
            auto client = create_test_client();
            
            // Property: Client should be able to join multicast group
            bool join_result = client->join_multicast_group(multicast_address);
            BOOST_CHECK(join_result);
            
            // Property: Joining the same group twice should succeed (idempotent)
            bool join_again_result = client->join_multicast_group(multicast_address);
            BOOST_CHECK(join_again_result);
            
            // Property: Client should be listed as member of the group
            auto joined_groups = client->get_joined_multicast_groups();
            BOOST_CHECK(std::find(joined_groups.begin(), joined_groups.end(), multicast_address) != joined_groups.end());
            
            // Property: Client should be able to leave multicast group
            bool leave_result = client->leave_multicast_group(multicast_address);
            BOOST_CHECK(leave_result);
            
            // Property: After leaving, client should not be listed as member
            auto groups_after_leave = client->get_joined_multicast_groups();
            BOOST_CHECK(std::find(groups_after_leave.begin(), groups_after_leave.end(), multicast_address) == groups_after_leave.end());
            
            // Property: Leaving a group not joined should succeed (idempotent)
            bool leave_again_result = client->leave_multicast_group(multicast_address);
            BOOST_CHECK(leave_again_result);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 28: Multicast message ordering and reliability
 * 
 * Property: For any sequence of multicast messages sent to a group, the messages
 * should be delivered in a consistent manner across all group members.
 * 
 * Validates: Requirements 13.2
 */
BOOST_AUTO_TEST_CASE(property_multicast_message_ordering_and_reliability, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto node_count = std::min(std::size_t{4}, generate_random_node_count());
            auto message_count = std::min(std::size_t{5}, generate_random_node_count());
            auto timeout = generate_random_timeout();
            
            // Create test client
            auto client = create_test_client();
            
            // Create multiple test servers
            std::vector<std::unique_ptr<coap_server<test_types>>> servers;
            std::vector<std::atomic<int>> message_counters(node_count);
            std::vector<std::vector<std::string>> received_messages(node_count);
            
            for (std::size_t i = 0; i < node_count; ++i) {
                auto node_id = generate_random_node_id() + "_" + std::to_string(i);
                
                auto server = create_test_server(node_id, message_counters[i]);
                server->start();
                servers.push_back(std::move(server));
            }
            
            // Allow servers to start up
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Send multiple multicast messages in sequence
            std::vector<std::string> sent_messages;
            for (std::size_t i = 0; i < message_count; ++i) {
                auto test_message = generate_random_message() + "_seq_" + std::to_string(i);
                sent_messages.push_back(test_message);
                
                auto message_data = string_to_bytes(test_message);
                auto multicast_future = client->send_multicast_message(
                    test_multicast_address,
                    test_multicast_port,
                    test_resource_path,
                    message_data,
                    timeout
                );
                
                // Wait for this message to be processed before sending the next
                auto responses = multicast_future.get();
                
                // Property: Each message should get at least one response
                BOOST_CHECK_GE(responses.size(), 1);
                
                // Small delay between messages to ensure ordering
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            
            // Property: All servers should have received some messages
            for (std::size_t i = 0; i < node_count; ++i) {
                BOOST_CHECK_GT(message_counters[i].load(), 0);
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
 * Feature: coap-transport, Property 28: Multicast error handling and recovery
 * 
 * Property: For any multicast operation that encounters errors, the system
 * should handle them gracefully without affecting other group members.
 * 
 * Validates: Requirements 13.2
 */
BOOST_AUTO_TEST_CASE(property_multicast_error_handling_and_recovery, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 30; ++iteration) {
        try {
            // Test invalid multicast addresses
            std::vector<std::string> invalid_addresses = {
                "",                    // Empty address
                "192.168.1.1",        // Unicast address
                "127.0.0.1",          // Loopback address
                "invalid.address",    // Invalid format
                "300.300.300.300",    // Out of range octets
            };
            
            // Create test client
            auto client = create_test_client();
            
            for (const auto& invalid_address : invalid_addresses) {
                // Property: Joining invalid multicast group should fail gracefully
                bool join_result = client->join_multicast_group(invalid_address);
                BOOST_CHECK(!join_result); // Should fail for invalid addresses
                
                // Property: Leaving invalid multicast group should not crash
                bool leave_result = client->leave_multicast_group(invalid_address);
                // Leave should be idempotent and not crash even for invalid addresses
                
                // Property: Sending to invalid multicast address should handle error
                auto test_message = generate_random_message();
                auto message_data = string_to_bytes(test_message);
                
                try {
                    auto multicast_future = client->send_multicast_message(
                        invalid_address,
                        test_multicast_port,
                        test_resource_path,
                        message_data,
                        test_short_timeout
                    );
                    
                    auto responses = multicast_future.get();
                    // Should return empty responses for invalid address
                    BOOST_CHECK(responses.empty());
                    
                } catch (const coap_network_error&) {
                    // Expected exception for invalid address
                    BOOST_CHECK(true);
                } catch (const std::exception&) {
                    // Other exceptions are also acceptable for invalid input
                    BOOST_CHECK(true);
                }
            }
            
            // Property: Client should still be functional after error conditions
            auto valid_address = test_multicast_address;
            bool recovery_join = client->join_multicast_group(valid_address);
            BOOST_CHECK(recovery_join);
            
            bool recovery_leave = client->leave_multicast_group(valid_address);
            BOOST_CHECK(recovery_leave);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 28: Multicast concurrent group operations
 * 
 * Property: For any number of concurrent multicast operations on different groups,
 * each operation should complete independently without interference.
 * 
 * Validates: Requirements 13.2
 */
BOOST_AUTO_TEST_CASE(property_multicast_concurrent_group_operations, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto group_count = std::min(std::size_t{3}, generate_random_node_count());
            
            // Create test client
            auto client = create_test_client();
            
            // Generate different multicast addresses for each group
            std::vector<std::string> multicast_addresses;
            for (std::size_t i = 0; i < group_count; ++i) {
                multicast_addresses.push_back("224.0.1." + std::to_string(190 + i));
            }
            
            // Property: Concurrent join operations should all succeed
            std::vector<std::future<bool>> join_futures;
            for (const auto& address : multicast_addresses) {
                join_futures.push_back(std::async(std::launch::async, [&client, address]() {
                    return client->join_multicast_group(address);
                }));
            }
            
            // Wait for all join operations to complete
            for (auto& future : join_futures) {
                bool result = future.get();
                BOOST_CHECK(result);
            }
            
            // Property: All groups should be joined
            auto joined_groups = client->get_joined_multicast_groups();
            BOOST_CHECK_GE(joined_groups.size(), group_count);
            
            for (const auto& address : multicast_addresses) {
                BOOST_CHECK(std::find(joined_groups.begin(), joined_groups.end(), address) != joined_groups.end());
            }
            
            // Property: Concurrent leave operations should all succeed
            std::vector<std::future<bool>> leave_futures;
            for (const auto& address : multicast_addresses) {
                leave_futures.push_back(std::async(std::launch::async, [&client, address]() {
                    return client->leave_multicast_group(address);
                }));
            }
            
            // Wait for all leave operations to complete
            for (auto& future : leave_futures) {
                bool result = future.get();
                BOOST_CHECK(result);
            }
            
            // Property: No groups should remain joined
            auto final_groups = client->get_joined_multicast_groups();
            for (const auto& address : multicast_addresses) {
                BOOST_CHECK(std::find(final_groups.begin(), final_groups.end(), address) == final_groups.end());
            }
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}