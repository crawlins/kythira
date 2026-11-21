// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE NetworkSimulatorClientServerIntegrationTest
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
#include <future>

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
 * Integration test for connectionless communication
 * Tests: basic send/receive operations using DefaultNetworkTypes
 * _Requirements: 4.1-4.4, 5.1-5.3_
 */
BOOST_AUTO_TEST_CASE(connectionless_communication_integration, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create bidirectional network topology: client <-> server
    NetworkEdge edge(network_latency, network_reliability);
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

    DefaultNetworkTypes::message_type msg(client_node_id, client_port, server_node_id, server_port,
                                          payload);

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

/**
 * Integration test for full client-server communication lifecycle
 * Tests: connection establishment, data transfer, and teardown using DefaultNetworkTypes
 * _Requirements: 6.1-6.5, 7.1-7.8, 8.1-8.6_
 */
BOOST_AUTO_TEST_CASE(full_client_server_communication_lifecycle, *boost::unit_test::timeout(30)) {
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

    // === SERVER SETUP ===
    // Server: bind to port and create listener
    auto listener_future = server->bind(server_port);
    auto listener = std::move(listener_future).get();

    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    BOOST_CHECK_EQUAL(listener->local_endpoint().address, server_node_id);
    BOOST_CHECK_EQUAL(listener->local_endpoint().port, server_port);

    // === CLIENT CONNECTION AND SERVER ACCEPT (CONCURRENT) ===
    // Start connection establishment asynchronously first
    auto connect_future = std::async(std::launch::async, [&client]() {
        // Small delay to ensure accept starts first
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        auto client_connection_future = client->connect(server_node_id, server_port, client_port);
        return std::move(client_connection_future).get();
    });

    // Start accept operation immediately after starting the async connection
    auto accept_future = listener->accept(test_timeout);

    // Wait for both operations to complete
    auto client_connection = std::move(connect_future).get();
    auto server_connection = std::move(accept_future).get();

    // Verify both connections are established correctly
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    BOOST_CHECK_EQUAL(client_connection->local_endpoint().address, client_node_id);
    BOOST_CHECK_EQUAL(client_connection->local_endpoint().port, client_port);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().address, server_node_id);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().port, server_port);

    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    BOOST_CHECK_EQUAL(server_connection->local_endpoint().address, server_node_id);
    BOOST_CHECK_EQUAL(server_connection->local_endpoint().port, server_port);
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().address, client_node_id);
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().port, client_port);

    // === DATA TRANSFER: CLIENT TO SERVER ===
    // Prepare test data
    std::vector<std::byte> client_data;
    for (char c : std::string(test_message)) {
        client_data.push_back(static_cast<std::byte>(c));
    }

    // Client writes data to server
    auto write_future = client_connection->write(client_data);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);

    // Server reads the data
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();

    BOOST_REQUIRE_EQUAL(received_data.size(), client_data.size());

    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_message, test_message);

    // === DATA TRANSFER: SERVER TO CLIENT ===
    // Prepare response data
    std::vector<std::byte> server_data;
    for (char c : std::string(response_message)) {
        server_data.push_back(static_cast<std::byte>(c));
    }

    // Server writes response to client
    auto server_write_future = server_connection->write(server_data);
    bool server_write_success = std::move(server_write_future).get();
    BOOST_CHECK(server_write_success);

    // Client reads the response
    auto client_read_future = client_connection->read(test_timeout);
    auto client_received_data = std::move(client_read_future).get();

    BOOST_REQUIRE_EQUAL(client_received_data.size(), server_data.size());

    std::string client_received_message;
    for (auto byte : client_received_data) {
        client_received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(client_received_message, response_message);

    // === CONNECTION TEARDOWN ===
    // Close connections
    client_connection->close();
    server_connection->close();
    listener->close();

    // Verify connections are closed
    BOOST_CHECK(!client_connection->is_open());
    BOOST_CHECK(!server_connection->is_open());
    BOOST_CHECK(!listener->is_listening());

    // Stop simulation
    sim.stop();
}

/**
 * Integration test for connect failures, each with the exception the spec names
 * Tests: an unknown destination (Req 13.4), a known destination with no path
 * (Req 13.5), a refused connection, and a handshake slower than the timeout
 * (Req 6.5, 13.1). These used to accept any outcome at all.
 * _Requirements: 6.5, 13.1, 13.4, 13.5, 13.6_
 */
