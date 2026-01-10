#include "include/network_simulator/simulator.hpp"
#include "include/network_simulator/types.hpp"
#include <iostream>
#include <random>

using namespace network_simulator;

int main() {
    std::mt19937 rng(std::random_device{}());
    
    for (int iteration = 0; iteration < 10; ++iteration) {
        std::cout << "=== Iteration " << iteration << " ===" << std::endl;
        
        // Create simulator
        NetworkSimulator<DefaultNetworkTypes> sim;
        sim.start();
        
        // Generate random addresses and ports (like property test)
        std::string addr1 = "node_" + std::to_string(iteration * 2);
        std::string addr2 = "node_" + std::to_string(iteration * 2 + 1);
        
        std::uniform_int_distribution<unsigned short> port_dist(1000, 65535);
        unsigned short src_port = port_dist(rng);
        unsigned short dst_port = port_dist(rng);
        
        // Add bidirectional edge with 100% reliability
        NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);
        sim.add_edge(addr1, addr2, edge);
        sim.add_edge(addr2, addr1, edge);
        
        // Create nodes
        auto node1 = sim.create_node(addr1);
        auto node2 = sim.create_node(addr2);
        
        std::cout << "Created nodes: " << addr1 << " and " << addr2 << std::endl;
        
        try {
            // Server side: bind to destination port
            std::cout << "Binding to port " << dst_port << std::endl;
            auto listener = node2->bind(dst_port).get();
            
            if (!listener) {
                std::cout << "Failed to create listener - null" << std::endl;
                return 1;
            }
            
            if (!listener->is_listening()) {
                std::cout << "Failed to create listener - not listening" << std::endl;
                return 1;
            }
            
            std::cout << "Listener created successfully" << std::endl;
            
            // Client side: establish connection
            std::cout << "Connecting from " << addr1 << ":" << src_port << " to " << addr2 << ":" << dst_port << std::endl;
            auto client_connection = node1->connect(addr2, dst_port, src_port).get();
            
            if (!client_connection) {
                std::cout << "Failed to create client connection - null" << std::endl;
                return 1;
            }
            
            std::cout << "Client connection created successfully" << std::endl;
            
            // Small delay to allow connection establishment to complete (like in property test)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            
            // Server side: accept the connection with timeout (like in property test)
            std::cout << "Accepting connection with 100ms timeout" << std::endl;
            auto server_connection = listener->accept(std::chrono::milliseconds{100}).get();
            
            if (!server_connection) {
                std::cout << "Failed to accept server connection - null or timeout" << std::endl;
                return 1;
            }
            
            std::cout << "Server connection accepted successfully" << std::endl;
            
        } catch (const std::exception& e) {
            std::cout << "Exception: " << e.what() << std::endl;
            return 1;
        }
    }
    
    std::cout << "All iterations PASSED" << std::endl;
    return 0;
}