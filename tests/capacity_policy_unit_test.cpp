// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file capacity_policy_unit_test.cpp
/// @brief The capacity decision layer (tasks 3 and 4 of
///        `.kiro/specs/elastic-shard-capacity/`): the value types, the
///        `capacity_policy` concept, and `threshold_capacity_policy`.
///
/// The policy is a pure function of a snapshot plus its own crossing history,
/// so every test here builds snapshots by hand and moves time by setting
/// `_taken_at`. Nothing sleeps; a "five minute sustained crossing" is two
/// snapshots five minutes apart.

#define BOOST_TEST_MODULE capacity_policy_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/capacity_policy.hpp>

#include <chrono>
#include <map>
#include <tuple>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using kythira::capacity_action;
using kythira::capacity_decision;
using kythira::capacity_evidence;
using kythira::capacity_reason;
using kythira::capacity_signal;
using kythira::threshold_capacity_policy;
using kythira::threshold_capacity_policy_config;

using node_id_t = std::uint64_t;
using group_id_t = std::uint64_t;
using key_t = std::string;
using pg_t = std::string;

using snapshot_t = kythira::cluster_capacity_snapshot<node_id_t, group_id_t, key_t, pg_t>;
using policy_t = threshold_capacity_policy<node_id_t, group_id_t, key_t, pg_t>;
using decision_t = capacity_decision<pg_t, node_id_t>;

using namespace std::chrono_literals;

// ── concept conformance ──────────────────────────────────────────────────────

static_assert(kythira::capacity_policy<policy_t, node_id_t, group_id_t, key_t, pg_t>);

/// Returns the wrong decision type: a policy that thinks in shard groups rather
/// than placement groups must not be mistaken for a capacity policy.
struct wrong_decision_policy {
    auto evaluate(const snapshot_t&) -> capacity_decision<group_id_t, node_id_t> { return {}; }
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return {}; }
    [[nodiscard]] auto validate() const -> bool { return true; }
    [[nodiscard]] auto get_validation_errors() const -> std::vector<std::string> { return {}; }
};
static_assert(!kythira::capacity_policy<wrong_decision_policy, node_id_t, group_id_t, key_t, pg_t>);

/// No `validate()`: a policy that cannot say whether it is safely configured
/// is not a policy the controller will run.
struct unvalidated_policy {
    auto evaluate(const snapshot_t&) -> decision_t { return {}; }
    [[nodiscard]] auto cooldown() const -> std::chrono::milliseconds { return {}; }
};
static_assert(!kythira::capacity_policy<unvalidated_policy, node_id_t, group_id_t, key_t, pg_t>);

// ── snapshot builders ────────────────────────────────────────────────────────

const auto k_t0 = std::chrono::system_clock::time_point{} + std::chrono::hours{1000};

struct node_spec {
    std::size_t _shards{0};
    std::size_t _leaders{0};
    std::uint64_t _capacity{0};
    std::uint64_t _used{0};
    double _write_bps{0.0};
    bool _overloaded{false};
    bool _stale{false};
    pg_t _group{"a"};
};

auto make_snapshot(const std::vector<node_spec>& nodes,
                   std::chrono::system_clock::time_point at = k_t0) -> snapshot_t {
    snapshot_t s;
    s._taken_at = at;
    node_id_t id = 1;
    std::map<pg_t, std::size_t> counts;
    for (const auto& n : nodes) {
        kythira::node_capacity_view<node_id_t, pg_t> v;
        v._report._node_id = id++;
        v._report._shard_count = n._shards;
        v._report._leader_count = n._leaders;
        v._report._capacity_bytes = n._capacity;
        v._report._used_bytes = n._used;
        v._report._available_bytes = n._capacity - n._used;
        v._report._write_bytes_per_sec = n._write_bps;
        v._report._overloaded = n._overloaded;
        v._stale = n._stale;
        v._placement_group = n._group;
        s._nodes.push_back(v);
        ++counts[n._group];
    }
    for (const auto& [g, c] : counts) {
        s._groups.push_back({._group = g, ._target = 1, ._live = c, ._known = c});
        s._topology.groups.push_back({.group_id = g, .target_count = 1});
    }
    s._rate_window = 15min;
    s._max_cluster_size = 100;
    return s;
}

