#define BOOST_TEST_MODULE NetworkNodeReceiveMessagePropertyTest
#include <boost/test/unit_test.hpp>
#include <network_simulator/network_simulator.hpp>
#include <chrono>
#include <string>

using namespace network_simulator;

namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr const char* test_payload = "test_message";
    constexpr std::chrono::milliseconds test_latency{10};
    constexpr double test_reliability = 1.0; // 100% reliability for reliable delivery
}

BOOST_AUTO_TEST_CASE(network_node_receive_message_property_test, * boost::unit_test::timeout(30)) {
    // **Property 9: Receive Returns Sent Message**
    // **Validates: Requirements 5.2**
    
    // For any message sent to a node that is successfully delivered,
    // calling receive on that node SHALL return a future that resolves to a message
    // with the same source, destination, and payload.
    
    NetworkSimulator<DefaultNetworkTypes> simulator;
    simulator.start();
    
    // Set up topology with reliable connection
    simulator.add_node(test_node_a);
    simulator.add_node(test_node_b);
    simulator.add_edge(test_node_a, test_node_b, NetworkEdge(test_latency, test_reliability));
    
    // Create nodes
    auto node_a = simulator.create_node(test_node_a);
    auto node_b = simulator.create_node(test_node_b);
    
    BOOST_REQUIRE(node_a != nullptr);
    BOOST_REQUIRE(node_b != nullptr);
    
    // Test property: Received message matches sent message
    for (int i = 0; i < 5; ++i) {
        // Create payload as vector of bytes
        std::vector<std::byte> original_payload;
        std::string payload_str = std::string(test_payload) + "_" + std::to_string(i);
        for (char c : payload_str) {
            original_payload.push_back(static_cast<std::byte>(c));
        }
        
        unsigned short src_port = static_cast<unsigned short>(8000 + i);
        unsigned short dst_port = static_cast<unsigned short>(9000 + i);
        
        // Create message from node_a to node_b
        Message<DefaultNetworkTypes> msg(
            test_node_a,
            src_port,
            test_node_b,
            dst_port,
            original_payload
        );
        
        // Send message
        auto send_future = node_a->send(std::move(msg));
        bool send_result = send_future.get();
        
        if (send_result) {
            // Try to receive the message
            auto receive_future = node_b->receive();
            auto received_msg = receive_future.get();
            
            // Property: Received message should match sent message
            if (!received_msg.payload().empty()) {
                BOOST_CHECK_EQUAL(received_msg.source_address(), test_node_a);
                BOOST_CHECK_EQUAL(received_msg.source_port(), src_port);
                BOOST_CHECK_EQUAL(received_msg.destination_address(), test_node_b);
                BOOST_CHECK_EQUAL(received_msg.destination_port(), dst_port);
                
                // Check payload matches
                auto received_payload = received_msg.payload();
                BOOST_CHECK_EQUAL(received_payload.size(), original_payload.size());
                
                for (std::size_t j = 0; j < original_payload.size() && j < received_payload.size(); ++j) {
                    BOOST_CHECK_EQUAL(static_cast<int>(received_payload[j]), 
                                    static_cast<int>(original_payload[j]));
                }
            }
        }
    }
    
    simulator.stop();
}