BOOST_AUTO_TEST_CASE(connection_timeout_handling, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    sim.add_node(client_node_id);
    auto client = sim.create_node(client_node_id);

    // Known to the topology, but with no edge from the client.
    constexpr const char* island_node_id = "island";
    sim.add_node(island_node_id);

    // Reachable, but its one-way latency is longer than the connect timeout.
    constexpr const char* slow_node_id = "slow";
    sim.add_edge(client_node_id, slow_node_id, NetworkEdge(std::chrono::milliseconds{500}, 1.0));
    auto slow = sim.create_node(slow_node_id);

    sim.start();

    constexpr std::chrono::milliseconds short_timeout{100};

    // The server node was never added: NodeNotFoundException, through the future.
    {
        auto future = client->connect(server_node_id, server_port, short_timeout);
        BOOST_CHECK_THROW(std::move(future).get(), NodeNotFoundException);
    }

    // Both nodes exist, nothing joins them: NoRouteException.
    {
        auto future = client->connect(island_node_id, server_port, short_timeout);
        try {
            std::move(future).get();
            BOOST_FAIL("connect to an unreachable node should fail");
        } catch (const NoRouteException& e) {
            // The message names both ends once; it used to repeat itself.
            const std::string what = e.what();
            BOOST_CHECK_EQUAL(
                what, std::string("No route from ") + client_node_id + " to " + island_node_id);
        }
    }

    // A route but no listener on the port: ConnectionRefusedException, whose
    // message keeps the "Connection refused" prefix Raft's classifier reads.
    {
        auto future = client->connect(slow_node_id, server_port, short_timeout);
        try {
            std::move(future).get();
            BOOST_FAIL("connect with no listener should be refused");
        } catch (const ConnectionRefusedException& e) {
            BOOST_CHECK(std::string(e.what()).starts_with("Connection refused"));
        }
    }

    // A listener, but the handshake outlasts the timeout: TimeoutException.
    {
        auto listener = std::move(slow->bind(server_port)).get();
        BOOST_REQUIRE(listener != nullptr);
        auto future = client->connect(slow_node_id, server_port, short_timeout);
        BOOST_CHECK_THROW(std::move(future).get(), TimeoutException);
        listener->close();
    }

    sim.stop();
}

/**
 * Integration test for bind timeout handling
 * Tests: timeout exceptions when binding fails
 * _Requirements: 7.5_
 */
BOOST_AUTO_TEST_CASE(bind_timeout_handling, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    sim.add_node(server_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // First bind should succeed
    auto listener1 = std::move(server->bind(server_port)).get();
    BOOST_REQUIRE(listener1 != nullptr);
    BOOST_CHECK(listener1->is_listening());

    // An untimed bind to a taken port fails at once with PortInUseException
    // (Req 13.3).
    BOOST_CHECK_THROW(std::move(server->bind(server_port)).get(), PortInUseException);

    // A timed bind waits for the port and fails with TimeoutException only
    // once the deadline passes (Req 7.5, Property 16).
    constexpr std::chrono::milliseconds short_timeout{100};
    auto started = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(std::move(server->bind(server_port, short_timeout)).get(), TimeoutException);
    BOOST_CHECK(std::chrono::steady_clock::now() - started >= short_timeout);

    // The failed bind must not have released the port the first listener
    // holds: an ephemeral connect from this node must never be given it.
    BOOST_CHECK(listener1->is_listening());
    BOOST_CHECK_THROW(std::move(server->bind(server_port)).get(), PortInUseException);

    // A timed bind that starts while the port is taken succeeds once the
    // holder lets go inside its timeout.
    auto closer = std::async(std::launch::async, [&listener1] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        listener1->close();
    });
    auto listener3 = std::move(server->bind(server_port, std::chrono::seconds{5})).get();
    closer.get();
    BOOST_REQUIRE(listener3 != nullptr);
    BOOST_CHECK(listener3->is_listening());

    listener3->close();
    sim.stop();
}

/**
 * Integration test for accept timeout handling
 * Tests: timeout exceptions when no clients connect
 * _Requirements: 7.8_
 */
