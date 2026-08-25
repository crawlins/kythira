// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file shard_exceptions_unit_test.cpp
/// @brief Unit tests for the sharding layer's typed errors (task 3).
///
/// Two things are asserted for every type: that it lands in the existing
/// `raft_exception` hierarchy so a generic handler still catches it, and that
/// `what()` names the group — an operator reading a log line needs to know
/// *which* shard refused, and "shard busy" alone does not say.
///
/// `shard_epoch_mismatch_exception` gets extra attention because it is the one
/// carrying a payload the client acts on rather than merely reports.

#define BOOST_TEST_MODULE shard_exceptions_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/shard_exceptions.hpp>
#include <raft/shard_map.hpp>

#include <cstdint>
#include <string>

using kythira::cross_shard_command_exception;
using kythira::no_valid_split_key_exception;
using kythira::raft_exception;
using kythira::shard_alignment_required_exception;
using kythira::shard_busy_exception;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_epoch_ahead_exception;
using kythira::shard_epoch_mismatch_exception;
using kythira::shard_exception;
using kythira::shard_map;
using kythira::shard_merging_exception;
using kythira::shard_not_adjacent_exception;
using kythira::shard_range;
using kythira::split_key_out_of_range_exception;
using kythira::unknown_shard_exception;
using kythira::unrouted_key_exception;

