// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The compatibility profile (`.kiro/specs/kinesis-compatible-streams/`
// task 4, Requirement 3): AWS's behaviour by default with throttling on,
// cluster settings, per-stream tags winning, typos refused.

#define BOOST_TEST_MODULE kinesis_compat_profile_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_compat_profile.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin = kythira::kinesis;

BOOST_AUTO_TEST_CASE(the_default_is_aws_with_throttling_on) {
    const kin::kinesis_compat_profile p;
    BOOST_TEST(p._throttling);
    BOOST_TEST(p._aws_limits);
    BOOST_TEST(p._aws_count_limits);
    BOOST_TEST(p._aws_iterator_lifetime);
    BOOST_TEST((p._id_format == kin::sequence_profile::aws));
    BOOST_TEST(p.relaxed().empty());
    BOOST_TEST((kin::cluster_profile({}).value() == p));
}

BOOST_AUTO_TEST_CASE(cluster_settings_relax_switches) {
    const auto p =
        kin::cluster_profile(
            {{"throttling", "off"}, {"ID-Format", "COMPACT"}, {"iterator-lifetime", "relaxed"}})
            .value();
    BOOST_TEST(!p._throttling);
    BOOST_TEST((p._id_format == kin::sequence_profile::compact));
    BOOST_TEST(!p._aws_iterator_lifetime);
    BOOST_TEST(p._aws_limits);
    BOOST_TEST((p.relaxed() ==
                std::vector<std::string_view>{"throttling", "iterator-lifetime", "id-format"}));

    const auto q =
        kin::cluster_profile({{"limits", "relaxed"}, {"count-limits", "relaxed"}}).value();
    BOOST_TEST((q.relaxed() == std::vector<std::string_view>{"limits", "count-limits"}));
    // Setting a switch back to AWS's value is allowed and clears it.
    BOOST_TEST(kin::cluster_profile({{"throttling", "off"}, {"throttling", "on"}})
                   .value()
                   .relaxed()
                   .empty());
}

BOOST_AUTO_TEST_CASE(unknown_settings_are_refused) {
    const auto bad_name = kin::cluster_profile({{"throtling", "off"}});
    BOOST_REQUIRE(!bad_name.has_value());
    BOOST_TEST(bad_name.error().type == "InvalidArgumentException");
    BOOST_TEST(bad_name.error().status == 400U);
    BOOST_TEST(!kin::cluster_profile({{"throttling", "maybe"}}).has_value());
    BOOST_TEST(!kin::cluster_profile({{"id-format", "relaxed"}}).has_value());

    BOOST_TEST(!kin::check_compat_tags({{"kythira:compat:limits", "aws"}, {"team", "x"}}));
    const auto e = kin::check_compat_tags({{"kythira:compat:limits", "none"}});
    BOOST_REQUIRE(e.has_value());
    BOOST_TEST(*e->message == "Unknown Kythira compatibility setting kythira:compat:limits=none");
    BOOST_TEST(kin::check_compat_tags({{"kythira:compat:nope", "on"}}).has_value());
    // The bare prefix is an ordinary tag, not a switch.
    BOOST_TEST(!kin::check_compat_tags({{"kythira:compat:", "x"}}));
}

BOOST_AUTO_TEST_CASE(stream_tags_win_over_the_cluster) {
    const auto cluster = kin::cluster_profile({{"throttling", "off"}}).value();
    const auto p = kin::resolve_profile(cluster, {{"kythira:compat:throttling", "on"},
                                                  {"kythira:compat:id-format", "compact"},
                                                  {"owner", "data-team"}});
    BOOST_TEST(p._throttling);
    BOOST_TEST((p._id_format == kin::sequence_profile::compact));
    // A bad tag left over from an older release is skipped, not fatal.
    const auto q = kin::resolve_profile(cluster, {{"kythira:compat:throttling", "sideways"}});
    BOOST_TEST(!q._throttling);
}
