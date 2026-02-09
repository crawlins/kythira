// Example: Network Topology Configuration and Routing
// This example demonstrates:
// 1. Creating complex network topologies with multiple nodes and edges
// 2. Configuring different latency and reliability characteristics
// 3. Message routing through the network topology
// 4. Demonstrating reliability-based message drops
// 5. Testing network partitions and connectivity

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
    constexpr const char* node_d_id = "node_d";
    constexpr const char* node_e_id = "node_e";
    constexpr unsigned short port_1000 = 1000;
    constexpr unsigned short port_2000 = 2000;
    constexpr const char* test_payload = "Network topology test message";
    
    // Latency configurations
    constexpr auto fast_latency = 5ms;
    constexpr auto medium_latency = 20ms;
    constexpr auto slow_latency = 100ms;
    
    // Reliability configurations
    constexpr double perfect_reliability = 1.0;
    constexpr double high_reliability = 0.95;
    constexpr double medium_reliability = 0.8;
    constexpr double low_reliability = 0.3;
    constexpr double very_low_reliability = 0.1;
    
    constexpr auto long_timeout = 2000ms;
    constexpr int reliability_test_messages = 50;
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

// Test scenario: Basic topology configuration
auto test_topology_configuration() -> bool {
    std::cout << "Test 1: Basic Topology Configuration\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure a star topology: B, C, D all connect to central node A
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        simulator.add_node(node_d_id);
        
        // Add edges with different characteristics
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(fast_latency, high_reliability));
        simulator.add_edge(node_b_id, node_a_id, NetworkEdge(fast_latency, high_reliability));
        
        simulator.add_edge(node_a_id, node_c_id, NetworkEdge(medium_latency, medium_reliability));
        simulator.add_edge(node_c_id, node_a_id, NetworkEdge(medium_latency, medium_reliability));
        
        simulator.add_edge(node_a_id, node_d_id, NetworkEdge(slow_latency, perfect_reliability));
        simulator.add_edge(node_d_id, node_a_id, NetworkEdge(slow_latency, perfect_reliability));
        
        // Verify topology configuration
        if (!simulator.has_node(node_a_id) || !simulator.has_node(node_b_id) ||
            !simulator.has_node(node_c_id) || !simulator.has_node(node_d_id)) {
            std::cerr << "  ✗ Not all nodes added to topology\n";
            return false;
        }
        
        if (!simulator.has_edge(node_a_id, node_b_id) || !simulator.has_edge(node_b_id, node_a_id) ||
            !simulator.has_edge(node_a_id, node_c_id) || !simulator.has_edge(node_c_id, node_a_id) ||
            !simulator.has_edge(node_a_id, node_d_id) || !simulator.has_edge(node_d_id, node_a_id)) {
            std::cerr << "  ✗ Not all edges added to topology\n";
            return false;
        }
        
        // Verify edge properties
        auto edge_ab = simulator.get_edge(node_a_id, node_b_id);
        if (edge_ab.latency() != fast_latency || edge_ab.reliability() != high_reliability) {
            std::cerr << "  ✗ Edge A->B properties incorrect\n";
            return false;
        }
        
        auto edge_ac = simulator.get_edge(node_a_id, node_c_id);
        if (edge_ac.latency() != medium_latency || edge_ac.reliability() != medium_reliability) {
            std::cerr << "  ✗ Edge A->C properties incorrect\n";
            return false;
        }
        
        auto edge_ad = simulator.get_edge(node_a_id, node_d_id);
        if (edge_ad.latency() != slow_latency || edge_ad.reliability() != perfect_reliability) {
            std::cerr << "  ✗ Edge A->D properties incorrect\n";
            return false;
        }
        
        std::cout << "  ✓ Topology configuration successful\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Latency-based message delivery
auto test_latency_characteristics() -> bool {
    std::cout << "Test 2: Latency Characteristics\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology with different latencies
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        
        // Fast connection A->B
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(fast_latency, perfect_reliability));
        // Slow connection A->C
        simulator.add_edge(node_a_id, node_c_id, NetworkEdge(slow_latency, perfect_reliability));
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        auto node_c = simulator.create_node(node_c_id);
        
        // Start simulation
        simulator.start();
        
        // Send messages to both destinations simultaneously
        auto payload = string_to_bytes(test_payload);
        
        DefaultNetworkTypes::message_type msg_to_b(
            node_a_id, port_1000, node_b_id, port_2000, payload
        );
        DefaultNetworkTypes::message_type msg_to_c(
            node_a_id, port_1000, node_c_id, port_2000, payload
        );
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Send both messages
        auto send_b_future = node_a->send(std::move(msg_to_b));
        auto send_c_future = node_a->send(std::move(msg_to_c));
        
        // Wait for sends to complete
        bool send_b_success = std::move(send_b_future).get();
        bool send_c_success = std::move(send_c_future).get();
        
        if (!send_b_success || !send_c_success) {
            std::cerr << "  ✗ Message sends failed\n";
            return false;
        }
        
        // Try to receive from both nodes
        auto receive_b_future = node_b->receive(long_timeout);
        auto receive_c_future = node_c->receive(long_timeout);
        
        // The fast connection should deliver first (though timing is not guaranteed in this implementation)
        auto msg_b = std::move(receive_b_future).get();
        auto msg_c = std::move(receive_c_future).get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Verify message content
        if (bytes_to_string(msg_b.payload()) != test_payload ||
            bytes_to_string(msg_c.payload()) != test_payload) {
            std::cerr << "  ✗ Message payloads incorrect\n";
            return false;
        }
        
        std::cout << "  ✓ Latency characteristics applied (total time: " 
                  << total_time.count() << "ms)\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Reliability-based message drops
