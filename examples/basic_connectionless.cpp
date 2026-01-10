// Example: Basic Connectionless Communication
// This example demonstrates:
// 1. Creating a network simulator with DefaultNetworkTypes
// 2. Sending and receiving connectionless messages
// 3. Timeout handling for send and receive operations
// 4. Error handling and graceful failure reporting

#include <network_simulator/network_simulator.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <vector>

using namespace network_simulator;
using namespace std::chrono_literals;

namespace {
    // Named constants for test configuration
    constexpr const char* node_a_id = "node_a";
    constexpr const char* node_b_id = "node_b";
    constexpr const char* node_c_id = "node_c";
    constexpr unsigned short port_1000 = 1000;
    constexpr unsigned short port_2000 = 2000;
    constexpr const char* test_payload = "Hello, Network Simulator!";
    constexpr auto default_latency = 10ms;
    constexpr double high_reliability = 0.99;
    constexpr double low_reliability = 0.1;
    constexpr auto short_timeout = 50ms;
    constexpr auto long_timeout = 1000ms;
}

// Helper function to convert string to bytes
auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(str.size());
    for (char c : str) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

// Helper function to convert bytes to string
auto bytes_to_string(const std::vector<std::byte>& bytes) -> std::string {
    std::string str;
    str.reserve(bytes.size());
    for (std::byte b : bytes) {
        str.push_back(static_cast<char>(b));
    }
    return str;
}

// Test scenario: Basic send/receive functionality
auto test_basic_send_receive() -> bool {
    std::cout << "Test 1: Basic Send/Receive\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        
        // Start simulation
        simulator.start();
        
        // Create and send message using DefaultNetworkTypes::message_type
        auto payload = string_to_bytes(test_payload);
        DefaultNetworkTypes::message_type msg(
            node_a_id, port_1000,
            node_b_id, port_2000,
            payload
        );
        
        // Send message
        auto send_future = node_a->send(std::move(msg));
        bool send_result = std::move(send_future).get();
        
        if (!send_result) {
            std::cerr << "  ✗ Send operation failed\n";
            return false;
        }
        
        // Receive message
        auto receive_future = node_b->receive(long_timeout);
        auto received_msg = std::move(receive_future).get();
        
        // Verify message content
        if (received_msg.source_address() != node_a_id ||
            received_msg.source_port() != port_1000 ||
            received_msg.destination_address() != node_b_id ||
            received_msg.destination_port() != port_2000) {
            std::cerr << "  ✗ Message addressing incorrect\n";
            return false;
        }
        
        auto received_payload = bytes_to_string(received_msg.payload());
        if (received_payload != test_payload) {
            std::cerr << "  ✗ Message payload incorrect. Expected: '" 
                      << test_payload << "', Got: '" << received_payload << "'\n";
            return false;
        }
        
        std::cout << "  ✓ Basic send/receive successful\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Send timeout handling
auto test_send_timeout() -> bool {
    std::cout << "Test 2: Send Timeout Handling\n";
    
    try {
        // Create simulator with no connectivity (no edges)
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology with isolated nodes
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        // Note: No edge between nodes - this should cause routing failure
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        
        // Start simulation
        simulator.start();
        
        // Create message
        auto payload = string_to_bytes(test_payload);
        DefaultNetworkTypes::message_type msg(
            node_a_id, port_1000,
            node_b_id, port_2000,
            payload
        );
        
        // Send message with short timeout - should fail due to no route
        auto send_future = node_a->send(std::move(msg), short_timeout);
        bool send_result = std::move(send_future).get();
        
        if (send_result) {
            std::cerr << "  ✗ Send should have failed due to no route\n";
            return false;
        }
        
        std::cout << "  ✓ Send timeout handled correctly\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Receive timeout handling
auto test_receive_timeout() -> bool {
    std::cout << "Test 3: Receive Timeout Handling\n";
    
    try {
        // Create simulator
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology
        simulator.add_node(node_a_id);
        
        // Create node
        auto node_a = simulator.create_node(node_a_id);
        
        // Start simulation
        simulator.start();
        
        // Try to receive with timeout - should timeout since no messages sent
        try {
            auto receive_future = node_a->receive(short_timeout);
            auto received_msg = std::move(receive_future).get();
            
            std::cerr << "  ✗ Receive should have timed out\n";
            return false;
            
        } catch (const TimeoutException&) {
            std::cout << "  ✓ Receive timeout handled correctly\n";
            return true;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Reliability-based message drops
auto test_reliability_drops() -> bool {
    std::cout << "Test 4: Reliability-Based Message Drops\n";
    
    try {
        // Create simulator
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology with very low reliability
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(default_latency, low_reliability));
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        
        // Start simulation
        simulator.start();
        
        // Send multiple messages - some should be dropped due to low reliability
        constexpr int message_count = 20;
        int successful_sends = 0;
        
        for (int i = 0; i < message_count; ++i) {
            auto payload = string_to_bytes(std::format("Message {}", i));
            DefaultNetworkTypes::message_type msg(
                node_a_id, port_1000,
                node_b_id, port_2000,
                payload
            );
            
            auto send_future = node_a->send(std::move(msg));
            bool send_result = std::move(send_future).get();
            
            if (send_result) {
                successful_sends++;
            }
        }
        
        // With 10% reliability, we expect roughly 2 successful sends out of 20
        // Allow some variance due to randomness (0-8 is reasonable range)
        if (successful_sends >= 0 && successful_sends <= 8) {
            std::cout << "  ✓ Reliability simulation working (" 
                      << successful_sends << "/" << message_count << " messages sent)\n";
            return true;
        } else {
            std::cerr << "  ✗ Unexpected reliability behavior (" 
                      << successful_sends << "/" << message_count << " messages sent)\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Multi-hop routing (if supported)
auto test_multi_hop_routing() -> bool {
    std::cout << "Test 5: Multi-Hop Routing\n";
    
    try {
        // Create simulator
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure linear topology: A -> B -> C
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(node_b_id, node_c_id, NetworkEdge(default_latency, high_reliability));
        // Note: No direct edge from A to C
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_c = simulator.create_node(node_c_id);
        
        // Start simulation
        simulator.start();
        
        // Try to send from A to C - should fail since current implementation
        // only supports direct routing
        auto payload = string_to_bytes(test_payload);
        DefaultNetworkTypes::message_type msg(
            node_a_id, port_1000,
            node_c_id, port_2000,
            payload
        );
        
        auto send_future = node_a->send(std::move(msg));
        bool send_result = std::move(send_future).get();
        
        if (!send_result) {
            std::cout << "  ✓ Multi-hop routing correctly not supported (direct routing only)\n";
            return true;
        } else {
            std::cerr << "  ✗ Unexpected success - multi-hop routing not expected\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  Basic Connectionless Communication Example\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_basic_send_receive()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_send_timeout()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_receive_timeout()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_reliability_drops()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_multi_hop_routing()) failed_scenarios++;
    std::cout << "\n";
    
    // Report final results
    std::cout << std::string(60, '=') << "\n";
    if (failed_scenarios == 0) {
        std::cout << "All scenarios passed! ✓\n";
        std::cout << "Exit code: 0\n";
        return 0;
    } else {
        std::cout << failed_scenarios << " scenario(s) failed ✗\n";
        std::cout << "Exit code: 1\n";
        return 1;
    }
}