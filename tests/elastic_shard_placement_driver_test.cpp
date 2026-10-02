// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file elastic_shard_placement_driver_test.cpp
/// @brief The placement-driver decorator (task 13 of
///        `.kiro/specs/elastic-shard-capacity/`): ids straight through, the
///        inner driver's operators first and winning conflicts, a split as a
///        trigger edge, each host given only its own groups' operators, and a
///        failing controller degrading to the inner driver alone.

#define BOOST_TEST_MODULE elastic_shard_placement_driver_test
#include <boost/test/unit_test.hpp>

#include "elastic_capacity_test_support.hpp"

#include <raft/capacity_ledger.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/elastic_shard_placement_driver.hpp>
#include <raft/shard_placement_driver.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kythira::capacity_reason;
using kythira::elastic_capacity_config;
using kythira::elastic_capacity_controller;
using kythira::elastic_shard_placement_driver;
using kythira::memory_capacity_ledger;
using kythira::no_op_shard_placement_driver;
using kythira::testing::cap_decision;
using kythira::testing::cap_descriptor;
using kythira::testing::cap_group;
using kythira::testing::cap_key;
using kythira::testing::cap_node;
using kythira::testing::cap_operation;
using kythira::testing::cap_pg;
using kythira::testing::cap_shard_report;
using kythira::testing::manual_clock;
using kythira::testing::manual_executor;
using kythira::testing::scripted_capacity_policy;
using kythira::testing::simulated_cluster;
using kythira::testing::toggle_lease;

using mock_t = kythira::testing::mock_capacity_quorum_manager<>;
using ledger_t = memory_capacity_ledger<cap_node, cap_pg>;

/// A ledger whose reads fail: a controller built on it throws from every step.
class broken_ledger : public ledger_t {
public:
    [[nodiscard]] auto intents() const -> std::vector<kythira::capacity_intent<cap_node, cap_pg>> {
        throw std::runtime_error("ledger unavailable");
    }
};

template<typename Ledger>
using ctl_for = elastic_capacity_controller<mock_t, scripted_capacity_policy, Ledger, toggle_lease,
                                            cap_group, cap_key>;
using ctl_t = ctl_for<ledger_t>;
using no_op_t = no_op_shard_placement_driver<cap_group, cap_key, cap_node>;
using adapter_t = elastic_shard_placement_driver<no_op_t, ctl_t, cap_group, cap_key, cap_node>;

static_assert(kythira::shard_placement_driver<adapter_t, cap_group, cap_key, cap_node>,
              "the adapter must itself be a placement driver");

/// An inner driver that answers every heartbeat with whatever the test queued.
class scripted_inner {
public:
    std::vector<cap_operation> _next;
    std::size_t _splits{0};
    std::size_t _allocations{0};

    auto allocate_shard_ids(std::size_t n)
        -> kythira::future_default<std::vector<kythira::shard_id_allocation<cap_group, cap_node>>> {
        ++_allocations;
        std::vector<kythira::shard_id_allocation<cap_group, cap_node>> out;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back({._group_id = 500 + i});
        }
        return kythira::future_factory_default::makeFuture(std::move(out));
    }
    auto report_shard_heartbeat(const std::vector<cap_shard_report>&)
        -> kythira::future_default<std::vector<cap_operation>> {
        auto ops = std::move(_next);
        _next.clear();
        return kythira::future_factory_default::makeFuture(std::move(ops));
    }
    auto report_node_heartbeat(const kythira::node_report<cap_node>&)
        -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    auto report_split(const cap_descriptor&, const std::vector<cap_descriptor>&)
        -> kythira::future_default<void> {
        ++_splits;
        return kythira::future_factory_default::makeFuture();
    }
    auto report_merge(const cap_descriptor&, const cap_descriptor&)
        -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
};

static_assert(kythira::shard_placement_driver<scripted_inner, cap_group, cap_key, cap_node>);

