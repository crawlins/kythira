// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_placement_driver_unit_test.cpp
/// @brief The placement-driver concept, its reports, its operator variant, and
///        the no-op default (task 27 of `.kiro/specs/multi-raft/`).
///
/// Two things are actually being defended here. The first is that
/// `no_op_shard_placement_driver` is a *complete* driver rather than a stub
/// with holes: a static, pre-split deployment must run against it with no
/// control plane at all, and the moment one of its methods is unimplemented
/// that stops being true. The second is that its id range is honest about
/// running out — returning fewer ids than asked for is what the host reads as
/// "the authority is unavailable", and the one thing it must never do is
/// wrap around and hand out an id twice.

#define BOOST_TEST_MODULE shard_placement_driver_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/shard_placement_driver.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

using kythira::add_replica_operator;
using kythira::merge_operator;
using kythira::no_op_shard_placement_driver;
using kythira::node_report;
using kythira::remove_replica_operator;
using kythira::scatter_operator;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_operation;
using kythira::shard_placement_driver;
using kythira::shard_report;
using kythira::skipped_operator_reason;
using kythira::split_operator;
using kythira::transfer_leader_operator;

namespace {

using group_type = std::uint64_t;
using key_type = std::string;
using node_type = std::uint64_t;

using driver_type = no_op_shard_placement_driver<group_type, key_type, node_type>;
using descriptor_type = shard_descriptor<group_type, key_type, node_type>;
using operation_type = shard_operation<group_type, key_type, node_type>;
using report_type = shard_report<group_type, key_type, node_type>;

auto descriptor(group_type id, std::uint64_t version) -> descriptor_type {
    descriptor_type d;
    d._group_id = id;
    d._epoch = shard_epoch{._version = version, ._conf_version = 1};
    d._voters = {1, 2, 3};
    return d;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_placement_driver_unit)

// ── the concept ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_no_op_driver_satisfies_the_concept) {
    static_assert(shard_placement_driver<driver_type, group_type, key_type, node_type>);

    // A driver missing any one method does not qualify. This is the property
    // that makes the no-op safe to ship as the default: a static deployment
    // calls every one of these, and a partial implementation would compile
    // right up until the first split asked for an id.
    struct missing_allocate {
        auto report_shard_heartbeat(const std::vector<report_type>&)
            -> kythira::future_default<std::vector<operation_type>> {
            return kythira::future_factory_default::makeFuture(std::vector<operation_type>{});
        }
        auto report_node_heartbeat(const node_report<node_type>&) -> kythira::future_default<void> {
            return kythira::future_factory_default::makeFuture();
        }
        auto report_split(const descriptor_type&, const std::vector<descriptor_type>&)
            -> kythira::future_default<void> {
            return kythira::future_factory_default::makeFuture();
        }
        auto report_merge(const descriptor_type&, const descriptor_type&)
            -> kythira::future_default<void> {
            return kythira::future_factory_default::makeFuture();
        }
    };
    static_assert(!shard_placement_driver<missing_allocate, group_type, key_type, node_type>);
    BOOST_CHECK(true);
}

// ── the reserved id range ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ids_come_from_the_reserved_range_in_order) {
    driver_type driver{100, 110};
    auto ids = std::move(driver.allocate_shard_ids(3)).get();

    BOOST_REQUIRE_EQUAL(ids.size(), 3U);
    BOOST_CHECK_EQUAL(ids[0]._group_id, 100U);
    BOOST_CHECK_EQUAL(ids[1]._group_id, 101U);
    BOOST_CHECK_EQUAL(ids[2]._group_id, 102U);

    // Empty suggested placement means "inherit the parent's replica set", which
    // is what a split does when nobody has a reason to move the data.
    BOOST_CHECK(ids[0]._suggested_voters.empty());
    BOOST_CHECK_EQUAL(driver.allocated_count(), 3U);
}

BOOST_AUTO_TEST_CASE(an_exhausted_range_returns_fewer_ids_and_never_repeats_one) {
    // The range is half-open, so [10, 13) is exactly three ids.
    driver_type driver{10, 13};

    auto first = std::move(driver.allocate_shard_ids(2)).get();
    BOOST_REQUIRE_EQUAL(first.size(), 2U);

    // Asked for four, only one left. Short is the correct answer: the host
    // reads it as "the authority is unavailable" and abandons the split. The
    // failure this rules out is the other one — wrapping and reissuing an id
    // that some other shard already owns, which no later check would catch.
    auto second = std::move(driver.allocate_shard_ids(4)).get();
    BOOST_REQUIRE_EQUAL(second.size(), 1U);
    BOOST_CHECK_EQUAL(second[0]._group_id, 12U);

    auto third = std::move(driver.allocate_shard_ids(1)).get();
    BOOST_CHECK(third.empty());
    BOOST_CHECK(!driver.remaining_ids());

    std::vector<group_type> seen;
    for (const auto& a : first) {
        seen.push_back(a._group_id);
    }
    for (const auto& a : second) {
        seen.push_back(a._group_id);
    }
    const std::vector<group_type> expected{10, 11, 12};
    BOOST_CHECK(seen == expected);
}

BOOST_AUTO_TEST_CASE(a_default_constructed_driver_allocates_nothing) {
    // No reserved range configured means no ids, which is the honest answer for
    // a deployment that never told anyone which slice of the id space is
    // theirs. Silently starting at zero would be the way two machines collide.
    driver_type driver;
    BOOST_CHECK(std::move(driver.allocate_shard_ids(1)).get().empty());
    BOOST_CHECK(!driver.remaining_ids());
}

