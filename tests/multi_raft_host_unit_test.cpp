// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_host_unit_test.cpp
/// @brief Unit tests for the `multi_raft` host skeleton: registry and
///        lifecycle (task 9), the batched tick (task 11), and hibernation
///        (task 12) of `.kiro/specs/multi-raft/`.
///
/// The groups here are single-node clusters. That is not a shortcut around the
/// transport — it is what isolates the properties under test. A single-node
/// group elects itself with no RPC at all, so "five groups reach five
/// independent terms and leaders" is a statement about the *registry* and the
/// *tick*, not about replication. Multi-node behaviour over a shared transport
/// is task 14's integration test.

#define BOOST_TEST_MODULE multi_raft_host_unit_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
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
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_host_unit_test"), nullptr};
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

/// Drive `host` until `predicate` holds or `budget` elapses.
///
/// A predicate rather than a fixed sleep: an election takes a randomised
/// number of ticks, and a fixed count would either be flaky or slow.
template<typename Predicate>
auto tick_until(host_type& host, Predicate predicate,
                std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        host.tick();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_host_unit)

// ── task 9: registry and lifecycle ───────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_missing_store_factory_is_refused_at_construction) {
    // A store is scoped by its construction argument, so there is no prototype
    // to copy — failing at construction beats failing on the first group.
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.store_factory = nullptr;
    BOOST_CHECK_THROW(host_type{std::move(cfg)}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(five_groups_reach_five_independent_leaders_and_terms,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};

    for (group_id_type g = 1; g <= 5; ++g) {
        host.create_group(g, {1});
    }
    BOOST_CHECK_EQUAL(host.group_count(), 5u);
    host.start();

    const bool all_leaders = tick_until(host, [&] {
        for (group_id_type g = 1; g <= 5; ++g) {
            auto* n = host.group_node(g);
            if (n == nullptr || !n->is_leader()) {
                return false;
            }
        }
        return true;
    });
    BOOST_REQUIRE(all_leaders);

    // Independent: each group has its own term, advanced by its own election,
    // and nothing about group 3 is visible in group 4.
    for (group_id_type g = 1; g <= 5; ++g) {
        auto* n = host.group_node(g);
        BOOST_REQUIRE(n != nullptr);
        BOOST_CHECK(n->is_leader());
        BOOST_CHECK_GE(n->get_current_term(), 1u);
    }

    host.stop();
}

BOOST_AUTO_TEST_CASE(groups_are_spread_across_stripes_not_given_a_thread_each,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.executor_stripes = 4;
    host_type host{std::move(cfg)};

    for (group_id_type g = 1; g <= 40; ++g) {
        host.create_group(g, {1});
    }
    BOOST_CHECK_EQUAL(host.executor_stripe_count(), 4u);

    std::set<std::size_t> used;
    for (group_id_type g = 1; g <= 40; ++g) {
        const auto s = host.stripe_of(g);
        BOOST_CHECK_LT(s, 4u);
        used.insert(s);
    }
    // With 40 groups over 4 stripes, every stripe should carry work; a mapping
    // that collapsed everything onto one stripe would still "pass" a per-group
    // lookup test.
    BOOST_CHECK_EQUAL(used.size(), 4u);
    BOOST_CHECK_EQUAL(host.stripe_of(999), kythira::striped_serial_executor::npos);
}

BOOST_AUTO_TEST_CASE(creating_a_group_twice_is_refused) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(1, {1});
    BOOST_CHECK_THROW(host.create_group(1, {1}), kythira::shard_exception);
    BOOST_CHECK_EQUAL(host.group_count(), 1u);
}

BOOST_AUTO_TEST_CASE(destroying_a_group_tombstones_it_and_removes_its_routing_row,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(1, {1});
    host.create_group(2, {1});
    host.start();

    BOOST_CHECK(host.has_group(2));
    BOOST_CHECK(!host.is_tombstoned(2));

    BOOST_CHECK(host.destroy_group(2, tombstone_reason::merged_away));
    BOOST_CHECK(!host.has_group(2));
    BOOST_CHECK(host.is_tombstoned(2));
    BOOST_CHECK(!host.shard_map_snapshot().find(2).has_value());
    BOOST_CHECK_EQUAL(host.group_count(), 1u);

    // Destroying it again reports nothing to do rather than throwing.
    BOOST_CHECK(!host.destroy_group(2, tombstone_reason::merged_away));

    // The surviving group is unaffected and still driveable.
    BOOST_REQUIRE(host.group_node(1) != nullptr);
    host.tick();
    host.stop();
}

