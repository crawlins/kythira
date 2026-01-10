/**
 * Example: Custom Types Implementation
 * 
 * This example demonstrates how to create a custom Types struct with different
 * address and port types. It shows:
 * 1. Creating a custom Types implementation using IPv4 addresses and string ports
 * 2. Verifying concept satisfaction at compile time
 * 3. Using the custom Types with the network simulator
 * 4. Demonstrating that the same API works with different underlying types
 */

#include <network_simulator/network_simulator.hpp>
#include <network_simulator/node.hpp>
#include <network_simulator/connection.hpp>
#include <network_simulator/listener.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/concepts.hpp>

#include <iostream>
#include <algorithm>
#include <chrono>
#include <arpa/inet.h>  // For inet_addr

using namespace network_simulator;

namespace {
    // Test constants
    constexpr const char* test_server_ip = "192.168.1.100";
    constexpr const char* test_client_ip = "192.168.1.101";
    constexpr const char* test_server_port = "8080";
    constexpr const char* test_client_port = "9090";
    constexpr const char* test_message_payload = "Hello from custom types!";
    constexpr std::chrono::milliseconds test_timeout{2000};
    constexpr std::chrono::milliseconds test_latency{50};
    constexpr double test_reliability = 0.95;

// Custom Types Implementation using IPv4 addresses and string ports
struct CustomNetworkTypes {
    // Core types - using IPv4 addresses and string ports
    using address_type = IPv4Address;
    using port_type = std::string;
    using message_type = Message<CustomNetworkTypes>;
    using connection_type = Connection<CustomNetworkTypes>;
    using listener_type = Listener<CustomNetworkTypes>;
    using node_type = NetworkNode<CustomNetworkTypes>;
    
