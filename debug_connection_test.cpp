#define BOOST_TEST_MODULE DebugConnectionTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <iostream>

#include <folly/init/Init.h>

using namespace network_simulator;

// Type alias for the correct NetworkSimulator template instantiation
using TestNetworkSimulator = NetworkSimulator<DefaultNetworkTypes>;

// Global fixture to initialize folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        init_obj = std::make_unique<folly::Init>(&argc, &argv_ptr);
    }
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> init_obj;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_CASE(debug_connection_establishment, * boost::unit_test::timeout(30)) {
    std::cout << "=== DEBUG CONNECTION ESTABLISHMENT ===" << std::endl;
    
    // Create simulator with reliable edge
    TestNetworkSimulator sim;
    sim.start();
    std::cout << "Simulator started" << std::endl;
    
    std::string addr1 = "node_1";
    std::string addr2 = "node_2";
    unsigned short src_port = 12345;
    unsigned short dst_port = 54321;
    
    NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);  // 100% reliability
    sim.add_edge(addr1, addr2, edge);
    sim.add_edge(addr2, addr1, edge);  // Bidirectional
    std::cout << "Added edges" << std::endl;
    
    // Create nodes
    auto node1 = sim.create_node(addr1);
    auto node2 = sim.create_node(addr2);
    std::cout << "Created nodes" << std::endl;
    
    try {
        // Server side: bind to destination port
        std::cout << "Binding to port " << dst_port << std::endl;
        auto listener = node2->bind(dst_port).get();
        
        if (!listener) {
            std::cout << "ERROR: Failed to create listener" << std::endl;
            BOOST_FAIL("Failed to create listener");
        }
        
        if (!listener->is_listening()) {
            std::cout << "ERROR: Listener is not listening" << std::endl;
            BOOST_FAIL("Listener is not listening");
        }
        
        std::cout << "Listener created and listening" << std::endl;
        
        // Client side: establish connection from node1 to node2
        std::cout << "Connecting from " << addr1 << ":" << src_port << " to " << addr2 << ":" << dst_port << std::endl;
        auto client_connection = node1->connect(addr2, dst_port, src_port).get();
        
        if (!client_connection) {
            std::cout << "ERROR: Failed to create client connection" << std::endl;
            BOOST_FAIL("Failed to create client connection");
        }
        
        std::cout << "Client connection created successfully" << std::endl;
        
        // Small delay to allow connection establishment to complete
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        
        // Server side: accept the connection
        std::cout << "Accepting connection with 1000ms timeout..." << std::endl;
        auto server_connection = listener->accept(std::chrono::milliseconds{1000}).get();
        
        if (!server_connection) {
            std::cout << "ERROR: Failed to accept server connection" << std::endl;
            BOOST_FAIL("Failed to accept server connection");
        }
        
        std::cout << "Server connection accepted successfully" << std::endl;
        std::cout << "SUCCESS: Connection establishment works!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "ERROR: Exception: " << e.what() << std::endl;
        BOOST_FAIL("Exception during connection establishment: " + std::string(e.what()));
    }
}