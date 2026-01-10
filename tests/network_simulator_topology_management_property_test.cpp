#define BOOST_TEST_MODULE NetworkSimulatorTopologyManagementPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <set>

using namespace network_simulator;

namespace {
    constexpr std::chrono::milliseconds default_latency{50};
    constexpr double default_reliability = 0.99;
    constexpr std::size_t test_iterations = 100;
    constexpr const char* node_prefix = "node_";
    constexpr std::size_t max_nodes_per_test = 20;
    constexpr std::size_t max_edges_per_test = 50;
}

/**
 * **Feature: network-simulator, Property 22: Topology Management Operations**
 * 
 * Property: For any node or edge added to the topology, the topology query methods 
 * SHALL reflect the addition, and for any node or edge removed, the query methods 
 * SHALL reflect the removal.
 * 
 * **Validates: Requirements 11.1, 11.2, 11.4, 11.5**
 */
BOOST_AUTO_TEST_CASE(network_simulator_topology_management_property_test, * boost::unit_test::timeout(120)) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate random number of nodes and edges for each test iteration
    std::uniform_int_distribution<std::size_t> node_count_dist(2, max_nodes_per_test);
    std::uniform_int_distribution<std::size_t> edge_count_dist(1, max_edges_per_test);
    std::uniform_int_distribution<int> node_id_dist(1, 10000);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        // Create simulator instance
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Generate random number of nodes for this iteration
        std::size_t node_count = node_count_dist(gen);
        std::vector<std::string> nodes;
        std::set<std::string> node_set; // For uniqueness checking
        
        // Generate unique node addresses
        for (std::size_t i = 0; i < node_count; ++i) {
            std::string node_addr;
            do {
                node_addr = node_prefix + std::to_string(node_id_dist(gen));
            } while (node_set.find(node_addr) != node_set.end());
            
            nodes.push_back(node_addr);
            node_set.insert(node_addr);
        }
        
        // Test node addition - Property: added nodes should be queryable
        for (const auto& node : nodes) {
            // Initially, node should not exist
            BOOST_CHECK(!simulator.has_node(node));
            
            // Add the node
            simulator.add_node(node);
            
            // After addition, node should exist
            BOOST_CHECK(simulator.has_node(node));
        }
        
        // Generate random edges between existing nodes
        std::size_t edge_count = std::min(edge_count_dist(gen), node_count * (node_count - 1));
        std::vector<std::pair<std::string, std::string>> edges;
        std::set<std::pair<std::string, std::string>> edge_set; // For uniqueness
        
        std::uniform_int_distribution<std::size_t> node_index_dist(0, nodes.size() - 1);
        
        for (std::size_t i = 0; i < edge_count; ++i) {
            std::string from_node, to_node;
            std::pair<std::string, std::string> edge_pair;
            
            // Generate unique edge (avoid self-loops and duplicates)
            do {
                from_node = nodes[node_index_dist(gen)];
                to_node = nodes[node_index_dist(gen)];
                edge_pair = std::make_pair(from_node, to_node);
            } while (from_node == to_node || edge_set.find(edge_pair) != edge_set.end());
            
            edges.push_back(edge_pair);
            edge_set.insert(edge_pair);
        }
        
        // Test edge addition - Property: added edges should be queryable
        for (const auto& [from_node, to_node] : edges) {
            // Initially, edge should not exist
            BOOST_CHECK(!simulator.has_edge(from_node, to_node));
            
            // Add the edge
            NetworkEdge edge(default_latency, default_reliability);
            simulator.add_edge(from_node, to_node, edge);
            
            // After addition, edge should exist and be queryable
            BOOST_CHECK(simulator.has_edge(from_node, to_node));
            
            // Edge properties should be preserved
            auto retrieved_edge = simulator.get_edge(from_node, to_node);
            BOOST_CHECK_EQUAL(retrieved_edge.latency().count(), default_latency.count());
            BOOST_CHECK_EQUAL(retrieved_edge.reliability(), default_reliability);
        }
        
        // Test edge removal - Property: removed edges should not be queryable
        for (const auto& [from_node, to_node] : edges) {
            // Edge should exist before removal
            BOOST_CHECK(simulator.has_edge(from_node, to_node));
            
            // Remove the edge
            simulator.remove_edge(from_node, to_node);
            
            // After removal, edge should not exist
            BOOST_CHECK(!simulator.has_edge(from_node, to_node));
        }
        
        // Test node removal - Property: removed nodes and associated edges should not be queryable
        for (const auto& node : nodes) {
            // Node should exist before removal
            BOOST_CHECK(simulator.has_node(node));
            
            // Remove the node
            simulator.remove_node(node);
            
            // After removal, node should not exist
            BOOST_CHECK(!simulator.has_node(node));
        }
        
        // Verify all nodes are removed
        for (const auto& node : nodes) {
            BOOST_CHECK(!simulator.has_node(node));
        }
    }
}

/**
 * Test edge case: Adding duplicate nodes
 */
