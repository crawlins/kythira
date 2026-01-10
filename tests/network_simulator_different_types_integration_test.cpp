#define BOOST_TEST_MODULE NetworkSimulatorDifferentTypesIntegrationTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <arpa/inet.h>  // For inet_addr

using namespace network_simulator;

namespace {
    // Test constants for IPv4 custom types
    constexpr const char* test_server_ipv4 = "192.168.1.100";
    constexpr const char* test_client_ipv4 = "192.168.1.101";
    constexpr const char* test_server_string_port = "8080";
    constexpr const char* test_client_string_port = "9090";
    
    // Test constants for unsigned long custom types
    constexpr unsigned long test_server_ulong = 0x12345678UL;
    constexpr unsigned long test_client_ulong = 0x87654321UL;
    constexpr unsigned short test_server_ushort_port = 8080;
    constexpr unsigned short test_client_ushort_port = 9090;
    
    // Common test constants
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 1.0;  // Perfect reliability for integration tests
    constexpr std::chrono::seconds test_timeout{5};
    constexpr const char* test_message = "Integration test message";
}

// Custom Types Implementation 1: IPv4 addresses with string ports
struct IPv4StringPortTypes {
    using address_type = IPv4Address;
    using port_type = std::string;
    using message_type = Message<IPv4StringPortTypes>;
    using connection_type = Connection<IPv4StringPortTypes>;
    using listener_type = Listener<IPv4StringPortTypes>;
    using node_type = NetworkNode<IPv4StringPortTypes>;
    
    // Future types using kythira::Future
    using future_bool_type = kythira::Future<bool>;
    using future_message_type = kythira::Future<message_type>;
    using future_connection_type = kythira::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = kythira::Future<std::shared_ptr<listener_type>>;
    using future_bytes_type = kythira::Future<std::vector<std::byte>>;
};

// Custom Types Implementation 2: unsigned long addresses with unsigned short ports
struct ULongUShortPortTypes {
    using address_type = unsigned long;
    using port_type = unsigned short;
    using message_type = Message<ULongUShortPortTypes>;
    using connection_type = Connection<ULongUShortPortTypes>;
    using listener_type = Listener<ULongUShortPortTypes>;
    using node_type = NetworkNode<ULongUShortPortTypes>;
    
    // Future types using kythira::Future
    using future_bool_type = kythira::Future<bool>;
    using future_message_type = kythira::Future<message_type>;
    using future_connection_type = kythira::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = kythira::Future<std::shared_ptr<listener_type>>;
    using future_bytes_type = kythira::Future<std::vector<std::byte>>;
};

// Verify concept satisfaction at compile time
static_assert(network_simulator_types<DefaultNetworkTypes>, 
              "DefaultNetworkTypes must satisfy network_simulator_types concept");
static_assert(network_simulator_types<IPv4StringPortTypes>, 
              "IPv4StringPortTypes must satisfy network_simulator_types concept");
static_assert(network_simulator_types<ULongUShortPortTypes>, 
              "ULongUShortPortTypes must satisfy network_simulator_types concept");

// Helper functions for IPv4 addresses
auto create_ipv4_address(const std::string& ip_str) -> IPv4Address {
    in_addr addr{};
    if (inet_aton(ip_str.c_str(), &addr) == 0) {
        throw std::invalid_argument("Invalid IPv4 address: " + ip_str);
    }
    return IPv4Address(addr);
}

auto ipv4_to_string(const IPv4Address& addr) -> std::string {
    return inet_ntoa(addr.get());
}