auto config() -> elastic_capacity_config {
    elastic_capacity_config c;
    c.enabled = true;
    c.dry_run = false;
    c.evaluation_interval = 1h;  // only trigger edges evaluate in these tests
    c.min_provider_call_interval = 0ms;
    c.jitter_seed = 3;
    return c;
}

auto topology() -> kythira::desired_topology<std::string> {
    return {.groups = {{.group_id = "a", .target_count = 1},
                       {.group_id = "b", .target_count = 1},
                       {.group_id = "c", .target_count = 1}}};
}

struct fixture {
    manual_clock clock;
    manual_executor ex;
    mock_t mgr{clock, topology()};
    ledger_t ledger;
    scripted_capacity_policy policy;
    toggle_lease lease;
    simulated_cluster sim;
    std::unique_ptr<ctl_t> ctl;

    fixture() {
        for (cap_node id = 1; id <= 3; ++id) {
            sim.add_host(id, std::string(1, static_cast<char>('a' + id - 1)));
            mgr.add_existing(id, std::string(1, static_cast<char>('a' + id - 1)));
        }
        for (cap_group g = 1; g <= 3; ++g) {
            sim.add_shard(g, {1, 2, 3});
            sim.set_leader(g, g);
        }
        ctl = std::make_unique<ctl_t>(mgr, policy, ledger, lease, config(), ex.fn(), clock.fn());
    }

    /// Reports for the groups `host` leads.
    [[nodiscard]] auto reports_of(cap_node host) const -> std::vector<cap_shard_report> {
        std::vector<cap_shard_report> out;
        for (const auto& r : sim.shard_reports()) {
            if (r.leader() == host) {
                out.push_back(r);
            }
        }
        return out;
    }

    template<typename A> auto heartbeat(A& adapter, cap_node host) -> std::vector<cap_operation> {
        for (const auto& n : sim.node_reports()) {
            if (n._node_id == host) {
                static_cast<void>(adapter.report_node_heartbeat(n));
            }
        }
        auto ops = adapter.report_shard_heartbeat(reports_of(host)).get();
        adapter.report_operator_outcomes(sim.apply(ops));
        ex.run_all();
        mgr.settle();
        sim.tick();
        clock.advance(10s);
        return ops;
    }
};

