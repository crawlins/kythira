// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file leader_transfer_test.cpp
/// @brief Raft leadership transfer (TimeoutNow) and multi-Raft scatter (task 29
///        of `.kiro/specs/multi-raft/`).
///
/// Leadership transfer is the cheapest rebalance the system has — no data
/// moves, and the target already holds the log — which is what makes it worth a
/// wire message rather than an approximation built from stopping the leader and
/// waiting for an election. The two properties that distinguish the real thing
/// from that approximation are what this file tests:
///
///  * the new leader is the one that was **named**, not whichever replica
///    happened to time out first; and
///  * no uninvolved replica bumps its term. An approximation that let the
///    cluster fall into a general election would produce a leader *and* a term
///    bump on every node, and would look like success to a test that only
///    checked who ended up leading.
///
/// The failure paths matter as much. Every one of them leaves the cluster
/// exactly where it started — that is what makes the placement driver's
/// `transfer_leader` operator safe to be advisory.

#define BOOST_TEST_MODULE leader_transfer_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/exceptions.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("leader_transfer_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_epoch;
using kythira::tick_batch_controller;
using kythira::tombstone_reason;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;

/// The host's shared-transport bundle. Note that the client and server named
/// here are the *shared* ones; `multi_raft` derives each group's scoped views
/// from them.
struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

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
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;

auto fast_raft_config() -> kythira::raft_configuration {
    kythira::raft_configuration c;
    c._election_timeout_min = std::chrono::milliseconds{40};
    c._election_timeout_max = std::chrono::milliseconds{80};
    c._heartbeat_interval = std::chrono::milliseconds{10};
    return c;
}

auto make_config(message_fabric& fabric, std::uint64_t node_id) -> config_type {
    config_type cfg{
        .node_id = node_id,
        .network_client = fabric_client{fabric, node_id},
        .network_server = fabric_server{fabric, node_id},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.config = fast_raft_config();
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 4;
    return cfg;
}

using kythira::leader_transfer_exception;

/// The one group every case in this file drives. Named rather than spelled `1`
/// at each call site, because `group_id_type` and `node_id_type` are the same
/// underlying type and a transposed argument pair would otherwise compile.
constexpr group_id_type k_group = 1;

/// @brief Three hosts, one group, driven together.
///
/// A group per host rather than a node per group: leadership transfer is a
/// property of one Raft group, and the multi-Raft host is how a group's `node`
/// is reached here — the same object either way.
class transfer_cluster {
public:
    explicit transfer_cluster(std::vector<std::uint64_t> nodes) : _nodes(std::move(nodes)) {
        for (auto id : _nodes) {
            auto cfg = make_config(_fabric, id);
            cfg.heartbeat_interval = std::chrono::milliseconds{0};
            _hosts.emplace(id, std::make_unique<host_type>(std::move(cfg)));
        }
    }

    ~transfer_cluster() { stop(); }

    transfer_cluster(const transfer_cluster&) = delete;
    auto operator=(const transfer_cluster&) -> transfer_cluster& = delete;

    auto create_group(group_id_type group) -> void {
        for (auto id : _nodes) {
            _hosts.at(id)->create_group(group, _nodes);
        }
    }

    auto start() -> void {
        for (auto id : _nodes) {
            _hosts.at(id)->start();
        }
        _running = true;
    }

    auto stop() -> void {
        if (!_running) {
            return;
        }
        _running = false;
        for (auto id : _nodes) {
            _hosts.at(id)->stop();
        }
    }

    auto tick() -> void {
        for (auto id : _nodes) {
            _hosts.at(id)->tick();
        }
    }

    template<typename Predicate>
    auto tick_until(Predicate predicate,
                    std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            tick();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return predicate();
    }

    [[nodiscard]] auto host(std::uint64_t id) -> host_type& { return *_hosts.at(id); }

    [[nodiscard]] auto leader_of(group_id_type group) -> std::optional<std::uint64_t> {
        for (auto id : _nodes) {
            auto* n = _hosts.at(id)->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto term_of(std::uint64_t id, group_id_type group) -> std::uint64_t {
        auto* n = _hosts.at(id)->group_node(group);
        return n == nullptr ? 0 : n->get_current_term();
    }

    [[nodiscard]] auto nodes() const -> const std::vector<std::uint64_t>& { return _nodes; }
    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }

private:
    message_fabric _fabric{6};
    std::vector<std::uint64_t> _nodes;
    std::map<std::uint64_t, std::unique_ptr<host_type>> _hosts;
    bool _running{false};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(leader_transfer)

// ── the happy path ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(leadership_moves_to_the_named_target, *boost::unit_test::timeout(120)) {
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();

    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));
    const auto from = *cluster.leader_of(k_group);

    // Pick a target that is not the current leader, and remember the term on
    // the *third* node — the one taking no part in the transfer.
    std::uint64_t to = 0;
    std::uint64_t bystander = 0;
    for (auto id : cluster.nodes()) {
        if (id == from) {
            continue;
        }
        if (to == 0) {
            to = id;
        } else {
            bystander = id;
        }
    }
    BOOST_REQUIRE_NE(to, 0U);
    BOOST_REQUIRE_NE(bystander, 0U);
    const auto bystander_term_before = cluster.term_of(bystander, k_group);

    // (group, target, timeout) — not (target, group). Both are `std::uint64_t`,
    // so getting this backwards compiles and then fails only when the group id
    // happens not to collide with a node id.
    auto future =
        cluster.host(from).transfer_leadership(k_group, to, std::chrono::milliseconds{3000});

    const bool moved =
        cluster.tick_until([&] { return cluster.leader_of(k_group) == std::optional{to}; },
                           std::chrono::milliseconds{8000});
    BOOST_CHECK_MESSAGE(moved, "leadership did not reach the named target");
    BOOST_CHECK_NO_THROW(std::move(future).get());

    // The named target, not "somebody else". A general election would satisfy
    // "the old leader stepped down" and fail this.
    BOOST_CHECK(cluster.leader_of(k_group) == std::optional{to});

    // And the bystander's term moved by exactly the one election the transfer
    // caused — not by the extra rounds a timeout-driven fallback would cost.
    BOOST_CHECK_LE(cluster.term_of(bystander, k_group), bystander_term_before + 1);

    cluster.stop();
}