/**
 * Integration test for DefaultNetworkTypes (baseline)
 * Tests: basic functionality with string addresses and unsigned short ports
 * _Requirements: 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(default_types_integration, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Use string addresses and unsigned short ports (default types)
    std::string server_addr = "server_node";
    std::string client_addr = "client_node";
    unsigned short server_port = 8080;
    unsigned short client_port = 9090;
    
    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(server_addr);
    sim.add_node(client_addr);
    sim.add_edge(server_addr, client_addr, edge);
    sim.add_edge(client_addr, server_addr, edge);
    
    // Create nodes
    auto server = sim.create_node(server_addr);
    auto client = sim.create_node(client_addr);
    
    BOOST_REQUIRE(server != nullptr);
    BOOST_REQUIRE(client != nullptr);
    BOOST_CHECK_EQUAL(server->address(), server_addr);
    BOOST_CHECK_EQUAL(client->address(), client_addr);
    
    sim.start();
    
    // === TEST CONNECTIONLESS COMMUNICATION ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    DefaultNetworkTypes::message_type msg(
        client_addr, client_port,
        server_addr, server_port,
        payload
    );
    
    // Send message
    auto send_future = client->send(msg);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Allow time for delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Receive message
    auto receive_future = server->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Verify message content
    BOOST_CHECK_EQUAL(received_msg.source_address(), client_addr);
    BOOST_CHECK_EQUAL(received_msg.source_port(), client_port);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), server_addr);
    BOOST_CHECK_EQUAL(received_msg.destination_port(), server_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // === TEST CONNECTION-ORIENTED COMMUNICATION ===
    
    // Server bind
    auto listener_future = server->bind(server_port);
    auto listener = std::move(listener_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    
    // Client connect
    auto connect_future = client->connect(server_addr, server_port, client_port);
    auto client_connection = std::move(connect_future).get();
    
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    
    // Server accept
    auto accept_future = listener->accept(test_timeout);
    auto server_connection = std::move(accept_future).get();
    
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    
    // Data transfer
    auto write_future = client_connection->write(payload);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);
    
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();
    
    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_message, test_message);
    
    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    
    sim.stop();
}

/**
 * Integration test for IPv4 addresses with string ports
 * Tests: custom Types using IPv4Address and std::string
 * _Requirements: 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(ipv4_string_port_types_integration, * boost::unit_test::timeout(30)) {
    NetworkSimulator<IPv4StringPortTypes> sim;
    
    // Use IPv4 addresses and string ports
    auto server_addr = create_ipv4_address(test_server_ipv4);
    auto client_addr = create_ipv4_address(test_client_ipv4);
    std::string server_port = test_server_string_port;
    std::string client_port = test_client_string_port;
    
    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(server_addr);
    sim.add_node(client_addr);
    sim.add_edge(server_addr, client_addr, edge);
    sim.add_edge(client_addr, server_addr, edge);
    
    // Verify topology with custom address types
    BOOST_CHECK(sim.has_node(server_addr));
    BOOST_CHECK(sim.has_node(client_addr));
    BOOST_CHECK(sim.has_edge(server_addr, client_addr));
    BOOST_CHECK(sim.has_edge(client_addr, server_addr));
    
    // Create nodes
    auto server = sim.create_node(server_addr);
    auto client = sim.create_node(client_addr);
    
    BOOST_REQUIRE(server != nullptr);
    BOOST_REQUIRE(client != nullptr);
    BOOST_CHECK(server->address() == server_addr);
    BOOST_CHECK(client->address() == client_addr);
    
    sim.start();
    
    // === TEST CONNECTIONLESS COMMUNICATION WITH IPv4 ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    IPv4StringPortTypes::message_type msg(
        client_addr, client_port,
        server_addr, server_port,
        payload
    );
    
    // Send message
    auto send_future = client->send(msg);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Allow time for delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Receive message
    auto receive_future = server->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Verify message content with IPv4 addresses
    BOOST_CHECK(received_msg.source_address() == client_addr);
    BOOST_CHECK_EQUAL(received_msg.source_port(), client_port);
    BOOST_CHECK(received_msg.destination_address() == server_addr);
    BOOST_CHECK_EQUAL(received_msg.destination_port(), server_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // === TEST CONNECTION-ORIENTED COMMUNICATION WITH IPv4 ===
    
    // Server bind with string port
    auto listener_future = server->bind(server_port);
    auto listener = std::move(listener_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    BOOST_CHECK(listener->local_endpoint().address == server_addr);
    BOOST_CHECK_EQUAL(listener->local_endpoint().port, server_port);
    
    // Client connect with IPv4 address and string port
    auto connect_future = client->connect(server_addr, server_port, client_port);
    auto client_connection = std::move(connect_future).get();
    
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    BOOST_CHECK(client_connection->local_endpoint().address == client_addr);
    BOOST_CHECK_EQUAL(client_connection->local_endpoint().port, client_port);
    BOOST_CHECK(client_connection->remote_endpoint().address == server_addr);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().port, server_port);
    
    // Server accept
    auto accept_future = listener->accept(test_timeout);
    auto server_connection = std::move(accept_future).get();
    
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    BOOST_CHECK(server_connection->local_endpoint().address == server_addr);
    BOOST_CHECK_EQUAL(server_connection->local_endpoint().port, server_port);
    BOOST_CHECK(server_connection->remote_endpoint().address == client_addr);
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().port, client_port);
    
    // Data transfer
    auto write_future = client_connection->write(payload);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);
    
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();
    
    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_message, test_message);
    
    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    
    sim.stop();
}

/**
 * Integration test for unsigned long addresses with unsigned short ports
 * Tests: custom Types using unsigned long and unsigned short
 * _Requirements: 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(ulong_ushort_port_types_integration, * boost::unit_test::timeout(30)) {
    NetworkSimulator<ULongUShortPortTypes> sim;
    
    // Use unsigned long addresses and unsigned short ports
    unsigned long server_addr = test_server_ulong;
    unsigned long client_addr = test_client_ulong;
    unsigned short server_port = test_server_ushort_port;
    unsigned short client_port = test_client_ushort_port;
    
    // Create topology
    NetworkEdge edge(network_latency, network_reliability);
    sim.add_node(server_addr);
    sim.add_node(client_addr);
    sim.add_edge(server_addr, client_addr, edge);
    sim.add_edge(client_addr, server_addr, edge);
    
    // Verify topology with unsigned long addresses
    BOOST_CHECK(sim.has_node(server_addr));
    BOOST_CHECK(sim.has_node(client_addr));
    BOOST_CHECK(sim.has_edge(server_addr, client_addr));
    BOOST_CHECK(sim.has_edge(client_addr, server_addr));
    
    // Create nodes
    auto server = sim.create_node(server_addr);
    auto client = sim.create_node(client_addr);
    
    BOOST_REQUIRE(server != nullptr);
    BOOST_REQUIRE(client != nullptr);
    BOOST_CHECK_EQUAL(server->address(), server_addr);
    BOOST_CHECK_EQUAL(client->address(), client_addr);
    
    sim.start();
    
    // === TEST CONNECTIONLESS COMMUNICATION WITH UNSIGNED LONG ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    ULongUShortPortTypes::message_type msg(
        client_addr, client_port,
        server_addr, server_port,
        payload
    );
    
    // Send message
    auto send_future = client->send(msg);
    bool send_success = std::move(send_future).get();
    BOOST_CHECK(send_success);
    
    // Allow time for delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Receive message
    auto receive_future = server->receive();
    auto received_msg = std::move(receive_future).get();
    
    // Verify message content with unsigned long addresses
    BOOST_CHECK_EQUAL(received_msg.source_address(), client_addr);
    BOOST_CHECK_EQUAL(received_msg.source_port(), client_port);
    BOOST_CHECK_EQUAL(received_msg.destination_address(), server_addr);
    BOOST_CHECK_EQUAL(received_msg.destination_port(), server_port);
    
    std::string received_payload;
    for (auto byte : received_msg.payload()) {
        received_payload += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_payload, test_message);
    
    // === TEST CONNECTION-ORIENTED COMMUNICATION WITH UNSIGNED LONG ===
    
    // Server bind
    auto listener_future = server->bind(server_port);
    auto listener = std::move(listener_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    BOOST_CHECK_EQUAL(listener->local_endpoint().address, server_addr);
    BOOST_CHECK_EQUAL(listener->local_endpoint().port, server_port);
    
    // Client connect
    auto connect_future = client->connect(server_addr, server_port, client_port);
    auto client_connection = std::move(connect_future).get();
    
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    BOOST_CHECK_EQUAL(client_connection->local_endpoint().address, client_addr);
    BOOST_CHECK_EQUAL(client_connection->local_endpoint().port, client_port);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().address, server_addr);
    BOOST_CHECK_EQUAL(client_connection->remote_endpoint().port, server_port);
    
    // Server accept
    auto accept_future = listener->accept(test_timeout);
    auto server_connection = std::move(accept_future).get();
    
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    BOOST_CHECK_EQUAL(server_connection->local_endpoint().address, server_addr);
    BOOST_CHECK_EQUAL(server_connection->local_endpoint().port, server_port);
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().address, client_addr);
    BOOST_CHECK_EQUAL(server_connection->remote_endpoint().port, client_port);
    
    // Data transfer
    auto write_future = client_connection->write(payload);
    bool write_success = std::move(write_future).get();
    BOOST_CHECK(write_success);
    
    auto read_future = server_connection->read(test_timeout);
    auto received_data = std::move(read_future).get();
    
    std::string received_message;
    for (auto byte : received_data) {
        received_message += static_cast<char>(byte);
    }
    BOOST_CHECK_EQUAL(received_message, test_message);
    
    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    
    sim.stop();
}

/**
 * Integration test for multiple Types implementations coexisting
 * Tests: different simulators with different Types can coexist
 * _Requirements: 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(multiple_types_coexistence, * boost::unit_test::timeout(30)) {
    // Create simulators with different Types implementations
    NetworkSimulator<DefaultNetworkTypes> default_sim;
    NetworkSimulator<IPv4StringPortTypes> ipv4_sim;
    NetworkSimulator<ULongUShortPortTypes> ulong_sim;
    
    // === SETUP DEFAULT TYPES SIMULATOR ===
    
    std::string default_server = "default_server";
    std::string default_client = "default_client";
    
    NetworkEdge edge(network_latency, network_reliability);
    default_sim.add_node(default_server);
    default_sim.add_node(default_client);
    default_sim.add_edge(default_server, default_client, edge);
    default_sim.add_edge(default_client, default_server, edge);
    
    auto default_server_node = default_sim.create_node(default_server);
    auto default_client_node = default_sim.create_node(default_client);
    
    // === SETUP IPv4 TYPES SIMULATOR ===
    
    auto ipv4_server = create_ipv4_address(test_server_ipv4);
    auto ipv4_client = create_ipv4_address(test_client_ipv4);
    
    ipv4_sim.add_node(ipv4_server);
    ipv4_sim.add_node(ipv4_client);
    ipv4_sim.add_edge(ipv4_server, ipv4_client, edge);
    ipv4_sim.add_edge(ipv4_client, ipv4_server, edge);
    
    auto ipv4_server_node = ipv4_sim.create_node(ipv4_server);
    auto ipv4_client_node = ipv4_sim.create_node(ipv4_client);
    
    // === SETUP UNSIGNED LONG TYPES SIMULATOR ===
    
    unsigned long ulong_server = test_server_ulong;
    unsigned long ulong_client = test_client_ulong;
    
    ulong_sim.add_node(ulong_server);
    ulong_sim.add_node(ulong_client);
    ulong_sim.add_edge(ulong_server, ulong_client, edge);
    ulong_sim.add_edge(ulong_client, ulong_server, edge);
    
    auto ulong_server_node = ulong_sim.create_node(ulong_server);
    auto ulong_client_node = ulong_sim.create_node(ulong_client);
    
    // === VERIFY ALL SIMULATORS WORK INDEPENDENTLY ===
    
    // Start all simulators
    default_sim.start();
    ipv4_sim.start();
    ulong_sim.start();
    
    // Verify nodes are created correctly
    BOOST_REQUIRE(default_server_node != nullptr);
    BOOST_REQUIRE(default_client_node != nullptr);
    BOOST_REQUIRE(ipv4_server_node != nullptr);
    BOOST_REQUIRE(ipv4_client_node != nullptr);
    BOOST_REQUIRE(ulong_server_node != nullptr);
    BOOST_REQUIRE(ulong_client_node != nullptr);
    
    // Verify addresses are correct for each type
    BOOST_CHECK_EQUAL(default_server_node->address(), default_server);
    BOOST_CHECK_EQUAL(default_client_node->address(), default_client);
    BOOST_CHECK(ipv4_server_node->address() == ipv4_server);
    BOOST_CHECK(ipv4_client_node->address() == ipv4_client);
    BOOST_CHECK_EQUAL(ulong_server_node->address(), ulong_server);
    BOOST_CHECK_EQUAL(ulong_client_node->address(), ulong_client);
    
    // Verify topology queries work for each type
    BOOST_CHECK(default_sim.has_node(default_server));
    BOOST_CHECK(default_sim.has_edge(default_server, default_client));
    
    BOOST_CHECK(ipv4_sim.has_node(ipv4_server));
    BOOST_CHECK(ipv4_sim.has_edge(ipv4_server, ipv4_client));
    
    BOOST_CHECK(ulong_sim.has_node(ulong_server));
    BOOST_CHECK(ulong_sim.has_edge(ulong_server, ulong_client));
    
    // === TEST SIMULTANEOUS OPERATIONS ===
    
    std::vector<std::byte> payload;
    for (char c : std::string(test_message)) {
        payload.push_back(static_cast<std::byte>(c));
    }
    
    // Send messages on all simulators simultaneously
    DefaultNetworkTypes::message_type default_msg(
        default_client, static_cast<unsigned short>(9090),
        default_server, static_cast<unsigned short>(8080),
        payload
    );
    
    IPv4StringPortTypes::message_type ipv4_msg(
        ipv4_client, test_client_string_port,
        ipv4_server, test_server_string_port,
        payload
    );
    
    ULongUShortPortTypes::message_type ulong_msg(
        ulong_client, test_client_ushort_port,
        ulong_server, test_server_ushort_port,
        payload
    );
    
    // Send all messages
    auto default_send = default_client_node->send(default_msg);
    auto ipv4_send = ipv4_client_node->send(ipv4_msg);
    auto ulong_send = ulong_client_node->send(ulong_msg);
    
    // Verify all sends succeed
    BOOST_CHECK(std::move(default_send).get());
    BOOST_CHECK(std::move(ipv4_send).get());
    BOOST_CHECK(std::move(ulong_send).get());
    
    // Stop all simulators
    default_sim.stop();
    ipv4_sim.stop();
    ulong_sim.stop();
}

/**
 * Integration test for type safety and compile-time verification
 * Tests: that different Types cannot be mixed incorrectly
 * _Requirements: 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(type_safety_verification, * boost::unit_test::timeout(30)) {
    // This test verifies that the type system prevents incorrect usage
    // Most verification happens at compile time through static_assert
    
    // Verify all custom types satisfy the concept
    static_assert(network_simulator_types<DefaultNetworkTypes>);
    static_assert(network_simulator_types<IPv4StringPortTypes>);
    static_assert(network_simulator_types<ULongUShortPortTypes>);
    
    // Verify individual type concepts
    static_assert(address<std::string>);
    static_assert(address<IPv4Address>);
    static_assert(address<unsigned long>);
    
    static_assert(port<unsigned short>);
    static_assert(port<std::string>);
    
    // Create instances to verify runtime behavior
    NetworkSimulator<DefaultNetworkTypes> default_sim;
    NetworkSimulator<IPv4StringPortTypes> ipv4_sim;
    NetworkSimulator<ULongUShortPortTypes> ulong_sim;
    
    // Verify that each simulator only accepts its own address type
    std::string string_addr = "test_node";
    auto ipv4_addr = create_ipv4_address("192.168.1.1");
    unsigned long ulong_addr = 0x12345678UL;
    
    // Add nodes with correct types
    default_sim.add_node(string_addr);
    ipv4_sim.add_node(ipv4_addr);
    ulong_sim.add_node(ulong_addr);
    
    // Verify nodes were added
    BOOST_CHECK(default_sim.has_node(string_addr));
    BOOST_CHECK(ipv4_sim.has_node(ipv4_addr));
    BOOST_CHECK(ulong_sim.has_node(ulong_addr));
    
    // Note: Attempting to add wrong types would cause compile errors:
    // default_sim.add_node(ipv4_addr);  // Compile error - wrong address type
    // ipv4_sim.add_node(string_addr);   // Compile error - wrong address type
    // ulong_sim.add_node(ipv4_addr);    // Compile error - wrong address type
    
    // This demonstrates that the type system provides compile-time safety
    BOOST_CHECK(true);  // Test passes if it compiles
}

/**
 * Integration test for edge properties with different Types
 * Tests: that edge latency and reliability work with all Types
 * _Requirements: 1.1-1.5, 2.1-2.15_
 */