auto uniform(std::size_t n, node_spec spec) -> std::vector<node_spec> {
    return std::vector<node_spec>(n, spec);
}

/// A config with every signal on and short windows, so tests read in minutes.
auto enabled_config() -> threshold_capacity_policy_config {
    threshold_capacity_policy_config c;
    c._enabled = true;
    c._sustained_for = 5min;
    c._sustained_for_scale_in = 30min;
    c._cooldown = 1min;
    return c;
}

/// Feed the same shape of snapshot at `t0`, `t0 + step`, ... up to `until`,
/// returning the first non-hold decision (or hold).
auto run(policy_t& p, const std::vector<node_spec>& nodes, std::chrono::minutes until,
         std::chrono::minutes step = 1min, std::chrono::system_clock::time_point start = k_t0)
    -> decision_t {
    for (auto t = 0min; t <= until; t += step) {
        auto d = p.evaluate(make_snapshot(nodes, start + t));
        if (!d.is_hold()) {
            return d;
        }
    }
    return decision_t::hold();
}

auto has_error_containing(const std::vector<std::string>& errors, const std::string& needle)
    -> bool {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(capacity_policy_unit)

// ── value types (task 3) ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_default_decision_is_hold) {
    const decision_t d;
    BOOST_CHECK(d.is_hold());
    BOOST_CHECK(d.action() == capacity_action::hold);
    BOOST_CHECK_EQUAL(d.count(), 0U);
    BOOST_CHECK(!d.group().has_value());
    BOOST_CHECK(!d.node().has_value());
    BOOST_CHECK(d == decision_t::hold());
}