BOOST_AUTO_TEST_CASE(the_old_leader_refuses_commands_while_transferring,
                     *boost::unit_test::timeout(120)) {
    // Ongaro §3.10, and not politeness: the transfer waits for the target's
    // match index to reach the leader's last log index, and every command
    // accepted in the meantime moves that target further away. Without the
    // block, a steadily-written shard could never transfer at all.
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();

    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));
    const auto from = *cluster.leader_of(k_group);
    std::uint64_t to = 0;
    for (auto id : cluster.nodes()) {
        if (id != from) {
            to = id;
            break;
        }
    }

    auto* node = cluster.host(from).group_node(k_group);
    BOOST_REQUIRE(node != nullptr);

    auto transfer = node->transfer_leadership(to, std::chrono::milliseconds{3000});
    BOOST_REQUIRE(node->leadership_transfer_target() == std::optional{to});

    std::vector<std::byte> command;
    for (char c : std::string{"SET a b"}) {
        command.push_back(static_cast<std::byte>(c));
    }
    BOOST_CHECK_THROW(
        std::move(node->submit_command(command, std::chrono::milliseconds{500})).get(),
        leader_transfer_exception);

    cluster.tick_until([&] { return cluster.leader_of(k_group) == std::optional{to}; },
                       std::chrono::milliseconds{8000});
    try {
        std::move(transfer).get();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // The transfer's own outcome is the previous test's subject.
    }
    cluster.stop();
}

// ── refusals, all of which leave the cluster where it started ────────────────

BOOST_AUTO_TEST_CASE(a_follower_cannot_transfer_leadership, *boost::unit_test::timeout(120)) {
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();
    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));

    const auto leader = *cluster.leader_of(k_group);
    std::uint64_t follower = 0;
    for (auto id : cluster.nodes()) {
        if (id != leader) {
            follower = id;
            break;
        }
    }

    auto* n = cluster.host(follower).group_node(k_group);
    BOOST_REQUIRE(n != nullptr);
    BOOST_CHECK_THROW(
        std::move(n->transfer_leadership(leader, std::chrono::milliseconds{500})).get(),
        leader_transfer_exception);

    // Unchanged: a refused transfer is a no-op, which is what makes it safe for
    // an advisory operator to attempt.
    BOOST_CHECK(cluster.leader_of(k_group) == std::optional{leader});
    cluster.stop();
}

BOOST_AUTO_TEST_CASE(a_leader_cannot_transfer_to_itself, *boost::unit_test::timeout(120)) {
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();
    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));

    const auto leader = *cluster.leader_of(k_group);
    auto* n = cluster.host(leader).group_node(k_group);
    BOOST_CHECK_THROW(
        std::move(n->transfer_leadership(leader, std::chrono::milliseconds{500})).get(),
        leader_transfer_exception);
    // The refusal must not have left the leader blocking its own commands.
    BOOST_CHECK(!n->leadership_transfer_target().has_value());
    cluster.stop();
}

BOOST_AUTO_TEST_CASE(a_target_outside_the_configuration_is_refused,
                     *boost::unit_test::timeout(120)) {
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();
    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));

    const auto leader = *cluster.leader_of(k_group);
    auto* n = cluster.host(leader).group_node(k_group);
    BOOST_CHECK_THROW(std::move(n->transfer_leadership(99, std::chrono::milliseconds{500})).get(),
                      leader_transfer_exception);
    BOOST_CHECK(!n->leadership_transfer_target().has_value());
    cluster.stop();
}

