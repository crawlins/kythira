// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file elastic_capacity_harness.hpp
/// @brief One elastic capacity controller over the mock quorum manager and a
///        simulated cluster, on a manual clock and a manual executor. Shared by
///        the controller suite and the property and failover suites (tasks
///        8-12 and 15 of `.kiro/specs/elastic-shard-capacity/`).

#include "elastic_capacity_test_support.hpp"

#include <raft/capacity_ledger.hpp>
#include <raft/elastic_capacity_controller.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kythira::testing {

using namespace std::chrono_literals;

using cap_ledger = memory_capacity_ledger<cap_node, cap_pg>;
using cap_intent = capacity_intent<cap_node, cap_pg>;

inline auto evidence(const std::string& note = "test") -> capacity_evidence {
    capacity_evidence e;
    e._signals.push_back({._name = "shards_per_node", ._value = 250.0, ._threshold = 200.0});
    e._note = note;
    return e;
}

inline auto scale_out(std::size_t n = 1, capacity_reason r = capacity_reason::density)
    -> cap_decision {
    return cap_decision::scale_out(n, r, evidence());
}

inline auto three_zones() -> kythira::desired_topology<std::string> {
    return {.groups = {{.group_id = "a", .target_count = 1},
                       {.group_id = "b", .target_count = 1},
                       {.group_id = "c", .target_count = 1}}};
}

inline auto base_config() -> elastic_capacity_config {
    elastic_capacity_config c;
    c.enabled = true;
    c.dry_run = false;
    c.evaluation_interval = 1min;
    c.min_provider_call_interval = 0ms;
    c.max_provisions_per_window = 100;
    c.max_in_flight_intents = 1;
    c.node_report_staleness = 30s;
    c.assess_interval = 1min;
    c.operator_retry_interval = 20s;
    c.operator_busy_backoff = 2min;
    c.move_cooldown_after_split = 5min;
    c.jitter_seed = 7;
    return c;
}

/// One controller over a simulated three-zone cluster.
template<bool Keyed = true, bool Resizable = false> struct harness_t {
    using mock_t = kythira::testing::mock_capacity_quorum_manager<Keyed, Resizable>;
    using ctl_t = elastic_capacity_controller<mock_t, scripted_capacity_policy, cap_ledger,
                                              toggle_lease, cap_group, cap_key>;

    manual_clock clock;
    manual_executor ex;
    mock_t mgr;
    cap_ledger ledger;
    scripted_capacity_policy policy;
    toggle_lease lease;
    simulated_cluster sim;
    elastic_capacity_config cfg;
    std::unique_ptr<ctl_t> ctl;
    std::vector<cap_operation> last_ops;
    /// Machines the mock created that the harness should never let join.
    std::set<cap_node> held_back;

    explicit harness_t(elastic_capacity_config c = base_config(),
                       kythira::desired_topology<std::string> topo = three_zones(),
                       std::size_t shards = 6)
        : mgr(clock, topo), cfg(std::move(c)) {
        const std::vector<std::pair<cap_node, std::string>> hosts{{1, "a"}, {2, "b"}, {3, "c"}};
        for (const auto& [id, zone] : hosts) {
            sim.add_host(id, zone);
            mgr.add_existing(id, zone);
        }
        for (cap_group g = 1; g <= shards; ++g) {
            sim.add_shard(g, {1, 2, 3});
            // Spread leadership the way a real cluster would.
            sim.set_leader(g, static_cast<cap_node>(1 + (g % 3)));
        }
    }

    auto start() -> ctl_t& {
        ctl = std::make_unique<ctl_t>(mgr, policy, ledger, lease, cfg, ex.fn(), clock.fn());
        return *ctl;
    }

    /// Hosts report, the controller steps, hosts apply, provider work runs,
    /// time moves on. The order of a real heartbeat.
    auto round(std::chrono::milliseconds dt = 10s, bool run_executor = true) -> void {
        for (const auto& r : sim.node_reports()) {
            ctl->observe_node(r);
        }
        ctl->observe_shards(sim.shard_reports());
        last_ops = ctl->step();
        ctl->observe_operator_outcomes(sim.apply(last_ops));
        if (run_executor) {
            ex.run_all();
        }
        mgr.settle();
        sim.tick();
        for (const auto& m : mgr.machines()) {
            if (m.joins() && !sim.has_host(m._id) && !held_back.contains(m._id)) {
                sim.add_host(m._id, m._group, 1'000'000, std::chrono::seconds{5});
            }
            if (m._decommissioned && sim.has_host(m._id)) {
                sim.remove_host(m._id);
            }
        }
        clock.advance(dt);
    }

    auto rounds(std::size_t n, std::chrono::milliseconds dt = 10s) -> void {
        for (std::size_t i = 0; i < n; ++i) {
            round(dt);
        }
    }

    /// Past reconciliation, with a first assessment in hand, and with the
    /// next round due for an evaluation — so a decision a test queues next is
    /// acted on by the very next round.
    auto settle_in() -> void {
        rounds(3);
        clock.advance(cfg.evaluation_interval);
    }

    [[nodiscard]] auto intents() const -> std::vector<cap_intent> { return ledger.intents(); }

    auto dump() const -> void {
        for (const auto& i : intents()) {
            BOOST_TEST_MESSAGE("intent " << i._key << " state=" << kythira::to_string(i._state)
                                         << " node=" << (i._node ? std::to_string(*i._node) : "-")
                                         << " note=" << i._note);
        }
        for (const auto& [k, v] : ctl->counters()) {
            BOOST_TEST_MESSAGE("  " << k << "=" << v);
        }
    }

    [[nodiscard]] auto count_state(capacity_intent_state s) const -> std::size_t {
        const auto all = intents();
        return static_cast<std::size_t>(
            std::count_if(all.begin(), all.end(), [&](const auto& i) { return i._state == s; }));
    }

    [[nodiscard]] auto counter(const std::string& name) const -> std::uint64_t {
        return ctl->counter(name);
    }
};

using harness = harness_t<>;

/// Records `i` as a predecessor would have: `requested` first, then moved on
/// to `to` — a ledger refuses a record in any other state.
template<typename H>
auto seed(H& h, cap_intent i, capacity_intent_state to = capacity_intent_state::requested) -> void {
    i._state = capacity_intent_state::requested;
    h.ledger.record(i);
    if (to != capacity_intent_state::requested) {
        h.ledger.transition(i._key, to, {}, h.clock.now());
    }
}

inline auto predecessor_intent(manual_clock& clock, const std::string& key,
                               capacity_intent_state state = capacity_intent_state::requested)
    -> cap_intent {
    cap_intent i;
    i._key = key;
    i._kind = capacity_intent_kind::scale_out;
    i._state = state;
    i._group = "a";
    i._fencing_token = 1;
    i._created_at = clock.now();
    i._updated_at = clock.now();
    i._deadline = clock.now() + 10min;
    return i;
}

}  // namespace kythira::testing
