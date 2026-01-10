#define BOOST_TEST_MODULE NetworkSimulatorMultiNodeTopologyIntegrationTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <set>

using namespace network_simulator;

namespace {
    constexpr const char* node_a_id = "node_a";
    constexpr const char* node_b_id = "node_b";
    constexpr const char* node_c_id = "node_c";
    constexpr const char* node_d_id = "node_d";
    constexpr unsigned short test_port = 8080;
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 1.0;  // Perfect reliability for integration tests
    constexpr std::chrono::seconds test_timeout{5};
    constexpr const char* test_message = "Multi-hop message";
}

/**
 * Integration test for multi-node topology with message routing
 * Tests: messages routed through intermediate nodes
 * _Requirements: 1.1-1.5_
 */
BOOST_AUTO_TEST_CASE(multi_node_topology_routing, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create a linear topology: A -> B -> C -> D
    // This tests routing through intermediate nodes B and C
    NetworkEdge edge(network_latency, network_reliability);
    
    // Add all nodes
    sim.add_node(node_a_id);
    sim.add_node(node_b_id);
    sim.add_node(node_c_id);
    sim.add_node(node_d_id);
    
    // Create linear path: A -> B -> C -> D
    sim.add_edge(node_a_id, node_b_id, edge);
    sim.add_edge(node_b_id, node_c_id, edge);
    sim.add_edge(node_c_id, node_d_id, edge);
    
    // Also add reverse path for bidirectional communication
    sim.add_edge(node_d_id, node_c_id, edge);
    sim.add_edge(node_c_id, node_b_id, edge);
    sim.add_edge(node_b_id, node_a_id, edge);
    
    // Create nodes
    auto node_a = sim.create_node(node_a_id);
    auto node_b = sim.create_node(node_b_id);
    auto node_c = sim.create_node(node_c_id);
    auto node_d = sim.create_node(node_d_id);
    
    BOOST_REQUIRE(node_a != nullptr);
    BOOST_REQUIRE(node_b != nullptr);
    BOOST_REQUIRE(node_c != nullptr);
    BOOST_REQUIRE(node_d != nullptr);
    
    // Start simulation
    sim.start();
    
    // === TEST CONNECTIONLESS ROUTING THROUGH INTERMEDIATE NODES ===
    
    // Prepare test message from A to D (should route through B and C)
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg_a_to_d(
        node_a_id, test_port,
        node_d_id, test_port,
        payload
    );
    
    // Node A sends message to Node D
    auto send_future = node_a->send(msg_a_to_d);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Allow time for multi-hop routing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Node D should receive the message
    auto receive_future = node_d->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Verify message was received and routed correctly
    BOOST_CHECK_EQUAL(received_msg.source_address(), node_a_id);
    BOOST_CHECK_EQUAL(received_msg.source_port(), test_port);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), node_d_id);
    BOOST_CHECK_EQUAL(received_msg.destination_port(), test_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // === TEST REVERSE ROUTING ===
    
    // Prepare response message from D to A
    std::string response_message = "Response from D to A";
    std::vector<std::byte> response_payload;
    for (char c : response_message) {
        response_payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg_d_to_a(
        node_d_id, test_port,
        node_a_id, test_port,
        response_payload
    );
    
    // Node D sends response to Node A
    auto response_send_future = node_d->send(msg_d_to_a);
    bool response_send_success = std::move(response_send_future).get();
    BOOST_CHECK(response_send_success);
    
    // Allow time for multi-hop routing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Node A should receive the response
    auto response_receive_future = node_a->receive();
    auto received_response = std::move(response_receive_future).get();
    
    // Verify response was received and routed correctly
    BOOST_CHECK_EQUAL(received_response.source_address(), node_d_id);
    BOOST_CHECK_EQUAL(received_response.source_port(), test_port);
    BOOST_CHECK_EQUAL(received_response.destination_address(), node_a_id);
    BOOST_CHECK_EQUAL(received_response.destination_port(), test_port);
    
    std::string received_response_payload;
    for (auto byte : received_response.payload()) {
        received_response_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_response_payload, response_message);
    
    sim.stop();
}

/**
 * Integration test for star topology with central hub
 * Tests: messages routed through a central hub node
 * _Requirements: 1.1-1.5_
 */
BOOST_AUTO_TEST_CASE(star_topology_routing, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create a star topology with B as the central hub
    // A <-> B <-> C
    //       ^
    //       |
    //       v
    //       D
    NetworkEdge edge(network_latency, network_reliability);
    
    // Add all nodes
    sim.add_node(node_a_id);
    sim.add_node(node_b_id);  // Central hub
    sim.add_node(node_c_id);
    sim.add_node(node_d_id);
    
    // Create star topology with B as hub
    sim.add_edge(node_a_id, node_b_id, edge);
    sim.add_edge(node_b_id, node_a_id, edge);
    sim.add_edge(node_b_id, node_c_id, edge);
    sim.add_edge(node_c_id, node_b_id, edge);
    sim.add_edge(node_b_id, node_d_id, edge);
    sim.add_edge(node_d_id, node_b_id, edge);
    
    // Create nodes
    auto node_a = sim.create_node(node_a_id);
    auto node_b = sim.create_node(node_b_id);
    auto node_c = sim.create_node(node_c_id);
    auto node_d = sim.create_node(node_d_id);
    
    // Start simulation
    sim.start();
    
    // === TEST ROUTING FROM A TO C THROUGH HUB B ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg_a_to_c(
        node_a_id, test_port,
        node_c_id, test_port,
        payload
    );
    
    // Node A sends message to Node C (should route through B)
    auto send_future = node_a->send(msg_a_to_c);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Allow time for routing through hub
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Node C should receive the message
    auto receive_future = node_c->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Verify message was received
    BOOST_CHECK_EQUAL(received_msg.source_address(), node_a_id);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), node_c_id);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // === TEST ROUTING FROM A TO D THROUGH HUB B ===
    
    DefaultNetworkTypes::message_type msg_a_to_d(
        node_a_id, test_port,
        node_d_id, test_port,
        payload
    );
    
    auto send_future_2 = node_a->send(msg_a_to_d);
    bool send_success_2 = std::move(send_future_2).get();
    BOOST_CHECK(send_success_2);
    
    // Allow time for routing through hub
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Node D should receive the message
    auto receive_future_2 = node_d->receive();
    auto received_msg_2 = std::move(receive_future_2).get();
    
    // Verify message was received
    BOOST_CHECK_EQUAL(received_msg_2.source_address(), node_a_id);
    BOOST_CHECK_EQUAL(received_msg_2.destination_address(), node_d_id);
    
    sim.stop();
}