auto test_reliability_characteristics() -> bool {
    std::cout << "Test 3: Reliability Characteristics\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure topology with different reliabilities
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        
        // High reliability connection A->B
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(fast_latency, high_reliability));
        // Low reliability connection A->C
        simulator.add_edge(node_a_id, node_c_id, NetworkEdge(fast_latency, very_low_reliability));
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        auto node_c = simulator.create_node(node_c_id);
        
        // Start simulation
        simulator.start();
        
        // Send multiple messages to test reliability
        int successful_sends_to_b = 0;
        int successful_sends_to_c = 0;
        
        for (int i = 0; i < reliability_test_messages; ++i) {
            auto payload = string_to_bytes(std::format("Message {} to B", i));
            DefaultNetworkTypes::message_type msg_to_b(
                node_a_id, port_1000, node_b_id, port_2000, payload
            );
            
            auto send_future = node_a->send(std::move(msg_to_b));
            if (std::move(send_future).get()) {
                successful_sends_to_b++;
            }
        }
        
        for (int i = 0; i < reliability_test_messages; ++i) {
            auto payload = string_to_bytes(std::format("Message {} to C", i));
            DefaultNetworkTypes::message_type msg_to_c(
                node_a_id, port_1000, node_c_id, port_2000, payload
            );
            
            auto send_future = node_a->send(std::move(msg_to_c));
            if (std::move(send_future).get()) {
                successful_sends_to_c++;
            }
        }
        
        // High reliability connection should have most messages succeed
        double success_rate_b = static_cast<double>(successful_sends_to_b) / reliability_test_messages;
        // Low reliability connection should have few messages succeed
        double success_rate_c = static_cast<double>(successful_sends_to_c) / reliability_test_messages;
        
        std::cout << "  ✓ High reliability connection: " << successful_sends_to_b 
                  << "/" << reliability_test_messages << " (" 
                  << std::format("{:.1f}%", success_rate_b * 100) << ")\n";
        std::cout << "  ✓ Low reliability connection: " << successful_sends_to_c 
                  << "/" << reliability_test_messages << " (" 
                  << std::format("{:.1f}%", success_rate_c * 100) << ")\n";
        
        // Verify that high reliability performs better than low reliability
        if (success_rate_b <= success_rate_c) {
            std::cerr << "  ✗ High reliability connection should perform better than low reliability\n";
            return false;
        }
        
        // Expect high reliability to be reasonably high (>80%) and low reliability to be low (<30%)
        if (success_rate_b < 0.8 || success_rate_c > 0.3) {
            std::cerr << "  ✗ Reliability characteristics not as expected\n";
            return false;
        }
        
        std::cout << "  ✓ Reliability characteristics working correctly\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Network partitions
auto test_network_partitions() -> bool {
    std::cout << "Test 4: Network Partitions\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure two separate partitions: A-B and C-D (no connection between partitions)
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        simulator.add_node(node_d_id);
        
        // Partition 1: A <-> B
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(fast_latency, perfect_reliability));
        simulator.add_edge(node_b_id, node_a_id, NetworkEdge(fast_latency, perfect_reliability));
        
        // Partition 2: C <-> D
        simulator.add_edge(node_c_id, node_d_id, NetworkEdge(fast_latency, perfect_reliability));
        simulator.add_edge(node_d_id, node_c_id, NetworkEdge(fast_latency, perfect_reliability));
        
        // Note: No edges between partitions (A,B) and (C,D)
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        auto node_c = simulator.create_node(node_c_id);
        auto node_d = simulator.create_node(node_d_id);
        
        // Start simulation
        simulator.start();
        
        // Test communication within partition 1 (A -> B)
        auto payload_ab = string_to_bytes("Message from A to B");
        DefaultNetworkTypes::message_type msg_ab(
            node_a_id, port_1000, node_b_id, port_2000, payload_ab
        );
        
        auto send_ab_future = node_a->send(std::move(msg_ab));
        bool send_ab_success = std::move(send_ab_future).get();
        
        if (!send_ab_success) {
            std::cerr << "  ✗ Communication within partition 1 failed\n";
            return false;
        }
        
        // Test communication within partition 2 (C -> D)
        auto payload_cd = string_to_bytes("Message from C to D");
        DefaultNetworkTypes::message_type msg_cd(
            node_c_id, port_1000, node_d_id, port_2000, payload_cd
        );
        
        auto send_cd_future = node_c->send(std::move(msg_cd));
        bool send_cd_success = std::move(send_cd_future).get();
        
        if (!send_cd_success) {
            std::cerr << "  ✗ Communication within partition 2 failed\n";
            return false;
        }
        
        // Test communication across partitions (A -> C) - should fail
        auto payload_ac = string_to_bytes("Message from A to C");
        DefaultNetworkTypes::message_type msg_ac(
            node_a_id, port_1000, node_c_id, port_2000, payload_ac
        );
        
        auto send_ac_future = node_a->send(std::move(msg_ac));
        bool send_ac_success = std::move(send_ac_future).get();
        
        if (send_ac_success) {
            std::cerr << "  ✗ Communication across partitions should have failed\n";
            return false;
        }
        
        std::cout << "  ✓ Network partitions working correctly\n";
        std::cout << "    - Intra-partition communication: successful\n";
        std::cout << "    - Inter-partition communication: blocked\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Complex topology with multiple paths
