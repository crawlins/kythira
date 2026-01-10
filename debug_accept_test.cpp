#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <future>

using namespace network_simulator;

int main() {
    std::cout << "=== Debug Accept Test ===" << std::endl;
    
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create topology
    NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);
    sim.add_node("client");
    sim.add_node("server");
    sim.add_edge("client", "server", edge);
    sim.add_edge("server", "client", edge);
    
    // Create nodes
    auto client = sim.create_node("client");
    auto server = sim.create_node("server");
    
    std::cout << "Created nodes" << std::endl;
    
    // Start simulation
    sim.start();
    std::cout << "Started simulation" << std::endl;
    
    // Server: bind to port
    std::cout << "Server binding to port 8080..." << std::endl;
    auto listener_future = server->bind(8080);
    auto listener = std::move(listener_future).get();
    
    if (!listener) {
        std::cout << "ERROR: Failed to create listener" << std::endl;
        return 1;
    }
    
    std::cout << "Listener created, is_listening: " << listener->is_listening() << std::endl;
    std::cout << "Listener endpoint: " << listener->local_endpoint().address 
              << ":" << listener->local_endpoint().port << std::endl;
    
    // Start connection establishment thread FIRST
    std::cout << "Starting connection establishment thread..." << std::endl;
    
    std::shared_ptr<DefaultNetworkTypes::connection_type> client_connection;
    std::exception_ptr connect_exception;
    
    std::thread connect_thread([&client, &client_connection, &connect_exception]() {
        try {
            // Add a small delay to ensure accept starts first
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            std::cout << "[THREAD] Starting client connection..." << std::endl;
            auto client_connection_future = client->connect("server", 8080, 9090);
            client_connection = std::move(client_connection_future).get();
            std::cout << "[THREAD] Client connection completed" << std::endl;
        } catch (...) {
            connect_exception = std::current_exception();
        }
    });
    
    // Start accept operation immediately after starting the thread
    std::cout << "Starting accept operation with 5 second timeout..." << std::endl;
    auto accept_future = listener->accept(std::chrono::milliseconds{5000});
    
    try {
        // Wait for client connection to complete
        connect_thread.join();
        
        if (connect_exception) {
            std::rethrow_exception(connect_exception);
        }
        
        if (!client_connection) {
            std::cout << "ERROR: Client connection is null" << std::endl;
            return 1;
        }
        
        std::cout << "Client connection established" << std::endl;
        std::cout << "Client connection is_open: " << client_connection->is_open() << std::endl;
        std::cout << "Client local: " << client_connection->local_endpoint().address 
                  << ":" << client_connection->local_endpoint().port << std::endl;
        std::cout << "Client remote: " << client_connection->remote_endpoint().address 
                  << ":" << client_connection->remote_endpoint().port << std::endl;
        
        // Now try to get the server connection from accept
        std::cout << "Waiting for accept to complete..." << std::endl;
        auto server_connection = std::move(accept_future).get();
        
        if (!server_connection) {
            std::cout << "ERROR: Server connection is null" << std::endl;
            return 1;
        }
        
        std::cout << "SUCCESS: Server connection accepted" << std::endl;
        std::cout << "Server connection is_open: " << server_connection->is_open() << std::endl;
        std::cout << "Server local: " << server_connection->local_endpoint().address 
                  << ":" << server_connection->local_endpoint().port << std::endl;
        std::cout << "Server remote: " << server_connection->remote_endpoint().address 
                  << ":" << server_connection->remote_endpoint().port << std::endl;
        
        // Cleanup
        client_connection->close();
        server_connection->close();
        listener->close();
        
        std::cout << "Test completed successfully" << std::endl;
        return 0;
        
    } catch (const TimeoutException& e) {
        std::cout << "ERROR: TimeoutException: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cout << "ERROR: Exception: " << e.what() << std::endl;
        return 1;
    }
}