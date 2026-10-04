// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Property-Based Test for Commit Implies Replication
 *
 * Feature: raft-consensus, Property 12: Commit Implies Replication
 * Validates: Requirements 7.4
 *
 * Property: For any log entry that is committed, that entry has been
 * replicated to a majority of servers in the cluster.
 */

#define BOOST_TEST_MODULE RaftCommitImpliesReplicationPropertyTest
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <optional>
#include <string>

#include "test_timeout_scale.hpp"

// Global test fixture to initialize Folly
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_commit_implies_replication_property_test"),
                             nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }

    ~FollyInitFixture() = default;

    std::unique_ptr<folly::Init> _init;
};
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {
constexpr std::size_t property_test_iterations = 10;
constexpr std::chrono::milliseconds election_timeout_min{50};
constexpr std::chrono::milliseconds election_timeout_max{100};

// Types for simulator-based testing
struct test_raft_types {
    // Future types
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    // Basic data types
    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    // Serializer and data types
    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    // Network types
    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type,
                                          serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type,
                                          serialized_data_type>;

    // Component types
    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;

    // Configuration type
    using configuration_type = kythira::raft_configuration;

    // Type aliases for commonly used compound types
    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    // RPC message types
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
};

constexpr std::chrono::milliseconds heartbeat_interval{25};
constexpr std::chrono::milliseconds rpc_timeout{100};

using node_type = kythira::node<test_raft_types>;
using node_id = std::uint64_t;
using log_index = std::uint64_t;
using entry_type = test_raft_types::log_entry_type;

auto make_config() -> kythira::raft_configuration {
    auto config = kythira::raft_configuration{};
    config._election_timeout_min = election_timeout_min;
    config._election_timeout_max = election_timeout_max;
    config._heartbeat_interval = heartbeat_interval;
    config._rpc_timeout = rpc_timeout;
    return config;
}

auto entry_at(const node_type& n, log_index index) -> std::optional<entry_type> {
    auto entries = n.log_entries_between(index, index);
    if (entries.empty()) {
        return std::nullopt;
    }
    return entries.front();
}

/// A fully connected simulator cluster whose elections the test drives itself.
///
/// Nodes here have no timer of their own: an election starts only when the
/// test calls `check_election_timeout()` on a node, and a leader replicates
/// only when the test calls `check_heartbeat_timeout()` on it. The previous
/// version of this file ticked every node's election timer at once, so
/// several candidates raced and a retried RequestVote could legitimately
/// depose the winner; it then asserted that the first leader was still
/// leader, which is not the property under test and failed 6-12 runs in 25 on
/// the boost leg (doc/TODO.md). Campaigning exactly one chosen node removes
/// the race, and the assertions below check replication itself.
struct sim_cluster {
    network_simulator::NetworkSimulator<test_raft_types::raft_network_types> simulator;
    std::vector<node_id> ids;
    std::vector<std::unique_ptr<node_type>> nodes;

    explicit sim_cluster(std::size_t size) {
        simulator.start();
        for (node_id id = 1; id <= size; ++id) {
            ids.push_back(id);
        }
        const auto config = make_config();
        for (auto id : ids) {
            auto sim_node = simulator.create_node(std::to_string(id));
            nodes.push_back(std::make_unique<node_type>(
                id,
                test_raft_types::network_client_type{sim_node, test_raft_types::serializer_type{}},
                test_raft_types::network_server_type{sim_node, test_raft_types::serializer_type{}},
                test_raft_types::persistence_engine_type{},
                test_raft_types::logger_type{kythira::log_level::error},
                test_raft_types::metrics_type{}, test_raft_types::membership_manager_type{},
                config));
        }
        for (std::size_t i = 0; i < size; ++i) {
            for (std::size_t j = 0; j < size; ++j) {
                if (i != j) {
                    connect_one_way(i, j);
                }
            }
        }
        for (auto& n : nodes) {
            n->set_cluster_configuration(ids);
            n->start();
        }
    }

    sim_cluster(const sim_cluster&) = delete;
    auto operator=(const sim_cluster&) -> sim_cluster& = delete;

    ~sim_cluster() {
        for (auto& n : nodes) {
            n->stop();
        }
    }

    [[nodiscard]] auto majority() const -> std::size_t { return nodes.size() / 2 + 1; }

    auto connect_one_way(std::size_t from, std::size_t to) -> void {
        simulator.add_edge(std::to_string(ids[from]), std::to_string(ids[to]),
                           network_simulator::NetworkEdge(std::chrono::milliseconds{0}, 1.0));
    }

    auto connect(std::size_t a, std::size_t b) -> void {
        connect_one_way(a, b);
        connect_one_way(b, a);
    }

    auto disconnect(std::size_t a, std::size_t b) -> void {
        simulator.remove_edge(std::to_string(ids[a]), std::to_string(ids[b]));
        simulator.remove_edge(std::to_string(ids[b]), std::to_string(ids[a]));
    }

    auto isolate(std::size_t a) -> void {
        for (std::size_t b = 0; b < nodes.size(); ++b) {
            if (b != a) {
                disconnect(a, b);
            }
        }
    }

    /// Campaign `candidate` alone until it leads, or the deadline passes.
    [[nodiscard]] auto elect(std::size_t candidate) -> bool {
        auto& n = *nodes[candidate];
        const auto deadline =
            std::chrono::steady_clock::now() + kythira::testing::scaled_deadline(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            // check_election_timeout() campaigns only once the node's own
            // randomised timeout has elapsed since it last heard from a leader.
            std::this_thread::sleep_for(election_timeout_max + std::chrono::milliseconds{20});
            n.check_election_timeout();
            const auto campaign_end = std::chrono::steady_clock::now() + election_timeout_max * 2;
            while (std::chrono::steady_clock::now() < campaign_end) {
                if (n.is_leader()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        return n.is_leader();
    }

    /// Drive `leader`'s heartbeats until its commit index reaches `index`.
    [[nodiscard]] auto replicate_until_committed(std::size_t leader, log_index index) -> bool {
        auto& n = *nodes[leader];
        const auto deadline =
            std::chrono::steady_clock::now() + kythira::testing::scaled_deadline(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (n.debug_state().commit_index >= index) {
                return true;
            }
            n.check_heartbeat_timeout();
            std::this_thread::sleep_for(heartbeat_interval);
        }
        return n.debug_state().commit_index >= index;
    }

    /// Drive `leader`'s heartbeats for a fixed number of rounds.
    auto heartbeat_rounds(std::size_t leader, std::size_t rounds) -> void {
        for (std::size_t i = 0; i < rounds; ++i) {
            nodes[leader]->check_heartbeat_timeout();
            std::this_thread::sleep_for(heartbeat_interval);
        }
    }

    /// How many nodes hold exactly `expected` (same index, term and command).
    [[nodiscard]] auto copies_of(const entry_type& expected) const -> std::size_t {
        return static_cast<std::size_t>(
            std::count_if(nodes.begin(), nodes.end(), [&](const std::unique_ptr<node_type>& n) {
                auto e = entry_at(*n, expected.index());
                return e && e->term() == expected.term() && e->command() == expected.command() &&
                       e->type() == expected.type();
            }));
    }

    /// Log every node's view: term, commit and last index, and, on the leader,
    /// the match index it holds for each peer. Printed when the property fails
    /// (with --log_level=message), because a wrong count is only diagnosable
    /// next to who the leader believed held what.
    auto describe() const -> void {
        for (const auto& n : nodes) {
            const auto state = n->debug_state();
            std::string matches;
            for (const auto& peer : nodes) {
                const auto match = n->match_index_of(peer->get_node_id());
                matches += " " + std::to_string(peer->get_node_id()) + "=" +
                           (match ? std::to_string(*match) : "-");
            }
            BOOST_TEST_MESSAGE("node " << n->get_node_id() << ": term " << state.current_term
                                       << ", commit " << state.commit_index << ", last "
                                       << n->last_log_index() << ", leader " << state.is_leader
                                       << ", match_index" << matches);
        }
    }

    /// Property 12: every entry any node considers committed is held by a
    /// majority of the cluster.
    ///
    /// Safe to call while replication is still running: an entry a node has
    /// committed is never truncated from any log that holds it, so a copy
    /// counted here cannot disappear before the count is compared.
    auto check_commit_implies_replication() const -> void {
        for (const auto& n : nodes) {
            const auto commit_index = n->debug_state().commit_index;
            for (const auto& entry : n->log_entries_between(1, commit_index)) {
                const auto copies = copies_of(entry);
                BOOST_TEST_CONTEXT("node " << n->get_node_id() << " committed index "
                                           << entry.index() << " (term " << entry.term()
                                           << ") at commit_index " << commit_index) {
                    BOOST_CHECK_GE(copies, majority());
                    if (copies < majority()) {
                        describe();
                    }
                }
            }
        }
    }
};

auto make_command(std::size_t seed) -> std::vector<std::byte> {
    return test_raft_types::state_machine_type::make_put_command("key-" + std::to_string(seed),
                                                                 "value-" + std::to_string(seed));
}
}  // namespace

BOOST_AUTO_TEST_SUITE(commit_implies_replication_property_tests)

/**
 * Property: Committed entries are replicated to majority
 *
 * A randomly sized cluster (3 or 5 nodes) elects a random node, which then
 * cuts a random minority of followers off from the cluster before taking a random
 * number of commands. Every command must still commit, and at every point
 * the test samples, every entry any node has committed must be held by a
 * majority. The cut followers prove the check is not vacuous: they hold none
 * of the new entries, so a leader that committed on fewer acknowledgements
 * than a majority would be caught.
 */
BOOST_AUTO_TEST_CASE(committed_entries_replicated_to_majority,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::size_t> cluster_size_dist(3, 5);
    std::uniform_int_distribution<std::size_t> command_count_dist(1, 10);

    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        auto cluster_size = cluster_size_dist(rng);
        if (cluster_size % 2 == 0) {
            cluster_size++;
        }
        sim_cluster cluster{cluster_size};

        const auto leader = std::uniform_int_distribution<std::size_t>(0, cluster_size - 1)(rng);
        BOOST_TEST_CONTEXT("iteration " << iteration << ", " << cluster_size
                                        << " nodes, leader node " << cluster.ids[leader]) {
            BOOST_REQUIRE(cluster.elect(leader));
            auto& leader_node = *cluster.nodes[leader];

            // The new leader's no-op commits before anything is cut.
            BOOST_REQUIRE(cluster.replicate_until_committed(leader, leader_node.last_log_index()));
            cluster.check_commit_implies_replication();

            // Cut a random minority of followers (possibly none) off the leader.
            std::vector<std::size_t> followers;
            for (std::size_t i = 0; i < cluster_size; ++i) {
                if (i != leader) {
                    followers.push_back(i);
                }
            }
            std::shuffle(followers.begin(), followers.end(), rng);
            const auto cut = std::uniform_int_distribution<std::size_t>(
                0, cluster_size - cluster.majority())(rng);
            // The simulator routes over multiple hops, so a follower is cut off
            // from the leader only once it is cut off from everyone.
            for (std::size_t i = 0; i < cut; ++i) {
                cluster.isolate(followers[i]);
            }

            const auto first_index = leader_node.last_log_index() + 1;
            const auto num_commands = command_count_dist(rng);
            std::vector<node_type::future_type> pending;
            for (std::size_t i = 0; i < num_commands; ++i) {
                pending.push_back(leader_node.submit_command(
                    make_command(i), kythira::testing::scaled_deadline(5000)));
                cluster.heartbeat_rounds(leader, 1);
                cluster.check_commit_implies_replication();
            }
            const auto last_index = first_index + num_commands - 1;
            BOOST_REQUIRE_EQUAL(leader_node.last_log_index(), last_index);

            BOOST_REQUIRE(cluster.replicate_until_committed(leader, last_index));
            cluster.check_commit_implies_replication();

            // A cut follower holds none of the new entries, so the majority
            // above was assembled without it.
            for (std::size_t i = 0; i < cut; ++i) {
                BOOST_CHECK(!entry_at(*cluster.nodes[followers[i]], first_index).has_value());
            }

            // Exercise leave_cluster on a connected follower to cover the
            // ClusterLeave RPC code path.
            BOOST_CHECK_NO_THROW(
                cluster.nodes[followers.back()]->leave_cluster(std::chrono::milliseconds{3000}));
        }
    }
}

/**
 * Property: Majority replication before commit
 *
 * A leader cut off from every follower appends an entry and keeps sending
 * heartbeats into the void. Nothing may commit it while it is held by the
 * leader alone. Once one follower is reconnected, the entry is on a majority
 * of a 3-node cluster and must commit.
 */
BOOST_AUTO_TEST_CASE(no_commit_without_majority_replication,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        constexpr std::size_t cluster_size = 3;
        sim_cluster cluster{cluster_size};

        const auto leader = std::uniform_int_distribution<std::size_t>(0, cluster_size - 1)(rng);
        BOOST_TEST_CONTEXT("iteration " << iteration << ", leader node " << cluster.ids[leader]) {
            BOOST_REQUIRE(cluster.elect(leader));
            auto& leader_node = *cluster.nodes[leader];
            BOOST_REQUIRE(cluster.replicate_until_committed(leader, leader_node.last_log_index()));

            cluster.isolate(leader);
            auto future = leader_node.submit_command(make_command(42),
                                                     kythira::testing::scaled_deadline(10000));
            const auto index = leader_node.last_log_index();

            cluster.heartbeat_rounds(leader, 10);
            BOOST_CHECK(leader_node.is_leader());
            BOOST_CHECK_LT(leader_node.debug_state().commit_index, index);
            for (std::size_t i = 0; i < cluster_size; ++i) {
                if (i != leader) {
                    BOOST_CHECK(!entry_at(*cluster.nodes[i], index).has_value());
                    BOOST_CHECK_LT(cluster.nodes[i]->debug_state().commit_index, index);
                }
            }
            cluster.check_commit_implies_replication();

            const auto rejoined = (leader + 1) % cluster_size;
            cluster.connect(leader, rejoined);
            BOOST_REQUIRE(cluster.replicate_until_committed(leader, index));
            BOOST_CHECK(entry_at(*cluster.nodes[rejoined], index).has_value());
            cluster.check_commit_implies_replication();
        }
    }
}

/**
 * Property: Commit requires current term entry
 *
 * Raft §5.4.2: a leader commits by counting replicas only for an entry of its
 * own term; earlier-term entries commit indirectly, behind one.
 *
 * Leader A commits an entry X and is then isolated before it ticks again, so
 * its followers hold X without necessarily knowing it committed. One of them,
 * B, is elected in a later term with X still in its log beyond its own commit
 * index. From the moment B leads, its commit index may only move to an entry
 * of B's current term; X must survive (Leader Completeness) and commit behind
 * B's no-op. After A rejoins, every node agrees on the committed prefix.
 */
BOOST_AUTO_TEST_CASE(commit_requires_current_term,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    std::random_device rd;
    std::mt19937 rng(rd());

    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        constexpr std::size_t cluster_size = 3;
        sim_cluster cluster{cluster_size};

        const auto a = std::uniform_int_distribution<std::size_t>(0, cluster_size - 1)(rng);
        BOOST_TEST_CONTEXT("iteration " << iteration << ", first leader node " << cluster.ids[a]) {
            BOOST_REQUIRE(cluster.elect(a));
            auto& a_node = *cluster.nodes[a];
            BOOST_REQUIRE(cluster.replicate_until_committed(a, a_node.last_log_index()));

            auto future =
                a_node.submit_command(make_command(7), kythira::testing::scaled_deadline(5000));
            const auto x_index = a_node.last_log_index();
            BOOST_REQUIRE(cluster.replicate_until_committed(a, x_index));
            const auto x = entry_at(a_node, x_index);
            BOOST_REQUIRE(x.has_value());
            cluster.isolate(a);

            // X committed, so a majority holds it, so at least one follower
            // does; only such a follower can win the next election.
            std::vector<std::size_t> holders;
            for (std::size_t i = 0; i < cluster_size; ++i) {
                if (i != a && entry_at(*cluster.nodes[i], x_index)) {
                    holders.push_back(i);
                }
            }
            BOOST_REQUIRE(!holders.empty());
            const auto b =
                holders[std::uniform_int_distribution<std::size_t>(0, holders.size() - 1)(rng)];
            auto& b_node = *cluster.nodes[b];

            const auto inherited_commit = b_node.debug_state().commit_index;
            BOOST_REQUIRE(cluster.elect(b));
            const auto b_term = b_node.get_current_term();
            BOOST_REQUIRE_GT(b_term, x->term());
            const auto b_noop_index = b_node.last_log_index();
            BOOST_REQUIRE_GT(b_noop_index, x_index);

            const auto deadline =
                std::chrono::steady_clock::now() + kythira::testing::scaled_deadline(5000);
            while (std::chrono::steady_clock::now() < deadline) {
                const auto state = b_node.debug_state();
                if (state.commit_index > inherited_commit && state.is_leader &&
                    state.current_term == b_term) {
                    auto committed = entry_at(b_node, state.commit_index);
                    BOOST_REQUIRE(committed.has_value());
                    BOOST_CHECK_EQUAL(committed->term(), b_term);
                }
                if (state.commit_index >= b_noop_index) {
                    break;
                }
                b_node.check_heartbeat_timeout();
                std::this_thread::sleep_for(heartbeat_interval);
            }
            BOOST_REQUIRE_GE(b_node.debug_state().commit_index, b_noop_index);

            const auto x_on_b = entry_at(b_node, x_index);
            BOOST_REQUIRE(x_on_b.has_value());
            BOOST_CHECK_EQUAL(x_on_b->term(), x->term());
            BOOST_CHECK(x_on_b->command() == x->command());
            cluster.check_commit_implies_replication();

            // A rejoins, steps down on B's term, and catches up.
            for (std::size_t i = 0; i < cluster_size; ++i) {
                if (i != a) {
                    cluster.connect(a, i);
                }
            }
            const auto catch_up_deadline =
                std::chrono::steady_clock::now() + kythira::testing::scaled_deadline(5000);
            while (std::chrono::steady_clock::now() < catch_up_deadline &&
                   a_node.debug_state().commit_index < b_noop_index) {
                b_node.check_heartbeat_timeout();
                std::this_thread::sleep_for(heartbeat_interval);
            }
            BOOST_CHECK(!a_node.is_leader());
            BOOST_REQUIRE_GE(a_node.debug_state().commit_index, b_noop_index);
            cluster.check_commit_implies_replication();

            // State Machine Safety: the committed prefixes agree everywhere.
            const auto reference = b_node.log_entries_between(1, b_noop_index);
            for (const auto& n : cluster.nodes) {
                const auto prefix = n->log_entries_between(1, b_noop_index);
                BOOST_REQUIRE_EQUAL(prefix.size(), reference.size());
                for (std::size_t i = 0; i < prefix.size(); ++i) {
                    BOOST_CHECK_EQUAL(prefix[i].term(), reference[i].term());
                    BOOST_CHECK(prefix[i].command() == reference[i].command());
                }
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
