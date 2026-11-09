// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The compatibility profile (`.kiro/specs/dynamodb-compatible-api/` task 4,
// Requirement 3): AWS's behaviour by default (throttling on), cluster
// settings, per-table tags winning, typos refused.

#define BOOST_TEST_MODULE dynamodb_compat_profile_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/compat_profile.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ddb = kythira::dynamodb;
using settings = std::vector<std::pair<std::string, std::string>>;

BOOST_AUTO_TEST_CASE(the_default_is_aws_with_throttling_on) {
    const ddb::dynamo_compat_profile p;
    BOOST_TEST(p.throttling);
    BOOST_TEST(p.aws_limits);
    BOOST_TEST(p.async_gsi);
    BOOST_TEST(p.relaxed().empty());
    BOOST_TEST((ddb::cluster_profile({}).value() == p));
}

BOOST_AUTO_TEST_CASE(cluster_settings_relax_switches) {
    const auto p =
        ddb::cluster_profile(
            {{"throttling", "off"}, {"GSI-Consistency", "SYNC"}, {"ttl-delay", "prompt"}})
            .value();
    BOOST_TEST(!p.throttling);
    BOOST_TEST(!p.async_gsi);
    BOOST_TEST(!p.aws_ttl_delay);
    BOOST_TEST(p.aws_limits);
    BOOST_TEST((p.relaxed() ==
                std::vector<std::string_view>{"throttling", "gsi-consistency", "ttl-delay"}));
}

BOOST_AUTO_TEST_CASE(unknown_settings_are_refused) {
    const auto bad_name = ddb::cluster_profile({{"throtling", "off"}});
    BOOST_REQUIRE(!bad_name.has_value());
    BOOST_TEST(bad_name.error().type == "com.amazon.coral.validate#ValidationException");
    BOOST_TEST(!ddb::cluster_profile({{"throttling", "maybe"}}).has_value());

    BOOST_TEST(!ddb::check_compat_tags({{"kythira:compat:limits", "aws"}, {"team", "x"}}));
    const auto e = ddb::check_compat_tags({{"kythira:compat:limits", "none"}});
    BOOST_REQUIRE(e.has_value());
    BOOST_TEST(*e->message ==
               "One or more parameter values were invalid: Unknown Kythira compatibility setting "
               "kythira:compat:limits=none");
    BOOST_TEST(ddb::check_compat_tags({{"kythira:compat:nope", "on"}}).has_value());
}

BOOST_AUTO_TEST_CASE(table_tags_win_over_the_cluster) {
    const auto cluster = ddb::cluster_profile({{"throttling", "off"}}).value();
    const auto p = ddb::resolve_profile(cluster, {{"kythira:compat:throttling", "on"},
                                                  {"kythira:compat:capacity-reporting", "off"},
                                                  {"owner", "data-team"}});
    BOOST_TEST(p.throttling);
    BOOST_TEST(!p.capacity_reporting);
    // A bad tag left over from an older release is skipped, not fatal.
    const auto q = ddb::resolve_profile(cluster, {{"kythira:compat:throttling", "sideways"}});
    BOOST_TEST(!q.throttling);
}
