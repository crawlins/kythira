// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_raft_node_integration_test.cpp
/// @brief `node<Types>` over a real CoAP transport — the first one
///        (.kiro/specs/coap-transport-multi-raft/ tasks 5 and 6).
///
/// Every other CoAP test in the tree exercises the transport standalone. This
/// one runs three Raft nodes over the libcoap backend on loopback and asks the
/// three questions a consensus transport exists to answer: can the cluster
/// elect a leader, does a committed entry reach every replica, and does the
/// cluster recover when its leader dies. Then, because TimeoutNow now rides on
/// CoAP, whether leadership moves to a *named* node without a general election.
///
/// Timing follows doc/coap-flake-investigation.md: every Raft timing and every
/// wait is sized through KYTHIRA_TEST_TIMEOUT_SCALE, endpoints are numeric, and
/// no port is fixed (coap_node_cluster.hpp reserves ephemeral ones).

#include "coap_node_cluster.hpp"
#include "test_timeout_scale.hpp"

#define BOOST_TEST_MODULE coap_raft_node_integration_test
#include <boost/test/unit_test.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("coap_raft_node_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using cluster_type = kythira::testing::coap_node_cluster<>;
using state_machine = kythira::test_key_value_state_machine<std::uint64_t>;
using kythira::testing::scaled_deadline;

constexpr std::size_t cluster_size = 3;

/// Commits one entry through the current leader and returns its log index.
auto commit_put(cluster_type& cluster, std::uint64_t leader, const std::string& key,
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
auto all_live_applied(cluster_type& cluster, std::uint64_t index) -> bool {
    for (auto id : cluster.live()) {
        if (cluster.node(id).debug_state().last_applied < index) {
            return false;
        }
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_raft_node_integration)

#ifdef LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(three_nodes_elect_a_leader_over_coap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    cluster_type cluster(cluster_size);
    cluster.start_all();

    const auto leader = cluster.require_leader(scaled_deadline(15000));
    BOOST_TEST_MESSAGE("leader over CoAP: node " << leader);

    // One leader, and every other node has followed it into its term: the
    // election's RequestVote round trips really happened over the wire.
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

BOOST_AUTO_TEST_CASE(a_committed_entry_reaches_every_replica_over_coap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    cluster_type cluster(cluster_size);
    cluster.start_all();
    const auto leader = cluster.require_leader(scaled_deadline(15000));

    std::uint64_t last = 0;
    for (int i = 0; i < 5; ++i) {
        last = commit_put(cluster, leader, "key" + std::to_string(i), "v" + std::to_string(i));
    }
    BOOST_TEST(last >= 5u);

    // Committed is a majority; replicated is everyone. Followers learn the
    // commit index from the next AppendEntries, so give them heartbeats.
    BOOST_TEST(cluster.tick_until([&] { return all_live_applied(cluster, last); },
                                  scaled_deadline(10000)));
}

BOOST_AUTO_TEST_CASE(the_cluster_survives_a_leader_kill_over_coap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    cluster_type cluster(cluster_size);
    cluster.start_all();
    const auto first = cluster.require_leader(scaled_deadline(15000));
    const auto before = commit_put(cluster, first, "before", "kill");
    const auto first_term = cluster.node(first).get_current_term();

    cluster.kill(first);

    // The two survivors are a majority of three: they must elect one of
    // themselves in a later term, and commit through it.
    const auto second = cluster.require_leader(scaled_deadline(20000));
    BOOST_TEST(second != first);
    BOOST_TEST(cluster.node(second).get_current_term() > first_term);

    const auto after = commit_put(cluster, second, "after", "kill");
    BOOST_TEST(after > before);
    BOOST_TEST(cluster.tick_until([&] { return all_live_applied(cluster, after); },
                                  scaled_deadline(10000)));
}

// Task 6's end-to-end check: leadership moves to the node that was named,
// within an election timeout, and the third node never bumps its term on its
// own. An approximation built from stopping the leader would produce a general
// election, a leader of nobody's choosing, and a term bump everywhere.
BOOST_AUTO_TEST_CASE(timeout_now_moves_leadership_to_the_named_node_over_coap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(90))) {
    static_assert(kythira::network_client_with_timeout_now<cluster_type::client_type>);

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
    // Exactly one term bump, made by the target's TimeoutNow-triggered
    // election, which the bystander then follows. A general election after a
    // leader loss would also show a new term, but not necessarily this
    // target; the named target winning in term+1 is what TimeoutNow does.
    BOOST_TEST(cluster.node(target).get_current_term() == term_before + 1);
    BOOST_TEST(cluster.node(bystander).get_current_term() <= term_before + 1);
}

#else

BOOST_AUTO_TEST_CASE(skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available - no real CoAP transport to run Raft over");
    BOOST_TEST(true);
}

#endif

BOOST_AUTO_TEST_SUITE_END()
