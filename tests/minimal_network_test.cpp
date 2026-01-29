#include <network_simulator/network_simulator.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <folly/init/Init.h>

namespace {
    constexpr const char* client_node_id = "client";
    constexpr const char* server_node_id = "server";
    constexpr unsigned short server_port = 8080;
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 1.0;
    constexpr std::chrono::seconds test_timeout{5};
}

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    
    try {
        // Create simulator
        network_simulator::NetworkSimulator<network_simulator::DefaultNetworkTypes> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, 
                          network_simulator::NetworkEdge(network_latency, network_reliability));
        simulator.add_edge(client_node_id, server_node_id, 
                          network_simulator::NetworkEdge(network_latency, network_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        std::cout << "Simulator started" << std::endl;
        
        // Server: Bind to port
        auto bind_future = server_node->bind(server_port);
        auto listener = std::move(bind_future).get();
        
        if (!listener || !listener->is_listening()) {
            std::cerr << "Failed to bind server to port " << server_port << std::endl;
            return 1;
        }
        std::cout << "Server bound to port " << server_port << std::endl;
        
        // Start server thread to accept and handle connection
        std::atomic<bool> server_success{false};
        std::thread server_thread([&]() {
            try {
                std::cout << "Server: Waiting for connection..." << std::endl;
                auto accept_future = listener->accept(test_timeout);
                auto server_connection = std::move(accept_future).get();
                
                if (!server_connection || !server_connection->is_open()) {
                    std::cerr << "Server: Failed to accept connection" << std::endl;
                    return;
                }
                std::cout << "Server: Connection accepted" << std::endl;
                
                // Receive request
                auto read_future = server_connection->read(test_timeout);
                auto request_data = std::move(read_future).get();
                
                std::string request_str;
                for (auto byte : request_data) {
                    request_str += static_cast<char>(byte);
                }
                std::cout << "Server: Received request: " << request_str << std::endl;
                
                // Send response
                std::string response = "OK";
                std::vector<std::byte> response_data;
                for (char c : response) {
                    response_data.push_back(static_cast<std::byte>(c));
                }
                
                auto write_future = server_connection->write(response_data);
                bool write_success = std::move(write_future).get();
                
                if (write_success) {
                    std::cout << "Server: Response sent successfully" << std::endl;
                    server_success = true;
                } else {
                    std::cerr << "Server: Failed to send response" << std::endl;
                }
                
            } catch (const std::exception& ex) {
                std::cerr << "Server: Exception: " << ex.what() << std::endl;
            }
        });
        
        // Give server time to start listening
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Client: Connect to server
        std::cout << "Client: Connecting to server..." << std::endl;
        auto connect_future = client_node->connect(server_node_id, server_port);
        auto client_connection = std::move(connect_future).get();
        
        if (!client_connection || !client_connection->is_open()) {
            std::cerr << "Client: Failed to connect to server" << std::endl;
            server_thread.join();
            return 1;
        }
        std::cout << "Client: Connected to server" << std::endl;
        
        // Client: Send request
        std::string request = "Hello";
        std::vector<std::byte> request_data;
        for (char c : request) {
            request_data.push_back(static_cast<std::byte>(c));
        }
        
        auto write_future = client_connection->write(request_data);
        bool write_success = std::move(write_future).get();
        
        if (!write_success) {
            std::cerr << "Client: Failed to send request" << std::endl;
            server_thread.join();
            return 1;
        }
        std::cout << "Client: Request sent" << std::endl;
        
        // Client: Receive response
        auto read_future = client_connection->read(test_timeout);
        auto response_data = std::move(read_future).get();
        
        std::string response_str;
        for (auto byte : response_data) {
            response_str += static_cast<char>(byte);
        }
        std::cout << "Client: Received response: " << response_str << std::endl;
        
        // Wait for server thread
        server_thread.join();
        
        // Stop simulator
        simulator.stop();
        std::cout << "Simulator stopped" << std::endl;
        
        // Check overall success
        bool overall_success = server_success && (response_str == "OK");
        
        std::cout << "\nOverall result: " << (overall_success ? "SUCCESS" : "FAILED") << std::endl;
        
        return overall_success ? 0 : 1;
        
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return 1;
    }
}
