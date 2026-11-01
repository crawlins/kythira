// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_disk_guard_unit_test.cpp
/// @brief `cache_disk_guard`: the voter floors and the disk budget
///        (.kiro/specs/disk-backed-build-cache/ task 7, Requirement 6).

#define BOOST_TEST_MODULE cache_disk_guard_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/cache_disk_guard.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {

using kythira::cache_disk_guard;
using kythira::cache_disk_guard_options;
using kythira::cache_node_report;
using clock_type = cache_disk_guard::clock;

constexpr std::uint64_t GiB = std::uint64_t{1} << 30U;

auto options() -> cache_disk_guard_options {
    return cache_disk_guard_options{
        ._max_disk_bytes = 100 * GiB,
        ._min_free_bytes = 1 * GiB,
        ._min_free_inodes = 10000,
        ._report_ttl = std::chrono::seconds{30},
    };
}

/// A report as a peer configured with `options()` sends it.
auto roomy() -> cache_node_report {
    return cache_node_report{._free_bytes = 50 * GiB,
                             ._free_inodes = 1'000'000,
                             ._store_bytes = 0,
                             ._min_free_bytes = 1 * GiB,
                             ._min_free_inodes = 10000,
                             ._max_disk_bytes = 100 * GiB};
}

const std::vector<std::string> voters{"1", "2", "3"};

}  // namespace

BOOST_AUTO_TEST_CASE(a_report_round_trips_through_its_wire_form) {
    const cache_node_report r{._free_bytes = 123,
                              ._free_inodes = 45,
                              ._store_bytes = 6789,
                              ._pinned_bytes = 9,
                              ._min_free_bytes = 10,
                              ._min_free_inodes = 11,
                              ._max_disk_bytes = 12};
    BOOST_TEST(r.encode() == "123,45,6789,9,10,11,12");
    auto back = cache_node_report::decode(r.encode());
    BOOST_REQUIRE(back);
    BOOST_TEST(back->_free_bytes == 123U);
    BOOST_TEST(back->_free_inodes == 45U);
    BOOST_TEST(back->_store_bytes == 6789U);
    BOOST_TEST(back->_pinned_bytes == 9U);
    BOOST_TEST(back->_min_free_bytes == 10U);
    BOOST_TEST(back->_min_free_inodes == 11U);
    BOOST_TEST(back->_max_disk_bytes == 12U);
    for (const char* bad :
         {"", "1,2,3", "1,2,3,4,5,6", "1,2,3,4,5,6,7,8", "1,,3,4,5,6,7", "a,2,3,4,5,6,7",
          "-1,2,3,4,5,6,7", "99999999999999999999,1,1,1,1,1,1", "1,2,3,4,5,6,7,"}) {
        BOOST_TEST_INFO(bad);
        BOOST_TEST(!cache_node_report::decode(bad));
    }
}

BOOST_AUTO_TEST_CASE(writes_go_ahead_with_room_everywhere) {
    cache_disk_guard g("1", options());
    g.set_local(roomy());
    g.record("2", roomy());
    g.record("3", roomy());
    BOOST_TEST(!g.refusal(voters));
    BOOST_TEST(g.refusals() == 0U);
}

BOOST_AUTO_TEST_CASE(a_full_local_disk_refuses_writes) {
    cache_disk_guard g("1", options());
    auto r = roomy();
    r._free_bytes = GiB - 1;
    g.set_local(r);
    auto why = g.refusal(voters);
    BOOST_REQUIRE(why);
    BOOST_TEST(why->find("node 1") != std::string::npos);
    BOOST_TEST(why->find("bytes free") != std::string::npos);
    BOOST_TEST(g.refusals() == 1U);
}

// Requirement 6.6: the follower's floor is what the leader must see.
BOOST_AUTO_TEST_CASE(a_voter_below_either_floor_refuses_writes) {
    cache_disk_guard g("1", options());
    g.set_local(roomy());
    auto low_inodes = roomy();
    low_inodes._free_inodes = 9999;
    g.record("3", low_inodes);
    auto why = g.refusal(voters);
    BOOST_REQUIRE(why);
    BOOST_TEST(why->find("node 3") != std::string::npos);
    BOOST_TEST(why->find("inodes") != std::string::npos);
    // A shard node 3 does not vote in is not held back by it.
    BOOST_TEST(!g.refusal({"1", "2"}));
}

// A voter that has gone quiet is not applying; holding writes for it would
// turn one failure into an outage.
BOOST_AUTO_TEST_CASE(a_stale_report_is_ignored) {
    cache_disk_guard g("1", options());
    g.set_local(roomy());
    auto low = roomy();
    low._free_bytes = 0;
    const auto then = clock_type::now();
    g.record("2", low, then);
    BOOST_TEST(g.refusal(voters, then + std::chrono::seconds{29}).has_value());
    BOOST_TEST(!g.refusal(voters, then + std::chrono::seconds{31}));
    BOOST_TEST(g.fresh_peers(then + std::chrono::seconds{31}) == 0U);
    g.record("2", roomy(), then + std::chrono::seconds{40});
    BOOST_TEST(!g.refusal(voters, then + std::chrono::seconds{41}));
}

