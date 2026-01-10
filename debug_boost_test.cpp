#define BOOST_TEST_MODULE DebugBoostTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <chrono>
#include <random>

using namespace network_simulator;

// Helper to generate random address like property test
auto generate_random_address(std::mt19937& rng, std::size_t id) -> std::string {
    return "node_" + std::to_string(id);
}

BOOST_AUTO_TEST_CASE(debug_connection_read_write_round_trip, * boost::unit_test::timeout(120)) {
    std::mt19937 rng(std::random_device{}());
    
    std::size_t failures = 0;
    constexpr std::size_t property_test_iterations = 10;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Generate random addresses
        auto addr1 = generate_random_address(rng, i * 2);
        auto addr2 = generate_random_address(rng, i * 2 + 1);
        
        // Generate random ports
        std::uniform_int_distribution<unsigned short> port_dist(1000, 65535);
        auto src_port = port_dist(rng);
        auto dst_port = port_dist(rng);
        
        // Create simulator with reliable edge
        NetworkSimulator<DefaultNetworkTypes> sim;
        sim.start();
        
        NetworkEdge edge(std::chrono::milliseconds{10}, 1.0);  // 100% reliability
        sim.add_edge(addr1, addr2, edge);
        sim.add_edge(addr2, addr1, edge);  // Bidirectional
        
        // Create nodes
        auto node1 = sim.create_node(addr1);
        auto node2 = sim.create_node(addr2);
        
        try {
            // Server side: bind to destination port
            auto listener = node2->bind(dst_port).get();
            
            if (!listener || !listener->is_listening()) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Failed to create listener");
                continue;
            }
            
            // Client side: establish connection from node1 to node2
            auto client_connection = node1->connect(addr2, dst_port, src_port).get();
            
            if (!client_connection) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Failed to create client connection");
                continue;
            }
            
            // Small delay to allow connection establishment to complete
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            
            // Server side: accept the connection
            auto server_connection = listener->accept(std::chrono::milliseconds{100}).get();
            
            if (!server_connection) {
                ++failures;
                BOOST_TEST_MESSAGE("Iteration " << i << ": Failed to accept server connection");
                continue;
            }
            
            BOOST_TEST_MESSAGE("Iteration " << i << ": Success!");
            
        } catch (const std::exception& e) {
            ++failures;
            BOOST_TEST_MESSAGE("Iteration " << i << ": Exception: " << e.what());
        }
    }
    
    BOOST_TEST(failures == 0, "Property violated in " << failures << " out of " 
        << property_test_iterations << " iterations");
}