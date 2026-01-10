#define BOOST_TEST_MODULE NetworkSimulatorTopologyEdgeLatencyPreservationPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <chrono>
#include <random>
#include <string>

using namespace network_simulator;

namespace {
    constexpr std::chrono::milliseconds min_latency{1};
    constexpr std::chrono::milliseconds max_latency{1000};
    constexpr double default_reliability = 0.99;
    constexpr std::size_t test_iterations = 100;
    constexpr const char* node_prefix = "node_";
}

/**
 * **Feature: network-simulator, Property 1: Topology Edge Latency Preservation**
 * 
 * Property: For any pair of nodes and configured latency value, when an edge is added 
 * to the topology with that latency, querying the topology SHALL return the same latency value.
 * 
 * **Validates: Requirements 1.1, 11.3, 11.6**
 */
BOOST_AUTO_TEST_CASE(network_simulator_topology_edge_latency_preservation_property_test, * boost::unit_test::timeout(60)) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate random latency values
    std::uniform_int_distribution<int> latency_dist(min_latency.count(), max_latency.count());
    
    // Generate random node addresses
    std::uniform_int_distribution<int> node_id_dist(1, 1000);
    
    for (std::size_t i = 0; i < test_iterations; ++i) {
        // Create simulator instance
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Generate random node addresses
        std::string from_node = node_prefix + std::to_string(node_id_dist(gen));
        std::string to_node = node_prefix + std::to_string(node_id_dist(gen));
        
        // Ensure nodes are different
        if (from_node == to_node) {
            to_node += "_alt";
        }
        
        // Generate random latency
        auto latency_ms = std::chrono::milliseconds(latency_dist(gen));
        
        // Create network edge with the latency
        NetworkEdge edge(latency_ms, default_reliability);
        
        // Add nodes to topology
        simulator.add_node(from_node);
        simulator.add_node(to_node);
        
        // Add edge with configured latency
        simulator.add_edge(from_node, to_node, edge);
        
        // Verify the edge exists
        BOOST_REQUIRE(simulator.has_edge(from_node, to_node));
        
        // Query the topology and verify latency preservation
        auto retrieved_edge = simulator.get_edge(from_node, to_node);
        auto retrieved_latency = retrieved_edge.latency();
        
        // Property verification: latency should be preserved exactly
        BOOST_CHECK_EQUAL(retrieved_latency.count(), latency_ms.count());
        
        // Additional verification: the edge should be queryable
        BOOST_CHECK(simulator.has_node(from_node));
        BOOST_CHECK(simulator.has_node(to_node));
    }
}

/**
 * Test edge case: Zero latency
 */
BOOST_AUTO_TEST_CASE(topology_edge_zero_latency_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "node_a";
    std::string to_node = "node_b";
    
    // Create edge with zero latency
    NetworkEdge edge(std::chrono::milliseconds(0), default_reliability);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.latency().count(), 0);
}

/**
 * Test edge case: Maximum latency
 */
BOOST_AUTO_TEST_CASE(topology_edge_maximum_latency_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "node_x";
    std::string to_node = "node_y";
    
    // Create edge with maximum latency
    auto max_latency_value = std::chrono::milliseconds(std::numeric_limits<int>::max());
    NetworkEdge edge(max_latency_value, default_reliability);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.latency().count(), max_latency_value.count());
}

/**
 * Test multiple edges with different latencies
 */
BOOST_AUTO_TEST_CASE(topology_multiple_edges_latency_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::vector<std::string> nodes = {"node_1", "node_2", "node_3", "node_4"};
    std::vector<std::chrono::milliseconds> latencies = {
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(50),
        std::chrono::milliseconds(100),
        std::chrono::milliseconds(500)
    };
    
    // Add all nodes
    for (const auto& node : nodes) {
        simulator.add_node(node);
    }
    
    // Add edges with different latencies
    for (std::size_t i = 0; i < nodes.size() - 1; ++i) {
        NetworkEdge edge(latencies[i], default_reliability);
        simulator.add_edge(nodes[i], nodes[i + 1], edge);
    }
    
    // Verify all latencies are preserved
    for (std::size_t i = 0; i < nodes.size() - 1; ++i) {
        auto retrieved_edge = simulator.get_edge(nodes[i], nodes[i + 1]);
        BOOST_CHECK_EQUAL(retrieved_edge.latency().count(), latencies[i].count());
    }
}