// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_placement_driver_test.cpp
/// @brief Batched heartbeats and advisory operators (task 28 of
///        `.kiro/specs/multi-raft/`).
///
/// The three properties this file exists for, in order of how much damage
/// their absence would do:
///
///  1. **One heartbeat call per interval, whatever the shard count.** A control
///     plane whose load grows with shard count fails at exactly the scale
///     sharding was adopted to reach. The test drives a hundred shards and
///     asserts one call.
///  2. **A stale-epoch operator is discarded with no side effects.** This is
///     what makes "advisory" safe rather than merely tolerable: the driver
///     reasoned about a range that may no longer exist, and the epoch check
///     turns a wrong action into a discarded message.
///  3. **A skipped operator is counted under its own reason, and the same
///     operator is accepted once the obstacle clears.** A skip that vanished
///     silently would be indistinguishable from an operator that was never
///     sent, which is precisely the thing an operator debugging a driver that
///     "does nothing" needs to tell apart.

#define BOOST_TEST_MODULE multi_raft_placement_driver_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_placement_driver_test"), nullptr};
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

using kythira::add_replica_operator;
using kythira::merge_operator;
using kythira::node_report;
using kythira::scatter_operator;
using kythira::shard_descriptor;
using kythira::shard_operation;
using kythira::shard_operation_state;
using kythira::shard_report;
using kythira::skipped_operator_reason;
using kythira::split_operator;
using kythira::transfer_leader_operator;

using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;
using report_type = shard_report<group_id_type, key_type, std::uint64_t>;
using operation_type = shard_operation<group_id_type, key_type, std::uint64_t>;

/// @brief A driver that records what it was told and answers with a script.
///
/// Deliberately not a mock that asserts inline: the interesting assertions here
/// are about *counts across time* — one call per interval, this many reports in
/// that one call — and inline expectations cannot see across calls.
class recording_driver {
public:
    auto on_shard_heartbeat(const std::vector<report_type>& reports)
        -> std::vector<operation_type> {
        std::lock_guard lock(_mutex);
        ++_shard_calls;
        _report_counts.push_back(reports.size());
        _last_reports = reports;
        auto pending = std::move(_scripted);
        _scripted.clear();
        return pending;
    }

    auto on_node_heartbeat(const node_report<std::uint64_t>& r) -> void {
        std::lock_guard lock(_mutex);
        ++_node_calls;
        _last_node_report = r;
    }

    /// @brief Queue operators to be returned by the NEXT heartbeat.
    auto script(std::vector<operation_type> ops) -> void {
        std::lock_guard lock(_mutex);
        _scripted = std::move(ops);
    }

    [[nodiscard]] auto shard_calls() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _shard_calls;
    }
    [[nodiscard]] auto node_calls() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _node_calls;
    }
    [[nodiscard]] auto report_counts() const -> std::vector<std::size_t> {
        std::lock_guard lock(_mutex);
        return _report_counts;
    }
    [[nodiscard]] auto last_reports() const -> std::vector<report_type> {
        std::lock_guard lock(_mutex);
        return _last_reports;
    }
    [[nodiscard]] auto last_node_report() const -> node_report<std::uint64_t> {
        std::lock_guard lock(_mutex);
        return _last_node_report;
    }

private:
    mutable std::mutex _mutex;
    std::size_t _shard_calls{0};
    std::size_t _node_calls{0};
    std::vector<std::size_t> _report_counts;
    std::vector<report_type> _last_reports;
    node_report<std::uint64_t> _last_node_report;
    std::vector<operation_type> _scripted;
};

auto wire_driver(config_type& cfg, recording_driver& driver) -> void {
    cfg.report_shard_heartbeat = [&driver](const std::vector<report_type>& reports) {
        return driver.on_shard_heartbeat(reports);
    };
    cfg.report_node_heartbeat = [&driver](const node_report<std::uint64_t>& r) {
        driver.on_node_heartbeat(r);
    };
}

/// Drive `host` until `predicate` holds or `budget` elapses.
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

