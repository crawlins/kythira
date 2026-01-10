#include <network_simulator/network_simulator.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <folly/init/Init.h>

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    
    // Create simulator
    network_simulator::NetworkSimulator<network_simulator::DefaultNetworkTypes> simulator;
    
    // Add nodes
    simulator.add_node(1);
    simulator.add_node(2);
    
    // Add edge
    network_simulator::NetworkEdge edge{std::chrono::milliseconds{10}, 1.0};
    simulator.add_edge(1, 2, edge);
    simulator.add_edge(2, 1, edge);
    
    // Create nodes
    auto node1 = simulator.create_node(1);
    auto node2 = simulator.create_node(2);
    
    // Start simulator
    simulator.start();
    
    // Start server thread that listens for messages and responds
    std::atomic<bool> server_running{true};
    std::thread server_thread([&]() {
        std::cout << "Server: Starting..." << std::endl;
        while (server_running) {
            try {
                auto recv_future = node2->receive(std::chrono::milliseconds{100});
                auto request = std::move(recv_future).get();
                
                std::cout << "Server: Received request from " << request.source_address() << std::endl;
                
                // Send response back
                std::vector<std::byte> response_payload{std::byte{0x4F}, std::byte{0x4B}};  // "OK"
                network_simulator::Message<std::uint64_t, unsigned short> response(
                    2, 5000,  // From node 2, port 5000
                    request.source_address(), 0,  // To original sender
                    response_payload
                );
                
                auto send_future = node2->send(std::move(response));
                bool sent = std::move(send_future).get();
                std::cout << "Server: Response sent: " << (sent ? "SUCCESS" : "FAILED") << std::endl;
                
            } catch (...) {
                // Timeout - continue
            }
        }
        std::cout << "Server: Stopped" << std::endl;
    });
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    // Client sends request
    std::cout << "Client: Sending request..." << std::endl;
    std::vector<std::byte> payload{std::byte{0x48}, std::byte{0x69}};  // "Hi"
    network_simulator::Message<std::uint64_t, unsigned short> msg(
        1, 0,  // From node 1, port 0
        2, 5000,  // To node 2, port 5000
        payload
    );
    
    auto send_future = node1->send(std::move(msg), std::chrono::milliseconds{1000});
    bool send_result = std::move(send_future).get();
    
    std::cout << "Client: Send result: " << (send_result ? "SUCCESS" : "FAILED") << std::endl;
    
    bool overall_success = false;
    
    if (send_result) {
        // Wait for response
        std::cout << "Client: Waiting for response..." << std::endl;
        try {
            auto response_future = node1->receive(std::chrono::milliseconds{1000});
            auto response = std::move(response_future).get();
            std::cout << "Client: Received response from " << response.source_address() << std::endl;
            overall_success = true;
        } catch (const std::exception& ex) {
            std::cout << "Client: Failed to receive response: " << ex.what() << std::endl;
        }
    }
    
    // Stop server
    server_running = false;
    server_thread.join();
    
    simulator.stop();
    
    std::cout << "\nOverall result: " << (overall_success ? "SUCCESS" : "FAILED") << std::endl;
    
    return overall_success ? 0 : 1;
}