auto op_on(cap_group g, const cap_descriptor& d, std::uint64_t id) -> cap_operation {
    cap_operation op;
    op._group_id = g;
    op._operation_id = id;
    op._epoch = d._epoch;
    op._operator = kythira::transfer_leader_operator<cap_node>{._to = 2};
    return op;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(elastic_shard_placement_driver_unit)

BOOST_AUTO_TEST_CASE(ids_go_straight_to_the_inner_driver) {
    fixture f;
    elastic_shard_placement_driver<scripted_inner, ctl_t, cap_group, cap_key, cap_node> a{*f.ctl};
    const auto ids = a.allocate_shard_ids(2).get();
    BOOST_REQUIRE_EQUAL(ids.size(), 2U);
    BOOST_CHECK_EQUAL(ids[0]._group_id, 500U);
    BOOST_CHECK_EQUAL(a.inner()._allocations, 1U);
}

BOOST_AUTO_TEST_CASE(a_split_reevaluates_now_not_at_the_next_interval) {
    fixture f;
    elastic_shard_placement_driver<scripted_inner, ctl_t, cap_group, cap_key, cap_node> a{*f.ctl};
    for (int i = 0; i < 4; ++i) {
        f.heartbeat(a, 1);
    }
    const auto before = f.policy.evaluations();
    f.heartbeat(a, 1);
    BOOST_CHECK_EQUAL(f.policy.evaluations(), before);  // the hour has not passed

    const auto parent = f.sim.shard_of(1)._d;
    auto child = parent;
    child._group_id = 77;
    static_cast<void>(a.report_split(parent, {parent, child}));
    BOOST_CHECK_EQUAL(a.inner()._splits, 1U);  // still reaches the inner driver
    f.heartbeat(a, 1);
    BOOST_CHECK_EQUAL(f.policy.evaluations(), before + 1);
    // And the child is now a descriptor the controller can serve.
    BOOST_CHECK(a.lookup_descriptor(77).has_value());
}

BOOST_AUTO_TEST_CASE(the_inner_drivers_operator_wins_a_conflict) {
    const auto inner_op = op_on(1, cap_descriptor{}, 7);
    auto mine = op_on(1, cap_descriptor{}, ctl_t::first_operation_id);
    auto other = mine;
    other._group_id = 2;
    other._operation_id = ctl_t::first_operation_id + 1;
    const auto merged = adapter_t::merge({inner_op}, {mine, other});
    BOOST_REQUIRE_EQUAL(merged.size(), 2U);
    BOOST_CHECK_EQUAL(merged[0].operation_id(), 7U);  // inner first
    BOOST_CHECK_EQUAL(merged[1].group_id(), 2U);      // group 1's duplicate dropped
}

BOOST_AUTO_TEST_CASE(each_host_gets_only_the_operators_for_groups_it_leads) {
    // Several hosts share one controller; an operator for a group a host does
    // not lead would only be skipped there — and would cost the real leader a
    // retry interval.
    fixture f;
    adapter_t a{*f.ctl, cap_group{1000}, cap_group{2000}};
    for (int i = 0; i < 3; ++i) {
        for (cap_node h = 1; h <= 3; ++h) {
            f.heartbeat(a, h);
        }
    }
    f.clock.advance(1h);
    f.policy.push(cap_decision::scale_out(1, capacity_reason::density, {}));
    std::size_t emitted = 0;
    for (int i = 0; i < 30; ++i) {
        for (const auto& m : f.mgr.machines()) {
            if (m.joins() && !f.sim.has_host(m._id)) {
                f.sim.add_host(m._id, m._group, 1'000'000, 5s);
            }
        }
        // The new machine leads nothing yet but heartbeats all the same.
        for (const auto& n : f.sim.node_reports()) {
            static_cast<void>(a.report_node_heartbeat(n));
        }
        for (cap_node h = 1; h <= 3; ++h) {
            std::map<cap_group, cap_node> leaders;
            for (const auto& [g, s] : f.sim.shards()) {
                leaders[g] = s._leader;
            }
            const auto ops = f.heartbeat(a, h);
            for (const auto& op : ops) {
                ++emitted;
                BOOST_CHECK_EQUAL(leaders.at(op.group_id()), h);
            }
        }
    }
    BOOST_CHECK_GT(emitted, 0U);
    BOOST_CHECK(f.sim._skipped.empty());
}

BOOST_AUTO_TEST_CASE(a_failing_controller_degrades_to_the_inner_driver) {
    // Requirement 13.7.
    manual_clock clock;
    manual_executor ex;
    mock_t mgr{clock, topology()};
    broken_ledger ledger;
    scripted_capacity_policy policy;
    using broken_ctl = ctl_for<broken_ledger>;
    broken_ctl ctl{mgr, policy, ledger, toggle_lease{}, config(), ex.fn(), clock.fn()};
    elastic_shard_placement_driver<scripted_inner, broken_ctl, cap_group, cap_key, cap_node> a{ctl};
    const auto inner_op = op_on(1, cap_descriptor{}, 9);
    a.inner()._next = {inner_op};
    const auto ops = a.report_shard_heartbeat({}).get();
    BOOST_REQUIRE_EQUAL(ops.size(), 1U);
    BOOST_CHECK_EQUAL(ops[0].operation_id(), 9U);
    // Splits, node reports and lookups keep working through it too.
    static_cast<void>(a.report_split(cap_descriptor{}, {}));
    BOOST_CHECK_EQUAL(a.inner()._splits, 1U);
    BOOST_CHECK(!a.lookup_descriptor(1).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
