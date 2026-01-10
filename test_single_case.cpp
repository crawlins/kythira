#define BOOST_TEST_MODULE SingleTestCase
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <network_simulator/exceptions.hpp>

#include <chrono>
#include <future>

using namespace network_simulator;

namespace {
    constexpr const char* client_node_id = "client";
    constexpr const char* server_node_id = "server";
    constexpr unsigned short server_port = 8080;
    constexpr unsigned short client_port = 9090;
    constexpr std::chrono::milliseconds network_latency{10};
    constexpr double network_reliability = 1.0;
    constexpr std::chrono::seconds test_timeout{10};
    constexpr const char* test_message = "Hello, Server!";
}

BOOST_AUTO_TEST_CASE(test_accept_connect_order, * boost::unit_test::timeout(30)) {
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
    
    // Start simulation
    sim.start();
    
    // Server: bind to port and create listener
    auto listener_future = server->bind(server_port);
    auto listener = std::move(listener_future).get();
    
    BOOST_REQUIRE(listener != nullptr);
    BOOST_CHECK(listener->is_listening());
    
    // === CLIENT CONNECTION AND SERVER ACCEPT (CONCURRENT) ===
    std::cout << "Starting accept operation..." << std::endl;
    
    // Start connection establishment immediately using std::async
    auto connect_future = std::async(std::launch::async, [&client]() {
        // Small delay to ensure accept starts first
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        std::cout << "[ASYNC] Starting connection..." << std::endl;
        auto client_connection_future = client->connect(server_node_id, server_port, client_port);
        auto result = std::move(client_connection_future).get();
        std::cout << "[ASYNC] Connection completed" << std::endl;
        return result;
    });
    
    // Start accept operation immediately after starting the async task
    auto accept_future = listener->accept(test_timeout);
    
    // Wait for both operations to complete
    std::cout << "Waiting for connection..." << std::endl;
    auto client_connection = connect_future.get();
    
    std::cout << "Getting server connection from accept..." << std::endl;
    auto server_connection = std::move(accept_future).get();
    std::cout << "Accept completed" << std::endl;
    
    // Verify both connections are established correctly
    BOOST_REQUIRE(client_connection != nullptr);
    BOOST_CHECK(client_connection->is_open());
    BOOST_REQUIRE(server_connection != nullptr);
    BOOST_CHECK(server_connection->is_open());
    
    // Test data transfer
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
    
    // Cleanup
    client_connection->close();
    server_connection->close();
    listener->close();
    
    sim.stop();
}