// ── the no-op's remaining surface ────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_no_op_driver_accepts_every_report_and_returns_no_operators) {
    driver_type driver{1, 100};

    std::vector<report_type> reports;
    for (group_type g = 1; g <= 3; ++g) {
        report_type r;
        r._descriptor = descriptor(g, 1);
        r._leader = 1;
        r._approximate_size_bytes = 1024;
        r._size_available = true;
        reports.push_back(std::move(r));
    }

    auto operators = std::move(driver.report_shard_heartbeat(reports)).get();
    BOOST_CHECK(operators.empty());
    BOOST_CHECK_EQUAL(driver.shard_heartbeat_count(), 1U);
    BOOST_CHECK_EQUAL(driver.last_shard_report_count(), 3U);

    node_report<node_type> nr;
    nr._node_id = 1;
    nr._shard_count = 3;
    std::move(driver.report_node_heartbeat(nr)).get();
    BOOST_CHECK_EQUAL(driver.node_heartbeat_count(), 1U);

    std::move(driver.report_split(descriptor(1, 1), {descriptor(1, 2), descriptor(4, 2)})).get();
    std::move(driver.report_merge(descriptor(4, 2), descriptor(1, 3))).get();
    BOOST_CHECK_EQUAL(driver.splits_reported(), 1U);
    BOOST_CHECK_EQUAL(driver.merges_reported(), 1U);
}

// ── operators ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(every_operator_alternative_has_a_stable_name) {
    // The name is a metric dimension, so it is part of the interface an
    // operator builds a dashboard against — not a debug string.
    const auto name_of = [](auto op) {
        operation_type o;
        o._operator = op;
        return std::string(o.name());
    };

    BOOST_CHECK_EQUAL(name_of(add_replica_operator<node_type>{._node = 4}), "add_replica");
    BOOST_CHECK_EQUAL(name_of(remove_replica_operator<node_type>{._node = 4}), "remove_replica");
    BOOST_CHECK_EQUAL(name_of(transfer_leader_operator<node_type>{._to = 2}), "transfer_leader");
    BOOST_CHECK_EQUAL(name_of(split_operator<key_type>{._at_keys = {"m"}}), "split");
    BOOST_CHECK_EQUAL(name_of(merge_operator<group_type>{._into = 7}), "merge");
    BOOST_CHECK_EQUAL(name_of(scatter_operator{}), "scatter");
}

BOOST_AUTO_TEST_CASE(a_new_replica_is_added_as_a_learner_by_default) {
    // Raft's own membership-change safety argument prefers catching a new
    // member up as a non-voter first, so that adding it cannot enlarge the
    // quorum before it can answer.
    const add_replica_operator<node_type> op{._node = 4};
    BOOST_CHECK(op._as_learner);
}

BOOST_AUTO_TEST_CASE(an_operator_carries_the_epoch_it_was_computed_against) {
    operation_type op;
    op._group_id = 7;
    op._operation_id = 42;
    op._epoch = shard_epoch{._version = 3, ._conf_version = 1};
    op._operator = scatter_operator{};

    BOOST_CHECK_EQUAL(op.group_id(), 7U);
    BOOST_CHECK_EQUAL(op.operation_id(), 42U);
    BOOST_CHECK(op.epoch() == (shard_epoch{._version = 3, ._conf_version = 1}));
    BOOST_CHECK(std::holds_alternative<scatter_operator>(op.kind()));
}

BOOST_AUTO_TEST_CASE(every_skip_reason_has_a_distinct_printable_name) {
    // These are metric dimensions too. Two reasons sharing a string would merge
    // "the driver's view is one heartbeat stale" — which is fine and
    // self-correcting — with "this shard has been stuck mid-merge for an hour",
    // which is a page.
    const skipped_operator_reason all[] = {
        skipped_operator_reason::stale_epoch,    skipped_operator_reason::unknown_shard,
        skipped_operator_reason::not_leader,     skipped_operator_reason::shard_busy,
        skipped_operator_reason::unsupported,    skipped_operator_reason::precondition,
        skipped_operator_reason::driver_disabled};

    std::vector<std::string> names;
    for (auto r : all) {
        std::string name = to_string(r);
        BOOST_CHECK(!name.empty());
        BOOST_CHECK_NE(name, "unknown");
        for (const auto& seen : names) {
            BOOST_CHECK_NE(name, seen);
        }
        names.push_back(std::move(name));
    }
}

// ── report shapes ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_absent_size_measurement_is_not_a_zero_measurement) {
    // `_size_available == false` and `_approximate_size_bytes == 0` mean
    // opposite things to a driver: "I cannot tell" versus "this shard is
    // empty". Conflating them is how a live shard gets merged away.
    report_type blind;
    blind._descriptor = descriptor(1, 1);
    BOOST_CHECK(!blind._size_available);
    BOOST_CHECK_EQUAL(blind._approximate_size_bytes, 0U);

    report_type measured;
    measured._descriptor = descriptor(1, 1);
    measured._size_available = true;
    measured._approximate_size_bytes = 0;
    BOOST_CHECK(measured._size_available);
}

BOOST_AUTO_TEST_CASE(a_node_report_defaults_to_not_overloaded) {
    // The honest answer for a host with no way to tell. Defaulting to
    // "overloaded" would stop a cluster from ever placing a shard on a machine
    // that simply has no capacity probe configured.
    const node_report<node_type> nr;
    BOOST_CHECK(!nr.overloaded());
    BOOST_CHECK_EQUAL(nr._shard_count, 0U);
    BOOST_CHECK(nr._labels.empty());
}

BOOST_AUTO_TEST_SUITE_END()