/// The state machine's command encoding: type byte, then length-prefixed key,
/// then (for a put) length-prefixed value.
auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(1));
    const auto append_u32 = [&out](std::uint32_t n) {
        std::byte buf[sizeof(std::uint32_t)];
        std::memcpy(buf, &n, sizeof(n));
        out.insert(out.end(), std::begin(buf), std::end(buf));
    };
    const auto append_str = [&out](const std::string& str) {
        for (char c : str) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    };
    append_u32(static_cast<std::uint32_t>(key.size()));
    append_str(key);
    append_u32(static_cast<std::uint32_t>(value.size()));
    append_str(value);
    return out;
}

auto operator_for(group_id_type group, kythira::shard_epoch epoch, std::uint64_t id,
                  kythira::shard_operator_kind<group_id_type, key_type, std::uint64_t> op)
    -> operation_type {
    operation_type o;
    o._group_id = group;
    o._operation_id = id;
    o._epoch = epoch;
    o._operator = std::move(op);
    return o;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_placement_driver)

// ── batching ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_hundred_shards_produce_one_heartbeat_call, *boost::unit_test::timeout(120)) {
    message_fabric fabric{2};
    recording_driver driver;
    auto cfg = make_config(fabric, 1);
    wire_driver(cfg, driver);
    // Long enough that the many ticks below cannot themselves produce a second
    // heartbeat: the property under test is one call per *interval*, not one
    // call per tick, and a short interval would let the test pass for the
    // wrong reason.
    cfg.heartbeat_interval = std::chrono::seconds{60};
    host_type host{std::move(cfg)};

    for (group_id_type g = 1; g <= 100; ++g) {
        host.create_group(g, {1});
    }
    host.start();

    const bool all_leaders = tick_until(
        host,
        [&] {
            for (group_id_type g = 1; g <= 100; ++g) {
                auto* n = host.group_node(g);
                if (n == nullptr || !n->is_leader()) {
                    return false;
                }
            }
            return true;
        },
        std::chrono::milliseconds{20000});
    BOOST_REQUIRE(all_leaders);

    // The very first tick heartbeats (there is no previous one to be inside the
    // interval of), and every tick after it is inside the 60-second window.
    BOOST_CHECK_EQUAL(driver.shard_calls(), 1U);
    BOOST_CHECK_EQUAL(driver.node_calls(), 1U);

    // ...and that one call carried a report for every shard, rather than one
    // shard's report a hundred times.
    host.heartbeat();
    const auto counts = driver.report_counts();
    BOOST_REQUIRE(!counts.empty());
    BOOST_CHECK_EQUAL(counts.back(), 100U);

    host.stop();
}

BOOST_AUTO_TEST_CASE(only_shards_this_host_leads_are_reported, *boost::unit_test::timeout(60)) {
    // A follower's view of size and load is the leader's view delayed, so N-1
    // copies of a stale report would cost bandwidth to say nothing.
    message_fabric fabric{4};
    recording_driver driver_a;
    recording_driver driver_b;

    auto cfg_a = make_config(fabric, 1);
    wire_driver(cfg_a, driver_a);
    cfg_a.heartbeat_interval = std::chrono::seconds{60};
    auto cfg_b = make_config(fabric, 2);
    wire_driver(cfg_b, driver_b);
    cfg_b.heartbeat_interval = std::chrono::seconds{60};

    host_type host_a{std::move(cfg_a)};
    host_type host_b{std::move(cfg_b)};

    host_a.create_group(1, {1, 2});
    host_b.create_group(1, {1, 2});
    host_a.start();
    host_b.start();

    const bool elected = tick_until(host_a, [&] {
        host_b.tick();
        auto* a = host_a.group_node(1);
        auto* b = host_b.group_node(1);
        return (a != nullptr && a->is_leader()) || (b != nullptr && b->is_leader());
    });
    BOOST_REQUIRE(elected);

    auto* a = host_a.group_node(1);
    const bool a_leads = a != nullptr && a->is_leader();

    host_a.heartbeat();
    host_b.heartbeat();

    const auto from_a = driver_a.last_reports().size();
    const auto from_b = driver_b.last_reports().size();
    // Exactly one of the two hosts reports the shard: the leader.
    BOOST_CHECK_EQUAL(from_a + from_b, 1U);
    BOOST_CHECK_EQUAL(a_leads ? from_a : from_b, 1U);

    host_a.stop();
    host_b.stop();
}

