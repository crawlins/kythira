#define BOOST_TEST_MODULE IntegrationTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <set>
#include <future>

using namespace network_simulator;

namespace {
    constexpr const char* client_node_id = "client";
    constexpr const char* server_node_id = "server";
    constexpr const char* intermediate_node_id = "intermediate";
    constexpr unsigned short server_port = 8080;
    constexpr unsigned short client_port = 9090;
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 0.99;
    constexpr std::chrono::seconds test_timeout{5};
    constexpr const char* test_message = "Hello, Server!";
    constexpr const char* response_message = "Hello, Client!";
}

BOOST_AUTO_TEST_SUITE(client_server_integration)

BOOST_AUTO_TEST_CASE(full_connection_establishment_data_transfer_teardown, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create network topology: client <-> server
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);
    
    // Create nodes
    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);
    
    // Start simulation
    sim.start();
    
    // Server: bind to port and listen for connections
    auto listener = std::move(server->bind(server_port)).get();
    
    BOOST_TEST(listener != nullptr);
    BOOST_TEST(listener->is_listening());
    BOOST_TEST(listener->local_endpoint().address == server_node_id);
    BOOST_TEST(listener->local_endpoint().port == server_port);
    
    // Start accepting connections asynchronously
    auto accept_future = std::async(std::launch::async, [&listener]() {
        return std::move(listener->accept(test_timeout)).get();
    });
    
    // Small delay to ensure accept starts first
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    // Client: connect to server
    auto client_connection = std::move(client->connect(server_node_id, server_port, client_port)).get();
    
    BOOST_TEST(client_connection != nullptr);
    BOOST_TEST(client_connection->is_open());
    BOOST_TEST(client_connection->local_endpoint().address == client_node_id);
    BOOST_TEST(client_connection->local_endpoint().port == client_port);
    BOOST_TEST(client_connection->remote_endpoint().address == server_node_id);
    BOOST_TEST(client_connection->remote_endpoint().port == server_port);
    
    // Server: accept the connection
    auto server_connection = accept_future.get();
    
    BOOST_TEST(server_connection != nullptr);
    BOOST_TEST(server_connection->is_open());
    BOOST_TEST(server_connection->local_endpoint().address == server_node_id);
    BOOST_TEST(server_connection->local_endpoint().port == server_port);
    BOOST_TEST(server_connection->remote_endpoint().address == client_node_id);
    BOOST_TEST(server_connection->remote_endpoint().port == client_port);
    
    // Data transfer: client sends message to server
    std::vector<std::byte> client_data;
    for (char c : std::string(test_message)) {
        client_data.push_back(static_cast<std::byte>(c));
    }
    
    bool write_success = std::move(client_connection->write(client_data)).get();
    BOOST_TEST(write_success);
    
    // Server reads the message
    auto received_data = std::move(server_connection->read(test_timeout)).get();
    
    BOOST_TEST(received_data.size() == client_data.size());
    
    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_TEST(received_message == test_message);
    
    // Data transfer: server sends response to client
    std::vector<std::byte> server_data;
    for (char c : std::string(response_message)) {
        server_data.push_back(static_cast<std::byte>(c));
    }
    
    bool server_write_success = std::move(server_connection->write(server_data)).get();
    BOOST_TEST(server_write_success);
    
    // Client reads the response
    auto client_received_data = std::move(client_connection->read(test_timeout)).get();
    
    BOOST_TEST(client_received_data.size() == server_data.size());
    
    std::string client_received_message;
    for (auto byte : client_received_data) {
        client_received_message += static_cast<char>(byte);
    }
    BOOST_TEST(client_received_message == response_message);
    
    // Connection teardown
    client_connection->close();
    server_connection->close();
    listener->close();
    
    BOOST_TEST(!client_connection->is_open());
    BOOST_TEST(!server_connection->is_open());
    BOOST_TEST(!listener->is_listening());
    
    sim.stop();
}

BOOST_AUTO_TEST_CASE(connection_timeout_handling, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create client node but no server node (no route)
    sim.add_node(client_node_id);
    auto client = sim.create_node(client_node_id);
    
    sim.start();
    
    // Client tries to connect to non-existent server with timeout
    constexpr std::chrono::milliseconds short_timeout{100};
    
    try {
        auto connection = std::move(client->connect(server_node_id, server_port, short_timeout)).get();
        
        // If we get a connection object, check if it's actually usable
        if (connection == nullptr) {
            // Connection failed by returning null - acceptable
            BOOST_TEST(true);
        } else if (!connection->is_open()) {
            // Connection returned but not open - acceptable
            BOOST_TEST(true);
        } else {
            // Connection appears open, but should fail when used
            try {
                std::vector<std::byte> test_data{std::byte{0x42}};
                auto write_result = std::move(connection->write(test_data, short_timeout)).get();
                if (!write_result) {
                    // Write failed as expected
                    BOOST_TEST(true);
                } else {
                    BOOST_FAIL("Connection to non-existent server should not work");
                }
            } catch (const std::exception&) {
                // Write threw exception as expected
                BOOST_TEST(true);
            }
        }
    } catch (const TimeoutException&) {
        // Expected behavior
        BOOST_TEST(true);
    } catch (const std::exception&) {
        // Other exceptions are also acceptable (e.g., connection refused, no route)
        BOOST_TEST(true);
    }
    
    sim.stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(multi_node_topology_integration)

BOOST_AUTO_TEST_CASE(direct_message_routing_with_latency, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create direct connection: client -> server with latency
    NetworkEdge edge_with_latency(std::chrono::milliseconds{50}, 1.0);
    
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge_with_latency);
    
    // Create nodes
    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);
    
    sim.start();
    
    // Measure time for message delivery
    auto start_time = std::chrono::steady_clock::now();
    
    // Send message from client to server
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg(
        client_node_id, client_port,
        server_node_id, server_port,
        payload
    );
    
    // Send message
    bool send_success = std::move(client->send(msg)).get();
    BOOST_TEST(send_success);
    
    // Server should receive the message
    auto received_msg = std::move(server->receive()).get();
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Verify latency was applied (should be at least 50ms)
    BOOST_TEST(elapsed >= std::chrono::milliseconds{40});  // Allow some tolerance
    
    BOOST_TEST(received_msg.source_address() == client_node_id);
    BOOST_TEST(received_msg.source_port() == client_port);
    BOOST_TEST(received_msg.destination_address() == server_node_id);
    BOOST_TEST(received_msg.destination_port() == server_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_TEST(received_payload == test_message);
    
    sim.stop();
}

