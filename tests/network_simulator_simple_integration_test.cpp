#define BOOST_TEST_MODULE NetworkSimulatorSimpleIntegrationTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <memory>

using namespace network_simulator;

namespace {
    constexpr const char* client_node_id = "client";
    constexpr const char* server_node_id = "server";
    constexpr unsigned short server_port = 8080;
    constexpr unsigned short client_port = 9090;
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 1.0;  // Perfect reliability for integration tests
    constexpr std::chrono::seconds test_timeout{5};
    constexpr const char* test_message = "Hello, Server!";
    constexpr const char* response_message = "Hello, Client!";
}

/**
 * Simple integration test for client-server communication
 * Tests: basic connection establishment and data transfer using DefaultNetworkTypes
 * _Requirements: 6.1-6.5, 7.1-7.8, 8.1-8.6_
 */
BOOST_AUTO_TEST_CASE(simple_client_server_communication, * boost::unit_test::timeout(30)) {
    // Create simulator
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    // Configure topology
    simulator.add_node(server_node_id);
    simulator.add_node(client_node_id);
    simulator.add_edge(server_node_id, client_node_id, NetworkEdge(network_latency, network_reliability));
    simulator.add_edge(client_node_id, server_node_id, NetworkEdge(network_latency, network_reliability));
    
    // Create nodes
    auto server_node = simulator.create_node(server_node_id);
    auto client_node = simulator.create_node(client_node_id);
    
    // Start simulation
    simulator.start();
    
    // Server: Bind to port
    auto bind_future = server_node->bind(server_port);
    auto listener = std::move(bind_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    
    // Client: Connect to server
    auto connect_future = client_node->connect(server_node_id, server_port);
    auto client_connection = std::move(connect_future).get();
    
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    
    // Server: Accept connection
    auto accept_future = listener->accept(test_timeout);
    auto server_connection = std::move(accept_future).get();
    
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    
    // Client sends request
    std::vector<std::byte> request_data;
    for (char c : std::string(test_message)) {
        request_data.push_back(static_cast<std::byte>(c));
    }
    
    auto write_future = client_connection->write(request_data);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);
    
    // Server receives request
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();
    
    std::string received_request;
    for (auto byte : received_data) {
        received_request += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_request, test_message);
    
    // Server sends response
    std::vector<std::byte> response_data;
    for (char c : std::string(response_message)) {
        response_data.push_back(static_cast<std::byte>(c));
    }
    
    auto server_write_future = server_connection->write(response_data);
    bool server_write_success = std::move(server_write_future).get();
    BOOST_CHECK(server_write_success);
    
    // Client receives response
    auto client_read_future = client_connection->read(test_timeout);
    auto response_received = std::move(client_read_future).get();
    
    std::string received_response;
    for (auto byte : response_received) {
        received_response += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_response, response_message);
    
    // Clean up
    client_connection->close();
    server_connection->close();
    listener->close();
    
    simulator.stop();
}

/**
 * Integration test for connectionless communication
 * Tests: basic send/receive operations using DefaultNetworkTypes
 * _Requirements: 4.1-4.4, 5.1-5.3_
 */
BOOST_AUTO_TEST_CASE(connectionless_communication_integration, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create bidirectional network topology: client <-> server
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);
    
    // Create nodes
    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);
    
    BOOST_REQUIRE(client != nullptr);
    BOOST_REQUIRE(server != nullptr);
    BOOST_CHECK_EQUAL(client->address(), client_node_id);
    BOOST_CHECK_EQUAL(server->address(), server_node_id);
    
    // Start simulation
    sim.start();
    
    // === CONNECTIONLESS MESSAGE SENDING ===
    // Prepare test message
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg(
        client_node_id, client_port,
        server_node_id, server_port,
        payload
    );
    
    // Client sends message to server
    auto send_future = client->send(msg);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Add a small delay to allow message delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server receives the message
    auto receive_future = server->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Check if message was actually received (not empty)
    if (received_msg.source_address().empty()) {
        BOOST_FAIL("No message received - message delivery failed");
    }
    
    // Verify message content
    BOOST_CHECK_EQUAL(received_msg.source_address(), client_node_id);
    BOOST_CHECK_EQUAL(received_msg.source_port(), client_port);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), server_node_id);
    BOOST_CHECK_EQUAL(received_msg.destination_port(), server_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // Stop simulation
    sim.stop();
}