// The leader holds a voter to the voter's own floor: it is the voter that
// stops applying when it runs out, whatever the leader is configured with.
BOOST_AUTO_TEST_CASE(a_voter_is_held_to_its_own_floor) {
    cache_disk_guard g("1", options());
    g.set_local(roomy());
    auto strict = roomy();
    strict._min_free_bytes = 60 * GiB;  // 50 GiB free is below this node's floor
    g.record("2", strict);
    auto why = g.refusal(voters);
    BOOST_REQUIRE(why);
    BOOST_TEST(why->find("node 2") != std::string::npos);
    BOOST_TEST(why->find(std::to_string(60 * GiB)) != std::string::npos);
    auto lax = roomy();
    lax._free_bytes = GiB / 2;  // below the leader's floor, not its own
    lax._min_free_bytes = GiB / 4;
    g.record("2", lax);
    BOOST_TEST(!g.refusal(voters));
}

BOOST_AUTO_TEST_CASE(set_local_stamps_this_nodes_settings) {
    cache_disk_guard g("1", options());
    const auto sent = g.set_local(cache_node_report{._free_bytes = 5});
    BOOST_TEST(sent._min_free_bytes == 1 * GiB);
    BOOST_TEST(sent._min_free_inodes == 10000U);
    BOOST_TEST(sent._max_disk_bytes == 100 * GiB);
    BOOST_REQUIRE(g.local());
    BOOST_TEST(g.local()->_max_disk_bytes == 100 * GiB);
}

BOOST_AUTO_TEST_CASE(a_report_claiming_to_be_this_node_is_ignored) {
    cache_disk_guard g("1", options());
    g.set_local(roomy());
    auto low = roomy();
    low._free_bytes = 0;
    g.record("1", low);
    BOOST_TEST(!g.refusal(voters));
    BOOST_TEST(g.reports_received() == 0U);
}

// Requirement 6.1: the fullest voter sets how much the shard frees.
BOOST_AUTO_TEST_CASE(the_excess_is_the_fullest_voters) {
    cache_disk_guard g("1", options());
    auto mine = roomy();
    mine._store_bytes = 101 * GiB;
    g.set_local(mine);
    BOOST_TEST(g.excess_bytes(voters) == 1 * GiB);
    auto theirs = roomy();
    theirs._store_bytes = 103 * GiB;
    g.record("2", theirs);
    BOOST_TEST(g.excess_bytes(voters) == 3 * GiB);
    BOOST_TEST(g.excess_bytes({"1", "3"}) == 1 * GiB);
    mine._store_bytes = 99 * GiB;
    g.set_local(mine);
    BOOST_TEST(g.excess_bytes({"1", "3"}) == 0U);
}

BOOST_AUTO_TEST_CASE(no_budget_means_no_excess) {
    auto o = options();
    o._max_disk_bytes = 0;
    cache_disk_guard g("1", o);
    auto r = roomy();
    r._store_bytes = 1000 * GiB;
    g.set_local(r);
    BOOST_TEST(g.excess_bytes(voters) == 0U);
    r._max_disk_bytes = 0;  // a peer with no budget
    g.record("2", r);
    BOOST_TEST(g.excess_bytes(voters) == 0U);
    r._max_disk_bytes = 999 * GiB;  // a peer with its own, smaller budget
    g.record("3", r);
    BOOST_TEST(g.excess_bytes(voters) == 1 * GiB);
}

// A leader that just evicted ignores reports taken before it did: they
// still count the bytes it freed.
BOOST_AUTO_TEST_CASE(reports_before_the_last_eviction_are_left_out) {
    cache_disk_guard g("1", options());
    const auto then = clock_type::now();
    auto full = roomy();
    full._store_bytes = 102 * GiB;
    g.set_local(roomy(), then);
    g.record("2", full, then);
    BOOST_TEST(g.excess_bytes(voters, then) == 2 * GiB);
    const auto evicted = then + std::chrono::milliseconds{1};
    BOOST_TEST(g.excess_bytes(voters, evicted, evicted) == 0U);
    g.record("2", full, evicted + std::chrono::seconds{1});
    BOOST_TEST(g.excess_bytes(voters, evicted + std::chrono::seconds{1}, evicted) == 2 * GiB);
}

// Bytes inside the orphan grace cannot be evicted away. While they alone
// fill the budget, asking for eviction would empty the shard for nothing.
BOOST_AUTO_TEST_CASE(pinned_bytes_that_fill_the_budget_ask_for_no_eviction) {
    cache_disk_guard g("1", options());
    auto r = roomy();
    r._store_bytes = 103 * GiB;
    r._pinned_bytes = 100 * GiB;
    g.set_local(r);
    BOOST_TEST(g.excess_bytes(voters) == 0U);
    r._pinned_bytes = 99 * GiB;  // the rest can make room
    g.set_local(r);
    BOOST_TEST(g.excess_bytes(voters) == 3 * GiB);
}