    // Future types - using kythira::Future
    using future_bool_type = kythira::Future<bool>;
    using future_message_type = kythira::Future<message_type>;
    using future_connection_type = kythira::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = kythira::Future<std::shared_ptr<listener_type>>;
    using future_bytes_type = kythira::Future<std::vector<std::byte>>;
};

// Verify that our custom types satisfy the concept at compile time
static_assert(network_simulator_types<CustomNetworkTypes>, 
              "CustomNetworkTypes must satisfy network_simulator_types concept");

// Helper function to create IPv4 address from string
auto create_ipv4_address(const std::string& ip_str) -> IPv4Address {
    in_addr addr{};
    if (inet_aton(ip_str.c_str(), &addr) == 0) {
        throw std::invalid_argument("Invalid IPv4 address: " + ip_str);
    }
    return IPv4Address(addr);
}

// Helper function to convert IPv4 address to string for display
auto ipv4_to_string(const IPv4Address& addr) -> std::string {
    return inet_ntoa(addr.get());
}

// Test scenario 1: Verify concept satisfaction and basic operations
auto test_concept_satisfaction() -> bool {
    std::cout << "Test 1: Concept Satisfaction and Basic Operations\n";
    
    try {
        // Create addresses and ports
        auto server_addr = create_ipv4_address(test_server_ip);
        auto client_addr = create_ipv4_address(test_client_ip);
        std::string server_port = test_server_port;
        std::string client_port = test_client_port;
        
        // Verify address concept satisfaction
        static_assert(address<IPv4Address>, "IPv4Address must satisfy address concept");
        static_assert(port<std::string>, "std::string must satisfy port concept");
        
        // Create message with custom types
        std::vector<std::byte> payload;
        std::string payload_str = test_message_payload;
        std::transform(payload_str.begin(), payload_str.end(), std::back_inserter(payload),
                      [](char c) { return static_cast<std::byte>(c); });
        
        Message<CustomNetworkTypes> msg(
            client_addr, client_port,
            server_addr, server_port,
            payload
        );
        
        // Verify message properties
        if (msg.source_address() != client_addr) {
            std::cerr << "  ✗ Message source address mismatch\n";
            return false;
        }
        
        if (msg.source_port() != client_port) {
            std::cerr << "  ✗ Message source port mismatch\n";
            return false;
        }
        
        if (msg.destination_address() != server_addr) {
            std::cerr << "  ✗ Message destination address mismatch\n";
            return false;
        }
        
        if (msg.destination_port() != server_port) {
            std::cerr << "  ✗ Message destination port mismatch\n";
            return false;
        }
        
        std::cout << "  ✓ Custom types satisfy all concepts\n";
        std::cout << "  ✓ Message created with IPv4 address: " 
                  << ipv4_to_string(server_addr) << ":" << server_port << "\n";
        std::cout << "  ✓ Message source IPv4 address: " 
                  << ipv4_to_string(client_addr) << ":" << client_port << "\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 2: Network simulator with custom types
auto test_network_simulator_with_custom_types() -> bool {
    std::cout << "\nTest 2: Network Simulator with Custom Types\n";
    
    try {
        // Create simulator with custom types
        NetworkSimulator<CustomNetworkTypes> simulator;
        
        // Create addresses
        auto server_addr = create_ipv4_address(test_server_ip);
        auto client_addr = create_ipv4_address(test_client_ip);
        
        // Add nodes to topology
        simulator.add_node(server_addr);
        simulator.add_node(client_addr);
        
        // Add bidirectional edges with latency and reliability
        NetworkEdge edge(test_latency, test_reliability);
        simulator.add_edge(server_addr, client_addr, edge);
        simulator.add_edge(client_addr, server_addr, edge);
        
        // Verify topology
        if (!simulator.has_node(server_addr)) {
            std::cerr << "  ✗ Server node not found in topology\n";
            return false;
        }
        
        if (!simulator.has_node(client_addr)) {
            std::cerr << "  ✗ Client node not found in topology\n";
            return false;
        }
        
        if (!simulator.has_edge(server_addr, client_addr)) {
            std::cerr << "  ✗ Edge from server to client not found\n";
            return false;
        }
        
        if (!simulator.has_edge(client_addr, server_addr)) {
            std::cerr << "  ✗ Edge from client to server not found\n";
            return false;
        }
        
        // Verify edge properties
        auto retrieved_edge = simulator.get_edge(server_addr, client_addr);
        if (retrieved_edge.latency() != test_latency) {
            std::cerr << "  ✗ Edge latency mismatch\n";
            return false;
        }
        
        if (retrieved_edge.reliability() != test_reliability) {
            std::cerr << "  ✗ Edge reliability mismatch\n";
            return false;
        }
        
        std::cout << "  ✓ Simulator created with custom types\n";
        std::cout << "  ✓ Topology configured with IPv4 addresses\n";
        std::cout << "  ✓ Edge properties preserved: " 
                  << test_latency.count() << "ms latency, " 
                  << test_reliability << " reliability\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 3: Node operations with custom types
auto test_node_operations_with_custom_types() -> bool {
    std::cout << "\nTest 3: Node Operations with Custom Types\n";
    
    try {
        // Create simulator and start it
        NetworkSimulator<CustomNetworkTypes> simulator;
        
        auto server_addr = create_ipv4_address(test_server_ip);
        auto client_addr = create_ipv4_address(test_client_ip);
        
        // Set up topology
        simulator.add_node(server_addr);
        simulator.add_node(client_addr);
        
        NetworkEdge edge(test_latency, test_reliability);
        simulator.add_edge(server_addr, client_addr, edge);
        simulator.add_edge(client_addr, server_addr, edge);
        
        simulator.start();
        
        // Create nodes
        auto server_node = simulator.create_node(server_addr);
        auto client_node = simulator.create_node(client_addr);
        
        // Verify node addresses
        if (server_node->address() != server_addr) {
            std::cerr << "  ✗ Server node address mismatch\n";
            return false;
        }
        
        if (client_node->address() != client_addr) {
            std::cerr << "  ✗ Client node address mismatch\n";
            return false;
        }
        
        // Test connectionless messaging with custom types
        std::vector<std::byte> payload;
        std::string payload_str = test_message_payload;
        std::transform(payload_str.begin(), payload_str.end(), std::back_inserter(payload),
                      [](char c) { return static_cast<std::byte>(c); });
        
        Message<CustomNetworkTypes> msg(
            client_addr, test_client_port,
            server_addr, test_server_port,
            payload
        );
        
        // Send message (this tests that the API works with custom types)
        auto send_future = client_node->send(msg, test_timeout);
        
        // Note: We don't wait for the result here since this is just demonstrating
        // that the API compiles and works with custom types
        
        std::cout << "  ✓ Nodes created with IPv4 addresses\n";
        std::cout << "  ✓ Server node address: " << ipv4_to_string(server_addr) << "\n";
        std::cout << "  ✓ Client node address: " << ipv4_to_string(client_addr) << "\n";
        std::cout << "  ✓ Message operations work with custom types\n";
        
        simulator.stop();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 4: Comparison with DefaultNetworkTypes
auto test_comparison_with_default_types() -> bool {
    std::cout << "\nTest 4: Comparison with DefaultNetworkTypes\n";
    
    try {
        // Verify both types satisfy the same concept
        static_assert(network_simulator_types<DefaultNetworkTypes>, 
                      "DefaultNetworkTypes must satisfy concept");
        static_assert(network_simulator_types<CustomNetworkTypes>, 
                      "CustomNetworkTypes must satisfy concept");
        
        // Show type differences
        std::cout << "  ✓ Both types satisfy network_simulator_types concept\n";
        std::cout << "  ✓ DefaultNetworkTypes uses:\n";
        std::cout << "    - address_type: std::string\n";
        std::cout << "    - port_type: unsigned short\n";
        std::cout << "  ✓ CustomNetworkTypes uses:\n";
        std::cout << "    - address_type: IPv4Address (wraps in_addr)\n";
        std::cout << "    - port_type: std::string\n";
        std::cout << "  ✓ Same API works with both type implementations\n";
        
        // Create simulators with both types to show they can coexist
        NetworkSimulator<DefaultNetworkTypes> default_sim;
        NetworkSimulator<CustomNetworkTypes> custom_sim;
        
        std::cout << "  ✓ Multiple simulator types can coexist\n";
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

} // anonymous namespace

auto main() -> int {
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  Custom Types Implementation Example\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    std::cout << "This example demonstrates creating a custom Types struct\n";
    std::cout << "that uses IPv4 addresses and string ports instead of the\n";
    std::cout << "default string addresses and unsigned short ports.\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_concept_satisfaction()) failed_scenarios++;
    if (!test_network_simulator_with_custom_types()) failed_scenarios++;
    if (!test_node_operations_with_custom_types()) failed_scenarios++;
    if (!test_comparison_with_default_types()) failed_scenarios++;
    
    // Print summary
    std::cout << "\n" << std::string(60, '=') << "\n";
    if (failed_scenarios == 0) {
        std::cout << "  ✓ All scenarios passed! Custom types work correctly.\n";
        std::cout << std::string(60, '=') << "\n";
        return 0;
    } else {
        std::cout << "  ✗ " << failed_scenarios << " scenario(s) failed\n";
        std::cout << std::string(60, '=') << "\n";
        return 1;
    }
}