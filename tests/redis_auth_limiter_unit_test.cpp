// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_auth_limiter_unit_test.cpp
/// @brief AUTH admission control for the Redis gateway (vulnerability audit
///        2026-10-02, M22): attempts are charged before the KDF, IPv6
///        sources are keyed by /64, and derivations are capped gateway-wide.

#define BOOST_TEST_MODULE redis_auth_limiter_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/redis_auth_limiter.hpp>

#include <boost/asio/ip/address.hpp>

#include <chrono>
#include <string>

using kythira::redis_auth_limiter;
using verdict = redis_auth_limiter::verdict;

namespace {

auto key_of(const char* address) -> std::string {
    return redis_auth_limiter::rate_key(boost::asio::ip::make_address(address));
}

}  // namespace

BOOST_AUTO_TEST_CASE(rate_key_groups_ipv6_by_64, *boost::unit_test::timeout(10)) {
    BOOST_CHECK_EQUAL(key_of("192.0.2.7"), "192.0.2.7");
    BOOST_CHECK_EQUAL(key_of("::ffff:192.0.2.7"), "192.0.2.7");
    BOOST_CHECK_EQUAL(key_of("2001:db8:1:2:aaaa:bbbb:cccc:dddd"), "2001:db8:1:2::/64");
    BOOST_CHECK_EQUAL(key_of("2001:db8:1:2::1"), key_of("2001:db8:1:2:ffff::9"));
    BOOST_CHECK_NE(key_of("2001:db8:1:2::1"), key_of("2001:db8:1:3::1"));
}

// The first version recorded a failure only after the KDF, so any number of
// attempts from one source could be in flight at once. Now each attempt is
// charged on admission, and concurrent ones see each other.
BOOST_AUTO_TEST_CASE(attempts_are_charged_before_the_kdf, *boost::unit_test::timeout(10)) {
    redis_auth_limiter l(3, std::chrono::seconds{60}, 100);
    auto now = redis_auth_limiter::clock::now();
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    // Three still running, none finished: the fourth is already refused.
    BOOST_CHECK(l.begin("a", now) == verdict::rate_limited);
    BOOST_CHECK(l.begin("b", now) == verdict::allowed);
    BOOST_CHECK_EQUAL(l.in_flight(), 4u);
}

BOOST_AUTO_TEST_CASE(success_refunds_its_charge, *boost::unit_test::timeout(10)) {
    redis_auth_limiter l(2, std::chrono::seconds{60}, 100);
    auto now = redis_auth_limiter::clock::now();
    for (int i = 0; i < 10; ++i) {
        BOOST_REQUIRE(l.begin("a", now) == verdict::allowed);
        l.finish("a", true);
    }
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    l.finish("a", false);
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    l.finish("a", false);
    BOOST_CHECK(l.begin("a", now) == verdict::rate_limited);
    BOOST_CHECK_EQUAL(l.in_flight(), 0u);
}

BOOST_AUTO_TEST_CASE(window_expiry_restores_the_budget, *boost::unit_test::timeout(10)) {
    redis_auth_limiter l(1, std::chrono::seconds{60}, 100);
    auto t0 = redis_auth_limiter::clock::now();
    BOOST_CHECK(l.begin("a", t0) == verdict::allowed);
    l.finish("a", false);
    BOOST_CHECK(l.begin("a", t0 + std::chrono::seconds{30}) == verdict::rate_limited);
    BOOST_CHECK(l.begin("a", t0 + std::chrono::seconds{61}) == verdict::allowed);
}

// Many sources together may not run more than `max_concurrent` KDFs; a busy
// refusal is not charged to the source.
BOOST_AUTO_TEST_CASE(concurrency_cap_is_gateway_wide, *boost::unit_test::timeout(10)) {
    redis_auth_limiter l(10, std::chrono::seconds{60}, 2);
    auto now = redis_auth_limiter::clock::now();
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    BOOST_CHECK(l.begin("b", now) == verdict::allowed);
    BOOST_CHECK(l.begin("c", now) == verdict::busy);
    l.finish("a", false);
    BOOST_CHECK(l.begin("c", now) == verdict::allowed);
    l.finish("b", false);
    l.finish("c", false);
    BOOST_CHECK_EQUAL(l.in_flight(), 0u);
    // "c" was charged once (its admitted attempt), not for the busy refusal.
    for (int i = 0; i < 9; ++i) {
        BOOST_REQUIRE(l.begin("c", now) == verdict::allowed);
        l.finish("c", false);
    }
    BOOST_CHECK(l.begin("c", now) == verdict::rate_limited);
}

BOOST_AUTO_TEST_CASE(zero_concurrency_means_one, *boost::unit_test::timeout(10)) {
    redis_auth_limiter l(10, std::chrono::seconds{60}, 0);
    auto now = redis_auth_limiter::clock::now();
    BOOST_CHECK(l.begin("a", now) == verdict::allowed);
    BOOST_CHECK(l.begin("b", now) == verdict::busy);
}

BOOST_AUTO_TEST_CASE(source_table_is_bounded, *boost::unit_test::timeout(30)) {
    redis_auth_limiter l(10, std::chrono::seconds{60}, 1);
    auto now = redis_auth_limiter::clock::now();
    for (std::size_t i = 0; i < redis_auth_limiter::k_max_tracked_sources + 10; ++i) {
        auto key = "s" + std::to_string(i);
        BOOST_REQUIRE(l.begin(key, now) == verdict::allowed);
        l.finish(key, false);
    }
    BOOST_CHECK_LE(l.tracked_sources(), redis_auth_limiter::k_max_tracked_sources);
}