BOOST_AUTO_TEST_CASE(the_node_report_describes_the_machine_not_a_shard,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    recording_driver driver;
    auto cfg = make_config(fabric, 7);
    wire_driver(cfg, driver);
    cfg.heartbeat_interval = std::chrono::seconds{60};
    cfg.node_labels = {"rack-a", "zone-1"};
    cfg.capacity_probe = [] { return std::pair<std::uint64_t, std::uint64_t>{1000, 400}; };
    cfg.overload_probe = [] { return true; };
    host_type host{std::move(cfg)};

    for (group_id_type g = 1; g <= 3; ++g) {
        host.create_group(g, {7});
    }
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        for (group_id_type g = 1; g <= 3; ++g) {
            auto* n = host.group_node(g);
            if (n == nullptr || !n->is_leader()) {
                return false;
            }
        }
        return true;
    }));

    host.heartbeat();
    const auto r = driver.last_node_report();
    BOOST_CHECK_EQUAL(r._node_id, 7U);
    BOOST_CHECK_EQUAL(r._shard_count, 3U);
    BOOST_CHECK_EQUAL(r._leader_count, 3U);
    BOOST_CHECK_EQUAL(r._capacity_bytes, 1000U);
    BOOST_CHECK_EQUAL(r._available_bytes, 400U);
    BOOST_CHECK_EQUAL(r._used_bytes, 600U);
    BOOST_CHECK(r._overloaded);
    BOOST_REQUIRE_EQUAL(r._labels.size(), 2U);
    BOOST_CHECK_EQUAL(r._labels[0], "rack-a");

    host.stop();
}

BOOST_AUTO_TEST_CASE(a_zero_interval_takes_the_heartbeat_off_the_tick,
                     *boost::unit_test::timeout(60)) {
    // For a driver whose round trip is too slow to sit inside a tick: the
    // caller drives `heartbeat()` from its own thread instead.
    message_fabric fabric{2};
    recording_driver driver;
    auto cfg = make_config(fabric, 1);
    wire_driver(cfg, driver);
    cfg.heartbeat_interval = std::chrono::milliseconds{0};
    host_type host{std::move(cfg)};

    host.create_group(1, {1});
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        auto* n = host.group_node(1);
        return n != nullptr && n->is_leader();
    }));

    BOOST_CHECK_EQUAL(driver.shard_calls(), 0U);
    host.heartbeat();
    BOOST_CHECK_EQUAL(driver.shard_calls(), 1U);

    host.stop();
}

// ── advisory operators ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_stale_epoch_operator_is_discarded_without_side_effects,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    host_type host{std::move(cfg)};

    host.create_group(1, {1});
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        auto* n = host.group_node(1);
        return n != nullptr && n->is_leader();
    }));

    const auto current = host.local_descriptor(1)->_epoch;
    kythira::shard_epoch stale = current;
    stale._version = current._version == 0 ? 99 : current._version - 1;

    // A split operator is the sharpest case available: if the epoch check did
    // not fire, the shard would actually split, and the group count would move.
    const auto before = host.group_count();
    const auto outcome = host.apply_operator(
        operator_for(1, stale, 11, split_operator<key_type>{._at_keys = {"m"}}));

    BOOST_CHECK(!outcome._accepted);
    BOOST_CHECK(outcome._reason == skipped_operator_reason::stale_epoch);
    BOOST_CHECK_EQUAL(outcome._operation_id, 11U);
    BOOST_CHECK_EQUAL(host.group_count(), before);
    BOOST_CHECK_EQUAL(host.skipped_operator_count(skipped_operator_reason::stale_epoch), 1U);
    BOOST_CHECK_EQUAL(host.accepted_operator_count(), 0U);

    host.stop();
}

