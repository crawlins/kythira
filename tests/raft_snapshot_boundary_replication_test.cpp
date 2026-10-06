// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Replication across a snapshot boundary (Raft §7).
//
// When a node's log starts after a snapshot, the entry at the snapshot's
// last included index is gone, but its term is not: the snapshot carries it.
// That boundary is exactly the prevLogIndex an AppendEntries carries to a
// follower that is caught up to the snapshot, so both ends must match there.
//
// Before the node tracked the boundary, neither could. The leader found no
// entry at prevLogIndex and fell back to InstallSnapshot; the follower,
// already committed past it, dropped the snapshot as stale; the leader then
// reset nextIndex to one past the snapshot and found no entry at
// prevLogIndex again. Nothing after the snapshot ever replicated. A shard
// split is the everyday way to get there: every child begins at the split
// index from a synthetic snapshot with an empty log, so after a leader
// change no child could commit anything, membership changes included.
#define BOOST_TEST_MODULE raft_snapshot_boundary_replication_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <atomic>
#include <chrono>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_snapshot_boundary_replication_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
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

using sm_t = kythira::test_key_value_state_machine<test_types::log_index_type>;

constexpr std::uint64_t k_snapshot_index = 50;
constexpr std::uint64_t k_snapshot_term = 3;

/// A store holding what a split child holds: a snapshot at the boundary,
/// the term it was taken in, and @p trailing entries after it.
auto seeded_store(std::uint64_t trailing) -> test_types::persistence_engine_type {
    test_types::persistence_engine_type store;
    store.save_current_term(k_snapshot_term);
    sm_t sm;
    store.save_snapshot(test_types::snapshot_type{
        ._last_included_index = k_snapshot_index,
        ._last_included_term = k_snapshot_term,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt},
        ._state_machine_state = sm.get_state()});
    for (std::uint64_t i = 1; i <= trailing; ++i) {
        store.append_log_entry(
            test_types::log_entry_type{k_snapshot_term, k_snapshot_index + i,
                                       sm_t::make_put_command("seed" + std::to_string(i), "v")});
    }
    return store;
}

struct cluster {
    sim_t sim;
    std::vector<std::unique_ptr<test_node>> nodes;

    /// Node 1 starts with @p leader_trailing entries after the snapshot; the
    /// others with none.
    explicit cluster(std::uint64_t leader_trailing) {
        sim.start();
        connect_all(sim, {"1", "2", "3"});
        const auto cfg = make_fast_config();
        for (std::uint64_t id = 1; id <= 3; ++id) {
            auto net = sim.create_node(std::to_string(id));
            nodes.push_back(std::make_unique<test_node>(
                id, test_types::network_client_type{net, test_types::serializer_type{}},
                test_types::network_server_type{net, test_types::serializer_type{}},
                seeded_store(id == 1 ? leader_trailing : 0),
                kythira::console_logger{kythira::log_level::error}, test_types::metrics_type{},
                test_types::membership_manager_type{}, cfg, std::to_string(id),
                preset_peer_discovery<std::uint64_t, std::string>{}));
        }
        for (auto& n : nodes) {
            n->start();
        }
        std::this_thread::sleep_for(cfg._election_timeout_max + std::chrono::milliseconds{20});
        nodes[0]->check_election_timeout();
    }

    ~cluster() {
        for (auto& node : std::views::reverse(nodes)) {
            node->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    auto leader() -> test_node& { return *nodes[0]; }

    /// Drives heartbeats until @p pred holds or @p deadline passes.
    template<typename Pred> auto pump_until(Pred pred, std::chrono::milliseconds deadline) -> bool {
        return wait_until(
            [&] {
                leader().check_heartbeat_timeout();
                return pred();
            },
            deadline);
    }

    /// Submits one command and reports whether it committed.
    auto commit(const std::string& key) -> bool {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        leader()
            .submit_command(sm_t::make_put_command(key, "x"), std::chrono::milliseconds{3000})
            .thenValue([&](std::vector<std::byte>) {
                ok = true;
                done = true;
            })
            .thenError([&](const std::exception_ptr&) { done = true; })
            .detach();
        pump_until([&] { return done.load(); }, std::chrono::milliseconds{5000});
        return ok.load();
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(snapshot_boundary_replication)

// The split-child shape: every replica holds the snapshot and nothing else.
BOOST_AUTO_TEST_CASE(replicas_starting_from_a_snapshot_with_empty_logs_commit,
                     *boost::unit_test::timeout(30)) {
    cluster c{0};
    BOOST_REQUIRE(
        wait_until([&] { return c.leader().is_leader(); }, std::chrono::milliseconds{4000}));

    BOOST_REQUIRE(c.commit("after-snapshot"));
    BOOST_CHECK(c.pump_until(
        [&] {
            for (const auto& n : c.nodes) {
                if (n->debug_state().last_applied <= k_snapshot_index) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::milliseconds{3000}));
}

// A follower whose log ends exactly at the leader's snapshot boundary is
// caught up by AppendEntries from the boundary, not by a snapshot it would
// refuse as already committed.
BOOST_AUTO_TEST_CASE(a_follower_at_the_boundary_receives_the_entries_after_it,
                     *boost::unit_test::timeout(30)) {
    constexpr std::uint64_t trailing = 5;
    cluster c{trailing};
    BOOST_REQUIRE(
        wait_until([&] { return c.leader().is_leader(); }, std::chrono::milliseconds{4000}));

    BOOST_REQUIRE(c.commit("after-trailing"));
    BOOST_CHECK(c.pump_until(
        [&] {
            for (const auto& n : c.nodes) {
                if (n->debug_state().last_applied < k_snapshot_index + trailing + 1) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::milliseconds{3000}));
}

BOOST_AUTO_TEST_SUITE_END()
