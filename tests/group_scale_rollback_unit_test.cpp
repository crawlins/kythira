// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file group_scale_rollback_unit_test.cpp
/// @brief The shared timeout-rollback planner
///        (`.kiro/specs/group-scale-up-rollback/`, Requirement 6).
///
/// Registered outside every cloud Kconfig gate: the planner is the one place
/// the ASG, VMSS and MIG managers' victim choice can be tested without a
/// cloud, so it has to run on every build leg.

#define BOOST_TEST_MODULE group_scale_rollback_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/group_scale_rollback.hpp>

#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using kythira::group_rollback::audit_after_shrink;
using kythira::group_rollback::describe;
using kythira::group_rollback::listed_member;
using kythira::group_rollback::member_state;
using kythira::group_rollback::plan_scale_up_rollback;
using kythira::group_rollback::rollback_outcome;
using kythira::group_rollback::settle_listing;
using kythira::group_rollback::settle_window;

auto live(std::string id, std::string node = {}) -> listed_member {
    return {.id = std::move(id),
            .state = member_state::live,
            .lifecycle = "InService",
            .node = std::move(node)};
}
auto pending(std::string id) -> listed_member {
    return {.id = std::move(id), .state = member_state::pending, .lifecycle = "Pending"};
}
auto terminal(std::string id) -> listed_member {
    return {.id = std::move(id), .state = member_state::terminal, .lifecycle = "Terminating"};
}

const std::vector<std::string> voters{"i-1", "i-2", "i-3"};

}  // namespace

BOOST_AUTO_TEST_CASE(a_fresh_pending_instance_is_removed_without_a_shrink) {
    const auto plan =
        plan_scale_up_rollback(voters, {live("i-1"), live("i-2"), live("i-3"), pending("i-4")});
    BOOST_TEST(plan.remove == std::vector<std::string>{"i-4"});
    BOOST_TEST(!plan.restore_desired_size);
}

BOOST_AUTO_TEST_CASE(a_fresh_terminal_instance_alone_plans_a_shrink) {
    const auto plan =
        plan_scale_up_rollback(voters, {live("i-1"), live("i-2"), live("i-3"), terminal("i-4")});
    BOOST_TEST(plan.remove.empty());
    BOOST_TEST(plan.restore_desired_size);
}

BOOST_AUTO_TEST_CASE(two_fresh_instances_are_both_removed_and_terminal_ones_skipped) {
    // The cloud replaced a failed launch: one terminating, two live or pending.
    const auto plan = plan_scale_up_rollback(voters, {live("i-1"), terminal("i-4"), pending("i-5"),
                                                      live("i-2"), live("i-6"), live("i-3")});
    BOOST_TEST(plan.remove == (std::vector<std::string>{"i-5", "i-6"}));
    BOOST_TEST(!plan.restore_desired_size);
}

BOOST_AUTO_TEST_CASE(a_pre_growth_pending_instance_is_never_removed) {
    // The old ASG `existing_ids` bug: a Pending instance left by an earlier
    // timeout is in the snapshot and must not be treated as this call's.
    const std::vector<std::string> pre{"i-1", "i-2", "i-3", "i-9"};
    const auto plan =
        plan_scale_up_rollback(pre, {live("i-1"), live("i-2"), live("i-3"), pending("i-9")});
    BOOST_TEST(plan.remove.empty());
    BOOST_TEST(plan.restore_desired_size);
}

BOOST_AUTO_TEST_CASE(an_empty_final_listing_plans_a_shrink) {
    const auto plan = plan_scale_up_rollback(voters, {});
    BOOST_TEST(plan.remove.empty());
    BOOST_TEST(plan.restore_desired_size);
}

BOOST_AUTO_TEST_CASE(duplicate_and_empty_ids_are_ignored) {
    const auto plan = plan_scale_up_rollback(voters, {pending("i-4"), pending("i-4"), pending("")});
    BOOST_TEST(plan.remove == std::vector<std::string>{"i-4"});
}

BOOST_AUTO_TEST_CASE(the_audit_reports_a_member_the_shrink_took) {
    const std::vector<listed_member> before{live("i-1", "1"), live("i-2", "2"), live("i-3", "3")};
    const auto audit = audit_after_shrink(voters, before, {live("i-2"), live("i-3")});
    BOOST_REQUIRE_EQUAL(audit.lost_members.size(), 1U);
    BOOST_TEST(audit.lost_members.front().id == "i-1");
    BOOST_TEST(audit.lost_members.front().node == "1");
    BOOST_TEST(audit.late_arrivals.empty());
}

BOOST_AUTO_TEST_CASE(the_audit_reports_a_terminating_member_with_its_node_id) {
    const std::vector<listed_member> before{live("i-1", "1"), live("i-2", "2"), live("i-3", "3")};
    const auto audit =
        audit_after_shrink(voters, before, {live("i-1"), terminal("i-2"), live("i-3")});
    BOOST_REQUIRE_EQUAL(audit.lost_members.size(), 1U);
    BOOST_TEST(audit.lost_members.front().id == "i-2");
    BOOST_TEST(audit.lost_members.front().node == "2");
}

BOOST_AUTO_TEST_CASE(a_member_already_leaving_before_the_shrink_is_not_reported) {
    const std::vector<listed_member> before{live("i-1"), terminal("i-2"), live("i-3")};
    const auto audit = audit_after_shrink(voters, before, {live("i-1"), live("i-3")});
    BOOST_TEST(audit.lost_members.empty());
}

