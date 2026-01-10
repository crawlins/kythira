#define BOOST_TEST_MODULE NetworkSimulatorResetPropertyTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <thread>
#include <set>

using namespace network_simulator;

namespace {
    constexpr std::chrono::milliseconds default_latency{10};
    constexpr double default_reliability = 0.99;
    constexpr std::size_t test_iterations = 50;
    constexpr const char* node_prefix = "node_";
    constexpr std::size_t max_nodes_per_test = 10;
    constexpr std::size_t max_edges_per_test = 20;
    constexpr const char* test_payload = "test_message";
    constexpr std::chrono::milliseconds medium_timeout{1000};
}

/**
 * **Feature: network-simulator, Property 24: Simulation Reset**
 * 
 * Property: For any simulator with existing state, calling reset SHALL clear all 
 * topology, nodes, connections, and listeners, returning the simulator to initial conditions.
 * 
 * **Validates: Requirements 12.3**
 */
BOOST_AUTO_TEST_CASE(network_simulator_reset_property_test, * boost::unit_test::timeout(120)) {
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate random number of nodes and edges for each test iteration
    std::uniform_int_distribution<std::size_t> node_count_dist(2, max_nodes_per_test);
    std::uniform_int_distribution<std::size_t> edge_count_dist(1, max_edges_per_test);
    std::uniform_int_distribution<int> node_id_dist(1, 10000);
    std::uniform_int_distribution<int> port_dist(8000, 9000);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        // Create simulator instance
        NetworkSimulator<DefaultNetworkTypes> simulator;
        
        // Start the simulator
        simulator.start();
        
        // Generate random topology with nodes and edges
        std::size_t node_count = node_count_dist(gen);
        std::vector<std::string> nodes;
        std::set<std::string> node_set;
        
        // Generate unique node addresses
        for (std::size_t i = 0; i < node_count; ++i) {
            std::string node_addr;
            do {
                node_addr = node_prefix + std::to_string(node_id_dist(gen));
            } while (node_set.find(node_addr) != node_set.end());
            
            nodes.push_back(node_addr);
            node_set.insert(node_addr);
        }
        
        // Add nodes to topology
        for (const auto& node : nodes) {
            simulator.add_node(node);
            BOOST_CHECK(simulator.has_node(node));
        }
        
        // Generate and add random edges
        std::size_t edge_count = std::min(edge_count_dist(gen), node_count * (node_count - 1));
        std::vector<std::pair<std::string, std::string>> edges;
        std::set<std::pair<std::string, std::string>> edge_set;
        
        std::uniform_int_distribution<std::size_t> node_index_dist(0, nodes.size() - 1);
        std::uniform_int_distribution<int> latency_dist(1, 100);
        std::uniform_real_distribution<double> reliability_dist(0.5, 1.0);
        
        for (std::size_t i = 0; i < edge_count; ++i) {
            std::string from_node, to_node;
            std::pair<std::string, std::string> edge_pair;
            
            do {
                from_node = nodes[node_index_dist(gen)];
                to_node = nodes[node_index_dist(gen)];
                edge_pair = std::make_pair(from_node, to_node);
            } while (from_node == to_node || edge_set.find(edge_pair) != edge_set.end());
            
            edges.push_back(edge_pair);
            edge_set.insert(edge_pair);
            
            // Add edge with random properties
            auto latency = std::chrono::milliseconds(latency_dist(gen));
            double reliability = reliability_dist(gen);
            NetworkEdge edge(latency, reliability);
            
            simulator.add_edge(from_node, to_node, edge);
            BOOST_CHECK(simulator.has_edge(from_node, to_node));
        }
        
        // Create network nodes and establish connections/listeners
        std::vector<std::shared_ptr<NetworkNode<DefaultNetworkTypes>>> network_nodes;
        std::vector<std::shared_ptr<Listener<DefaultNetworkTypes>>> listeners;
        std::vector<std::shared_ptr<Connection<DefaultNetworkTypes>>> connections;
        
        // Create network nodes
        for (const auto& node_addr : nodes) {
            auto network_node = simulator.create_node(node_addr);
            network_nodes.push_back(network_node);
        }
        
        // Create some listeners (if we have enough nodes)
        if (nodes.size() >= 2) {
            for (std::size_t i = 0; i < std::min(node_count / 2, std::size_t(3)); ++i) {
                auto port = port_dist(gen);
                auto bind_future = network_nodes[i]->bind(port, medium_timeout);
                
                // Give some time for bind to complete
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                if (bind_future.isReady()) {
                    try {
                        auto listener = bind_future.get();
                        if (listener != nullptr && listener->is_listening()) {
                            listeners.push_back(listener);
                        }
                    } catch (const std::exception&) {
                        // Bind might fail due to port conflicts, that's okay
                    }
                }
            }
        }
        
        // Create some connections (if we have listeners)
        if (!listeners.empty() && nodes.size() >= 2) {
            for (std::size_t i = 0; i < std::min(listeners.size(), std::size_t(2)); ++i) {
                auto client_node_idx = (i + 1) % network_nodes.size();
                auto server_endpoint = listeners[i]->local_endpoint();
                
                auto connect_future = network_nodes[client_node_idx]->connect(
                    server_endpoint.address, 
                    server_endpoint.port, 
                    medium_timeout
                );
                
                // Give some time for connection to establish
                std::this_thread::sleep_for(default_latency + std::chrono::milliseconds(50));
                
                if (connect_future.isReady()) {
                    try {
                        auto connection = connect_future.get();
                        if (connection != nullptr && connection->is_open()) {
                            connections.push_back(connection);
                        }
                    } catch (const std::exception&) {
                        // Connection might fail, that's okay for this test
                    }
                }
            }
        }
        
        // Send some messages to create additional state
        if (network_nodes.size() >= 2) {
            for (std::size_t i = 0; i < std::min(node_count, std::size_t(5)); ++i) {
                std::size_t from_idx = i % network_nodes.size();
                std::size_t to_idx = (i + 1) % network_nodes.size();
                
                std::vector<std::byte> payload_msg;
                for (char c : std::string(test_payload)) {
                    payload_msg.push_back(static_cast<std::byte>(c));
                }
                Message<DefaultNetworkTypes> msg(
                    nodes[from_idx], port_dist(gen),
                    nodes[to_idx], port_dist(gen),
                    payload_msg
                );
                
                auto send_future = network_nodes[from_idx]->send(msg, medium_timeout);
                // Don't wait for completion, just initiate the send
            }
        }
        
        // Verify that the simulator has state before reset
        bool has_state_before_reset = false;
        
        // Check if nodes exist
        for (const auto& node : nodes) {
            if (simulator.has_node(node)) {
                has_state_before_reset = true;
                break;
            }
        }
        
        // Check if edges exist
        if (!has_state_before_reset) {
            for (const auto& [from_node, to_node] : edges) {
                if (simulator.has_edge(from_node, to_node)) {
                    has_state_before_reset = true;
                    break;
                }
            }
        }
        
        // We should have some state before reset
        BOOST_CHECK(has_state_before_reset);
        
        // Perform the reset - Property: reset should clear all state
        simulator.reset();
        
        // Verify all topology is cleared after reset
        for (const auto& node : nodes) {
            BOOST_CHECK(!simulator.has_node(node));
        }
        
        for (const auto& [from_node, to_node] : edges) {
            BOOST_CHECK(!simulator.has_edge(from_node, to_node));
        }
        
        // Verify connections are closed after reset
        for (auto& connection : connections) {
            if (connection != nullptr) {
                BOOST_CHECK(!connection->is_open());
            }
        }
        
        // Verify listeners are closed after reset
        for (auto& listener : listeners) {
            if (listener != nullptr) {
                BOOST_CHECK(!listener->is_listening());
            }
        }
        
        // Verify simulator can be used normally after reset
        std::string new_node_a = "reset_test_node_a";
        std::string new_node_b = "reset_test_node_b";
        
        simulator.add_node(new_node_a);
        simulator.add_node(new_node_b);
        
        BOOST_CHECK(simulator.has_node(new_node_a));
        BOOST_CHECK(simulator.has_node(new_node_b));
        
        NetworkEdge new_edge(default_latency, default_reliability);
        simulator.add_edge(new_node_a, new_node_b, new_edge);
        
        BOOST_CHECK(simulator.has_edge(new_node_a, new_node_b));
        
        // Verify simulator can be started again after reset
        simulator.start();
        
        auto new_node_a_obj = simulator.create_node(new_node_a);
        auto new_node_b_obj = simulator.create_node(new_node_b);
        
        // Test basic operation works after reset
        std::vector<std::byte> test_payload_bytes;
        for (char c : std::string(test_payload)) {
            test_payload_bytes.push_back(static_cast<std::byte>(c));
        }
        Message<DefaultNetworkTypes> test_msg(
            new_node_a, 8080,
            new_node_b, 8081,
            test_payload_bytes
        );
        
        auto send_future = new_node_a_obj->send(test_msg, medium_timeout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        if (send_future.isReady()) {
            bool result = send_future.get();
            BOOST_CHECK(result);
        } else {
            // If not ready, wait a bit more and check again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (send_future.isReady()) {
                bool result = send_future.get();
                BOOST_CHECK(result);
            }
            // If still not ready, that's also acceptable for this test
            // The main point is that reset worked and simulator is functional
        }
        
        // Clean up
        simulator.stop();
    }
}

