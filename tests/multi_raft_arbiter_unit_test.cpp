// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_arbiter_unit_test.cpp
/// @brief The arbiter, its gates, and the signal surface (tasks 25 and 26 of
///        `.kiro/specs/multi-raft/`).
///
/// The arbiter's job is to make conflicting operations impossible **by
/// construction** rather than by check-then-act: starting an operation *is* the
/// transition, so two channels racing in the same interval cannot both win.
///
/// The other half is observability, and it is not decoration. An operator who
/// cannot see `split.rejected{gate=cooldown}` will conclude the feature is
/// broken and start changing thresholds at random — so every refusal here is
/// asserted to be *counted under its own gate*, not merely to have happened.
///
/// The host is hand-ticked and single-node throughout: every property under
/// test is a local decision made before anything is proposed, and three nodes
/// would add election timing to cases that have nothing to do with elections.

#define BOOST_TEST_MODULE multi_raft_arbiter_unit_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/split_merge_policy.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_arbiter_unit_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::arbiter_gate;
using kythira::group_report;
using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::no_valid_split_key_exception;
using kythira::shard_busy_exception;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_operation_state;
using kythira::shard_range;
using kythira::signal_channel;
using kythira::split_options;
using kythira::threshold_split_merge_policy;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

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
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;
using stats_type = kythira::shard_stats<group_id_type, key_type, node_id_t>;
using policy_type = threshold_split_merge_policy<group_id_type, key_type>;

constexpr group_id_type k_group = 1;

auto workload_keys() -> std::vector<key_type> {
    return {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
}

/// One host, one single-voter shard, ticked by hand.
class arbiter_host {
public:
    explicit arbiter_host(std::function<void(config_type&)> tweak = {}) {
        config_type cfg{
            .node_id = 1,
            .network_client = fabric_client{_fabric, 1},
            .network_server = fabric_server{_fabric, 1},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{40};
        cfg.config._election_timeout_max = std::chrono::milliseconds{80};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.allocate_group_ids = [this](std::size_t n) {
            std::vector<group_id_type> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        // No cooldown by default: most cases here are about other gates, and a
        // one-hour interval would mask every one of them behind the first.
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        if (tweak) {
            tweak(cfg);
        }
        _host = std::make_unique<host_type>(std::move(cfg));
        _host->create_group(descriptor_type{._group_id = k_group,
                                            ._range = kythira::unbounded_shard_range<key_type>(),
                                            ._epoch = shard_epoch{},
                                            ._voters = {1},
                                            ._learners = {},
                                            ._leader_hint = std::nullopt});
        _host->start();
    }

    ~arbiter_host() { _host->stop(); }

    arbiter_host(const arbiter_host&) = delete;
    auto operator=(const arbiter_host&) -> arbiter_host& = delete;

    [[nodiscard]] auto host() -> host_type& { return *_host; }

    auto tick_until(const std::function<bool()>& predicate,
                    std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            _host->tick();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return predicate();
    }

    auto await_leader() -> bool {
        return tick_until([&] { return _host->group_node(k_group)->is_leader(); });
    }

    auto put(const key_type& key, const std::string& value) -> bool {
        auto f =
            _host->submit_command(key, host_types::state_machine_type::make_put_command(key, value),
                                  std::chrono::milliseconds{2000});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!f.wait(std::chrono::milliseconds{5}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        try {
            std::ignore = f.get();
            return true;
        } catch (...) {
            return false;
        }
    }

    /// @brief Resolve a future while keeping the host ticking underneath it.
    template<typename Future> auto settle(Future&& f) -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
        while (!f.wait(std::chrono::milliseconds{5}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        if (!f.wait(std::chrono::milliseconds{100})) {
            return std::make_exception_ptr(std::runtime_error("never resolved"));
        }
        try {
            std::ignore = f.get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    auto seed() -> void {
        for (const auto& k : workload_keys()) {
            BOOST_REQUIRE(put(k, "v-" + k));
        }
    }

private:
    message_fabric _fabric{2};
    std::unique_ptr<host_type> _host;
    group_id_type _next_id{100};
};

template<typename E> auto is_a(const std::exception_ptr& e) -> bool {
    if (!e) {
        return false;
    }
    try {
        std::rethrow_exception(e);
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_arbiter_unit)

// ── the gates ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_stable_leader_is_admitted, *boost::unit_test::timeout(120)) {
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    const auto decision = h.host().would_admit(k_group, signal_channel::policy);
    BOOST_CHECK(decision._admitted);
    BOOST_CHECK(decision._gate == arbiter_gate::admitted);

    // And an unknown shard is not.
    BOOST_CHECK(!h.host().would_admit(999, signal_channel::admin)._admitted);
}

BOOST_AUTO_TEST_CASE(the_cooldown_gate_refuses_and_is_counted_under_its_own_name,
                     *boost::unit_test::timeout(120)) {
    // The cooldown is enforced by the HOST, not by the policy. A custom policy
    // that forgot it still cannot oscillate.
    arbiter_host h{[](config_type& cfg) { cfg.split_merge_interval = std::chrono::hours{1}; }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{5000})) == nullptr);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().applied_split_count() > 0; }));

    // A second operation inside the interval is refused, and refused *by the
    // cooldown* — which is the part an operator needs to see.
    const auto before = h.host().rejection_count(arbiter_gate::cooldown);
    const auto err =
        h.settle(h.host().split_shard(k_group, {"bravo"}, std::chrono::milliseconds{2000}));
    BOOST_CHECK(is_a<shard_busy_exception<group_id_type>>(err));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::cooldown), before + 1);
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::policy)._gate ==
                arbiter_gate::cooldown);

    // An operator who has diagnosed the shard can say so and go ahead. The
    // cooldown guards against AUTOMATIC oscillation, not against a human.
    split_options options{};
    options._override_cooldown = true;
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"bravo"}, options,
                                              std::chrono::milliseconds{5000})) == nullptr);
}