BOOST_AUTO_TEST_CASE(recreating_a_tombstoned_group_clears_its_tombstone) {
    // The placement driver may put a fresh replica of a merged-away group back
    // on this node. Leaving the tombstone would have the transport silently
    // drop every message for it.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(1, {1});
    host.destroy_group(1, tombstone_reason::replica_removed);
    BOOST_REQUIRE(host.is_tombstoned(1));

    host.create_group(1, {1});
    BOOST_CHECK(!host.is_tombstoned(1));
    BOOST_CHECK(host.has_group(1));
}

BOOST_AUTO_TEST_CASE(stop_is_synchronous_and_start_stop_start_stop_works,
                     *boost::unit_test::timeout(60)) {
    // The same regression node::stop() itself guards against, one level up.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    for (group_id_type g = 1; g <= 3; ++g) {
        host.create_group(g, {1});
    }

    host.start();
    BOOST_CHECK(host.is_running());
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    host.stop();
    BOOST_CHECK(!host.is_running());
    // Idempotent.
    host.stop();

    host.start();
    BOOST_CHECK(host.is_running());
    host.tick();
    host.stop();
    BOOST_CHECK(!host.is_running());
}

BOOST_AUTO_TEST_CASE(tombstones_survive_a_host_restart_when_a_data_dir_is_configured) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("kythira-multiraft-host-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);

    message_fabric fabric{2};
    {
        auto cfg = make_config(fabric, 1);
        cfg.host_data_dir = dir;
        host_type host{std::move(cfg)};
        host.create_group(7, {1});
        host.destroy_group(7, tombstone_reason::merged_away);
        BOOST_CHECK(host.is_tombstoned(7));
    }
    {
        auto cfg = make_config(fabric, 1);
        cfg.host_data_dir = dir;
        host_type host{std::move(cfg)};
        BOOST_CHECK(host.is_tombstoned(7));
        BOOST_CHECK(!host.is_tombstoned(8));
    }
    std::filesystem::remove_all(dir);
}

// ── task 11: the batched tick ────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_tick_report_counts_ready_and_hibernating_groups) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    for (group_id_type g = 1; g <= 6; ++g) {
        host.create_group(g, {1});
    }
    host.start();

    const auto report = host.tick();
    BOOST_CHECK_EQUAL(report._total_count, 6u);
    BOOST_CHECK_EQUAL(report._ready_count, 6u);
    BOOST_CHECK_EQUAL(report._hibernating_count, 0u);
    host.stop();
}

BOOST_AUTO_TEST_CASE(n_ready_groups_produce_one_durability_barrier_per_tick) {
    // The claim the batch controller exists for. Without it the persist phase
    // pays one barrier per ready group; a single barrier for N groups requires
    // a store that spans N groups, and no wrapper can manufacture one from N
    // independent engines.
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);

    std::atomic<int> begins{0};
    std::atomic<int> commits{0};
    std::atomic<int> aborts{0};
    cfg.batch_controller = tick_batch_controller{
        ._begin = [&] { begins.fetch_add(1); },
        ._commit = [&] { commits.fetch_add(1); },
        ._abort = [&] { aborts.fetch_add(1); },
    };

    host_type host{std::move(cfg)};
    for (group_id_type g = 1; g <= 12; ++g) {
        host.create_group(g, {1});
    }
    host.start();

    const auto report = host.tick();
    BOOST_CHECK_EQUAL(report._batch_size, 12u);
    BOOST_CHECK_EQUAL(begins.load(), 1);
    BOOST_CHECK_EQUAL(commits.load(), 1);
    BOOST_CHECK_EQUAL(aborts.load(), 0);

    host.tick();
    BOOST_CHECK_EQUAL(begins.load(), 2);
    BOOST_CHECK_EQUAL(commits.load(), 2);
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_tick_with_no_ready_groups_opens_no_batch) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    std::atomic<int> begins{0};
    cfg.batch_controller = tick_batch_controller{
        ._begin = [&] { begins.fetch_add(1); },
        ._commit = [] {},
        ._abort = [] {},
    };
    host_type host{std::move(cfg)};
    host.start();
    const auto report = host.tick();
    BOOST_CHECK_EQUAL(report._ready_count, 0u);
    BOOST_CHECK_EQUAL(report._batch_size, 0u);
    BOOST_CHECK_EQUAL(begins.load(), 0);
    host.stop();
}

