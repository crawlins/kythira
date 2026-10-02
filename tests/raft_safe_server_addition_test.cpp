// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// raft-consensus Requirement 9.4 and 9.6: add_server() admits a new server as
// a non-voting learner and promotes it only once it has caught up, and a
// server outside the configuration cannot disrupt elections with RequestVote.

#define BOOST_TEST_MODULE raft_safe_server_addition_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_safe_server_addition_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

// ── Shared types ───────────────────────────────────────────────────────────

namespace {

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;

    preset_peer_discovery() = default;
    explicit preset_peer_discovery(std::vector<kythira::peer_info<NodeId, Address>> peers)
        : _peers(std::move(peers)) {}

    auto register_node(NodeId, Address) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::future_default<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::future_factory_default::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>(_peers));
    }

private:
    std::vector<kythira::peer_info<NodeId, Address>> _peers;
};

struct test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type,
                                          serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type,
                                          serialized_data_type>;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

using test_node = kythira::node<test_types>;
using sim_t = network_simulator::NetworkSimulator<test_types::raft_network_types>;

kythira::raft_configuration make_fast_config() {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{80};
    cfg._election_timeout_max = std::chrono::milliseconds{160};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
    return cfg;
}

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

void connect_all(sim_t& sim, std::initializer_list<std::string> addresses) {
    network_simulator::NetworkEdge edge{};
    for (const auto& from : addresses) {
        for (const auto& to : addresses) {
            if (from != to) {
                sim.add_edge(from, to, edge);
            }
        }
    }
}

bool contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

test_node make_node(std::uint64_t id, sim_t& sim, const kythira::raft_configuration& cfg) {
    auto net = sim.create_node(std::to_string(id));
    return test_node{id,
                     {net, test_types::serializer_type{}},
                     {net, test_types::serializer_type{}},
                     {},
                     kythira::console_logger{},
                     {},
                     {},
                     cfg,
                     std::to_string(id),
                     preset_peer_discovery<std::uint64_t, std::string>{}};
}

// Elects node1 as leader of {1, 2, 3}.
void elect_node1(test_node& node1, test_node& node2, test_node& node3,
                 const kythira::raft_configuration& cfg) {
    node1.set_cluster_configuration({1, 2, 3});
    node2.set_cluster_configuration({1, 2, 3});
    node3.set_cluster_configuration({1, 2, 3});
    node1.start();
    node2.start();
    node3.start();
    std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.is_leader(); }, std::chrono::milliseconds{4000}));
}