BOOST_AUTO_TEST_CASE(accept_timeout_handling, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    sim.add_node(server_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Bind to port
    auto listener = std::move(server->bind(server_port)).get();
    BOOST_REQUIRE(listener != nullptr);

    // Accept with timeout but no connecting clients
    constexpr std::chrono::milliseconds short_timeout{100};

    // Property 18: no client before the deadline is a TimeoutException, not
    // a null connection.
    BOOST_CHECK_THROW(std::move(listener->accept(short_timeout)).get(), TimeoutException);

    listener->close();
    sim.stop();
}

/**
 * Integration test for read timeout handling
 * Tests: timeout exceptions when no data is available
 * _Requirements: 8.3_
 */
BOOST_AUTO_TEST_CASE(read_timeout_handling, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create bidirectional topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);

    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Establish connection using async pattern
    auto listener = std::move(server->bind(server_port)).get();

    // Start connection establishment asynchronously first
    auto connect_future = std::async(std::launch::async, [&client]() {
        // Small delay to ensure accept starts first
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return std::move(client->connect(server_node_id, server_port, client_port)).get();
    });

    // Start accept operation immediately after starting the async connection
    auto accept_future = listener->accept(test_timeout);

    // Wait for both operations to complete
    auto client_connection = std::move(connect_future).get();
    auto server_connection = std::move(accept_future).get();

    // Try to read with timeout but no data available
    constexpr std::chrono::milliseconds short_timeout{100};

    BOOST_CHECK_THROW(std::move(server_connection->read(short_timeout)).get(), TimeoutException);

    // Req 13.2: once closed, reads and writes fail with ConnectionClosedException.
    server_connection->close();
    BOOST_CHECK_THROW(std::move(server_connection->read(short_timeout)).get(),
                      ConnectionClosedException);
    BOOST_CHECK_THROW(std::move(server_connection->write({std::byte{0x01}})).get(),
                      ConnectionClosedException);

    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    sim.stop();
}

/**
 * Integration test for write timeout handling
 * Tests: timeout exceptions when write operations cannot complete
 * _Requirements: 8.6_
 */
BOOST_AUTO_TEST_CASE(write_timeout_handling, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create topology with very low reliability to simulate congestion
    NetworkEdge unreliable_edge(network_latency, 0.1);  // 10% reliability
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, unreliable_edge);
    sim.add_edge(server_node_id, client_node_id, unreliable_edge);

    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Establish connection using async pattern
    auto listener = std::move(server->bind(server_port)).get();

    // Start connection establishment asynchronously first
    auto connect_future = std::async(std::launch::async, [&client]() {
        // Small delay to ensure accept starts first
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return std::move(client->connect(server_node_id, server_port, client_port)).get();
    });

    // Start accept operation immediately after starting the async connection
    auto accept_future = listener->accept(test_timeout);

    // Wait for both operations to complete
    auto client_connection = std::move(connect_future).get();
    auto server_connection = std::move(accept_future).get();

    // Try to write with very short timeout
    std::vector<std::byte> large_data(1000, std::byte{0x42});  // Large data
    constexpr std::chrono::milliseconds very_short_timeout{1};

    // The edge's 10 ms latency cannot fit in a 1 ms timeout (Req 8.6, 13.1).
    BOOST_CHECK_THROW(std::move(client_connection->write(large_data, very_short_timeout)).get(),
                      TimeoutException);

    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    sim.stop();
}

/**
 * Integration test for ephemeral port allocation
 * Tests: automatic assignment of unique ephemeral ports
 * _Requirements: 6.3_
 */
BOOST_AUTO_TEST_CASE(ephemeral_port_allocation, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);

    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Server setup
    auto listener = std::move(server->bind(server_port)).get();

    // Create multiple connections without specifying source port
    constexpr std::size_t connection_count = 3;
    std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> connections;
    std::vector<unsigned short> allocated_ports;

    for (std::size_t i = 0; i < connection_count; ++i) {
        // Start connection establishment asynchronously first
        auto connect_future = std::async(std::launch::async, [&client]() {
            // Small delay to ensure accept starts first
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            // Connect without specifying source port (should get ephemeral port)
            return std::move(client->connect(server_node_id, server_port)).get();
        });

        // Start accept operation immediately after starting the async connection
        auto accept_future = listener->accept(test_timeout);

        // Wait for both operations to complete
        auto connection = std::move(connect_future).get();
        auto server_connection = std::move(accept_future).get();

        BOOST_REQUIRE(connection != nullptr);
        BOOST_CHECK(connection->is_open());
        BOOST_REQUIRE(server_connection != nullptr);

        // Collect allocated port
        unsigned short allocated_port = connection->local_endpoint().port;
        allocated_ports.push_back(allocated_port);
        connections.push_back(connection);
    }

    // Verify all allocated ports are unique
    std::set<unsigned short> unique_ports(allocated_ports.begin(), allocated_ports.end());
    BOOST_CHECK_EQUAL(unique_ports.size(), connection_count);

    // Cleanup
    for (auto& conn : connections) {
        conn->close();
    }
    listener->close();
    sim.stop();
}

/**
 * Integration test for multiple concurrent connections
 * Tests: handling multiple simultaneous client-server connections
 * _Requirements: 6.1-6.5, 7.1-7.8, 8.1-8.6, 14.1-14.5_
 */