BOOST_AUTO_TEST_CASE(a_late_arrival_is_reported_not_removed) {
    const std::vector<listed_member> before{live("i-1"), live("i-2"), live("i-3")};
    const auto audit = audit_after_shrink(
        voters, before, {live("i-1"), live("i-2"), live("i-3"), pending("i-7"), terminal("i-8")});
    BOOST_TEST(audit.lost_members.empty());
    BOOST_REQUIRE_EQUAL(audit.late_arrivals.size(), 1U);
    BOOST_TEST(audit.late_arrivals.front().id == "i-7");
}

BOOST_AUTO_TEST_CASE(a_failed_final_listing_still_lets_the_audit_report_a_loss) {
    const auto audit = audit_after_shrink(voters, {}, {live("i-2"), live("i-3")});
    BOOST_REQUIRE_EQUAL(audit.lost_members.size(), 1U);
    BOOST_TEST(audit.lost_members.front().id == "i-1");
}

BOOST_AUTO_TEST_CASE(the_plan_never_names_a_pre_growth_instance) {
    // Property: for random snapshots and listings, `remove` and the
    // snapshot are disjoint, and `remove` holds no terminal instance.
    std::mt19937 rng(20261003);
    const member_state states[] = {member_state::live, member_state::pending,
                                   member_state::terminal};
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::string> pre;
        std::vector<listed_member> listing;
        const int universe = 1 + static_cast<int>(rng() % 12);
        for (int i = 0; i < universe; ++i) {
            const auto id = "i-" + std::to_string(i);
            if (rng() % 2 == 0) {
                pre.push_back(id);
            }
            if (rng() % 3 != 0) {
                listing.push_back({.id = id, .state = states[rng() % 3]});
            }
        }
        const auto plan = plan_scale_up_rollback(pre, listing);
        for (const auto& id : plan.remove) {
            BOOST_TEST((std::ranges::find(pre, id) == pre.end()));
            const auto it = std::ranges::find(listing, id, &listed_member::id);
            BOOST_REQUIRE(it != listing.end());
            BOOST_TEST((it->state != member_state::terminal));
        }
        BOOST_TEST(plan.restore_desired_size == plan.remove.empty());
    }
}

BOOST_AUTO_TEST_CASE(settle_window_is_bounded_both_ways) {
    using namespace std::chrono_literals;
    BOOST_TEST(settle_window(300s, 5s).count() == std::chrono::milliseconds(30s).count());
    BOOST_TEST(settle_window(300s, 20s).count() == std::chrono::milliseconds(60s).count());
    BOOST_TEST(settle_window(2s, 5s).count() == std::chrono::milliseconds(2s).count());
}

BOOST_AUTO_TEST_CASE(settle_listing_stops_once_two_listings_agree) {
    using namespace std::chrono_literals;
    int calls = 0;
    const auto result = settle_listing(
        [&]() -> std::vector<listed_member> {
            ++calls;
            if (calls == 1) {
                throw std::runtime_error("transient");
            }
            if (calls == 2) {
                return {live("i-1"), pending("i-2")};
            }
            return {live("i-1"), live("i-2")};
        },
        10s, 0ms);
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(calls == 4);
    BOOST_TEST((result->at(1).state == member_state::live));
}

BOOST_AUTO_TEST_CASE(settle_listing_returns_nothing_when_every_listing_fails) {
    using namespace std::chrono_literals;
    const auto result = settle_listing(
        []() -> std::vector<listed_member> { throw std::runtime_error("down"); }, 20ms, 1ms);
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(describe_names_removed_instances_with_their_lifecycle) {
    rollback_outcome outcome;
    outcome.plan.remove = {"i-4"};
    outcome.removed = {"i-4"};
    outcome.final_listing = {live("i-1"), pending("i-4")};
    BOOST_TEST(describe(outcome, 4) == "rollback: removed i-4 (fresh, Pending)");
}

BOOST_AUTO_TEST_CASE(describe_reports_a_restore_and_a_lost_member) {
    rollback_outcome outcome;
    outcome.plan.restore_desired_size = true;
    outcome.restored_size = 3;
    outcome.audit.emplace();
    outcome.audit->lost_members.push_back(live("i-1", "7"));
    BOOST_TEST(describe(outcome, 4) ==
               "rollback: desired size restored to 3; lost member i-1 (node 7) during rollback");
}

BOOST_AUTO_TEST_CASE(describe_reports_a_failed_removal_and_the_size_left) {
    rollback_outcome outcome;
    outcome.plan.remove = {"i-4"};
    outcome.removal_failures = {{"i-4", "AccessDenied"}};
    BOOST_TEST(describe(outcome, 4) ==
               "rollback: removal of i-4 failed: AccessDenied; desired size left at 4");
}

BOOST_AUTO_TEST_CASE(describe_reports_a_failed_restore_and_an_unavailable_audit) {
    rollback_outcome outcome;
    outcome.plan.restore_desired_size = true;
    outcome.restore_error = "Throttling";
    outcome.audit_unavailable = true;
    const auto text = describe(outcome, 4);
    BOOST_TEST(text.find("restoring the desired size failed: Throttling; desired size left at 4") !=
               std::string::npos);
    BOOST_TEST(text.find("is unknown") != std::string::npos);
}