BOOST_AUTO_TEST_CASE(the_apply_phase_runs_deferred_work_on_the_groups_own_stripe) {
    // Phase 3 exists so that Phase 6's admin-entry dispatch has a place that is
    // already ordered after the send phase and already on the right stripe.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(3, {1});
    host.start();

    std::atomic<std::size_t> observed_stripe{kythira::striped_serial_executor::npos};
    std::atomic<int> ran{0};
    BOOST_REQUIRE(host.defer_to_apply_phase(3, [&] {
        observed_stripe = kythira::striped_serial_executor::current_stripe();
        ran.fetch_add(1);
    }));
    BOOST_CHECK_EQUAL(ran.load(), 0);  // not yet — the tick runs it

    host.tick();
    BOOST_CHECK_EQUAL(ran.load(), 1);
    BOOST_CHECK_EQUAL(observed_stripe.load(), host.stripe_of(3));

    // Drained, not repeated.
    host.tick();
    BOOST_CHECK_EQUAL(ran.load(), 1);

    BOOST_CHECK(!host.defer_to_apply_phase(99, [] {}));
    host.stop();
}

BOOST_AUTO_TEST_CASE(the_phases_run_in_persist_send_apply_order) {
    // Ordering is TiKV's and matters: persist before send (never advertise an
    // append that is not durably taken), apply after send (a follower's copy is
    // on the wire before the leader spends time in the state machine).
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);

    std::vector<std::string> trace;
    std::mutex trace_mutex;
    cfg.batch_controller = tick_batch_controller{
        ._begin =
            [&] {
                std::lock_guard lock(trace_mutex);
                trace.emplace_back("persist-begin");
            },
        ._commit =
            [&] {
                std::lock_guard lock(trace_mutex);
                trace.emplace_back("persist-commit");
            },
        ._abort = [] {},
    };

    host_type host{std::move(cfg)};
    host.create_group(1, {1});
    host.start();
    host.defer_to_apply_phase(1, [&] {
        std::lock_guard lock(trace_mutex);
        trace.emplace_back("apply");
    });
    host.tick();

    std::lock_guard lock(trace_mutex);
    BOOST_REQUIRE_EQUAL(trace.size(), 3u);
    BOOST_CHECK_EQUAL(trace[0], "persist-begin");
    BOOST_CHECK_EQUAL(trace[1], "persist-commit");
    BOOST_CHECK_EQUAL(trace[2], "apply");
    host.stop();
}

BOOST_AUTO_TEST_CASE(the_policy_phase_runs_on_its_own_interval_not_every_tick) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.policy_interval = std::chrono::milliseconds{200};
    host_type host{std::move(cfg)};
    host.create_group(1, {1});
    host.start();

    // The first tick after construction is inside the interval.
    BOOST_CHECK(!host.tick()._policy_ran);
    BOOST_CHECK(!host.tick()._policy_ran);

    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    BOOST_CHECK(host.tick()._policy_ran);
    BOOST_CHECK(!host.tick()._policy_ran);
    host.stop();
}

// ── task 12: hibernation ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(hibernation_is_off_below_the_auto_threshold) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::auto_above_group_count;
    cfg.hibernation_group_threshold = 10;
    cfg.hibernate_after = std::chrono::milliseconds{1};
    host_type host{std::move(cfg)};

    for (group_id_type g = 1; g <= 5; ++g) {
        host.create_group(g, {1});
    }
    host.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    host.tick();
    BOOST_CHECK_EQUAL(host.hibernating_count(), 0u);
    host.stop();
}

