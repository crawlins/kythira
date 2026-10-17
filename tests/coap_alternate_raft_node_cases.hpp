// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The `node<Types>` cases coap-transport-multi-raft task 7 runs against each
// alternate CoAP backend (cantcoap, libnyoci), mirroring
// coap_raft_node_integration_test's libcoap cases: three nodes elect a leader,
// replicate a committed entry, and move leadership to a named node with
// TimeoutNow. Task 7 asks for "the same two tests as task 6" on every backend
// that ships TimeoutNow; the serializer round trip is backend-independent, so
// the end-to-end transfer is the one each backend has to pass itself, and the
// election and replication cases are what it stands on.
//
// Each suite includes this, only when its backend is built, after defining
// `raft_node_cases::backend` in the shape of coap_node_cluster_base.hpp's
// backend struct.

#include "coap_node_cluster_base.hpp"
#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <exception>
#include <string>

namespace raft_node_cases {

using cluster_type = kythira::testing::basic_coap_node_cluster<backend>;
using state_machine = kythira::test_key_value_state_machine<std::uint64_t>;
using kythira::testing::scaled_deadline;

constexpr std::size_t cluster_size = 3;

/// Commits one entry through the current leader and returns its log index.
inline auto commit_put(cluster_type& cluster, std::uint64_t leader, const std::string& key,
                       const std::string& value) -> std::uint64_t {
    auto future = cluster.node(leader).submit_command(state_machine::make_put_command(key, value),
                                                      scaled_deadline(5000));
    const auto error = cluster.settle(std::move(future), scaled_deadline(10000));
    if (error) {
        std::rethrow_exception(error);
    }
    return cluster.node(leader).debug_state().commit_index;
}

/// True once every live node has applied at least `index`.
inline auto all_live_applied(cluster_type& cluster, std::uint64_t index) -> bool {
    for (auto id : cluster.live()) {
        if (cluster.node(id).debug_state().last_applied < index) {
            return false;
        }
    }
    return true;
}

}  // namespace raft_node_cases

BOOST_AUTO_TEST_SUITE(alternate_coap_raft_node)

BOOST_AUTO_TEST_CASE(three_nodes_elect_a_leader,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    using namespace raft_node_cases;
    cluster_type cluster(cluster_size);
    cluster.start_all();

    const auto leader = cluster.require_leader(scaled_deadline(15000));
    BOOST_TEST_MESSAGE("leader: node " << leader);

    // Every other node has followed the leader into its term, so the
    // election's RequestVote round trips really crossed this backend.
    const auto term = cluster.node(leader).get_current_term();
    BOOST_TEST(cluster.tick_until(
        [&] {
            for (auto id : cluster.live()) {
                if (cluster.node(id).get_current_term() != term) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(5000)));
}

BOOST_AUTO_TEST_CASE(a_committed_entry_reaches_every_replica,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    using namespace raft_node_cases;
    cluster_type cluster(cluster_size);
    cluster.start_all();
    const auto leader = cluster.require_leader(scaled_deadline(15000));

    std::uint64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        last = commit_put(cluster, leader, "key" + std::to_string(i), "v" + std::to_string(i));
    }
    BOOST_TEST(last >= 5U);
    BOOST_TEST(cluster.tick_until([&] { return all_live_applied(cluster, last); },
                                  scaled_deadline(10000)));
}

// Task 6's end-to-end check, on this backend: leadership moves to the node
// that was named, in exactly one term bump, which a general election after a
// leader loss would not guarantee.
BOOST_AUTO_TEST_CASE(timeout_now_moves_leadership_to_the_named_node,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    using namespace raft_node_cases;
    static_assert(kythira::network_client_with_timeout_now<cluster_type::client_type>);
    static_assert(kythira::network_server_with_timeout_now<cluster_type::server_type>);

    cluster_type cluster(cluster_size);
    cluster.start_all();
    const auto leader = cluster.require_leader(scaled_deadline(15000));
    // Every follower must hold the leader's whole log before it is eligible.
    const auto index = commit_put(cluster, leader, "transfer", "me");
    BOOST_TEST(cluster.tick_until([&] { return all_live_applied(cluster, index); },
                                  scaled_deadline(10000)));

    std::uint64_t target = 0;
    std::uint64_t bystander = 0;
    for (auto id : cluster.ids()) {
        if (id == leader) {
            continue;
        }
        (target == 0 ? target : bystander) = id;
    }
    const auto term_before = cluster.node(leader).get_current_term();

    auto transfer = cluster.node(leader).transfer_leadership(target, scaled_deadline(3000));
    const auto error = cluster.settle(std::move(transfer), scaled_deadline(10000));
    BOOST_TEST(!error);

    BOOST_TEST(
        cluster.tick_until([&] { return cluster.leader() == target; }, scaled_deadline(5000)));
    BOOST_TEST(cluster.node(target).get_current_term() == term_before + 1);
    BOOST_TEST(cluster.node(bystander).get_current_term() <= term_before + 1);
}

BOOST_AUTO_TEST_SUITE_END()