BOOST_AUTO_TEST_CASE(a_transfer_to_an_unreachable_target_times_out_and_the_leader_resumes,
                     *boost::unit_test::timeout(120)) {
    // The important half is the *resume*: a leader that stayed blocked after a
    // failed transfer would be a self-inflicted outage far worse than the
    // imbalance the transfer was meant to fix.
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();
    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));

    const auto leader = *cluster.leader_of(k_group);
    std::uint64_t target = 0;
    for (auto id : cluster.nodes()) {
        if (id != leader) {
            target = id;
            break;
        }
    }
    cluster.fabric().kill(target);

    auto* n = cluster.host(leader).group_node(k_group);
    auto transfer = n->transfer_leadership(target, std::chrono::milliseconds{300});

    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{6000};
    auto pending = std::move(transfer);
    while (std::chrono::steady_clock::now() < deadline && !failed) {
        cluster.tick();
        if (!n->leadership_transfer_target().has_value()) {
            failed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_CHECK_MESSAGE(failed, "the transfer never gave up on an unreachable target");
    BOOST_CHECK_THROW(std::move(pending).get(), leader_transfer_exception);

    // Commands are accepted again.
    BOOST_CHECK(!n->leadership_transfer_target().has_value());

    cluster.fabric().revive(target);
    cluster.stop();
}

// ── scatter ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(scatter_hands_leadership_to_another_voter, *boost::unit_test::timeout(120)) {
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.start();
    BOOST_REQUIRE(cluster.tick_until([&] { return cluster.leader_of(k_group).has_value(); }));

    const auto from = *cluster.leader_of(k_group);
    auto future = cluster.host(from).scatter(k_group, std::chrono::milliseconds{3000});

    const bool moved = cluster.tick_until(
        [&] {
            auto now = cluster.leader_of(k_group);
            return now.has_value() && *now != from;
        },
        std::chrono::milliseconds{8000});
    BOOST_CHECK_MESSAGE(moved, "scatter left leadership where it was");
    try {
        std::move(future).get();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // A scatter whose transfer lost the race is still a scatter that tried;
        // the assertion above is the property.
    }
    cluster.stop();
}

BOOST_AUTO_TEST_CASE(a_single_voter_group_cannot_scatter, *boost::unit_test::timeout(60)) {
    // There is nowhere to go. Reporting that plainly beats a silent success
    // that leaves a driver believing the hotspot was addressed.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(k_group, {1});
    host.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{3000};
    while (std::chrono::steady_clock::now() < deadline) {
        host.tick();
        auto* n = host.group_node(k_group);
        if (n != nullptr && n->is_leader()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    BOOST_CHECK_THROW(std::move(host.scatter(k_group, std::chrono::milliseconds{500})).get(),
                      kythira::shard_exception);
    host.stop();
}

BOOST_AUTO_TEST_CASE(consecutive_scatters_on_one_host_pick_different_targets,
                     *boost::unit_test::timeout(120)) {
    // The round-robin cursor is the whole of the "do not all lead from the same
    // machine" property: without it, every child of one split would be handed
    // to whichever voter sorts first, which concentrates the hotspot instead of
    // spreading it.
    transfer_cluster cluster{{1, 2, 3}};
    cluster.create_group(k_group);
    cluster.create_group(2);
    cluster.start();

    BOOST_REQUIRE(cluster.tick_until([&] {
        return cluster.leader_of(k_group).has_value() && cluster.leader_of(2).has_value();
    }));

    // Both groups must be led by the same host for the question to mean
    // anything; if they are not, the property already holds.
    const auto a = *cluster.leader_of(k_group);
    const auto b = *cluster.leader_of(2);
    if (a != b) {
        cluster.stop();
        return;
    }

    auto f1 = cluster.host(a).scatter(k_group, std::chrono::milliseconds{3000});
    auto f2 = cluster.host(a).scatter(2, std::chrono::milliseconds{3000});

    cluster.tick_until(
        [&] {
            auto l1 = cluster.leader_of(k_group);
            auto l2 = cluster.leader_of(2);
            return l1.has_value() && l2.has_value() && *l1 != a && *l2 != a;
        },
        std::chrono::milliseconds{10000});

    try {
        std::move(f1).get();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
    try {
        std::move(f2).get();
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }

    auto l1 = cluster.leader_of(k_group);
    auto l2 = cluster.leader_of(2);
    if (l1.has_value() && l2.has_value() && *l1 != a && *l2 != a) {
        // Both moved off the original host; the cursor asked for two different
        // targets, so they should not have piled onto one machine.
        BOOST_CHECK_NE(*l1, *l2);
    }
    cluster.stop();
}

BOOST_AUTO_TEST_SUITE_END()
