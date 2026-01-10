#define BOOST_TEST_MODULE DebugAcceptTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <chrono>

using namespace network_simulator;

BOOST_AUTO_TEST_CASE(debug_accept_connection, * boost::unit_test::timeout(30)) {
    // Create simulator
    NetworkSimulator<DefaultNetworkTypes> sim;
    sim.start();
    
    // Set up topology
    std::string addr1 = "node1";
    std::string addr2 = "node2";
    unsigned short src_port = 1234;
    unsigned short dst_port = 5678;
    
    NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);
    sim.add_edge(addr1, addr2, edge);
    sim.add_edge(addr2, addr1, edge);
    
    // Create nodes
    auto node1 = sim.create_node(addr1);
    auto node2 = sim.create_node(addr2);
    
    BOOST_TEST_MESSAGE("Created nodes");
    
    // Server side: bind to destination port
    auto listener = node2->bind(dst_port).get();
    
    BOOST_REQUIRE(listener);
    BOOST_REQUIRE(listener->is_listening());
    
    BOOST_TEST_MESSAGE("Created listener");
    
    // Client side: establish connection
    BOOST_TEST_MESSAGE("Establishing connection...");
    auto client_connection = node1->connect(addr2, dst_port, src_port).get();
    
    BOOST_REQUIRE(client_connection);
    
    BOOST_TEST_MESSAGE("Created client connection");
    
    // Small delay to allow connection establishment to complete
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    // Server side: accept the connection
    BOOST_TEST_MESSAGE("Accepting connection...");
    auto server_connection = listener->accept(std::chrono::milliseconds{1000}).get();
    
    BOOST_REQUIRE(server_connection);
    
    BOOST_TEST_MESSAGE("Accepted server connection successfully!");
}