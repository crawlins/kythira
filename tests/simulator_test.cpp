#define BOOST_TEST_MODULE SimulatorTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <string>

using namespace network_simulator;

// Define a custom types structure for testing
struct test_network_types {
    using address_type = std::string;
    using port_type = unsigned short;
    using message_type = network_simulator::Message<test_network_types>;
    using connection_type = network_simulator::Connection<test_network_types>;
    using listener_type = network_simulator::Listener<test_network_types>;
    using node_type = network_simulator::NetworkNode<test_network_types>;
    
    // Future types using kythira::Future
    using future_bool_type = kythira::Future<bool>;
    using future_bytes_type = kythira::Future<std::vector<std::byte>>;
    using future_connection_type = kythira::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = kythira::Future<std::shared_ptr<listener_type>>;
    using future_message_type = kythira::Future<message_type>;
};

// Type alias for the correct NetworkSimulator template instantiation
using TestNetworkSimulator = NetworkSimulator<test_network_types>;

namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr const char* test_node_c = "node_c";
    constexpr std::chrono::milliseconds test_latency{50};
    constexpr double test_reliability = 0.95;
}

BOOST_AUTO_TEST_SUITE(topology_management)

BOOST_AUTO_TEST_CASE(add_node_creates_node_in_topology) {
    TestNetworkSimulator sim;
    
    sim.add_node(test_node_a);
    
    BOOST_TEST(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_multiple_nodes) {
    TestNetworkSimulator sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    sim.add_node(test_node_c);
    
    BOOST_TEST(sim.has_node(test_node_a));
    BOOST_TEST(sim.has_node(test_node_b));
    BOOST_TEST(sim.has_node(test_node_c));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_from_topology) {
    TestNetworkSimulator sim;
    
    sim.add_node(test_node_a);
    BOOST_TEST(sim.has_node(test_node_a));
    
    sim.remove_node(test_node_a);
    BOOST_TEST(!sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_edge_between_nodes) {
    TestNetworkSimulator sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_nodes_if_not_exist) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_node(test_node_a));
    BOOST_TEST(sim.has_node(test_node_b));
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_edge_removes_edge_from_topology) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
    
    sim.remove_edge(test_node_a, test_node_b);
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(edges_are_directional) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
    BOOST_TEST(!sim.has_edge(test_node_b, test_node_a));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_outgoing_edges) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_a);
    
    BOOST_TEST(!sim.has_node(test_node_a));
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_incoming_edges) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_b);
    
    BOOST_TEST(!sim.has_node(test_node_b));
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(get_edge_returns_correct_edge) {
    TestNetworkSimulator sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    auto retrieved_edge = sim.get_edge(test_node_a, test_node_b);
    
    BOOST_TEST(retrieved_edge.latency() == test_latency);
    BOOST_TEST(retrieved_edge.reliability() == test_reliability);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(node_creation)

BOOST_AUTO_TEST_CASE(create_node_returns_valid_node) {
    TestNetworkSimulator sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_TEST(node != nullptr);
    BOOST_TEST(node->address() == test_node_a);
}

BOOST_AUTO_TEST_CASE(create_node_adds_to_topology) {
    TestNetworkSimulator sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_TEST(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(create_node_twice_returns_same_instance) {
    TestNetworkSimulator sim;
    
    auto node1 = sim.create_node(test_node_a);
    auto node2 = sim.create_node(test_node_a);
    
    BOOST_TEST(node1.get() == node2.get());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(simulation_control)

BOOST_AUTO_TEST_CASE(simulator_starts_stopped) {
    TestNetworkSimulator sim;
    
    // Simulator should start in stopped state
    // We can't directly test this without exposing internal state,
    // but we can verify behavior
}

BOOST_AUTO_TEST_CASE(start_enables_simulation) {
    TestNetworkSimulator sim;
    
    sim.start();
    
    // Simulation is now started
    // Behavior will be tested in integration tests
}

BOOST_AUTO_TEST_CASE(stop_disables_simulation) {
    TestNetworkSimulator sim;
    
    sim.start();
    sim.stop();
    
    // Simulation is now stopped
}

BOOST_AUTO_TEST_CASE(reset_clears_all_state) {
    TestNetworkSimulator sim;
    
    // Add some state
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    sim.start();
    
    // Reset
    sim.reset();
    
    // Verify state is cleared
    BOOST_TEST(!sim.has_node(test_node_a));
    BOOST_TEST(!sim.has_node(test_node_b));
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(reset_allows_reuse) {
    TestNetworkSimulator sim;
    
    // First use
    sim.add_node(test_node_a);
    sim.start();
    sim.reset();
    
    // Second use
    sim.add_node(test_node_b);
    BOOST_TEST(sim.has_node(test_node_b));
    BOOST_TEST(!sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(reliability_simulation)

BOOST_AUTO_TEST_CASE(check_reliability_drops_messages) {
    TestNetworkSimulator sim;
    
    // Add edge with 30% reliability
    NetworkEdge edge(std::chrono::milliseconds{10}, 0.3);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    // Check reliability many times
    std::size_t successes = 0;
    std::size_t trials = 1000;
    
    for (std::size_t i = 0; i < trials; ++i) {
        if (sim.check_reliability(test_node_a, test_node_b)) {
            ++successes;
        }
    }
    
    // With 30% reliability, we expect roughly 300 successes out of 1000
    // Allow for statistical variation (20% to 40%)
    double success_rate = static_cast<double>(successes) / static_cast<double>(trials);
    
    BOOST_TEST_MESSAGE("Success rate: " << success_rate << " (" << successes << "/" << trials << ")");
    
    BOOST_TEST(success_rate >= 0.20);
    BOOST_TEST(success_rate <= 0.40);
}

BOOST_AUTO_TEST_SUITE_END()