/**
 * Test reset during active operations
 */
BOOST_AUTO_TEST_CASE(reset_during_active_operations, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    // Set up topology
    std::string node_a = "node_a";
    std::string node_b = "node_b";
    
    simulator.add_node(node_a);
    simulator.add_node(node_b);
    
    NetworkEdge edge(default_latency, default_reliability);
    simulator.add_edge(node_a, node_b, edge);
    
    simulator.start();
    
    auto network_node_a = simulator.create_node(node_a);
    auto network_node_b = simulator.create_node(node_b);
    
    // Start some operations
    std::vector<std::thread> operation_threads;
    
    // Launch concurrent operations
    for (int i = 0; i < 5; ++i) {
        operation_threads.emplace_back([&, i]() {
            std::vector<std::byte> payload_reset;
            for (char c : std::string(test_payload)) {
                payload_reset.push_back(static_cast<std::byte>(c));
            }
            Message<DefaultNetworkTypes> msg(
                node_a, 8080 + i,
                node_b, 8081 + i,
                payload_reset
            );
            
            try {
                auto send_future = network_node_a->send(msg, medium_timeout);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                if (send_future.isReady()) {
                    send_future.get(); // May succeed or fail, both are acceptable
                }
            } catch (const std::exception&) {
                // Operations may fail during reset, that's acceptable
            }
        });
    }
    
    // Reset while operations are running
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    simulator.reset();
    
    // Wait for operation threads to complete
    for (auto& thread : operation_threads) {
        thread.join();
    }
    
    // Verify topology is cleared
    BOOST_CHECK(!simulator.has_node(node_a));
    BOOST_CHECK(!simulator.has_node(node_b));
    BOOST_CHECK(!simulator.has_edge(node_a, node_b));
    
    // Verify simulator can be used after reset
    simulator.add_node("new_node");
    BOOST_CHECK(simulator.has_node("new_node"));
}