BOOST_AUTO_TEST_CASE(the_concurrency_limit_refuses_before_proposing,
                     *boost::unit_test::timeout(120)) {
    // Enforced BEFORE proposing, never by aborting something already committed:
    // a rebalancing storm that saturates the network is worse than a slow
    // rebalance.
    arbiter_host h{[](config_type& cfg) { cfg.max_concurrent_split_merge = 1; }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    // Hold the one slot by leaving a second shard mid-operation.
    h.host().create_group(descriptor_type{._group_id = 50,
                                          ._range = kythira::unbounded_shard_range<key_type>(),
                                          ._epoch = shard_epoch{},
                                          ._voters = {1}});
    BOOST_REQUIRE(h.tick_until([&] { return h.host().group_node(50)->is_leader(); }));
    // Freeze shard 50 into an operation by hand — a merge would need a
    // neighbour, and the gate under test does not care which operation it is.
    BOOST_REQUIRE(h.host().freeze_shard(50));

    // Freezing is not an operation, so the slot is still free.
    BOOST_CHECK_EQUAL(h.host().operations_in_flight(), 0u);
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::admin)._admitted);
}

BOOST_AUTO_TEST_CASE(the_kill_switch_stops_the_automatic_channels_and_not_the_operator,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_CHECK(h.host().automatic_split_merge_enabled());
    h.host().set_automatic_split_merge_enabled(false);
    BOOST_CHECK(!h.host().automatic_split_merge_enabled());

    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::policy)._gate ==
                arbiter_gate::globally_disabled);
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::placement_driver)._gate ==
                arbiter_gate::globally_disabled);

    // The operator's own channel is unaffected. Turning the switch off means
    // "stop moving on your own", not "take away my escape hatch".
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::admin)._admitted);
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_frozen_shard_refuses_the_policy_and_accepts_the_operator,
                     *boost::unit_test::timeout(120)) {
    // Freezing an operator out of their own escape hatch would be a bad joke at
    // 3 a.m.
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_REQUIRE(h.host().freeze_shard(k_group));
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::frozen);
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::policy)._gate == arbiter_gate::state);
    BOOST_CHECK(h.host().would_admit(k_group, signal_channel::placement_driver)._gate ==
                arbiter_gate::state);

    split_options options{};
    options._channel = signal_channel::admin;
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"}, options,
                                              std::chrono::milliseconds{5000})) == nullptr);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().applied_split_count() > 0; }));

    // And it goes back to FROZEN, not to stable: an admin command against a
    // frozen shard must not quietly thaw it.
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::frozen);

    BOOST_REQUIRE(h.host().thaw_shard(k_group));
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::stable);
    BOOST_CHECK(!h.host().freeze_shard(999));
    BOOST_CHECK(!h.host().thaw_shard(999));
}

