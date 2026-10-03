// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE membership_change_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/config_entry.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(membership_change_unit)

// entry_type enum has the expected discriminants
BOOST_AUTO_TEST_CASE(entry_type_enum_values) {
    BOOST_CHECK(static_cast<int>(kythira::entry_type::normal) == 0);
    BOOST_CHECK(static_cast<int>(kythira::entry_type::configuration) == 1);
}

// log_entry default-initialises _type to normal
BOOST_AUTO_TEST_CASE(log_entry_default_type) {
    using entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
    entry_t e{1, 1, {}};
    BOOST_CHECK(e.type() == kythira::entry_type::normal);
}

// log_entry can be constructed with configuration type
BOOST_AUTO_TEST_CASE(log_entry_configuration_type) {
    using entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
    entry_t e{1, 1, {}, kythira::entry_type::configuration};
    BOOST_CHECK(e.type() == kythira::entry_type::configuration);
}

// log_entry_type concept requires type() accessor
BOOST_AUTO_TEST_CASE(log_entry_type_concept) {
    using entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
    static_assert(kythira::log_entry_type<entry_t, std::uint64_t, std::uint64_t>);
}

// serialize/deserialize round-trip: simple non-joint configuration
BOOST_AUTO_TEST_CASE(config_round_trip_simple) {
    using cfg_t = kythira::cluster_configuration<std::uint64_t>;
    cfg_t orig{{1, 2, 3}, false, std::nullopt};
    auto bytes = kythira::serialize_configuration<std::uint64_t>(orig);
    auto restored = kythira::deserialize_configuration<std::uint64_t>(bytes);
    BOOST_CHECK(restored.nodes() == orig.nodes());
    BOOST_CHECK_EQUAL(restored.is_joint_consensus(), false);
    BOOST_CHECK(!restored.old_nodes().has_value());
}

// serialize/deserialize round-trip: joint consensus configuration
BOOST_AUTO_TEST_CASE(config_round_trip_joint) {
    using cfg_t = kythira::cluster_configuration<std::uint64_t>;
    std::vector<std::uint64_t> c_new{1, 2, 3, 4};
    std::vector<std::uint64_t> c_old{1, 2, 3};
    cfg_t orig{c_new, true, c_old};
    auto bytes = kythira::serialize_configuration<std::uint64_t>(orig);
    auto restored = kythira::deserialize_configuration<std::uint64_t>(bytes);
    BOOST_CHECK(restored.nodes() == c_new);
    BOOST_CHECK_EQUAL(restored.is_joint_consensus(), true);
    BOOST_REQUIRE(restored.old_nodes().has_value());
    BOOST_CHECK(*restored.old_nodes() == c_old);
}

// serialize/deserialize round-trip: string node IDs
BOOST_AUTO_TEST_CASE(config_round_trip_string_ids) {
    using cfg_t = kythira::cluster_configuration<std::string>;
    std::vector<std::string> nodes{"node-1", "node-2", "node-3"};
    cfg_t orig{nodes, false, std::nullopt};
    auto bytes = kythira::serialize_configuration<std::string>(orig);
    auto restored = kythira::deserialize_configuration<std::string>(bytes);
    BOOST_CHECK(restored.nodes() == nodes);
    BOOST_CHECK_EQUAL(restored.is_joint_consensus(), false);
}

// serialize/deserialize round-trip: joint string IDs
BOOST_AUTO_TEST_CASE(config_round_trip_joint_string_ids) {
    using cfg_t = kythira::cluster_configuration<std::string>;
    std::vector<std::string> c_new{"a", "b", "c", "d"};
    std::vector<std::string> c_old{"a", "b", "c"};
    cfg_t orig{c_new, true, c_old};
    auto bytes = kythira::serialize_configuration<std::string>(orig);
    auto restored = kythira::deserialize_configuration<std::string>(bytes);
    BOOST_CHECK(restored.nodes() == c_new);
    BOOST_CHECK_EQUAL(restored.is_joint_consensus(), true);
    BOOST_REQUIRE(restored.old_nodes().has_value());
    BOOST_CHECK(*restored.old_nodes() == c_old);
}

// joint consensus: old_nodes omitted when is_joint_consensus == false
BOOST_AUTO_TEST_CASE(config_no_old_nodes_when_non_joint) {
    using cfg_t = kythira::cluster_configuration<std::uint64_t>;
    cfg_t orig{{1, 2, 3}, false, std::vector<std::uint64_t>{4, 5}};
    auto bytes = kythira::serialize_configuration<std::uint64_t>(orig);
    auto restored = kythira::deserialize_configuration<std::uint64_t>(bytes);
    BOOST_CHECK(!restored.old_nodes().has_value());
}

// Placement-group assignments round-trip with the configuration, for
// numeric and string node IDs.
BOOST_AUTO_TEST_CASE(config_round_trip_placement) {
    kythira::cluster_configuration<std::uint64_t> numeric{{1, 2}, false, std::nullopt, {3}};
    std::map<std::uint64_t, std::string> numeric_placement{{1, "az-a"}, {2, "az-b"}, {3, "az-a"}};
    auto bytes = kythira::serialize_configuration<std::uint64_t>(numeric, &numeric_placement);
    BOOST_CHECK(kythira::deserialize_placement<std::uint64_t>(bytes) == numeric_placement);
    BOOST_CHECK(kythira::deserialize_configuration<std::uint64_t>(bytes).learners() ==
                std::vector<std::uint64_t>{3});

    kythira::cluster_configuration<std::string> named{{"n1"}, false, std::nullopt};
    std::map<std::string, std::string> named_placement{{"n1", "zone-1"}};
    auto named_bytes = kythira::serialize_configuration<std::string>(named, &named_placement);
    BOOST_CHECK(kythira::deserialize_placement<std::string>(named_bytes) == named_placement);
}

// An entry written without placement (by an older node, or one whose
// placement groups are not strings) yields an empty map.
BOOST_AUTO_TEST_CASE(config_without_placement_yields_empty_map) {
    kythira::cluster_configuration<std::uint64_t> cfg{{1, 2, 3}, false, std::nullopt};
    auto bytes = kythira::serialize_configuration<std::uint64_t>(cfg);
    BOOST_CHECK(kythira::deserialize_placement<std::uint64_t>(bytes).empty());
}

BOOST_AUTO_TEST_SUITE_END()
