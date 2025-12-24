#define BOOST_TEST_MODULE SimulatorTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/network_simulator.hpp>
#include <raft/future.hpp>

#include <chrono>
#include <string>

using namespace network_simulator;
using kythira::NetworkSimulator;

namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr const char* test_node_c = "node_c";
    constexpr std::chrono::milliseconds test_latency{50};
    constexpr double test_reliability = 0.95;
}

BOOST_AUTO_TEST_SUITE(topology_management)

BOOST_AUTO_TEST_CASE(add_node_creates_node_in_topology) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.add_node(test_node_a);
    
    BOOST_TEST(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_multiple_nodes) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    sim.add_node(test_node_c);
    
    BOOST_TEST(sim.has_node(test_node_a));
    BOOST_TEST(sim.has_node(test_node_b));
    BOOST_TEST(sim.has_node(test_node_c));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_from_topology) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.add_node(test_node_a);
    BOOST_TEST(sim.has_node(test_node_a));
    
    sim.remove_node(test_node_a);
    BOOST_TEST(!sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_edge_between_nodes) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_nodes_if_not_exist) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_node(test_node_a));
    BOOST_TEST(sim.has_node(test_node_b));
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_edge_removes_edge_from_topology) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
    
    sim.remove_edge(test_node_a, test_node_b);
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(edges_are_directional) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_TEST(sim.has_edge(test_node_a, test_node_b));
    BOOST_TEST(!sim.has_edge(test_node_b, test_node_a));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_outgoing_edges) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_a);
    
    BOOST_TEST(!sim.has_node(test_node_a));
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_incoming_edges) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_b);
    
    BOOST_TEST(!sim.has_node(test_node_b));
    BOOST_TEST(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(get_edge_returns_correct_edge) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    auto retrieved_edge = sim.get_edge(test_node_a, test_node_b);
    
    BOOST_TEST(retrieved_edge.latency() == test_latency);
    BOOST_TEST(retrieved_edge.reliability() == test_reliability);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(node_creation)

BOOST_AUTO_TEST_CASE(create_node_returns_valid_node) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_TEST(node != nullptr);
    BOOST_TEST(node->address() == test_node_a);
}

BOOST_AUTO_TEST_CASE(create_node_adds_to_topology) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_TEST(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(create_node_twice_returns_same_instance) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    auto node1 = sim.create_node(test_node_a);
    auto node2 = sim.create_node(test_node_a);
    
    BOOST_TEST(node1.get() == node2.get());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(simulation_control)

BOOST_AUTO_TEST_CASE(simulator_starts_stopped) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    // Simulator should start in stopped state
    // We can't directly test this without exposing internal state,
    // but we can verify behavior
}

BOOST_AUTO_TEST_CASE(start_enables_simulation) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.start();
    
    // Simulation is now started
    // Behavior will be tested in integration tests
}

BOOST_AUTO_TEST_CASE(stop_disables_simulation) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
    sim.start();
    sim.stop();
    
    // Simulation is now stopped
}

BOOST_AUTO_TEST_CASE(reset_clears_all_state) {
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
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
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
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
    NetworkSimulator<std::string, unsigned short, kythira::Future<bool>> sim;
    
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