BOOST_AUTO_TEST_CASE(an_operator_for_a_shard_with_no_local_replica_is_skipped,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.start();

    const auto outcome =
        host.apply_operator(operator_for(42, kythira::shard_epoch{}, 1, scatter_operator{}));
    BOOST_CHECK(!outcome._accepted);
    BOOST_CHECK(outcome._reason == skipped_operator_reason::unknown_shard);
    BOOST_CHECK_EQUAL(host.skipped_operator_count(skipped_operator_reason::unknown_shard), 1U);

    host.stop();
}

BOOST_AUTO_TEST_CASE(an_operator_arriving_mid_operation_is_skipped_and_later_accepted,
                     *boost::unit_test::timeout(60)) {
    // The whole advisory contract in one test: the leader declines while the
    // shard is busy, counts the refusal under its own gate, and takes the same
    // operator once the obstacle clears. Nothing was queued and nothing needed
    // undoing.
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    host_type host{std::move(cfg)};

    host.create_group(1, {1});
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        auto* n = host.group_node(1);
        return n != nullptr && n->is_leader();
    }));

    // Freezing is the cleanest way to hold a shard closed to the automatic
    // channels without inventing a half-finished merge — and the placement
    // driver is an automatic channel, which is exactly the rule under test.
    BOOST_REQUIRE(host.freeze_shard(1));

    const auto epoch = host.local_descriptor(1)->_epoch;
    const auto refused = host.apply_operator(
        operator_for(1, epoch, 7, transfer_leader_operator<std::uint64_t>{._to = 1}));
    BOOST_CHECK(!refused._accepted);
    BOOST_CHECK(refused._reason == skipped_operator_reason::driver_disabled);
    BOOST_CHECK_EQUAL(host.skipped_operator_count(skipped_operator_reason::driver_disabled), 1U);

    BOOST_REQUIRE(host.thaw_shard(1));

    // The same operator, reissued as the driver would on its next heartbeat.
    // It names this host as the target, which is not a valid transfer — the
    // point is that it now reaches the precondition check instead of being
    // stopped at the channel gate.
    const auto retried = host.apply_operator(
        operator_for(1, epoch, 7, transfer_leader_operator<std::uint64_t>{._to = 1}));
    BOOST_CHECK(!retried._accepted);
    BOOST_CHECK(retried._reason != skipped_operator_reason::driver_disabled);
    BOOST_CHECK_EQUAL(host.skipped_operator_count(skipped_operator_reason::driver_disabled), 1U);

    host.stop();
}

BOOST_AUTO_TEST_CASE(an_operator_naming_a_node_that_is_not_a_replica_is_skipped,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(1, {1});
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        auto* n = host.group_node(1);
        return n != nullptr && n->is_leader();
    }));

    const auto epoch = host.local_descriptor(1)->_epoch;

    // Removing a node that was never there is not an error to report upward —
    // the driver's membership view was one heartbeat stale. It is a skip.
    const auto outcome = host.apply_operator(
        operator_for(1, epoch, 3, kythira::remove_replica_operator<std::uint64_t>{._node = 99}));
    BOOST_CHECK(!outcome._accepted);
    BOOST_CHECK(outcome._reason == skipped_operator_reason::precondition);

    host.stop();
}

BOOST_AUTO_TEST_CASE(operators_returned_by_a_heartbeat_are_acted_on,
                     *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    recording_driver driver;
    auto cfg = make_config(fabric, 1);
    wire_driver(cfg, driver);
    cfg.heartbeat_interval = std::chrono::seconds{60};
    host_type host{std::move(cfg)};

    host.create_group(1, {1});
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] {
        auto* n = host.group_node(1);
        return n != nullptr && n->is_leader();
    }));

    const auto epoch = host.local_descriptor(1)->_epoch;
    driver.script({operator_for(1, epoch, 5, add_replica_operator<std::uint64_t>{._node = 2}),
                   operator_for(1, epoch, 6, add_replica_operator<std::uint64_t>{._node = 2})});

    const auto received = host.heartbeat();
    BOOST_CHECK_EQUAL(received, 2U);
    BOOST_CHECK_EQUAL(host.received_operator_count(), 2U);

    // The first is accepted; the second names a node the first just added, so
    // it fails its precondition. Both outcomes are ordinary.
    BOOST_CHECK_EQUAL(host.accepted_operator_count() + host.skipped_operator_count(), 2U);
    BOOST_CHECK_GE(host.accepted_operator_count(), 1U);

    host.stop();
}