BOOST_AUTO_TEST_CASE(multiple_concurrent_connections, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);

    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Server setup
    auto listener = std::move(server->bind(server_port)).get();

    // Create multiple concurrent connections
    constexpr std::size_t connection_count = 3;
    std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> client_connections;
    std::vector<std::shared_ptr<DefaultNetworkTypes::connection_type>> server_connections;

    // Establish all connections
    for (std::size_t i = 0; i < connection_count; ++i) {
        // Use different client ports for each connection
        unsigned short client_port_i = client_port + static_cast<unsigned short>(i);

        // Start connection establishment asynchronously first
        auto connect_future = std::async(std::launch::async, [&client, client_port_i]() {
            // Small delay to ensure accept starts first
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            return std::move(client->connect(server_node_id, server_port, client_port_i)).get();
        });

        // Start accept operation immediately after starting the async connection
        auto accept_future = listener->accept(test_timeout);

        // Wait for both operations to complete
        auto client_connection = std::move(connect_future).get();
        auto server_connection = std::move(accept_future).get();

        BOOST_REQUIRE(client_connection != nullptr);
        BOOST_CHECK(client_connection->is_open());
        client_connections.push_back(client_connection);

        BOOST_REQUIRE(server_connection != nullptr);
        BOOST_CHECK(server_connection->is_open());
        server_connections.push_back(server_connection);
    }

    // Send data on all connections simultaneously
    for (std::size_t i = 0; i < connection_count; ++i) {
        std::string message = "Message from connection " + std::to_string(i);
        std::vector<std::byte> data;
        for (char c : message) {
            data.push_back(static_cast<std::byte>(c));
        }

        bool write_success = std::move(client_connections[i]->write(data)).get();
        BOOST_CHECK(write_success);
    }

    // Read data on all connections
    for (std::size_t i = 0; i < connection_count; ++i) {
        auto received_data = std::move(server_connections[i]->read(test_timeout)).get();

        std::string received_message;
        for (auto byte : received_data) {
            received_message += static_cast<char>(byte);
        }

        std::string expected_message = "Message from connection " + std::to_string(i);
        BOOST_CHECK_EQUAL(received_message, expected_message);
    }

    // Cleanup all connections
    for (auto& conn : client_connections) {
        conn->close();
    }
    for (auto& conn : server_connections) {
        conn->close();
    }
    listener->close();
    sim.stop();
}

/**
 * Integration test for connection state management
 * Tests: proper handling of connection lifecycle and state transitions
 * _Requirements: 8.1-8.6_
 */
BOOST_AUTO_TEST_CASE(connection_state_management, *boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;

    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(client_node_id);
    sim.add_node(server_node_id);
    sim.add_edge(client_node_id, server_node_id, edge);
    sim.add_edge(server_node_id, client_node_id, edge);

    auto client = sim.create_node(client_node_id);
    auto server = sim.create_node(server_node_id);

    sim.start();

    // Establish connection using async pattern
    auto listener = std::move(server->bind(server_port)).get();

    // Start connection establishment asynchronously first
    auto connect_future = std::async(std::launch::async, [&client]() {
        // Small delay to ensure accept starts first
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return std::move(client->connect(server_node_id, server_port, client_port)).get();
    });

    // Start accept operation immediately after starting the async connection
    auto accept_future = listener->accept(test_timeout);

    // Wait for both operations to complete
    auto client_connection = std::move(connect_future).get();
    auto server_connection = std::move(accept_future).get();

    // Verify connections are open
    BOOST_CHECK(client_connection->is_open());
    BOOST_CHECK(server_connection->is_open());

    // Test operations on open connections
    std::vector<std::byte> test_data{std::byte{0x48}, std::byte{0x69}};  // "Hi"

    bool write_success = std::move(client_connection->write(test_data)).get();
    BOOST_CHECK(write_success);

    auto received_data = std::move(server_connection->read(test_timeout)).get();
    BOOST_CHECK_EQUAL(received_data.size(), test_data.size());

    // Close client connection
    client_connection->close();
    BOOST_CHECK(!client_connection->is_open());

    // Server connection should still be open
    BOOST_CHECK(server_connection->is_open());

    // Close server connection
    server_connection->close();
    BOOST_CHECK(!server_connection->is_open());

    // Test operations on closed connections should throw
    try {
        std::move(client_connection->write(test_data)).get();
        BOOST_FAIL("Write to closed connection should have failed");
    } catch (const ConnectionClosedException&) {
        // Expected behavior
        BOOST_CHECK(true);
    }

    try {
        std::move(server_connection->read()).get();
        BOOST_FAIL("Read from closed connection should have failed");
    } catch (const ConnectionClosedException&) {
        // Expected behavior
        BOOST_CHECK(true);
    }

    listener->close();
    sim.stop();
}