namespace {

using key_type = std::string;
using group_type = std::uint64_t;
using node_type = std::uint64_t;
using desc_type = shard_descriptor<group_type, key_type, node_type>;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

auto mentions(const std::exception& e, const std::string& needle) -> bool {
    return std::string{e.what()}.find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(shard_exceptions_unit)

BOOST_AUTO_TEST_CASE(every_shard_error_is_a_raft_exception) {
    static_assert(std::is_base_of_v<raft_exception, shard_exception>);
    static_assert(
        std::is_base_of_v<shard_exception, shard_epoch_mismatch_exception<group_type, key_type>>);
    static_assert(std::is_base_of_v<shard_exception, shard_busy_exception<group_type>>);
    static_assert(std::is_base_of_v<shard_exception, unknown_shard_exception<group_type>>);

    // And a generic handler really does catch a concrete one.
    bool caught = false;
    try {
        throw shard_busy_exception<group_type>{42, "splitting"};
    } catch (const raft_exception& e) {
        caught = true;
        BOOST_CHECK(mentions(e, "42"));
    }
    BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(an_epoch_mismatch_names_the_group_and_both_epochs) {
    const shard_epoch requested{._version = 3, ._conf_version = 1};
    const shard_epoch current{._version = 5, ._conf_version = 1};
    const shard_epoch_mismatch_exception<group_type, key_type> e{7, requested, current, {}};

    BOOST_CHECK(mentions(e, "7"));
    BOOST_CHECK(mentions(e, "v=3"));
    BOOST_CHECK(mentions(e, "v=5"));
    BOOST_CHECK(mentions(e, "cv=1"));
    BOOST_CHECK(e.requested_epoch() == requested);
    BOOST_CHECK(e.current_epoch() == current);
}

BOOST_AUTO_TEST_CASE(an_epoch_mismatch_round_trips_its_descriptor_payload) {
    // The payload is not decoration: the client feeds it straight into its
    // routing map and retries, which is what makes a split cost one extra hop
    // instead of a placement-driver query.
    const std::vector<desc_type> children{
        desc_type{._group_id = 1,
                  ._range = range_of(std::nullopt, key_type{"m"}),
                  ._epoch = shard_epoch{._version = 5, ._conf_version = 1},
                  ._voters = {1, 2, 3}},
        desc_type{._group_id = 8,
                  ._range = range_of(key_type{"m"}, std::nullopt),
                  ._epoch = shard_epoch{._version = 5, ._conf_version = 1},
                  ._voters = {1, 2, 3}}};

    const shard_epoch_mismatch_exception<group_type, key_type> e{
        1, shard_epoch{._version = 3, ._conf_version = 1},
        shard_epoch{._version = 5, ._conf_version = 1}, children};

    BOOST_REQUIRE_EQUAL(e.current_descriptors().size(), 2u);
    BOOST_CHECK(e.current_descriptors()[0] == children[0]);
    BOOST_CHECK(e.current_descriptors()[1] == children[1]);
    BOOST_CHECK(mentions(e, "2 current descriptor(s)"));

    // And the payload really does repair a stale map.
    auto stale = shard_map<group_type, key_type, node_type>::single_shard(1, {1, 2, 3});
    BOOST_CHECK(stale.upsert_all(e.current_descriptors()));
    BOOST_CHECK_EQUAL(stale.size(), 2u);
    BOOST_CHECK(!stale.check_tiling().has_value());
    BOOST_CHECK_EQUAL(stale.lookup("z")->_group_id, 8u);
}

BOOST_AUTO_TEST_CASE(an_epoch_ahead_error_says_which_side_is_behind) {
    const shard_epoch_ahead_exception<group_type> e{4,
                                                    shard_epoch{._version = 9, ._conf_version = 0},
                                                    shard_epoch{._version = 6, ._conf_version = 0}};
    BOOST_CHECK(mentions(e, "4"));
    BOOST_CHECK(mentions(e, "ahead"));
    BOOST_CHECK(mentions(e, "v=9"));
    BOOST_CHECK(mentions(e, "v=6"));
}

BOOST_AUTO_TEST_CASE(a_non_adjacent_merge_names_both_ranges) {
    const shard_not_adjacent_exception<group_type, key_type> e{
        2, 5, range_of(key_type{"a"}, key_type{"f"}), range_of(key_type{"m"}, key_type{"z"})};
    BOOST_CHECK(mentions(e, "not adjacent"));
    BOOST_CHECK(mentions(e, "[a, f)"));
    BOOST_CHECK(mentions(e, "[m, z)"));
    BOOST_CHECK_EQUAL(e.source_group_id(), 2u);
    BOOST_CHECK_EQUAL(e.target_group_id(), 5u);
}

BOOST_AUTO_TEST_CASE(a_busy_shard_names_the_state_that_refused) {
    const shard_busy_exception<group_type> e{11, "merging_source"};
    BOOST_CHECK(mentions(e, "11"));
    BOOST_CHECK(mentions(e, "merging_source"));
    BOOST_CHECK_EQUAL(e.state(), "merging_source");
}

BOOST_AUTO_TEST_CASE(an_alignment_error_points_at_the_opt_in_that_fixes_it) {
    const shard_alignment_required_exception<group_type, node_type> e{3, 4, {1, 2, 3}, {1, 2, 9}};
    BOOST_CHECK(mentions(e, "not colocated"));
    BOOST_CHECK(mentions(e, "_auto_align"));
    BOOST_CHECK_EQUAL(e.source_members().size(), 3u);
    BOOST_CHECK_EQUAL(e.target_members().size(), 3u);
}

BOOST_AUTO_TEST_CASE(an_out_of_range_split_key_names_the_key_and_the_range) {
    const split_key_out_of_range_exception<group_type, key_type> e{
        6, key_type{"zz"}, range_of(key_type{"a"}, key_type{"f"})};
    BOOST_CHECK(mentions(e, "6"));
    BOOST_CHECK(mentions(e, "zz"));
    BOOST_CHECK(mentions(e, "[a, f)"));
}

BOOST_AUTO_TEST_CASE(an_exhausted_veto_reports_how_many_keys_were_tried) {
    const no_valid_split_key_exception<group_type> e{9, 12};
    BOOST_CHECK(mentions(e, "9"));
    BOOST_CHECK(mentions(e, "12 candidate(s)"));
    BOOST_CHECK(mentions(e, "vetoed"));
    BOOST_CHECK_EQUAL(e.candidates_considered(), 12u);
}

BOOST_AUTO_TEST_CASE(a_merging_shard_tells_the_client_to_retry) {
    const shard_merging_exception<group_type> e{3, 4};
    BOOST_CHECK(mentions(e, "3"));
    BOOST_CHECK(mentions(e, "4"));
    BOOST_CHECK(mentions(e, "retry"));
}

BOOST_AUTO_TEST_CASE(a_cross_shard_command_is_named_as_unsupported_not_as_a_bug) {
    const cross_shard_command_exception<group_type, key_type> e{
        2, key_type{"q"}, range_of(key_type{"a"}, key_type{"f"})};
    BOOST_CHECK(mentions(e, "q"));
    BOOST_CHECK(mentions(e, "cross-shard commands are not supported"));
}

BOOST_AUTO_TEST_CASE(unknown_and_unrouted_are_distinct_failures) {
    const unknown_shard_exception<group_type> by_group{77};
    BOOST_CHECK(mentions(by_group, "77"));
    BOOST_CHECK_EQUAL(by_group.group_id(), 77u);

    const unknown_shard_exception<group_type> with_detail{77, "tombstoned by an earlier merge"};
    BOOST_CHECK(mentions(with_detail, "tombstoned"));

    const unrouted_key_exception<key_type> by_key{key_type{"h"}};
    BOOST_CHECK(mentions(by_key, "h"));
    BOOST_CHECK(mentions(by_key, "gap"));
}

BOOST_AUTO_TEST_CASE(an_unstreamable_key_degrades_to_a_placeholder_rather_than_failing_to_compile) {
    // `shard_key` deliberately does not require streamability. A diagnostic
    // that could not be built for such a key would force the concept to widen.
    struct opaque_key {
        int _v{0};
        auto operator<=>(const opaque_key&) const = default;
    };
    static_assert(kythira::shard_key<opaque_key>);

    const unrouted_key_exception<opaque_key> e{opaque_key{._v = 3}};
    BOOST_CHECK(mentions(e, "<opaque>"));
}

BOOST_AUTO_TEST_SUITE_END()