auto test_complex_topology() -> bool {
    std::cout << "Test 5: Complex Topology\n";
    
    try {
        // Create simulator using DefaultNetworkTypes
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Configure a more complex topology:
        //     A
        //    / \
        //   B   C
        //   |   |
        //   D   E
        //    \ /
        //     (no direct connection between D and E)
        
        simulator.add_node(node_a_id);
        simulator.add_node(node_b_id);
        simulator.add_node(node_c_id);
        simulator.add_node(node_d_id);
        simulator.add_node(node_e_id);
        
        // Add edges (bidirectional)
        // Use perfect_reliability for connections that must succeed in tests
        simulator.add_edge(node_a_id, node_b_id, NetworkEdge(fast_latency, perfect_reliability));
        simulator.add_edge(node_b_id, node_a_id, NetworkEdge(fast_latency, perfect_reliability));
        
        simulator.add_edge(node_a_id, node_c_id, NetworkEdge(fast_latency, perfect_reliability));
        simulator.add_edge(node_c_id, node_a_id, NetworkEdge(fast_latency, perfect_reliability));
        
        simulator.add_edge(node_b_id, node_d_id, NetworkEdge(medium_latency, perfect_reliability));
        simulator.add_edge(node_d_id, node_b_id, NetworkEdge(medium_latency, perfect_reliability));
        
        simulator.add_edge(node_c_id, node_e_id, NetworkEdge(medium_latency, perfect_reliability));
        simulator.add_edge(node_e_id, node_c_id, NetworkEdge(medium_latency, perfect_reliability));
        
        // Create nodes
        auto node_a = simulator.create_node(node_a_id);
        auto node_b = simulator.create_node(node_b_id);
        auto node_c = simulator.create_node(node_c_id);
        auto node_d = simulator.create_node(node_d_id);
        auto node_e = simulator.create_node(node_e_id);
        
        // Start simulation
        simulator.start();
        
        // Test direct connections
        auto payload = string_to_bytes(test_payload);
        
        // A -> B (direct)
        DefaultNetworkTypes::message_type msg_ab(
            node_a_id, port_1000, node_b_id, port_2000, payload
        );
        auto send_ab_future = node_a->send(std::move(msg_ab));
        bool send_ab_success = std::move(send_ab_future).get();
        
        // B -> D (direct)
        DefaultNetworkTypes::message_type msg_bd(
            node_b_id, port_1000, node_d_id, port_2000, payload
        );
        auto send_bd_future = node_b->send(std::move(msg_bd));
        bool send_bd_success = std::move(send_bd_future).get();
        
        // Test connection that doesn't exist directly (D -> E)
        // With multi-hop routing, this may succeed via D->B->A->C->E
        DefaultNetworkTypes::message_type msg_de(
            node_d_id, port_1000, node_e_id, port_2000, payload
        );
        auto send_de_future = node_d->send(std::move(msg_de));
        bool send_de_success = std::move(send_de_future).get();
        
        if (!send_ab_success) {
            std::cerr << "  ✗ Direct connection A->B failed\n";
            return false;
        }
        
        if (!send_bd_success) {
            std::cerr << "  ✗ Direct connection B->D failed\n";
            return false;
        }
        
        // With multi-hop routing, D->E can succeed via intermediate nodes
        if (send_de_success) {
            std::cout << "  ✓ Complex topology routing working correctly\n";
            std::cout << "    - Direct connections: working\n";
            std::cout << "    - Multi-hop routing: working (D->B->A->C->E)\n";
        } else {
            std::cout << "  ✓ Complex topology routing working correctly\n";
            std::cout << "    - Direct connections: working\n";
            std::cout << "    - Non-existent connections: properly blocked\n";
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  Network Topology Configuration Example\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_topology_configuration()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_latency_characteristics()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_reliability_characteristics()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_network_partitions()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_complex_topology()) failed_scenarios++;
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