BOOST_AUTO_TEST_CASE(topology_duplicate_node_addition, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string node_addr = "duplicate_node";
    
    // Add node first time
    simulator.add_node(node_addr);
    BOOST_CHECK(simulator.has_node(node_addr));
    
    // Add same node again - should not cause issues
    simulator.add_node(node_addr);
    BOOST_CHECK(simulator.has_node(node_addr));
}

/**
 * Test edge case: Adding duplicate edges
 */
BOOST_AUTO_TEST_CASE(topology_duplicate_edge_addition, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string from_node = "node_a";
    std::string to_node = "node_b";
    
    simulator.add_node(from_node);
    simulator.add_node(to_node);
    
    NetworkEdge edge1(std::chrono::milliseconds(10), 0.9);
    NetworkEdge edge2(std::chrono::milliseconds(20), 0.8);
    
    // Add edge first time
    simulator.add_edge(from_node, to_node, edge1);
    BOOST_CHECK(simulator.has_edge(from_node, to_node));
    
    auto retrieved_edge1 = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge1.latency().count(), 10);
    BOOST_CHECK_EQUAL(retrieved_edge1.reliability(), 0.9);
    
    // Add same edge again with different properties - should update
    simulator.add_edge(from_node, to_node, edge2);
    BOOST_CHECK(simulator.has_edge(from_node, to_node));
    
    auto retrieved_edge2 = simulator.get_edge(from_node, to_node);
    BOOST_CHECK_EQUAL(retrieved_edge2.latency().count(), 20);
    BOOST_CHECK_EQUAL(retrieved_edge2.reliability(), 0.8);
}

/**
 * Test edge case: Removing non-existent nodes and edges
 */
BOOST_AUTO_TEST_CASE(topology_remove_non_existent, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string non_existent_node = "non_existent";
    std::string from_node = "node_a";
    std::string to_node = "node_b";
    
    // Removing non-existent node should not cause issues
    simulator.remove_node(non_existent_node);
    BOOST_CHECK(!simulator.has_node(non_existent_node));
    
    // Removing non-existent edge should not cause issues
    simulator.remove_edge(from_node, to_node);
    BOOST_CHECK(!simulator.has_edge(from_node, to_node));
}

/**
 * Test node removal cascades to edge removal
 */
BOOST_AUTO_TEST_CASE(topology_node_removal_cascades_edges, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    std::string node_c = "node_c";
    
    // Add nodes
    simulator.add_node(node_a);
    simulator.add_node(node_b);
    simulator.add_node(node_c);
    
    // Add edges involving node_a
    NetworkEdge edge(default_latency, default_reliability);
    simulator.add_edge(node_a, node_b, edge);
    simulator.add_edge(node_b, node_a, edge);
    simulator.add_edge(node_a, node_c, edge);
    simulator.add_edge(node_c, node_a, edge);
    
    // Verify edges exist
    BOOST_CHECK(simulator.has_edge(node_a, node_b));
    BOOST_CHECK(simulator.has_edge(node_b, node_a));
    BOOST_CHECK(simulator.has_edge(node_a, node_c));
    BOOST_CHECK(simulator.has_edge(node_c, node_a));
    
    // Remove node_a
    simulator.remove_node(node_a);
    
    // Verify node_a is removed
    BOOST_CHECK(!simulator.has_node(node_a));
    
    // Verify all edges involving node_a are removed
    BOOST_CHECK(!simulator.has_edge(node_a, node_b));
    BOOST_CHECK(!simulator.has_edge(node_b, node_a));
    BOOST_CHECK(!simulator.has_edge(node_a, node_c));
    BOOST_CHECK(!simulator.has_edge(node_c, node_a));
    
    // Verify other nodes still exist
    BOOST_CHECK(simulator.has_node(node_b));
    BOOST_CHECK(simulator.has_node(node_c));
}

/**
 * Test large topology operations
 */
BOOST_AUTO_TEST_CASE(topology_large_scale_operations, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    constexpr std::size_t large_node_count = 100;
    std::vector<std::string> nodes;
    
    // Add many nodes
    for (std::size_t i = 0; i < large_node_count; ++i) {
        std::string node_addr = node_prefix + std::to_string(i);
        nodes.push_back(node_addr);
        simulator.add_node(node_addr);
        BOOST_CHECK(simulator.has_node(node_addr));
    }
    
    // Add edges in a ring topology
    NetworkEdge edge(default_latency, default_reliability);
    for (std::size_t i = 0; i < large_node_count; ++i) {
        std::size_t next_i = (i + 1) % large_node_count;
        simulator.add_edge(nodes[i], nodes[next_i], edge);
        BOOST_CHECK(simulator.has_edge(nodes[i], nodes[next_i]));
    }
    
    // Remove every other node
    for (std::size_t i = 0; i < large_node_count; i += 2) {
        simulator.remove_node(nodes[i]);
        BOOST_CHECK(!simulator.has_node(nodes[i]));
    }
    
    // Verify remaining nodes still exist
    for (std::size_t i = 1; i < large_node_count; i += 2) {
        BOOST_CHECK(simulator.has_node(nodes[i]));
    }
}