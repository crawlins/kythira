// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file mock_capacity_quorum_manager_test.cpp
/// @brief The deterministic mock every elastic-capacity suite is built on
///        (task 7 of `.kiro/specs/elastic-shard-capacity/`).
///
/// A mock that lied would make every suite above it pass for the wrong
/// reason, so each programmable behaviour is checked here on its own: that a
/// latency really holds the future back until the clock moves, that a
/// never-returning call really never returns, that partial success really
/// leaves a billed, unreachable machine.

#define BOOST_TEST_MODULE mock_capacity_quorum_manager_test
#include <boost/test/unit_test.hpp>

#include "mock_capacity_quorum_manager.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using kythira::testing::manual_clock;
using kythira::testing::manual_executor;
using kythira::testing::mock_provision_outcome;
using kythira::testing::mock_provision_script;
using mock_t = kythira::testing::mock_capacity_quorum_manager<>;

using namespace std::chrono_literals;

auto two_zones() -> kythira::desired_topology<std::string> {
    return {.groups = {{.group_id = "a", .target_count = 1}, {.group_id = "b", .target_count = 1}}};
}

template<typename T>
concept has_key_lookup = requires(T& t) { t.find_by_idempotency_key(std::string{}); };
template<typename T>
concept has_group_target = requires(T& t) { t.set_group_target(std::string{}, std::size_t{}); };

template<typename F> auto failed(F&& f) -> bool {
    try {
        static_cast<void>(std::forward<F>(f).get());
        return false;
    } catch (...) {
        return true;
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(mock_capacity_quorum_manager_unit)

BOOST_AUTO_TEST_CASE(a_successful_call_creates_a_reachable_machine_that_joins) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    auto f = m.provision_node_keyed("a", std::nullopt, "key-1");
    BOOST_REQUIRE(f.isReady());
    const auto peer = std::move(f).get();
    BOOST_CHECK_EQUAL(peer.node_id, 1000U);
    BOOST_CHECK_EQUAL(peer.address, "mock-1000:7000");
    const auto machine = m.machine(1000);
    BOOST_REQUIRE(machine.has_value());
    BOOST_CHECK(machine->joins());
    BOOST_CHECK(machine->reachable());
    BOOST_CHECK_EQUAL(machine->_idempotency_key, "key-1");
    BOOST_CHECK_EQUAL(m.call_count("provision"), 1U);
    BOOST_CHECK_EQUAL(m.live_created_count(), 1U);
}

BOOST_AUTO_TEST_CASE(latency_holds_the_future_until_the_clock_moves) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.script({{._outcome = mock_provision_outcome::success, ._latency = 4min}});
    auto f = m.provision_node("a", std::nullopt);
    BOOST_CHECK(!f.isReady());
    clock.advance(3min);
    BOOST_CHECK_EQUAL(m.settle(), 0U);
    BOOST_CHECK(!f.isReady());
    clock.advance(1min);
    BOOST_CHECK_EQUAL(m.settle(), 1U);
    BOOST_CHECK(f.isReady());
    BOOST_CHECK(!failed(std::move(f)));
}

BOOST_AUTO_TEST_CASE(failure_and_stock_out_create_nothing) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.set_group_outcome("b", {._outcome = mock_provision_outcome::stock_out});
    m.script({{._outcome = mock_provision_outcome::failure}});
    BOOST_CHECK(failed(m.provision_node("a", std::nullopt)));
    BOOST_CHECK(failed(m.provision_node("b", std::nullopt)));
    BOOST_CHECK(failed(m.provision_node("b", std::nullopt)));
    // The script is consumed; the group rule persists; "a" is untouched.
    BOOST_CHECK(!failed(m.provision_node("a", std::nullopt)));
    BOOST_CHECK_EQUAL(m.live_created_count(), 1U);
}

BOOST_AUTO_TEST_CASE(partial_success_leaves_a_billed_unreachable_machine) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.script({{._outcome = mock_provision_outcome::partial}});
    const auto peer = std::move(m.provision_node("a", std::nullopt)).get();
    const auto machine = m.machine(peer.node_id);
    BOOST_REQUIRE(machine.has_value());
    BOOST_CHECK(!machine->reachable());
    BOOST_CHECK(!machine->joins());
    BOOST_CHECK_EQUAL(m.live_created_count(), 1U);

    const auto health = std::move(m.assess_quorum({})).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 0U);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(health.unreachable_nodes[0], peer.node_id);
}