BOOST_AUTO_TEST_CASE(a_scale_out_of_zero_machines_cannot_be_built) {
    BOOST_CHECK_THROW(std::ignore = decision_t::scale_out(0, capacity_reason::density, {}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(std::ignore = decision_t::scale_out_in("a", 0, capacity_reason::density, {}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(factories_round_trip_through_the_accessors) {
    capacity_evidence ev;
    ev._signals.push_back(capacity_signal{._name = "x", ._value = 2.0, ._threshold = 1.0});
    ev._capacity_refusals = 3;

    const auto out = decision_t::scale_out(2, capacity_reason::split_pressure, ev);
    BOOST_CHECK(out.is_scale_out());
    BOOST_CHECK_EQUAL(out.count(), 2U);
    BOOST_CHECK(out.reason() == capacity_reason::split_pressure);
    BOOST_CHECK(!out.group().has_value());
    BOOST_CHECK(out.evidence() == ev);

    const auto out_in = decision_t::scale_out_in("zone-b", 1, capacity_reason::storage, ev);
    BOOST_CHECK(out_in.group() == std::optional<pg_t>{"zone-b"});
    BOOST_CHECK(out_in != out);

    const auto in = decision_t::scale_in("zone-c", 7, capacity_reason::density, ev);
    BOOST_CHECK(in.is_scale_in());
    BOOST_CHECK_EQUAL(in.count(), 1U);
    BOOST_CHECK(in.group() == std::optional<pg_t>{"zone-c"});
    BOOST_CHECK(in.node() == std::optional<node_id_t>{7});

    // Copies are equal; a decision is a value.
    const auto copy = in;
    BOOST_CHECK(copy == in);
}

BOOST_AUTO_TEST_CASE(reasons_and_actions_have_stable_names_in_declaration_order) {
    // The names are metric dimension values; the order is the wire value.
    const std::vector<std::pair<capacity_reason, std::string>> reasons{
        {capacity_reason::density, "density"},
        {capacity_reason::storage, "storage"},
        {capacity_reason::load, "load"},
        {capacity_reason::overload, "overload"},
        {capacity_reason::split_pressure, "split_pressure"},
        {capacity_reason::topology_floor, "topology_floor"},
        {capacity_reason::manual, "manual"},
    };
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        BOOST_CHECK_EQUAL(static_cast<std::size_t>(reasons[i].first), i);
        BOOST_CHECK_EQUAL(kythira::to_string(reasons[i].first), reasons[i].second);
    }
    BOOST_CHECK(capacity_reason::density < capacity_reason::manual);
    BOOST_CHECK_EQUAL(kythira::to_string(capacity_action::hold), std::string{"hold"});
    BOOST_CHECK_EQUAL(kythira::to_string(capacity_action::scale_out), std::string{"scale_out"});
    BOOST_CHECK_EQUAL(kythira::to_string(capacity_action::scale_in), std::string{"scale_in"});
}

BOOST_AUTO_TEST_CASE(the_snapshot_ignores_stale_machines_in_its_averages_but_counts_them) {
    // Requirement 2.4: stale is unknown, not absent.
    auto s = make_snapshot({{._shards = 100, ._capacity = 100, ._used = 50},
                            {._shards = 300, ._capacity = 100, ._used = 90},
                            {._shards = 1000, ._capacity = 100, ._used = 100, ._stale = true}});
    BOOST_CHECK_EQUAL(s.cluster_size(), 3U);
    BOOST_CHECK_EQUAL(s.fresh_node_count(), 2U);
    BOOST_CHECK_CLOSE(*s.mean_shards_per_node(), 200.0, 1e-9);
    BOOST_CHECK_CLOSE(*s.storage_utilisation(), 140.0 / 200.0, 1e-9);
    BOOST_CHECK_CLOSE(*s.fresh_max([](const auto& r) { return r._shard_count; }), 300.0, 1e-9);

    // A machine whose probe reports nothing is left out of utilisation, not
    // read as empty.
    auto t = make_snapshot({{._capacity = 100, ._used = 90}, {._capacity = 0, ._used = 0}});
    BOOST_CHECK_CLOSE(*t.storage_utilisation(), 0.9, 1e-9);

    // All stale: no average at all.
    auto u = make_snapshot({{._shards = 10, ._stale = true}});
    BOOST_CHECK(!u.mean_shards_per_node().has_value());
}

// ── the default policy (task 4) ──────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_default_configuration_is_disabled_and_validates) {
    policy_t p;
    BOOST_CHECK(p.validate());
    BOOST_CHECK(!p.config()._enabled);
    // Wildly over every watermark, for an hour: still nothing. An unconfigured
    // deployment behaves exactly as it does today (Requirement 4.6).
    const auto d = run(p,
                       uniform(3, {._shards = 10'000,
                                   ._leaders = 10'000,
                                   ._capacity = 100,
                                   ._used = 100,
                                   ._overloaded = true}),
                       60min);
    BOOST_CHECK(d.is_hold());
}

BOOST_AUTO_TEST_CASE(each_signal_scales_out_above_its_high_watermark_once_sustained) {
    struct row {
        const char* _name;
        node_spec _over;
        capacity_reason _reason;
    };
    auto cfg = enabled_config();
    cfg._write_bytes_per_sec = {._high = 1000.0, ._low = 100.0, ._enabled = true};
    cfg._split_pressure_enabled = false;
    const std::vector<row> rows{
        {"shards_per_node", {._shards = 201}, capacity_reason::density},
        {"leaders_per_node", {._leaders = 81}, capacity_reason::density},
        {"storage", {._capacity = 100, ._used = 76}, capacity_reason::storage},
        {"write_bps", {._write_bps = 1001.0}, capacity_reason::load},
        {"overloaded", {._overloaded = true}, capacity_reason::overload},
    };
    for (const auto& r : rows) {
        BOOST_TEST_CONTEXT(r._name) {
            policy_t p{cfg};
            BOOST_REQUIRE(p.validate());
            // Not yet: four minutes is under `sustained_for`.
            BOOST_CHECK(run(p, uniform(3, r._over), 4min).is_hold());
            // Five minutes after the first crossing: one machine.
            const auto d = p.evaluate(make_snapshot(uniform(3, r._over), k_t0 + 5min));
            BOOST_CHECK(d.is_scale_out());
            BOOST_CHECK_EQUAL(d.count(), 1U);
            BOOST_CHECK(d.reason() == r._reason);
            BOOST_CHECK(!d.evidence()._signals.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(exactly_at_the_high_watermark_is_not_above_it) {
    auto cfg = enabled_config();
    cfg._split_pressure_enabled = false;
    policy_t p{cfg};
    BOOST_CHECK(run(p, uniform(3, {._shards = 200}), 60min).is_hold());
}

BOOST_AUTO_TEST_CASE(a_single_spike_does_not_provision) {
    // Requirement 4.3: one heartbeat over, then back, then over again for less
    // than the window — never a decision, because the dip reset the clock.
    auto cfg = enabled_config();
    cfg._split_pressure_enabled = false;
    policy_t p{cfg};
    const auto over = uniform(3, {._shards = 500});
    const auto under = uniform(3, {._shards = 150});
    BOOST_CHECK(p.evaluate(make_snapshot(over, k_t0)).is_hold());
    BOOST_CHECK(p.evaluate(make_snapshot(under, k_t0 + 1min)).is_hold());
    for (auto t = 2min; t <= 6min; t += 1min) {
        BOOST_CHECK(p.evaluate(make_snapshot(over, k_t0 + t)).is_hold());
    }
    // ...and seven minutes after the dip it has been sustained for five.
    BOOST_CHECK(p.evaluate(make_snapshot(over, k_t0 + 7min)).is_scale_out());
}

BOOST_AUTO_TEST_CASE(a_decision_is_followed_by_the_cooldown_and_a_fresh_crossing) {
    auto cfg = enabled_config();
    cfg._split_pressure_enabled = false;
    cfg._cooldown = 10min;
    policy_t p{cfg};
    const auto over = uniform(3, {._shards = 500});
    BOOST_CHECK(run(p, over, 5min).is_scale_out());
    // Still over, but inside the cooldown and with the crossing consumed.
    for (auto t = 6min; t < 15min; t += 1min) {
        BOOST_CHECK(p.evaluate(make_snapshot(over, k_t0 + t)).is_hold());
    }
    // A new five-minute crossing that started inside the cooldown counts once
    // the cooldown is over.
    BOOST_CHECK(p.evaluate(make_snapshot(over, k_t0 + 15min)).is_scale_out());
}

BOOST_AUTO_TEST_CASE(stale_reports_alone_never_scale_out) {
    auto cfg = enabled_config();
    policy_t p{cfg};
    BOOST_CHECK(run(p, uniform(3, {._shards = 5000, ._stale = true}), 60min).is_hold());
}

BOOST_AUTO_TEST_CASE(the_ceiling_stops_the_policy_asking) {
    auto cfg = enabled_config();
    cfg._split_pressure_enabled = false;
    policy_t p{cfg};
    for (auto t = 0min; t <= 30min; t += 1min) {
        auto s = make_snapshot(uniform(3, {._shards = 500}), k_t0 + t);
        s._max_cluster_size = 3;
        BOOST_CHECK(p.evaluate(s).is_hold());
    }
}

BOOST_AUTO_TEST_CASE(every_signal_low_for_the_long_window_scales_in_the_emptiest_spare_machine) {
    auto cfg = enabled_config();
    policy_t p{cfg};
    // Four machines, target one per group, two groups: two spares.
    std::vector<node_spec> nodes{
        {._shards = 20, ._leaders = 5, ._capacity = 100, ._used = 10, ._group = "a"},
        {._shards = 10, ._leaders = 5, ._capacity = 100, ._used = 10, ._group = "a"},
        {._shards = 15, ._leaders = 5, ._capacity = 100, ._used = 10, ._group = "b"},
        {._shards = 15, ._leaders = 5, ._capacity = 100, ._used = 10, ._group = "b"},
    };
    // Under the long window: nothing.
    BOOST_CHECK(run(p, nodes, 29min).is_hold());
    const auto d = p.evaluate(make_snapshot(nodes, k_t0 + 30min));
    BOOST_REQUIRE(d.is_scale_in());
    // Node 2 holds the fewest shards (the least data moves) and is in a group
    // above its floor.
    BOOST_CHECK(d.node() == std::optional<node_id_t>{2});
    BOOST_CHECK(d.group() == std::optional<pg_t>{"a"});
}

BOOST_AUTO_TEST_CASE(scale_in_respects_the_floor_and_the_minimum_size) {
    auto cfg = enabled_config();
    const auto quiet = node_spec{._shards = 1, ._capacity = 100, ._used = 1};
    {
        // At the topology floor (one per group, two groups, two machines).
        policy_t p{cfg};
        std::vector<node_spec> nodes{quiet, quiet};
        nodes[1]._group = "b";
        BOOST_CHECK(run(p, nodes, 120min).is_hold());
    }
    {
        // Above the floor but at the configured minimum.
        policy_t p{cfg};
        for (auto t = 0min; t <= 120min; t += 1min) {
            auto s = make_snapshot(uniform(3, quiet), k_t0 + t);
            s._min_cluster_size = 3;
            BOOST_CHECK(p.evaluate(s).is_hold());
        }
    }
}

BOOST_AUTO_TEST_CASE(splits_outpacing_merges_block_scale_in_but_merges_never_block_scale_out) {
    // Requirement 5.6, both halves.
    auto cfg = enabled_config();
    {
        policy_t p{cfg};
        const auto quiet = uniform(3, {._shards = 1, ._capacity = 100, ._used = 1});
        for (auto t = 0min; t <= 120min; t += 1min) {
            auto s = make_snapshot(quiet, k_t0 + t);
            s._replicas_added_in_window = 3;
            s._replicas_removed_in_window = 1;
            BOOST_CHECK(p.evaluate(s).is_hold());
        }
    }
    {
        cfg._split_pressure_enabled = false;
        policy_t p{cfg};
        decision_t d;
        for (auto t = 0min; t <= 5min; t += 1min) {
            auto s = make_snapshot(uniform(3, {._shards = 500}), k_t0 + t);
            s._merges_in_window = 50;
            s._replicas_removed_in_window = 150;
            d = p.evaluate(s);
        }
        BOOST_CHECK(d.is_scale_out());
    }
}

BOOST_AUTO_TEST_CASE(a_scale_in_that_would_cross_the_out_mark_is_not_made) {
    auto cfg = enabled_config();
    cfg._shards_per_node = {._high = 100.0, ._low = 60.0};
    cfg._leaders_per_node._enabled = false;
    cfg._storage._enabled = false;
    cfg._overloaded._enabled = false;
    policy_t p{cfg};
    // 3 machines at 60 shards: removing one leaves 90 — still under 100, so
    // this one is allowed...
    std::vector<node_spec> fine(3, {._shards = 60});
    fine[1]._group = "b";
    BOOST_CHECK(run(p, fine, 30min).is_scale_in());
    // ...and 2 machines (one spare above a floor of 1) at 55 would leave 110.
    policy_t q{cfg};
    std::vector<node_spec> tight(2, {._shards = 55});
    BOOST_CHECK(run(q, tight, 120min).is_hold());
}

// ── split pressure ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_projection_matches_hand_computed_values) {
    auto cfg = enabled_config();
    cfg._horizon_minutes = 30.0;
    policy_t p{cfg};
    auto s = make_snapshot(uniform(4, {._shards = 100}));
    s._rate_window = 15min;
    s._replicas_added_in_window = 60;  // 4 per minute
    const auto proj = p.project(s);
    BOOST_REQUIRE(proj.has_value());
    BOOST_CHECK_CLOSE(proj->_current_shards_per_node, 100.0, 1e-9);
    BOOST_CHECK_CLOSE(proj->_split_rate_per_minute, 4.0, 1e-9);
    BOOST_CHECK_EQUAL(proj->_live_node_count, 4U);
    BOOST_CHECK_CLOSE(proj->_horizon_minutes, 30.0, 1e-9);
    // 100 + 4 * 30 / 4 = 130.
    BOOST_CHECK_CLOSE(proj->_projected_shards_per_node, 130.0, 1e-9);
    BOOST_CHECK(proj->_rate_window == 15min);

    // No window, no projection rather than a division by zero.
    s._rate_window = 0ms;
    BOOST_CHECK(!p.project(s).has_value());
}

BOOST_AUTO_TEST_CASE(projected_density_scales_out_before_any_watermark_is_crossed) {
    auto cfg = enabled_config();
    policy_t p{cfg};
    // 150 per node now (under 200), but 15 minutes of splits added 300
    // replicas: 20/min * 30 min / 3 nodes = +200 → 350 projected.
    decision_t d;
    for (auto t = 0min; t <= 5min && d.is_hold(); t += 1min) {
        auto s = make_snapshot(uniform(3, {._shards = 150}), k_t0 + t);
        s._replicas_added_in_window = 300;
        s._splits_in_window = 100;
        d = p.evaluate(s);
    }
    BOOST_REQUIRE(d.is_scale_out());
    BOOST_CHECK(d.reason() == capacity_reason::split_pressure);
    // The three projection inputs are in the evidence verbatim (Requirement 5.3).
    const auto& proj = d.evidence()._projection;
    BOOST_REQUIRE(proj.has_value());
    BOOST_CHECK_CLOSE(proj->_current_shards_per_node, 150.0, 1e-9);
    BOOST_CHECK_CLOSE(proj->_split_rate_per_minute, 20.0, 1e-9);
    BOOST_CHECK_CLOSE(proj->_horizon_minutes, 30.0, 1e-9);
    BOOST_CHECK_CLOSE(proj->_projected_shards_per_node, 350.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(a_capacity_refusal_short_circuits_the_projection) {
    // The cluster is already out of room; that is not a forecast and is not
    // held to `sustained_for`.
    auto cfg = enabled_config();
    policy_t p{cfg};
    auto s = make_snapshot(uniform(3, {._shards = 10}));
    s._capacity_refusals = 2;
    const auto d = p.evaluate(s);
    BOOST_REQUIRE(d.is_scale_out());
    BOOST_CHECK(d.reason() == capacity_reason::split_pressure);
    BOOST_CHECK_EQUAL(d.evidence()._capacity_refusals, 2U);
}

BOOST_AUTO_TEST_CASE(a_floor_repair_names_the_group_furthest_below_target) {
    auto cfg = enabled_config();
    cfg._repair_topology_floor = true;
    policy_t p{cfg};
    auto s = make_snapshot(uniform(3, {._shards = 10}));
    s._groups = {{._group = "a", ._target = 3, ._live = 2, ._known = 2},
                 {._group = "b", ._target = 3, ._live = 1, ._known = 1}};
    const auto d = p.evaluate(s);
    BOOST_REQUIRE(d.is_scale_out());
    BOOST_CHECK(d.reason() == capacity_reason::topology_floor);
    BOOST_CHECK(d.group() == std::optional<pg_t>{"b"});
}

// ── validate() ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_non_hysteretic_pair_is_rejected_by_name) {
    auto cfg = enabled_config();
    cfg._shards_per_node = {._high = 200.0, ._low = 190.0};
    policy_t p{cfg};
    BOOST_CHECK(!p.validate());
    const auto errors = p.get_validation_errors();
    BOOST_CHECK(has_error_containing(errors, "_shards_per_node is not hysteretic"));
}

BOOST_AUTO_TEST_CASE(an_inverted_pair_is_rejected_by_name) {
    auto cfg = enabled_config();
    cfg._storage = {._high = 0.3, ._low = 0.5};
    policy_t p{cfg};
    BOOST_CHECK(!p.validate());
    BOOST_CHECK(has_error_containing(p.get_validation_errors(), "_storage is inverted"));
}

BOOST_AUTO_TEST_CASE(every_error_is_reported_not_just_the_first) {
    auto cfg = enabled_config();
    cfg._shards_per_node = {._high = 200.0, ._low = 199.0};
    cfg._leaders_per_node = {._high = 10.0, ._low = 20.0};
    cfg._sustained_for_scale_in = 1min;
    cfg._scale_out_step = 0;
    policy_t p{cfg};
    const auto errors = p.get_validation_errors();
    BOOST_CHECK(has_error_containing(errors, "_shards_per_node is not hysteretic"));
    BOOST_CHECK(has_error_containing(errors, "_leaders_per_node is inverted"));
    BOOST_CHECK(has_error_containing(errors, "_sustained_for_scale_in"));
    BOOST_CHECK(has_error_containing(errors, "_scale_out_step"));
    BOOST_CHECK_GE(errors.size(), 4U);
}

BOOST_AUTO_TEST_CASE(a_disabled_signal_is_not_validated) {
    auto cfg = enabled_config();
    cfg._write_bytes_per_sec = {._high = 1.0, ._low = 5.0, ._enabled = false};
    policy_t p{cfg};
    BOOST_CHECK(p.validate());
}

BOOST_AUTO_TEST_CASE(split_pressure_without_a_density_watermark_is_rejected) {
    auto cfg = enabled_config();
    cfg._shards_per_node._enabled = false;
    policy_t p{cfg};
    BOOST_CHECK(has_error_containing(p.get_validation_errors(), "split pressure projects"));
}

BOOST_AUTO_TEST_SUITE_END()