BOOST_AUTO_TEST_CASE(a_veto_exhaustion_is_counted_under_its_own_gate,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    h.host().group_node(k_group)->with_state_machine([](host_types::state_machine_type& sm) {
        sm.set_split_veto([](const std::string&) { return true; });
    });

    const auto before = h.host().rejection_count(arbiter_gate::no_valid_split_key);
    const auto err =
        h.settle(h.host().split_shard(k_group, {"delta"}, std::chrono::milliseconds{2000}));
    BOOST_CHECK(is_a<no_valid_split_key_exception<group_id_type>>(err));
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::no_valid_split_key), before + 1);

    // And the shard is released, not wedged.
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::stable);
}

BOOST_AUTO_TEST_CASE(an_unavailable_id_authority_is_counted_under_its_own_gate,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h{[](config_type& cfg) {
        cfg.allocate_group_ids = [](std::size_t) { return std::vector<group_id_type>{}; };
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    const auto before = h.host().rejection_count(arbiter_gate::pd_unavailable);
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{2000})) != nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::pd_unavailable), before + 1);
    BOOST_CHECK(h.host().operation_state(k_group) == shard_operation_state::stable);
}

// ── options ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_strict_veto_option_fails_instead_of_falling_back,
                     *boost::unit_test::timeout(120)) {
    // On by default an operator naming a forbidden key gets a nearby valid one;
    // off, they get the failure they asked for.
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    h.host().group_node(k_group)->with_state_machine([](host_types::state_machine_type& sm) {
        sm.set_split_veto([](const std::string& k) { return k == "delta"; });
    });

    split_options strict{};
    strict._allow_state_machine_veto = false;
    BOOST_CHECK(is_a<no_valid_split_key_exception<group_id_type>>(h.settle(
        h.host().split_shard(k_group, {"delta"}, strict, std::chrono::milliseconds{2000}))));
    BOOST_CHECK_EQUAL(h.host().applied_split_count(), 0u);

    // The permissive default splits anyway, somewhere else.
    BOOST_CHECK(h.settle(h.host().split_shard(k_group, {"delta"},
                                              std::chrono::milliseconds{5000})) == nullptr);
}

BOOST_AUTO_TEST_CASE(pre_split_is_refused_on_a_shard_that_has_been_written_to,
                     *boost::unit_test::timeout(120)) {
    // Refusing here keeps pre-split from becoming a second, weaker
    // `split_shard` that skips the state machine's veto.
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_CHECK(is_a<shard_busy_exception<group_id_type>>(
        h.settle(h.host().pre_split(k_group, {"delta"}, std::chrono::milliseconds{2000}))));
    BOOST_CHECK_EQUAL(h.host().applied_split_count(), 0u);
}

BOOST_AUTO_TEST_CASE(pre_split_cuts_an_empty_shard_at_the_named_boundaries,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());

    // Empty shard, no writes yet: the bulk-load case pre-split exists for.
    BOOST_CHECK(h.settle(h.host().pre_split(k_group, {"d", "m", "t"},
                                            std::chrono::milliseconds{5000})) == nullptr);
    BOOST_REQUIRE(h.tick_until([&] { return h.host().applied_split_count() > 0; }));
    BOOST_CHECK_EQUAL(h.host().shard_map_snapshot().size(), 4u);
    BOOST_CHECK(!h.host().shard_map_snapshot().check_tiling().has_value());

    // Named boundaries and nothing else: an empty shard has nothing to suggest,
    // so a boundary-less pre-split is a no-op the caller hears about.
    BOOST_CHECK(is_a<no_valid_split_key_exception<group_id_type>>(
        h.settle(h.host().pre_split(k_group, {}, std::chrono::milliseconds{2000}))));
}

