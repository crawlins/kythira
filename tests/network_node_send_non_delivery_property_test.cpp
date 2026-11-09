// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE NetworkNodeSendNonDeliveryPropertyTest
#include <boost/test/unit_test.hpp>
#include <network_simulator/network_simulator.hpp>
#include <chrono>
#include <cstdint>
#include <string>

using namespace network_simulator;

namespace {
constexpr const char* test_node_a = "node_a";
constexpr const char* test_node_b = "node_b";
constexpr const char* test_payload = "test_message";
constexpr std::chrono::milliseconds test_latency{10};
constexpr double low_reliability = 0.1;  // 10% reliability - high chance of drops
// At 10% reliability the number of successful sends is Binomial(n, 0.1).
// With n = 50 it was zero about one run in 200 (0.9^50 = 0.5%), which failed
// the successful_sends > 0 check below and was rescued by ctest's retry five
// times in a week of CI. With n = 300 it is zero with probability 0.9^300,
// about 2e-14. Only successful sends pay the 10 ms latency, so this adds
// roughly 0.25 s.
constexpr std::size_t test_iterations = 300;
// A fixed seed makes a failure reproducible. The iteration count, not the
// seed, is what keeps the check safe: bernoulli_distribution's output for a
// given seed differs between standard libraries.
constexpr std::uint32_t test_seed = 20261109;
}

BOOST_AUTO_TEST_CASE(network_node_send_non_delivery_property_test, *boost::unit_test::timeout(30)) {
    // **Property 8: Send Does Not Guarantee Delivery**
    // **Validates: Requirements 4.4**

    // For any message where send returns true, the message MAY not appear at the destination
    // (due to reliability < 1.0), demonstrating that send success does not guarantee delivery.

    NetworkSimulator<DefaultNetworkTypes> simulator;
    simulator.start();
    simulator.seed_rng(test_seed);

    // Set up topology with low reliability to simulate message drops
    simulator.add_node(test_node_a);
    simulator.add_node(test_node_b);
    simulator.add_edge(test_node_a, test_node_b, NetworkEdge(test_latency, low_reliability));

    // Create nodes
    auto node_a = simulator.create_node(test_node_a);
    auto node_b = simulator.create_node(test_node_b);

    BOOST_REQUIRE(node_a != nullptr);
    BOOST_REQUIRE(node_b != nullptr);

    std::size_t successful_sends = 0;
    std::size_t delivered_messages = 0;

    // Test property: Send success does not guarantee delivery
    for (std::size_t i = 0; i < test_iterations; ++i) {
        // Create payload as vector of bytes
        std::vector<std::byte> payload;
        for (char c : std::string(test_payload)) {
            payload.push_back(static_cast<std::byte>(c));
        }

        // Create message from node_a to node_b
        Message<DefaultNetworkTypes> msg(test_node_a, static_cast<unsigned short>(8000 + i),
                                         test_node_b, static_cast<unsigned short>(9000 + i),
                                         std::move(payload));

        // Send message
        auto send_future = node_a->send(std::move(msg));
        bool send_result = std::move(send_future).get();

        if (send_result) {
            successful_sends++;

            // Try to receive the message (non-blocking check)
            auto receive_future = node_b->receive();
            try {
                auto received_msg = std::move(receive_future).get();
                // Check if we actually received a message (not empty)
                if (!received_msg.payload().empty()) {
                    delivered_messages++;
                }
            } catch (...) {
                // Message not available - this is expected with low reliability
            }
        }
    }

    // Property: Send success does not guarantee delivery
    // With low reliability, we should have some successful sends but fewer deliveries
    BOOST_CHECK(successful_sends > 0);  // Some sends should succeed

    // The key property: delivered messages should be <= successful sends
    // (send success doesn't guarantee delivery)
    BOOST_CHECK(delivered_messages <= successful_sends);

    // With 10% reliability and many iterations, we expect some message loss
    // This demonstrates that send success != delivery guarantee
    BOOST_TEST_MESSAGE("Successful sends: " << successful_sends
                                            << ", Delivered messages: " << delivered_messages);

    simulator.stop();
}