BOOST_AUTO_TEST_CASE(edge_properties_with_different_types, * boost::unit_test::timeout(30)) {
    // Test different edge characteristics with different Types
    
    constexpr std::chrono::milliseconds fast_latency{5};
    constexpr std::chrono::milliseconds slow_latency{100};
    constexpr double high_reliability = 0.99;
    constexpr double low_reliability = 0.5;
    
    // === TEST WITH DEFAULT TYPES ===
    
    NetworkSimulator<DefaultNetworkTypes> default_sim;
    
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    
    default_sim.add_node(node_a);
    default_sim.add_node(node_b);
    
    NetworkEdge fast_edge(fast_latency, high_reliability);
    NetworkEdge slow_edge(slow_latency, low_reliability);
    
    default_sim.add_edge(node_a, node_b, fast_edge);
    default_sim.add_edge(node_b, node_a, slow_edge);
    
    // Verify edge properties
    auto retrieved_fast = default_sim.get_edge(node_a, node_b);
    auto retrieved_slow = default_sim.get_edge(node_b, node_a);
    
    BOOST_CHECK_EQUAL(retrieved_fast.latency(), fast_latency);
    BOOST_CHECK_EQUAL(retrieved_fast.reliability(), high_reliability);
    BOOST_CHECK_EQUAL(retrieved_slow.latency(), slow_latency);
    BOOST_CHECK_EQUAL(retrieved_slow.reliability(), low_reliability);
    
    // === TEST WITH IPv4 TYPES ===
    
    NetworkSimulator<IPv4StringPortTypes> ipv4_sim;
    
    auto ipv4_a = create_ipv4_address("10.0.0.1");
    auto ipv4_b = create_ipv4_address("10.0.0.2");
    
    ipv4_sim.add_node(ipv4_a);
    ipv4_sim.add_node(ipv4_b);
    
    ipv4_sim.add_edge(ipv4_a, ipv4_b, fast_edge);
    ipv4_sim.add_edge(ipv4_b, ipv4_a, slow_edge);
    
    // Verify edge properties with IPv4 addresses
    auto ipv4_fast = ipv4_sim.get_edge(ipv4_a, ipv4_b);
    auto ipv4_slow = ipv4_sim.get_edge(ipv4_b, ipv4_a);
    
    BOOST_CHECK_EQUAL(ipv4_fast.latency(), fast_latency);
    BOOST_CHECK_EQUAL(ipv4_fast.reliability(), high_reliability);
    BOOST_CHECK_EQUAL(ipv4_slow.latency(), slow_latency);
    BOOST_CHECK_EQUAL(ipv4_slow.reliability(), low_reliability);
    
    // === TEST WITH UNSIGNED LONG TYPES ===
    
    NetworkSimulator<ULongUShortPortTypes> ulong_sim;
    
    unsigned long ulong_a = 0x11111111UL;
    unsigned long ulong_b = 0x22222222UL;
    
    ulong_sim.add_node(ulong_a);
    ulong_sim.add_node(ulong_b);
    
    ulong_sim.add_edge(ulong_a, ulong_b, fast_edge);
    ulong_sim.add_edge(ulong_b, ulong_a, slow_edge);
    
    // Verify edge properties with unsigned long addresses
    auto ulong_fast = ulong_sim.get_edge(ulong_a, ulong_b);
    auto ulong_slow = ulong_sim.get_edge(ulong_b, ulong_a);
    
    BOOST_CHECK_EQUAL(ulong_fast.latency(), fast_latency);
    BOOST_CHECK_EQUAL(ulong_fast.reliability(), high_reliability);
    BOOST_CHECK_EQUAL(ulong_slow.latency(), slow_latency);
    BOOST_CHECK_EQUAL(ulong_slow.reliability(), low_reliability);
    
    // All Types implementations preserve edge properties correctly
}