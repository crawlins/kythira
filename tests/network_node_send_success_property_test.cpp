#define BOOST_TEST_MODULE NetworkNodeSendSuccessPropertyTest
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
    constexpr double test_reliability = 1.0; // 100% reliability for success test
}

BOOST_AUTO_TEST_CASE(network_node_send_success_property_test, * boost::unit_test::timeout(30)) {
    // **Property 6: Send Success Result**
    // **Validates: Requirements 4.2**
    
    // For any message that is accepted by the network simulator for transmission,
    // the send operation SHALL return a future that resolves to true.
    
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
    
    // Test property: For any valid message, send should return true
    for (int i = 0; i < 10; ++i) {
        // Create payload as vector of bytes
        std::vector<std::byte> payload;
        for (char c : std::string(test_payload)) {
            payload.push_back(static_cast<std::byte>(c));
        }
        
        // Create message from node_a to node_b
        Message<DefaultNetworkTypes> msg(
            test_node_a,
            static_cast<unsigned short>(8000 + i),
            test_node_b,
            static_cast<unsigned short>(9000 + i),
            std::move(payload)
        );
        
        // Send message
        auto send_future = node_a->send(std::move(msg));
        
        // Property: Send operation should succeed (return true) for valid messages
        // when the network accepts them for transmission
        bool send_result = send_future.get();
        BOOST_CHECK(send_result == true);
    }
    
    simulator.stop();
}