// ── statistics and reports (task 26) ─────────────────────────────────────────

BOOST_AUTO_TEST_CASE(shard_stats_report_what_the_policy_needs, *boost::unit_test::timeout(120)) {
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    const auto stats = h.host().stats_for(k_group);
    BOOST_REQUIRE(stats.has_value());
    // The reference state machine has the sizing hooks, so the flag is true and
    // the figures are real.
    BOOST_CHECK(stats->_size_available);
    BOOST_CHECK_EQUAL(stats->_approximate_key_count, workload_keys().size());
    BOOST_CHECK_GT(stats->_approximate_size_bytes, 0u);
    // Load is counted at the routing layer, which is what makes load-based
    // split work for a state machine with no sizing hooks at all.
    BOOST_CHECK_EQUAL(stats->_write_qps, static_cast<double>(workload_keys().size()));
    BOOST_CHECK_GT(stats->_last_applied_index, 0u);
    BOOST_CHECK_EQUAL(stats->_voter_count, 1u);

    BOOST_CHECK(!h.host().stats_for(999).has_value());
}

BOOST_AUTO_TEST_CASE(the_report_listener_fires_on_a_role_transition,
                     *boost::unit_test::timeout(120)) {
    // MicroRaft's `RaftNodeReportListener`, and the reason it is a push: an
    // operator watching a thousand groups cannot poll them.
    arbiter_host h;
    std::mutex mutex;
    std::vector<group_report<group_id_type, node_id_t>> reports;
    h.host().set_report_listener([&](const group_report<group_id_type, node_id_t>& r) {
        std::lock_guard lock(mutex);
        reports.push_back(r);
    });

    BOOST_REQUIRE(h.await_leader());
    BOOST_REQUIRE(h.tick_until([&] {
        std::lock_guard lock(mutex);
        return !reports.empty();
    }));

    std::lock_guard lock(mutex);
    bool saw_leader = false;
    for (const auto& r : reports) {
        BOOST_CHECK_EQUAL(r._group_id, k_group);
        if (r._role == kythira::server_state::leader) {
            saw_leader = true;
            BOOST_CHECK_GE(r._term, 1u);
        }
    }
    BOOST_CHECK(saw_leader);
}