/**
 * Integration test for mesh topology with multiple paths
 * Tests: routing in a fully connected mesh network
 * _Requirements: 1.1-1.5_
 */
BOOST_AUTO_TEST_CASE(mesh_topology_routing, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create a mesh topology where every node connects to every other node
    NetworkEdge edge(network_latency, network_reliability);
    
    // Add all nodes
    sim.add_node(node_a_id);
    sim.add_node(node_b_id);
    sim.add_node(node_c_id);
    sim.add_node(node_d_id);
    
    // Create full mesh - every node connected to every other node
    std::vector<std::string> nodes = {node_a_id, node_b_id, node_c_id, node_d_id};
    
    for (const auto& from : nodes) {
        for (const auto& to : nodes) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
        }
    }
    
    // Create nodes
    auto node_a = sim.create_node(node_a_id);
    auto node_b = sim.create_node(node_b_id);
    auto node_c = sim.create_node(node_c_id);
    auto node_d = sim.create_node(node_d_id);
    
    // Start simulation
    sim.start();
    
    // === TEST DIRECT ROUTING IN MESH ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    // Test all possible node-to-node communications
    std::vector<std::pair<std::string, std::shared_ptr<DefaultNetworkTypes::node_type>>> node_pairs = {
        {node_a_id, node_a},
        {node_b_id, node_b},
        {node_c_id, node_c},
        {node_d_id, node_d}
    };
    
    // Test communication from A to all other nodes
    for (const auto& [dest_id, dest_node] : node_pairs) {
        if (dest_id == node_a_id) continue;  // Skip self
        
        DefaultNetworkTypes::message_type msg(
            node_a_id, test_port,
            dest_id, test_port,
            payload
        );
        
        // Send message
        auto send_future = node_a->send(msg);
        bool send_success = std::move(send_future).get();
        BOOST_CHECK(send_success);
        
        // Allow time for delivery
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Receive message
        auto receive_future = dest_node->receive();
        auto received_msg = std::move(receive_future).get();
        
        // Verify message
        BOOST_CHECK_EQUAL(received_msg.source_address(), node_a_id);
        BOOST_CHECK_EQUAL(received_msg.destination_address(), dest_id);
        
        std::string received_payload;
        for (auto byte : received_msg.payload()) {
            received_payload += static_cast<char>(byte);
        }
        BOOST_CHECK_EQUAL(received_payload, test_message);
    }
    
    sim.stop();
}