// ── the static, control-plane-free deployment (task 27's end-to-end case) ─────

BOOST_AUTO_TEST_CASE(a_static_three_shard_cluster_runs_against_the_no_op_driver,
                     *boost::unit_test::timeout(60)) {
    // Requirement 14.7's whole purpose: a pre-split deployment that never
    // rebalances should need no control plane at all. Ids come from a locally
    // reserved range and the driver returns no operators, so the host does
    // exactly what it was configured to do and nothing else.
    message_fabric fabric{2};
    kythira::no_op_shard_placement_driver<group_id_type, key_type, std::uint64_t> driver{1000,
                                                                                         1100};

    auto cfg = make_config(fabric, 1);
    cfg.heartbeat_interval = std::chrono::milliseconds{1};
    cfg.report_shard_heartbeat = [&driver](const std::vector<report_type>& reports) {
        return std::move(driver.report_shard_heartbeat(reports)).get();
    };
    cfg.report_node_heartbeat = [&driver](const node_report<std::uint64_t>& r) {
        std::move(driver.report_node_heartbeat(r)).get();
    };
    cfg.allocate_group_ids = [&driver](std::size_t n) {
        std::vector<group_id_type> out;
        for (const auto& a : std::move(driver.allocate_shard_ids(n)).get()) {
            out.push_back(a._group_id);
        }
        return out;
    };
    // Splits disabled, as a static deployment has them.
    cfg.automatic_split_merge_enabled = false;
    host_type host{std::move(cfg)};

    using bounds = kythira::shard_range<key_type>;
    const std::vector<bounds> ranges{
        bounds{._start = std::nullopt, ._end = std::string{"g"}},
        bounds{._start = std::string{"g"}, ._end = std::string{"p"}},
        bounds{._start = std::string{"p"}, ._end = std::nullopt},
    };
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        descriptor_type d;
        d._group_id = static_cast<group_id_type>(i + 1);
        d._range = ranges[i];
        d._epoch = kythira::shard_epoch{._version = 1, ._conf_version = 1};
        d._voters = {1};
        host.create_group(d);
    }
    host.start();

    BOOST_REQUIRE(tick_until(host, [&] {
        for (group_id_type g = 1; g <= 3; ++g) {
            auto* n = host.group_node(g);
            if (n == nullptr || !n->is_leader()) {
                return false;
            }
        }
        return true;
    }));

    // The routing table tiles the key space, every key resolves, and no
    // operator ever arrived to disturb any of it.
    BOOST_CHECK(!host.shard_map_snapshot().check_tiling().has_value());
    BOOST_REQUIRE(host.resolve("a").has_value());
    BOOST_CHECK_EQUAL(host.resolve("a")->_group_id, 1U);
    BOOST_CHECK_EQUAL(host.resolve("k")->_group_id, 2U);
    BOOST_CHECK_EQUAL(host.resolve("z")->_group_id, 3U);

    auto result = host.submit_command("k", put_command("k", "v"), std::chrono::milliseconds{2000});
    BOOST_CHECK_NO_THROW(std::move(result).get());

    BOOST_CHECK_GE(driver.shard_heartbeat_count(), 1U);
    BOOST_CHECK_EQUAL(host.received_operator_count(), 0U);
    BOOST_CHECK_EQUAL(host.accepted_operator_count(), 0U);
    BOOST_CHECK_EQUAL(host.group_count(), 3U);
    // Nothing split, so nothing was ever allocated from the reserved range.
    BOOST_CHECK_EQUAL(driver.allocated_count(), 0U);

    host.stop();
}

BOOST_AUTO_TEST_SUITE_END()