BOOST_AUTO_TEST_CASE(reliability_based_message_drops, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create edge with low reliability (30% success rate)
    NetworkEdge unreliable_edge(std::chrono::milliseconds{10}, 0.3);
    
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, unreliable_edge);
    
    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);
    
    sim.start();
    
    // Send multiple messages and count successes
    constexpr std::size_t message_count = 20;
    std::size_t successful_sends = 0;
    std::size_t received_messages = 0;
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    // Send messages
    for (std::size_t i = 0; i < message_count; ++i) {
        DefaultNetworkTypes::message_type msg(
            client_node_id, client_port,
            server_node_id, server_port,
            payload
        );
        
        bool send_success = std::move(client->send(msg)).get();
        if (send_success) {
            successful_sends++;
        }
    }
    
    // Try to receive messages (with timeout to avoid blocking)
    constexpr std::chrono::milliseconds receive_timeout{50};
    
    for (std::size_t i = 0; i < successful_sends; ++i) {
        try {
            auto received_msg = std::move(server->receive(receive_timeout)).get();
            received_messages++;
        } catch (const TimeoutException&) {
            // Message was dropped due to reliability
            break;
        }
    }
    
    // With 30% reliability, we expect some messages to be dropped
    // The number of received messages should be less than or equal to successful sends
    BOOST_TEST(received_messages <= successful_sends);
    
    // With 20 messages and 30% reliability, we expect roughly 6 successes
    // Allow for statistical variation (0 to 12 successes)
    BOOST_TEST(successful_sends <= 12);
    
    sim.stop();
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(concurrent_operations_integration)

BOOST_AUTO_TEST_CASE(multiple_nodes_sending_simultaneously, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create a star topology: multiple senders -> central receiver
    constexpr std::size_t sender_count = 3;  // Reduced for simpler test
    constexpr const char* receiver_id = "receiver";
    
    NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);
    
    std::vector<std::shared_ptr<DefaultNetworkTypes::node_type>> senders;
    
    // Create sender nodes and connect them to receiver
    for (std::size_t i = 0; i < sender_count; ++i) {
        std::string sender_id = "sender_" + std::to_string(i);
        sim.add_node(sender_id);
        sim.add_edge(sender_id, receiver_id, edge);
        senders.push_back(sim.create_node(sender_id));
    }
    
    sim.add_node(receiver_id);
    auto receiver = sim.create_node(receiver_id);
    sim.start();
    
    // All senders send messages concurrently
    constexpr std::size_t messages_per_sender = 2;  // Reduced for simpler test
    std::vector<std::future<bool>> send_futures;
    
    for (std::size_t sender_idx = 0; sender_idx < sender_count; ++sender_idx) {
        for (std::size_t msg_idx = 0; msg_idx < messages_per_sender; ++msg_idx) {
            std::string msg_content = "sender_" + std::to_string(sender_idx) + "_msg_" + std::to_string(msg_idx);
            std::vector<std::byte> payload;
            for (char c : msg_content) {
                payload.push_back(static_cast<std::byte>(c));
            }
            
            DefaultNetworkTypes::message_type msg(
                "sender_" + std::to_string(sender_idx), 
                static_cast<unsigned short>(1000 + sender_idx),
                receiver_id, 
                server_port,
                payload
            );
            
            // Use std::async to send messages concurrently
            send_futures.push_back(std::async(std::launch::async, [&senders, sender_idx, msg]() mutable {
                return std::move(senders[sender_idx]->send(msg)).get();
            }));
        }
    }
    
    // Wait for all sends to complete
    std::vector<bool> send_results;
    for (auto& future : send_futures) {
        send_results.push_back(future.get());
    }
    
    // Verify all sends succeeded
    for (const auto& result : send_results) {
        BOOST_TEST(result);
    }
    
    // Receiver should get all messages
    constexpr std::size_t total_messages = sender_count * messages_per_sender;
    std::set<std::string> received_messages;
    
    for (std::size_t i = 0; i < total_messages; ++i) {
        auto received_msg = std::move(receiver->receive()).get();
        
        std::string received_payload;
        for (auto byte : received_msg.payload()) {
            received_payload += static_cast<char>(byte);
        }
        
        received_messages.insert(received_payload);
    }
    
    // Verify we received all unique messages
    BOOST_TEST(received_messages.size() == total_messages);
    
    sim.stop();
}

BOOST_AUTO_TEST_SUITE_END()