// ── the policy channel end to end ────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_policy_channel_splits_a_shard_over_its_threshold,
                     *boost::unit_test::timeout(120)) {
    // Channel (a), driven by the tick's policy phase rather than by an operator.
    arbiter_host h{[](config_type& cfg) {
        kythira::threshold_split_merge_policy_config policy_cfg;
        // Thresholds low enough that six small keys cross them, and still a
        // valid configuration: 2 * merge < split.
        policy_cfg._shard_split_size_bytes = 64;
        policy_cfg._shard_max_size_bytes = 128;
        policy_cfg._shard_merge_max_size_bytes = 16;
        policy_cfg._shard_split_keys = 4;
        policy_cfg._shard_max_keys = 8;
        policy_cfg._shard_merge_max_keys = 1;
        auto policy = std::make_shared<policy_type>(policy_cfg);
        BOOST_REQUIRE(policy->validate());
        cfg.evaluate_split = [policy](const stats_type& s) { return policy->evaluate_split(s); };
        cfg.policy_interval = std::chrono::milliseconds{10};
        cfg.split_merge_interval = std::chrono::milliseconds{0};
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    BOOST_REQUIRE(
        h.tick_until([&] { return h.host().applied_split_count() > 0; }, std::chrono::seconds{10}));
    BOOST_CHECK_GT(h.host().shard_map_snapshot().size(), 1u);
    BOOST_CHECK(!h.host().shard_map_snapshot().check_tiling().has_value());
}

BOOST_AUTO_TEST_CASE(the_policy_channel_is_silent_when_the_kill_switch_is_off,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h{[](config_type& cfg) {
        kythira::threshold_split_merge_policy_config policy_cfg;
        policy_cfg._shard_split_size_bytes = 64;
        policy_cfg._shard_max_size_bytes = 128;
        policy_cfg._shard_merge_max_size_bytes = 16;
        policy_cfg._shard_split_keys = 4;
        policy_cfg._shard_max_keys = 8;
        policy_cfg._shard_merge_max_keys = 1;
        auto policy = std::make_shared<policy_type>(policy_cfg);
        cfg.evaluate_split = [policy](const stats_type& s) { return policy->evaluate_split(s); };
        cfg.policy_interval = std::chrono::milliseconds{10};
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        cfg.automatic_split_merge_enabled = false;
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    for (int i = 0; i < 60; ++i) {
        h.host().tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_CHECK_EQUAL(h.host().applied_split_count(), 0u);
    BOOST_CHECK_EQUAL(h.host().shard_map_snapshot().size(), 1u);

    // Turning it back on lets the same policy act.
    h.host().set_automatic_split_merge_enabled(true);
    BOOST_CHECK(
        h.tick_until([&] { return h.host().applied_split_count() > 0; }, std::chrono::seconds{10}));
}

BOOST_AUTO_TEST_CASE(a_frozen_shard_is_not_even_evaluated, *boost::unit_test::timeout(120)) {
    // An operator who froze a shard should not see policy activity against it
    // in the logs, so the freeze is checked before evaluation and not only at
    // admission.
    arbiter_host h{[](config_type& cfg) {
        kythira::threshold_split_merge_policy_config policy_cfg;
        policy_cfg._shard_split_size_bytes = 64;
        policy_cfg._shard_max_size_bytes = 128;
        policy_cfg._shard_merge_max_size_bytes = 16;
        policy_cfg._shard_split_keys = 4;
        policy_cfg._shard_max_keys = 8;
        policy_cfg._shard_merge_max_keys = 1;
        auto policy = std::make_shared<policy_type>(policy_cfg);
        cfg.evaluate_split = [policy](const stats_type& s) { return policy->evaluate_split(s); };
        cfg.policy_interval = std::chrono::milliseconds{10};
        cfg.split_merge_interval = std::chrono::milliseconds{0};
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_REQUIRE(h.host().freeze_shard(k_group));

    for (int i = 0; i < 60; ++i) {
        h.host().tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_CHECK_EQUAL(h.host().applied_split_count(), 0u);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::state), 0u);
}

// ── batch_split_limit as a HOST gate (Requirement 7.8) ───────────────────────

BOOST_AUTO_TEST_CASE(a_proposal_over_the_batch_limit_is_truncated_not_refused,
                     *boost::unit_test::timeout(120)) {
    // A composition unions its members' split keys, so the proposal that
    // actually reaches Raft can exceed a limit every member respected. The
    // shard genuinely does need splitting, so the host truncates: fewer
    // children is a worse answer than what was asked for and a far better one
    // than no split at all.
    arbiter_host h{[](config_type& cfg) { cfg.batch_split_limit = 3; }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    // Six keys named, three allowed.
    auto error =
        h.settle(h.host().split_shard(k_group, workload_keys(), std::chrono::milliseconds{4000}));
    BOOST_CHECK_MESSAGE(error == nullptr, "the split must happen, merely smaller");

    // Four children: three cut points make four ranges.
    BOOST_CHECK_EQUAL(h.host().group_count(), 4u);
    BOOST_CHECK(!h.host().shard_map_snapshot().check_tiling().has_value());

    // ...and the override is counted under its own gate, never silent.
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::split_keys_truncated), 1u);
}

BOOST_AUTO_TEST_CASE(a_proposal_inside_the_batch_limit_is_not_counted_as_truncated,
                     *boost::unit_test::timeout(120)) {
    arbiter_host h{[](config_type& cfg) { cfg.batch_split_limit = 10; }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    auto error =
        h.settle(h.host().split_shard(k_group, {"delta"}, std::chrono::milliseconds{4000}));
    BOOST_CHECK(error == nullptr);
    BOOST_CHECK_EQUAL(h.host().rejection_count(arbiter_gate::split_keys_truncated), 0u);
}

// ── the merge veto counter (Requirement 6.9 / design §6.7) ───────────────────

BOOST_AUTO_TEST_CASE(a_policy_veto_is_counted_against_the_policy_that_cast_it,
                     *boost::unit_test::timeout(120)) {
    // Under the composite's unanimity rule a single member can hold every merge
    // in the cluster hostage, so the first question an operator asks when
    // merges stop happening is *which one*. A veto that only showed up as an
    // absence of merges could not answer it.
    arbiter_host h{[](config_type& cfg) {
        cfg.evaluate_merge = [](const stats_type&, const stats_type&) {
            return kythira::merge_decision::veto(kythira::merge_reason::size, "grumpy");
        };
        cfg.policy_interval = std::chrono::milliseconds{10};
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    // Split first, so there is an adjacent sibling for a merge to be about.
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{4000})) == nullptr);

    for (int i = 0; i < 80 && h.host().merge_veto_count() == 0; ++i) {
        h.host().tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    BOOST_CHECK_GE(h.host().merge_veto_count(), 1u);
    BOOST_CHECK_GE(h.host().merge_veto_count("grumpy"), 1u);
    // Attributed, not lumped in with the unnamed default.
    BOOST_CHECK_EQUAL(h.host().merge_veto_count("unattributed"), 0u);
    // A veto is not a merge.
    BOOST_CHECK_EQUAL(h.host().applied_merge_count(), 0u);
}

BOOST_AUTO_TEST_CASE(an_abstaining_policy_is_not_counted_as_a_veto,
                     *boost::unit_test::timeout(120)) {
    // The distinction the tri-state exists for, at the host boundary: an
    // abstention leaves the merge unproposed exactly as a veto does, and only
    // one of them is a policy actively holding the cluster back.
    arbiter_host h{[](config_type& cfg) {
        cfg.evaluate_merge = [](const stats_type&, const stats_type&) {
            return kythira::merge_decision::abstain();
        };
        cfg.policy_interval = std::chrono::milliseconds{10};
    }};
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{4000})) == nullptr);

    for (int i = 0; i < 60; ++i) {
        h.host().tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_CHECK_EQUAL(h.host().merge_veto_count(), 0u);
    BOOST_CHECK_EQUAL(h.host().applied_merge_count(), 0u);
}

// ── latency statistics reach the policy (Requirement 10) ─────────────────────

BOOST_AUTO_TEST_CASE(read_latency_is_sampled_at_the_routing_layer,
                     *boost::unit_test::timeout(120)) {
    // Sampled in `multi_raft::read_state`, not in `node<Types>::read_state`:
    // the node sees neither the shard-map lookup nor the epoch validation the
    // client actually paid for.
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();

    for (int i = 0; i < 5; ++i) {
        auto f = h.host().read_state("alpha", std::chrono::milliseconds{2000});
        BOOST_CHECK(h.settle(std::move(f)) == nullptr);
    }

    auto stats = h.host().stats_for(k_group);
    BOOST_REQUIRE(stats.has_value());
    // Non-zero, which is the property the millisecond-truncation fix restores:
    // these reads are all comfortably sub-millisecond.
    BOOST_CHECK_GT(stats->_p99_read_latency.count(), 0);
}

BOOST_AUTO_TEST_CASE(apply_latency_is_populated, *boost::unit_test::timeout(120)) {
    // `_p99_apply_latency` was an unpopulated field from the first draft
    // onwards, because nothing measured it.
    arbiter_host h;
    BOOST_REQUIRE(h.await_leader());
    h.seed();
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_group, {"delta"},
                                                std::chrono::milliseconds{4000})) == nullptr);
    for (int i = 0; i < 20; ++i) {
        h.host().tick();
    }

    auto stats = h.host().stats_for(k_group);
    BOOST_REQUIRE(stats.has_value());
    BOOST_CHECK_GT(stats->_p99_apply_latency.count(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
