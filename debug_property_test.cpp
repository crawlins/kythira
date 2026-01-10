#include <network_simulator/network_simulator.hpp>
#include <iostream>
#include <chrono>
#include <random>

using namespace network_simulator;

// Helper to generate random address like property test
auto generate_random_address(std::mt19937& rng, std::size_t id) -> std::string {
    return "node_" + std::to_string(id);
}

int main() {
    try {
        std::mt19937 rng(std::random_device{}());
        
        std::size_t failures = 0;
        constexpr std::size_t property_test_iterations = 10;
        
        for (std::size_t i = 0; i < property_test_iterations; ++i) {
            std::cout << "=== Iteration " << i << " ===" << std::endl;
            
            // Generate random addresses like property test
            auto addr1 = generate_random_address(rng, i * 2);
            auto addr2 = generate_random_address(rng, i * 2 + 1);
            
            // Generate random ports like property test
            std::uniform_int_distribution<unsigned short> port_dist(1000, 65535);
            auto src_port = port_dist(rng);
            auto dst_port = port_dist(rng);
            
            // Create simulator with reliable edge - EXACTLY like property test
            NetworkSimulator<DefaultNetworkTypes> sim;
            sim.start();
            
            NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);  // 100% reliability
            sim.add_edge(addr1, addr2, edge);
            sim.add_edge(addr2, addr1, edge);  // Bidirectional
            
            // Create nodes
            auto node1 = sim.create_node(addr1);
            auto node2 = sim.create_node(addr2);
            
            try {
                // Server side: bind to destination port
                auto listener = node2->bind(dst_port).get();
                
                if (!listener || !listener->is_listening()) {
                    ++failures;
                    std::cout << "Iteration " << i << ": Failed to create listener" << std::endl;
                    continue;
                }
                
                // Client side: establish connection from node1 to node2
                auto client_connection = node1->connect(addr2, dst_port, src_port).get();
                
                if (!client_connection) {
                    ++failures;
                    std::cout << "Iteration " << i << ": Failed to create client connection" << std::endl;
                    continue;
                }
                
                // Small delay to allow connection establishment to complete
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                
                // Server side: accept the connection
                auto server_connection = listener->accept(std::chrono::milliseconds{100}).get();
                
                if (!server_connection) {
                    ++failures;
                    std::cout << "Iteration " << i << ": Failed to accept server connection" << std::endl;
                    continue;
                }
                
                std::cout << "Iteration " << i << ": Success!" << std::endl;
                
            } catch (const std::exception& e) {
                ++failures;
                std::cout << "Iteration " << i << ": Exception: " << e.what() << std::endl;
            }
        }
        
        if (failures == 0) {
            std::cout << "All iterations passed!" << std::endl;
            return 0;
        } else {
            std::cout << "Property violated in " << failures << " out of " << property_test_iterations << " iterations" << std::endl;
            return 1;
        }
        
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return 1;
    }
}