BOOST_AUTO_TEST_CASE(idle_groups_converge_to_hibernating_and_are_skipped_by_the_tick,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::on;
    cfg.hibernate_after = std::chrono::milliseconds{20};
    host_type host{std::move(cfg)};

    constexpr group_id_type group_count = 30;
    for (group_id_type g = 1; g <= group_count; ++g) {
        host.create_group(g, {1});
    }
    host.start();

    kythira::tick_report last;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        last = host.tick();
        if (last._hibernating_count == group_count) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    BOOST_CHECK_EQUAL(host.hibernating_count(), group_count);
    // And the next tick therefore drives nothing — which is the whole point:
    // tick cost tracks ready groups, not total groups.
    const auto quiet = host.tick();
    BOOST_CHECK_EQUAL(quiet._ready_count, 0u);
    BOOST_CHECK_EQUAL(quiet._hibernating_count, group_count);
    BOOST_CHECK_EQUAL(quiet._total_count, group_count);
    host.stop();
}

BOOST_AUTO_TEST_CASE(waking_one_group_wakes_exactly_that_group, *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::on;
    cfg.hibernate_after = std::chrono::milliseconds{20};
    host_type host{std::move(cfg)};

    for (group_id_type g = 1; g <= 8; ++g) {
        host.create_group(g, {1});
    }
    host.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline && host.hibernating_count() < 8) {
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_REQUIRE_EQUAL(host.hibernating_count(), 8u);

    BOOST_CHECK(host.wake(4));
    BOOST_CHECK(!host.is_hibernating(4));
    BOOST_CHECK_EQUAL(host.hibernating_count(), 7u);

    const auto report = host.tick();
    BOOST_CHECK_EQUAL(report._ready_count, 1u);
    BOOST_CHECK_EQUAL(report._hibernating_count, 7u);

    // Waking a group that is already awake reports that it was not hibernating.
    BOOST_CHECK(!host.wake(4));
    BOOST_CHECK(!host.wake(999));
    host.stop();
}

BOOST_AUTO_TEST_CASE(deferred_work_wakes_a_hibernating_group, *boost::unit_test::timeout(60)) {
    // A hibernating group with queued work would otherwise never be selected to
    // run it — the work would sit there until something else happened to wake
    // the group.
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::on;
    cfg.hibernate_after = std::chrono::milliseconds{20};
    host_type host{std::move(cfg)};
    host.create_group(1, {1});
    host.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline && !host.is_hibernating(1)) {
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_REQUIRE(host.is_hibernating(1));

    std::atomic<int> ran{0};
    BOOST_REQUIRE(host.defer_to_apply_phase(1, [&] { ran.fetch_add(1); }));
    BOOST_CHECK(!host.is_hibernating(1));
    host.tick();
    BOOST_CHECK_EQUAL(ran.load(), 1);
    host.stop();
}

BOOST_AUTO_TEST_CASE(hibernation_off_never_hibernates_anything) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::off;
    cfg.hibernate_after = std::chrono::milliseconds{1};
    host_type host{std::move(cfg)};
    for (group_id_type g = 1; g <= 100; ++g) {
        host.create_group(g, {1});
    }
    host.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto report = host.tick();
    BOOST_CHECK_EQUAL(report._hibernating_count, 0u);
    BOOST_CHECK_EQUAL(report._ready_count, 100u);
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_woken_group_still_elects_a_leader, *boost::unit_test::timeout(60)) {
    // Hibernation must not break liveness. It does change *when* liveness is
    // restored: a hibernating group does not campaign until something wakes it,
    // which is TiKV's own semantics and its stated trade. What must hold is
    // that a wake is followed by an election within the normal bound.
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.hibernation = hibernation_mode::on;
    cfg.hibernate_after = std::chrono::milliseconds{20};
    host_type host{std::move(cfg)};
    host.create_group(1, {1});
    host.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline && !host.is_hibernating(1)) {
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    BOOST_REQUIRE(host.is_hibernating(1));

    host.wake(1);
    const bool elected = tick_until(
        host, [&] { return host.group_node(1)->is_leader(); }, std::chrono::milliseconds{2000});
    BOOST_CHECK(elected);
    host.stop();
}

BOOST_AUTO_TEST_SUITE_END()