/**
 * Integration test for topology with varying latency and reliability
 * Tests: routing behavior with different edge characteristics
 * _Requirements: 1.1-1.5_
 */
BOOST_AUTO_TEST_CASE(topology_with_varying_characteristics, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create topology with different edge characteristics
    NetworkEdge fast_reliable_edge(std::chrono::milliseconds{5}, 1.0);    // Fast, reliable
    NetworkEdge slow_reliable_edge(std::chrono::milliseconds{50}, 1.0);   // Slow, reliable
    NetworkEdge fast_unreliable_edge(std::chrono::milliseconds{5}, 0.8);  // Fast, unreliable
    
    // Add nodes
    sim.add_node(node_a_id);
    sim.add_node(node_b_id);
    sim.add_node(node_c_id);
    sim.add_node(node_d_id);
    
    // Create topology with different edge types
    // A -> B (fast, reliable)
    sim.add_edge(node_a_id, node_b_id, fast_reliable_edge);
    sim.add_edge(node_b_id, node_a_id, fast_reliable_edge);
    
    // B -> C (slow, reliable)
    sim.add_edge(node_b_id, node_c_id, slow_reliable_edge);
    sim.add_edge(node_c_id, node_b_id, slow_reliable_edge);
    
    // C -> D (fast, unreliable)
    sim.add_edge(node_c_id, node_d_id, fast_unreliable_edge);
    sim.add_edge(node_d_id, node_c_id, fast_unreliable_edge);
    
    // Create nodes
    auto node_a = sim.create_node(node_a_id);
    auto node_b = sim.create_node(node_b_id);
    auto node_c = sim.create_node(node_c_id);
    auto node_d = sim.create_node(node_d_id);
    
    // Start simulation
    sim.start();
    
    // === TEST ROUTING WITH DIFFERENT LATENCIES ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    // Test A -> B (fast edge)
    DefaultNetworkTypes::message_type msg_a_to_b(
        node_a_id, test_port,
        node_b_id, test_port,
        payload
    );
    
    auto start_time = std::chrono::steady_clock::now();
    auto send_future = node_a->send(msg_a_to_b);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Small delay for fast edge
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    auto receive_future = node_b->receive();
    auto received_msg = std::move(receive_future).get();
    auto end_time = std::chrono::steady_clock::now();
    
    BOOST_CHECK_EQUAL(received_msg.source_address(), node_a_id);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), node_b_id);
    
    // Verify latency is reasonable for fast edge
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    BOOST_CHECK_LT(elapsed.count(), 100);  // Should be much less than 100ms
    
    // === TEST ROUTING WITH UNRELIABLE EDGE ===
    
    // Test C -> D (unreliable edge) - may need multiple attempts
    DefaultNetworkTypes::message_type msg_c_to_d(
        node_c_id, test_port,
        node_d_id, test_port,
        payload
    );
    
    bool message_delivered = false;
    constexpr int max_attempts = 10;  // With 80% reliability, should succeed within 10 attempts
    
    for (int attempt = 0; attempt < max_attempts && !message_delivered; ++attempt) {
        auto send_future_unreliable = node_c->send(msg_c_to_d);
        bool send_success_unreliable = std::move(send_future_unreliable).get();
        
        if (send_success_unreliable) {
            // Allow time for delivery
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            // Try to receive (may timeout if message was dropped)
            try {
                auto receive_future_unreliable = node_d->receive(std::chrono::milliseconds{100});
                auto received_msg_unreliable = std::move(receive_future_unreliable).get();
                
                if (!received_msg_unreliable.source_address().empty()) {
                    BOOST_CHECK_EQUAL(received_msg_unreliable.source_address(), node_c_id);
                    BOOST_CHECK_EQUAL(received_msg_unreliable.destination_address(), node_d_id);
                    message_delivered = true;
                }
            } catch (const std::exception&) {
                // Message may have been dropped due to unreliability - try again
                continue;
            }
        }
    }
    
    // With 80% reliability and 10 attempts, we should have succeeded
    // (probability of failure is 0.2^10 ≈ 0.000001)
    BOOST_CHECK(message_delivered);
    
    sim.stop();
}

