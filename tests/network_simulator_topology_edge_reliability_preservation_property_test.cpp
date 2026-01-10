#define BOOST_TEST_MODULE NetworkSimulatorTopologyEdgeReliabilityPreservationPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <chrono>
#include <random>
#include <string>

using namespace network_simulator;

namespace {
    constexpr std::chrono::milliseconds default_latency{10};
    constexpr double min_reliability = 0.0;
    constexpr double max_reliability = 1.0;
    constexpr std::size_t test_iterations = 100;
    constexpr const char* node_prefix = "node_";
}

/**
 * **Feature: network-simulator, Property 2: Topology Edge Reliability Preservation**
 * 
 * Property: For any pair of nodes and configured reliability value, when an edge is added 
 * to the topology with that reliability, querying the topology SHALL return the same reliability value.
 * 
 * **Validates: Requirements 1.2, 11.3, 11.6**
 */
BOOST_AUTO_TEST_CASE(network_simulator_topology_edge_reliability_preservation_property_test, * boost::unit_test::timeout(60)) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate random reliability values between 0.0 and 1.0
    std::uniform_real_distribution<double> reliability_dist(min_reliability, max_reliability);
    
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
        
        // Generate random reliability
        double reliability = reliability_dist(gen);
        
        // Create network edge with the reliability
        NetworkEdge edge(default_latency, reliability);
        
        // Add nodes to topology
        simulator.add_node(from_node);
        simulator.add_node(to_node);
        
        // Add edge with configured reliability
        simulator.add_edge(from_node, to_node, edge);
        
        // Verify the edge exists
        BOOST_REQUIRE(simulator.has_edge(from_node, to_node));
        
        // Query the topology and verify reliability preservation
        auto retrieved_edge = simulator.get_edge(from_node, to_node);
        double retrieved_reliability = retrieved_edge.reliability();
        
        // Property verification: reliability should be preserved exactly
        BOOST_CHECK_EQUAL(retrieved_reliability, reliability);
        
        // Additional verification: the edge should be queryable
        BOOST_CHECK(simulator.has_node(from_node));
        BOOST_CHECK(simulator.has_node(to_node));
    }
}

/**
 * Test edge case: Zero reliability (always fails)
 */
BOOST_AUTO_TEST_CASE(topology_edge_zero_reliability_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "node_a";
    std::string to_node = "node_b";
    
    // Create edge with zero reliability
    NetworkEdge edge(default_latency, 0.0);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.reliability(), 0.0);
}

/**
 * Test edge case: Perfect reliability (always succeeds)
 */
BOOST_AUTO_TEST_CASE(topology_edge_perfect_reliability_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "node_x";
    std::string to_node = "node_y";
    
    // Create edge with perfect reliability
    NetworkEdge edge(default_latency, 1.0);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.reliability(), 1.0);
}

/**
 * Test multiple edges with different reliabilities
 */
BOOST_AUTO_TEST_CASE(topology_multiple_edges_reliability_preservation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::vector<std::string> nodes = {"node_1", "node_2", "node_3", "node_4"};
    std::vector<double> reliabilities = {0.1, 0.5, 0.9, 0.99};
    
    // Add all nodes
    for (const auto& node : nodes) {
        simulator.add_node(node);
    }
    
    // Add edges with different reliabilities
    for (std::size_t i = 0; i < nodes.size() - 1; ++i) {
        NetworkEdge edge(default_latency, reliabilities[i]);
        simulator.add_edge(nodes[i], nodes[i + 1], edge);
    }
    
    // Verify all reliabilities are preserved
    for (std::size_t i = 0; i < nodes.size() - 1; ++i) {
        auto retrieved_edge = simulator.get_edge(nodes[i], nodes[i + 1]);
        BOOST_CHECK_EQUAL(retrieved_edge.reliability(), reliabilities[i]);
    }
}

/**
 * Test precision preservation for small reliability values
 */
BOOST_AUTO_TEST_CASE(topology_edge_small_reliability_precision, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "precision_from";
    std::string to_node = "precision_to";
    
    // Test very small reliability value
    double small_reliability = 0.001;
    NetworkEdge edge(default_latency, small_reliability);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.reliability(), small_reliability);
}

/**
 * Test precision preservation for reliability values close to 1.0
 */
BOOST_AUTO_TEST_CASE(topology_edge_high_reliability_precision, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "high_from";
    std::string to_node = "high_to";
    
    // Test reliability value very close to 1.0
    double high_reliability = 0.999999;
    NetworkEdge edge(default_latency, high_reliability);
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    simulator.add_edge(from_node, to_node, edge);
    
    auto retrieved_edge = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge.reliability(), high_reliability);
}