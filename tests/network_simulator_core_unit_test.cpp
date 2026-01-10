#define BOOST_TEST_MODULE NetworkSimulatorCoreUnitTest
#include <boost/test/unit_test.hpp>

#include <network_simulator/simulator.hpp>
#include <network_simulator/types.hpp>
#include <chrono>
#include <string>

using namespace network_simulator;

namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr const char* test_node_c = "node_c";
    constexpr std::chrono::milliseconds test_latency{50};
    constexpr double test_reliability = 0.95;
}

BOOST_AUTO_TEST_SUITE(topology_management_new_api)

/**
 * Test add/remove nodes and edges
 * _Requirements: 1.1, 1.2, 11.1-11.6_
 */
BOOST_AUTO_TEST_CASE(add_node_creates_node_in_topology, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    
    BOOST_CHECK(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_multiple_nodes, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    sim.add_node(test_node_c);
    
    BOOST_CHECK(sim.has_node(test_node_a));
    BOOST_CHECK(sim.has_node(test_node_b));
    BOOST_CHECK(sim.has_node(test_node_c));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_from_topology, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    BOOST_CHECK(sim.has_node(test_node_a));
    
    sim.remove_node(test_node_a);
    BOOST_CHECK(!sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_edge_between_nodes, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_CHECK(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(add_edge_creates_nodes_if_not_exist, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_CHECK(sim.has_node(test_node_a));
    BOOST_CHECK(sim.has_node(test_node_b));
    BOOST_CHECK(sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_edge_removes_edge_from_topology, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_CHECK(sim.has_edge(test_node_a, test_node_b));
    
    sim.remove_edge(test_node_a, test_node_b);
    BOOST_CHECK(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(edges_are_directional, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    BOOST_CHECK(sim.has_edge(test_node_a, test_node_b));
    BOOST_CHECK(!sim.has_edge(test_node_b, test_node_a));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_outgoing_edges, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_a);
    
    BOOST_CHECK(!sim.has_node(test_node_a));
    BOOST_CHECK(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(remove_node_removes_incoming_edges, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    sim.remove_node(test_node_b);
    
    BOOST_CHECK(!sim.has_node(test_node_b));
    BOOST_CHECK(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(get_edge_returns_correct_edge, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    auto retrieved_edge = sim.get_edge(test_node_a, test_node_b);
    
    BOOST_CHECK_EQUAL(retrieved_edge.latency().count(), test_latency.count());
    BOOST_CHECK_EQUAL(retrieved_edge.reliability(), test_reliability);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(node_creation_new_api)

/**
 * Test topology queries
 * _Requirements: 11.5, 11.6_
 */
BOOST_AUTO_TEST_CASE(create_node_returns_valid_node, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_CHECK(node != nullptr);
    BOOST_CHECK_EQUAL(node->address(), test_node_a);
}

BOOST_AUTO_TEST_CASE(create_node_adds_to_topology, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    auto node = sim.create_node(test_node_a);
    
    BOOST_CHECK(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(create_node_twice_returns_same_instance, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    auto node1 = sim.create_node(test_node_a);
    auto node2 = sim.create_node(test_node_a);
    
    BOOST_CHECK_EQUAL(node1.get(), node2.get());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(simulation_control_new_api)

/**
 * Test simulation start/stop/reset
 * _Requirements: 12.1-12.5_
 */
BOOST_AUTO_TEST_CASE(start_enables_simulation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Should be able to start without error
    BOOST_CHECK_NO_THROW(sim.start());
}

BOOST_AUTO_TEST_CASE(stop_disables_simulation, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.start();
    
    // Should be able to stop without error
    BOOST_CHECK_NO_THROW(sim.stop());
}

BOOST_AUTO_TEST_CASE(reset_clears_all_state, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Add some state
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    sim.start();
    
    // Reset
    sim.reset();
    
    // Verify state is cleared
    BOOST_CHECK(!sim.has_node(test_node_a));
    BOOST_CHECK(!sim.has_node(test_node_b));
    BOOST_CHECK(!sim.has_edge(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_CASE(reset_allows_reuse, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // First use
    sim.add_node(test_node_a);
    sim.start();
    sim.reset();
    
    // Second use
    sim.add_node(test_node_b);
    BOOST_CHECK(sim.has_node(test_node_b));
    BOOST_CHECK(!sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_CASE(multiple_start_stop_cycles, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Multiple start/stop cycles should work
    sim.start();
    sim.stop();
    sim.start();
    sim.stop();
    
    // Should be able to add nodes after stop
    sim.add_node(test_node_a);
    BOOST_CHECK(sim.has_node(test_node_a));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(message_routing_new_api)

/**
 * Test message routing logic
 * _Requirements: 1.3, 1.4, 1.5_
 */
BOOST_AUTO_TEST_CASE(route_message_requires_started_simulator, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Create nodes and edge
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    // Create message
    DefaultNetworkTypes::message_type msg(test_node_a, 8080, test_node_b, 8081);
    
    // Should fail when simulator is not started
    auto future = sim.route_message(std::move(msg));
    auto result = future.get();
    BOOST_CHECK_EQUAL(result, false);
}

BOOST_AUTO_TEST_CASE(route_message_succeeds_when_started, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Start simulator
    sim.start();
    
    // Create nodes and edge with perfect reliability
    NetworkEdge edge(test_latency, 1.0);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    // Create message
    DefaultNetworkTypes::message_type msg(test_node_a, 8080, test_node_b, 8081);
    
    // Should succeed when simulator is started and route exists
    auto future = sim.route_message(std::move(msg));
    auto result = future.get();
    BOOST_CHECK_EQUAL(result, true);
}

BOOST_AUTO_TEST_CASE(route_message_fails_without_route, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.start();
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    // No edge between nodes
    
    // Create message
    DefaultNetworkTypes::message_type msg(test_node_a, 8080, test_node_b, 8081);
    
    // Should fail when no route exists
    auto future = sim.route_message(std::move(msg));
    auto result = future.get();
    BOOST_CHECK_EQUAL(result, false);
}

BOOST_AUTO_TEST_CASE(apply_latency_returns_edge_latency, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    NetworkEdge edge(test_latency, test_reliability);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    auto latency = sim.apply_latency(test_node_a, test_node_b);
    BOOST_CHECK_EQUAL(latency.count(), test_latency.count());
}

BOOST_AUTO_TEST_CASE(apply_latency_returns_zero_without_edge, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    // No edge
    
    auto latency = sim.apply_latency(test_node_a, test_node_b);
    BOOST_CHECK_EQUAL(latency.count(), 0);
}

BOOST_AUTO_TEST_CASE(check_reliability_with_perfect_reliability, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Perfect reliability should always succeed
    NetworkEdge edge(test_latency, 1.0);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    // Test multiple times
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK(sim.check_reliability(test_node_a, test_node_b));
    }
}

BOOST_AUTO_TEST_CASE(check_reliability_with_zero_reliability, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // Zero reliability should always fail
    NetworkEdge edge(test_latency, 0.0);
    sim.add_edge(test_node_a, test_node_b, edge);
    
    // Test multiple times
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK(!sim.check_reliability(test_node_a, test_node_b));
    }
}

BOOST_AUTO_TEST_CASE(check_reliability_without_edge_fails, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    // No edge
    
    BOOST_CHECK(!sim.check_reliability(test_node_a, test_node_b));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(message_delivery_new_api)

/**
 * Test message delivery and queuing
 * _Requirements: 4.2, 5.2_
 */
BOOST_AUTO_TEST_CASE(deliver_message_queues_at_destination, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_b);
    
    // Create and deliver message
    DefaultNetworkTypes::message_type msg(test_node_a, 8080, test_node_b, 8081);
    sim.deliver_message(std::move(msg));
    
    // Should be able to retrieve message
    auto future = sim.retrieve_message(test_node_b);
    auto retrieved_msg = future.get();
    
    BOOST_CHECK_EQUAL(retrieved_msg.source_address(), test_node_a);
    BOOST_CHECK_EQUAL(retrieved_msg.destination_address(), test_node_b);
}

BOOST_AUTO_TEST_CASE(retrieve_message_returns_empty_when_no_messages, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    
    // Should return empty message when no messages queued
    auto future = sim.retrieve_message(test_node_a);
    auto retrieved_msg = future.get();
    
    // Empty message should have empty addresses
    BOOST_CHECK(retrieved_msg.source_address().empty());
    BOOST_CHECK(retrieved_msg.destination_address().empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(error_handling_new_api)

/**
 * Test error conditions
 */
BOOST_AUTO_TEST_CASE(get_edge_throws_on_nonexistent_edge, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    sim.add_node(test_node_a);
    sim.add_node(test_node_b);
    // No edge
    
    BOOST_CHECK_THROW(sim.get_edge(test_node_a, test_node_b), NoRouteException);
}

BOOST_AUTO_TEST_CASE(get_edge_throws_on_nonexistent_node, * boost::unit_test::timeout(30)) {
    NetworkSimulator<DefaultNetworkTypes> sim;
    
    // No nodes
    BOOST_CHECK_THROW(sim.get_edge(test_node_a, test_node_b), NoRouteException);
}

BOOST_AUTO_TEST_SUITE_END()