// Ticks the leader until `pred` holds or `deadline` elapses.
template<typename Pred>
bool pump_until(test_node& leader, Pred pred,
                std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        leader.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

struct outcome {
    std::atomic<bool> resolved{false};
    std::atomic<bool> failed{false};
    [[nodiscard]] auto done() const -> bool { return resolved || failed; }
};

void watch(test_node::future_type future, outcome& out) {
    std::move(future)
        .thenValue([&out](std::vector<std::byte>) { out.resolved = true; })
        .thenError([&out](const std::exception_ptr&) { out.failed = true; })
        .detach();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(safe_server_addition)

/**
 * Requirement 9.4: the new server is a learner until it catches up. While it
 * is unreachable it stays out of the voting set, so the three existing voters
 * keep committing on their own; once it is reachable it catches up and is
 * promoted, and add_server() resolves.
 */
BOOST_AUTO_TEST_CASE(new_server_votes_only_after_catching_up, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto cfg = make_fast_config();

    auto node1 = make_node(1, sim, cfg);
    auto node2 = make_node(2, sim, cfg);
    auto node3 = make_node(3, sim, cfg);
    auto node4 = make_node(4, sim, cfg);
    // node4 has no edges yet: it cannot receive anything.
    connect_all(sim, {"1", "2", "3"});
    node4.set_cluster_configuration({4});
    node4.start();
    elect_node1(node1, node2, node3, cfg);

    outcome added;
    watch(node1.add_server(4), added);

    // Admitted as a learner, synchronously; not a voter.
    auto view = node1.current_membership();
    BOOST_CHECK(contains(view.learners, 4));
    BOOST_CHECK(!contains(view.voters, 4));
    BOOST_CHECK(!view.joint);

    // A second change waits for this one.
    outcome second;
    watch(node1.add_server(5), second);
    BOOST_REQUIRE(wait_until([&] { return second.done(); }));
    BOOST_CHECK(second.failed);

    // The existing voters still commit without node4.
    using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;
    outcome cmd;
    watch(node1.submit_command(sm_t::make_put_command("k", "v"), std::chrono::milliseconds{3000}),
          cmd);
    BOOST_REQUIRE(pump_until(node1, [&] { return cmd.done(); }));
    BOOST_CHECK(cmd.resolved);

    // Still not promoted: it has not caught up.
    for (int i = 0; i < 10; ++i) {
        node1.check_heartbeat_timeout();
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK(!added.done());
    view = node1.current_membership();
    BOOST_CHECK(!contains(view.voters, 4));
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 3u);

    // Reachable now: it catches up, is promoted, and add_server() resolves.
    connect_all(sim, {"1", "2", "3", "4"});
    BOOST_REQUIRE(
        pump_until(node1, [&] { return added.done(); }, std::chrono::milliseconds{10000}));
    BOOST_CHECK(added.resolved);
    BOOST_REQUIRE(pump_until(node1, [&] {
        auto v = node1.current_membership();
        return !v.joint && contains(v.voters, 4);
    }));
    view = node1.current_membership();
    BOOST_CHECK(!contains(view.learners, 4));
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 4u);
    // It holds the command committed while it was away.
    BOOST_CHECK(pump_until(node1, [&] {
        return node4.debug_state().last_applied >= node1.debug_state().last_applied;
    }));

    node4.stop();
    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Requirement 9.4: a new server that never answers is abandoned rather than
 * promoted. The future fails, the voting set is unchanged, and the node is
 * left as a harmless learner that a later add_server() could resume.
 */
BOOST_AUTO_TEST_CASE(unreachable_new_server_is_never_promoted, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto cfg = make_fast_config();
    // The stall budget is append_entries_timeout * 10; keep it short.
    cfg._append_entries_timeout = std::chrono::milliseconds{100};

    auto node1 = make_node(1, sim, cfg);
    auto node2 = make_node(2, sim, cfg);
    auto node3 = make_node(3, sim, cfg);
    connect_all(sim, {"1", "2", "3"});
    elect_node1(node1, node2, node3, cfg);

    outcome added;
    watch(node1.add_server(4), added);
    BOOST_REQUIRE(pump_until(node1, [&] { return added.done(); }, std::chrono::milliseconds{8000}));
    BOOST_CHECK(added.failed);

    auto view = node1.current_membership();
    BOOST_CHECK(!contains(view.voters, 4));
    BOOST_CHECK(contains(view.learners, 4));
    BOOST_CHECK(!view.joint);
    BOOST_CHECK_EQUAL(node1.get_cluster_size(), 3u);

    // Not stuck: the next membership change is accepted.
    outcome removed;
    watch(node1.remove_learner(4), removed);
    BOOST_REQUIRE(pump_until(node1, [&] { return removed.done(); }));
    BOOST_CHECK(removed.resolved);

    node3.stop();
    node2.stop();
    node1.stop();
}

/**
 * Requirement 9.6: a RequestVote from a server outside the configuration is
 * ignored, however high its term, so the leader neither steps down nor
 * adopts the term. The same RPC naming a voter is honoured, which shows the
 * refusal is by membership and not by term.
 */
BOOST_AUTO_TEST_CASE(removed_server_cannot_disrupt_the_leader, *boost::unit_test::timeout(30)) {
    sim_t sim;
    sim.start();
    auto cfg = make_fast_config();

    auto node1 = make_node(1, sim, cfg);
    auto node2 = make_node(2, sim, cfg);
    auto node3 = make_node(3, sim, cfg);
    auto removed_net = sim.create_node("9");
    connect_all(sim, {"1", "2", "3", "9"});
    elect_node1(node1, node2, node3, cfg);

    test_types::network_client_type removed{removed_net, test_types::serializer_type{}};
    const auto term = node1.get_current_term();
    const auto last = node1.debug_state().log.size();

    // Removed server 9, campaigning far ahead with a log at least as long.
    kythira::request_vote_request<> from_removed{._term = term + 5,
                                                 ._candidate_id = 9,
                                                 ._last_log_index = last + 10,
                                                 ._last_log_term = term + 5};
    for (std::uint64_t target : {1u, 2u, 3u}) {
        auto response =
            removed.send_request_vote(target, from_removed, std::chrono::milliseconds{1000}).get();
        BOOST_CHECK(!response.vote_granted());
        BOOST_CHECK_EQUAL(response.term(), term);
    }
    BOOST_CHECK(node1.is_leader());
    BOOST_CHECK_EQUAL(node1.get_current_term(), term);
    BOOST_CHECK_EQUAL(node2.get_current_term(), term);
    BOOST_CHECK_EQUAL(node3.get_current_term(), term);

    // Control: the same request from voter 2 deposes node1 and is granted.
    kythira::request_vote_request<> from_voter = from_removed;
    from_voter._candidate_id = 2;
    auto response = removed.send_request_vote(1, from_voter, std::chrono::milliseconds{1000}).get();
    BOOST_CHECK(response.vote_granted());
    BOOST_CHECK_EQUAL(node1.get_current_term(), term + 5);
    BOOST_CHECK(!node1.is_leader());

    node3.stop();
    node2.stop();
    node1.stop();
}

BOOST_AUTO_TEST_SUITE_END()