/**
 * Integration test for connection-oriented communication through multi-hop topology
 * Tests: TCP-like connections routed through intermediate nodes
 * _Requirements: 1.1-1.5, 6.1-6.5, 7.1-7.8, 8.1-8.6_
 */
BOOST_AUTO_TEST_CASE(connection_oriented_multi_hop, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create linear topology: A -> B -> C
    NetworkEdge edge(network_latency, network_reliability);
    
    sim.add_node(node_a_id);
    sim.add_node(node_b_id);
    sim.add_node(node_c_id);
    
    // Bidirectional edges for connection establishment
    sim.add_edge(node_a_id, node_b_id, edge);
    sim.add_edge(node_b_id, node_a_id, edge);
    sim.add_edge(node_b_id, node_c_id, edge);
    sim.add_edge(node_c_id, node_b_id, edge);
    
    // Create nodes
    auto node_a = sim.create_node(node_a_id);
    auto node_b = sim.create_node(node_b_id);
    auto node_c = sim.create_node(node_c_id);
    
    // Start simulation
    sim.start();
    
    // === SERVER SETUP ON NODE C ===
    
    auto listener_future = node_c->bind(test_port);
    auto listener = std::move(listener_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    
    // === CLIENT CONNECTION FROM NODE A TO NODE C ===
    
    auto connect_future = node_a->connect(node_c_id, test_port);
    auto client_connection = std::move(connect_future).get();
    
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().address, node_c_id);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().port, test_port);
    
    // === SERVER ACCEPT CONNECTION ===
    
    auto accept_future = listener->accept(test_timeout);
    auto server_connection = std::move(accept_future).get();
    
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().address, node_a_id);
    
    // === DATA TRANSFER OVER MULTI-HOP CONNECTION ===
    
    std::vector<std::byte> test_data;
    for (char c : std::string(test_message)) {
        test_data.push_back(static_cast<std::byte>(c));
    }
    
    // Client sends data to server through multi-hop path
    auto write_future = client_connection->write(test_data);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);
    
    // Server receives data
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();
    
    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_message, test_message);
    
    // === BIDIRECTIONAL DATA TRANSFER ===
    
    std::string response = "Response through multi-hop";
    std::vector<std::byte> response_data;
    for (char c : response) {
        response_data.push_back(static_cast<std::byte>(c));
    }
    
    // Server sends response back to client
    auto server_write_future = server_connection->write(response_data);
    bool server_write_success = std::move(server_write_future).get();
    BOOST_CHECK(server_write_success);
    
    // Client receives response
    auto client_read_future = client_connection->read(test_timeout);
    auto client_received_data = std::move(client_read_future).get();
    
    std::string client_received_message;
    for (auto byte : client_received_data) {
        client_received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(client_received_message, response);
    
    // === CLEANUP ===
    
    client_connection->close();
    server_connection->close();
    listener->close();
    
    sim.stop();
}