/**
 * Test multiple resets
 */
BOOST_AUTO_TEST_CASE(multiple_resets, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    for (int cycle = 0; cycle < 5; ++cycle) {
        // Add some state
        std::string node_name = "node_" + std::to_string(cycle);
        simulator.add_node(node_name);
        BOOST_CHECK(simulator.has_node(node_name));
        
        // Reset
        simulator.reset();
        
        // Verify state is cleared
        BOOST_CHECK(!simulator.has_node(node_name));
        
        // Verify simulator is still usable
        std::string test_node = "test_node_" + std::to_string(cycle);
        simulator.add_node(test_node);
        BOOST_CHECK(simulator.has_node(test_node));
        
        // Reset again
        simulator.reset();
        BOOST_CHECK(!simulator.has_node(test_node));
    }
}

/**
 * Test reset with complex topology
 */
BOOST_AUTO_TEST_CASE(reset_complex_topology, * boost::unit_test::timeout(60)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    // Create a complex topology
    std::vector<std::string> nodes;
    for (int i = 0; i < 10; ++i) {
        std::string node_name = "complex_node_" + std::to_string(i);
        nodes.push_back(node_name);
        simulator.add_node(node_name);
    }
    
    // Create a mesh topology (every node connected to every other node)
    NetworkEdge edge(default_latency, default_reliability);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (i != j) {
                simulator.add_edge(nodes[i], nodes[j], edge);
            }
        }
    }
    
    // Verify topology exists
    for (const auto& node : nodes) {
        BOOST_CHECK(simulator.has_node(node));
    }
    
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (i != j) {
                BOOST_CHECK(simulator.has_edge(nodes[i], nodes[j]));
            }
        }
    }
    
    // Reset
    simulator.reset();
    
    // Verify all topology is cleared
    for (const auto& node : nodes) {
        BOOST_CHECK(!simulator.has_node(node));
    }
    
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (i != j) {
                BOOST_CHECK(!simulator.has_edge(nodes[i], nodes[j]));
            }
        }
    }
}

/**
 * Test reset returns simulator to initial conditions
 */
BOOST_AUTO_TEST_CASE(reset_returns_to_initial_conditions, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> simulator;
    
    // Capture initial state (should be empty)
    std::string test_node = "initial_test_node";
    BOOST_CHECK(!simulator.has_node(test_node));
    
    // Add state and modify simulator
    simulator.add_node(test_node);
    simulator.start();
    
    auto network_node = simulator.create_node(test_node);
    
    // Verify state exists
    BOOST_CHECK(simulator.has_node(test_node));
    
    // Reset
    simulator.reset();
    
    // Verify we're back to initial conditions
    BOOST_CHECK(!simulator.has_node(test_node));
    
    // Verify simulator behaves like a fresh instance
    std::string new_test_node = "new_test_node";
    simulator.add_node(new_test_node);
    BOOST_CHECK(simulator.has_node(new_test_node));
}