BOOST_AUTO_TEST_CASE(never_joins_is_reachable_but_does_not_join) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.script({{._outcome = mock_provision_outcome::never_joins}});
    const auto peer = std::move(m.provision_node("a", std::nullopt)).get();
    BOOST_CHECK(m.machine(peer.node_id)->reachable());
    BOOST_CHECK(!m.machine(peer.node_id)->joins());
}

BOOST_AUTO_TEST_CASE(never_returns_never_returns_and_still_creates_the_orphan) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.script({{._outcome = mock_provision_outcome::never_returns}});
    auto f = m.provision_node_keyed("a", std::nullopt, "lost");
    clock.advance(24h);
    m.settle();
    BOOST_CHECK(!f.isReady());
    BOOST_CHECK_EQUAL(m.pending_count(), 1U);
    // The machine a lost call leaves behind is findable by its key — which is
    // exactly what reconciliation needs.
    const auto found = std::move(m.find_by_idempotency_key("lost")).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(m.live_created_count(), 1U);
}

BOOST_AUTO_TEST_CASE(decommission_is_idempotent) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    const auto peer = std::move(m.provision_node("a", std::nullopt)).get();
    BOOST_CHECK(!failed(m.decommission_node(peer.node_id)));
    BOOST_CHECK(!failed(m.decommission_node(peer.node_id)));
    BOOST_CHECK(!failed(m.decommission_node(424242)));
    BOOST_CHECK_EQUAL(m.live_created_count(), 0U);
    BOOST_CHECK(!std::move(m.find_by_idempotency_key("-")).get().has_value());
    BOOST_CHECK_EQUAL(m.call_count("decommission"), 3U);

    m.set_decommission_failure(true);
    BOOST_CHECK(failed(m.decommission_node(peer.node_id)));
}

BOOST_AUTO_TEST_CASE(assess_reports_per_group_health_and_unknown_nodes) {
    manual_clock clock;
    mock_t m{clock, two_zones()};
    m.add_existing(1, "a");
    m.add_existing(2, "b");
    const auto h = std::move(m.assess_quorum({{.node_id = 1, .group_id = "a"},
                                              {.node_id = 2, .group_id = "b"},
                                              {.node_id = 3, .group_id = "b"}}))
                       .get();
    BOOST_CHECK_EQUAL(h.live_node_count, 2U);
    BOOST_CHECK_EQUAL(h.total_node_count, 3U);
    BOOST_CHECK(h.status == kythira::quorum_status::degraded);
    BOOST_REQUIRE_EQUAL(h.groups.size(), 2U);
    BOOST_CHECK_EQUAL(h.groups[1].unreachable_nodes.size(), 1U);

    m.set_status_override(kythira::quorum_status::lost);
    BOOST_CHECK(std::move(m.assess_quorum({})).get().status == kythira::quorum_status::lost);
    m.set_assess_failure(true);
    BOOST_CHECK(failed(m.assess_quorum({})));
}

BOOST_AUTO_TEST_CASE(the_refinements_are_present_only_when_asked_for) {
    using plain = kythira::testing::mock_capacity_quorum_manager<false, false>;
    using resizable = kythira::testing::mock_capacity_quorum_manager<false, true>;
    static_assert(!has_key_lookup<plain>);
    static_assert(!has_group_target<plain>);
    static_assert(has_group_target<resizable>);
    static_assert(has_key_lookup<mock_t>);

    manual_clock clock;
    resizable r{clock, two_zones()};
    BOOST_CHECK(!failed(r.set_group_target("a", 4)));
    BOOST_CHECK_EQUAL(r.group_targets().at("a"), 4U);
}

BOOST_AUTO_TEST_CASE(the_manual_executor_runs_nothing_until_told) {
    manual_executor ex;
    int ran = 0;
    ex.add([&] {
        ++ran;
        ex.add([&] { ++ran; });
    });
    BOOST_CHECK_EQUAL(ran, 0);
    BOOST_CHECK_EQUAL(ex.pending(), 1U);
    BOOST_CHECK_EQUAL(ex.run_all(), 2U);
    BOOST_CHECK_EQUAL(ran, 2);
}

BOOST_AUTO_TEST_SUITE_END()
