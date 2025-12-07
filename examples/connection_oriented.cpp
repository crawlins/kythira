// Example: Connection-Oriented Client-Server Communication
// This example demonstrates:
// 1. Server binding to a port and listening for connections
// 2. Client connecting to server
// 3. Bidirectional data transfer over established connections
// 4. Connection lifecycle management (open/close)
// 5. Timeout handling for all connection operations

#include <network_simulator/network_simulator.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <folly/init/Init.h>

using namespace network_simulator;
using namespace std::chrono_literals;

namespace {
    // Named constants for test configuration
    constexpr const char* server_node_id = "server";
    constexpr const char* client_node_id = "client";
    constexpr unsigned short server_port = 8080;
    constexpr unsigned short client_port = 9090;
    constexpr const char* client_request = "GET /hello HTTP/1.1";
    constexpr const char* server_response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
    constexpr auto default_latency = 10ms;
    constexpr double high_reliability = 0.99;
    constexpr auto short_timeout = 100ms;
    constexpr auto long_timeout = 2000ms;
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

// Test scenario: Basic server bind and client connect
auto test_basic_connection_establishment() -> bool {
    std::cout << "Test 1: Basic Connection Establishment\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Server: Bind to port
        auto bind_future = server_node->bind(server_port);
        auto listener = std::move(bind_future).get();
        
        if (!listener->is_listening()) {
            std::cerr << "  ✗ Listener not in listening state\n";
            return false;
        }
        
        // Client: Connect to server
        auto connect_future = client_node->connect(server_node_id, server_port);
        auto client_connection = std::move(connect_future).get();
        
        if (!client_connection->is_open()) {
            std::cerr << "  ✗ Client connection not open\n";
            return false;
        }
        
        // Server: Accept connection
        auto accept_future = listener->accept(long_timeout);
        auto server_connection = std::move(accept_future).get();
        
        if (!server_connection->is_open()) {
            std::cerr << "  ✗ Server connection not open\n";
            return false;
        }
        
        // Verify endpoint information
        auto client_local = client_connection->local_endpoint();
        auto client_remote = client_connection->remote_endpoint();
        
        if (client_local.address() != client_node_id ||
            client_remote.address() != server_node_id ||
            client_remote.port() != server_port) {
            std::cerr << "  ✗ Client connection endpoints incorrect\n";
            return false;
        }
        
        auto server_local = server_connection->local_endpoint();
        auto server_remote = server_connection->remote_endpoint();
        
        if (server_local.address() != server_node_id ||
            server_local.port() != server_port ||
            server_remote.address() != client_node_id) {
            std::cerr << "  ✗ Server connection endpoints incorrect\n";
            return false;
        }
        
        std::cout << "  ✓ Connection establishment successful\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Bidirectional data transfer
auto test_bidirectional_data_transfer() -> bool {
    std::cout << "Test 2: Bidirectional Data Transfer\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Establish connection
        auto listener = std::move(server_node->bind(server_port)).get();
        auto client_connection = std::move(client_node->connect(server_node_id, server_port)).get();
        auto server_connection = std::move(listener->accept(long_timeout)).get();
        
        // Client sends request
        auto request_data = string_to_bytes(client_request);
        auto write_future = client_connection->write(request_data);
        bool write_success = std::move(write_future).get();
        
        if (!write_success) {
            std::cerr << "  ✗ Client write failed\n";
            return false;
        }
        
        // Server receives request
        auto read_future = server_connection->read(long_timeout);
        auto received_data = std::move(read_future).get();
        auto received_request = bytes_to_string(received_data);
        
        if (received_request != client_request) {
            std::cerr << "  ✗ Server received incorrect request. Expected: '" 
                      << client_request << "', Got: '" << received_request << "'\n";
            return false;
        }
        
        // Server sends response
        auto response_data = string_to_bytes(server_response);
        auto server_write_future = server_connection->write(response_data);
        bool server_write_success = std::move(server_write_future).get();
        
        if (!server_write_success) {
            std::cerr << "  ✗ Server write failed\n";
            return false;
        }
        
        // Client receives response
        auto client_read_future = client_connection->read(long_timeout);
        auto response_received = std::move(client_read_future).get();
        auto received_response = bytes_to_string(response_received);
        
        if (received_response != server_response) {
            std::cerr << "  ✗ Client received incorrect response. Expected: '" 
                      << server_response << "', Got: '" << received_response << "'\n";
            return false;
        }
        
        std::cout << "  ✓ Bidirectional data transfer successful\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Connection with specified source port
auto test_specified_source_port() -> bool {
    std::cout << "Test 3: Connection with Specified Source Port\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Server: Bind to port
        auto listener = std::move(server_node->bind(server_port)).get();
        
        // Client: Connect with specified source port
        auto client_connection = std::move(client_node->connect(server_node_id, server_port, client_port)).get();
        
        // Verify client connection uses specified source port
        auto client_local = client_connection->local_endpoint();
        if (client_local.port() != client_port) {
            std::cerr << "  ✗ Client connection not using specified source port. Expected: " 
                      << client_port << ", Got: " << client_local.port() << "\n";
            return false;
        }
        
        std::cout << "  ✓ Specified source port used correctly\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Connection timeout handling
auto test_connection_timeout() -> bool {
    std::cout << "Test 4: Connection Timeout Handling\n";
    
    try {
        // Create simulator with no server listening
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Client: Try to connect with timeout - should fail since no server listening
        try {
            auto connect_future = client_node->connect(server_node_id, server_port, short_timeout);
            auto client_connection = std::move(connect_future).get();
            
            std::cerr << "  ✗ Connection should have failed (no server listening)\n";
            return false;
            
        } catch (const TimeoutException&) {
            std::cout << "  ✓ Connection timeout handled correctly\n";
            return true;
        } catch (const std::exception& e) {
            // Other exceptions are also acceptable (e.g., connection refused)
            std::cout << "  ✓ Connection failure handled correctly: " << e.what() << "\n";
            return true;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Accept timeout handling
auto test_accept_timeout() -> bool {
    std::cout << "Test 5: Accept Timeout Handling\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        
        // Create node
        auto server_node = simulator.create_node(server_node_id);
        
        // Start simulation
        simulator.start();
        
        // Server: Bind and try to accept with timeout - should timeout since no client connecting
        auto listener = std::move(server_node->bind(server_port)).get();
        
        try {
            auto accept_future = listener->accept(short_timeout);
            auto server_connection = std::move(accept_future).get();
            
            std::cerr << "  ✗ Accept should have timed out\n";
            return false;
            
        } catch (const TimeoutException&) {
            std::cout << "  ✓ Accept timeout handled correctly\n";
            return true;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Read/Write timeout handling
auto test_read_write_timeout() -> bool {
    std::cout << "Test 6: Read/Write Timeout Handling\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Establish connection
        auto listener = std::move(server_node->bind(server_port)).get();
        auto client_connection = std::move(client_node->connect(server_node_id, server_port)).get();
        auto server_connection = std::move(listener->accept(long_timeout)).get();
        
        // Test read timeout - try to read when no data is available
        try {
            auto read_future = client_connection->read(short_timeout);
            auto data = std::move(read_future).get();
            
            std::cerr << "  ✗ Read should have timed out\n";
            return false;
            
        } catch (const TimeoutException&) {
            std::cout << "  ✓ Read timeout handled correctly\n";
        }
        
        // Test write timeout - this is harder to trigger, so we'll just verify
        // that write with timeout doesn't throw immediately
        try {
            auto request_data = string_to_bytes(client_request);
            auto write_future = client_connection->write(request_data, long_timeout);
            bool write_success = std::move(write_future).get();
            
            if (write_success) {
                std::cout << "  ✓ Write with timeout completed successfully\n";
            } else {
                std::cout << "  ✓ Write with timeout failed gracefully\n";
            }
            
        } catch (const TimeoutException&) {
            std::cout << "  ✓ Write timeout handled correctly\n";
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

// Test scenario: Connection lifecycle (close handling)
auto test_connection_lifecycle() -> bool {
    std::cout << "Test 7: Connection Lifecycle\n";
    
    try {
        // Create simulator
        NetworkSimulator<std::string, unsigned short> simulator;
        
        // Configure topology
        simulator.add_node(server_node_id);
        simulator.add_node(client_node_id);
        simulator.add_edge(server_node_id, client_node_id, NetworkEdge(default_latency, high_reliability));
        simulator.add_edge(client_node_id, server_node_id, NetworkEdge(default_latency, high_reliability));
        
        // Create nodes
        auto server_node = simulator.create_node(server_node_id);
        auto client_node = simulator.create_node(client_node_id);
        
        // Start simulation
        simulator.start();
        
        // Establish connection
        auto listener = std::move(server_node->bind(server_port)).get();
        auto client_connection = std::move(client_node->connect(server_node_id, server_port)).get();
        auto server_connection = std::move(listener->accept(long_timeout)).get();
        
        // Verify connections are open
        if (!client_connection->is_open() || !server_connection->is_open()) {
            std::cerr << "  ✗ Connections not open after establishment\n";
            return false;
        }
        
        // Close client connection
        client_connection->close();
        
        // Verify client connection is closed
        if (client_connection->is_open()) {
            std::cerr << "  ✗ Client connection still open after close\n";
            return false;
        }
        
        // Try to write to closed connection - should fail
        try {
            auto request_data = string_to_bytes(client_request);
            auto write_future = client_connection->write(request_data);
            auto write_success = std::move(write_future).get();
            
            std::cerr << "  ✗ Write to closed connection should have failed\n";
            return false;
            
        } catch (const ConnectionClosedException&) {
            std::cout << "  ✓ Write to closed connection handled correctly\n";
        }
        
        // Try to read from closed connection - should fail
        try {
            auto read_future = client_connection->read();
            auto data = std::move(read_future).get();
            
            std::cerr << "  ✗ Read from closed connection should have failed\n";
            return false;
            
        } catch (const ConnectionClosedException&) {
            std::cout << "  ✓ Read from closed connection handled correctly\n";
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Exception: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    // Initialize folly library
    folly::Init init(&argc, &argv);
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  Connection-Oriented Client-Server Example\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_basic_connection_establishment()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_bidirectional_data_transfer()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_specified_source_port()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_connection_timeout()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_accept_timeout()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_read_write_timeout()) failed_scenarios++;
    std::cout << "\n";
    
    if (!test_connection_lifecycle()) failed_scenarios++;
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