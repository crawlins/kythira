// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Unit tests for tcp_gossip_peer2peer_replicator's merge/prune logic — pure
// logic, no network I/O. See .kiro/specs/peer2peer-gossip-transport/,
// Requirement 10.1.
#define BOOST_TEST_MODULE tcp_gossip_transport_merge_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/tcp_gossip_transport.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using gossip_t =
    kythira::tcp_gossip_peer2peer_replicator<std::uint64_t, std::string, std::uint64_t>;

auto make_replicator(std::uint16_t port) -> gossip_t {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listen_port = port;
    cfg.address_book = {};
    cfg.gossip_round_interval = std::chrono::milliseconds{100000};  // don't fire during the test
    cfg.freshness_interval = std::chrono::seconds{5};
    return gossip_t{cfg};
}

}  // namespace

BOOST_AUTO_TEST_CASE(merge_higher_term_wins) {
    auto r = make_replicator(0);
    r.merge({{1, "a:1", 1, 100, 9999999999}});
    r.merge({{1, "a:1", 2, 50, 9999999999}});  // higher term, lower index — still wins

    // Read back via find_catch_up_source with a permissive membership.
    std::move(r.update_membership({1})).get();
    auto found = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 1u);
}

BOOST_AUTO_TEST_CASE(merge_equal_term_higher_index_wins) {
    auto r = make_replicator(0);
    r.merge({{1, "a:1", 5, 100, 9999999999}});
    r.merge({{1, "a:1", 5, 200, 9999999999}});

    std::move(r.update_membership({1})).get();
    auto found = std::move(r.find_catch_up_source(150, 999, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(
        found.has_value());  // only the higher-index (200) digest would satisfy from_index=150
}

BOOST_AUTO_TEST_CASE(merge_lower_term_loses) {
    auto r = make_replicator(0);
    r.merge({{1, "a:1", 5, 100, 9999999999}});
    r.merge({{1, "a:1", 4, 999, 9999999999}});  // lower term, would-be-higher index — must lose

    std::move(r.update_membership({1})).get();
    // from_index=150 would only be satisfied by the (rejected) index=999 digest.
    auto found = std::move(r.find_catch_up_source(150, 999, std::chrono::milliseconds{10})).get();
    BOOST_CHECK(!found.has_value());
}

BOOST_AUTO_TEST_CASE(merge_not_yet_present_always_added) {
    auto r = make_replicator(0);
    r.merge({{7, "seven:1", 1, 42, 9999999999}});

    std::move(r.update_membership({7})).get();
    auto found = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 7u);
    BOOST_CHECK_EQUAL(found->address, "seven:1");
}

BOOST_AUTO_TEST_CASE(prune_expired_removes_only_past_fresh_until) {
    auto r = make_replicator(0);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    r.merge({{1, "a:1", 1, 10, now - 100}});     // already expired
    r.merge({{2, "b:1", 1, 10, now + 100000}});  // still fresh

    r.prune_expired();

    std::move(r.update_membership({1, 2})).get();
    auto found1 = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    // Only node 2 should remain — node 1's expired digest was pruned.
    BOOST_REQUIRE(found1.has_value());
    BOOST_CHECK_EQUAL(found1->node_id, 2u);
}

BOOST_AUTO_TEST_CASE(eligible_peers_intersection) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.listen_port = 0;
    cfg.address_book = {{1, "a:1"}, {2, "b:1"}, {3, "c:1"}};
    cfg.gossip_round_interval = std::chrono::milliseconds{100000};  // don't fire during the test
    gossip_t r{cfg};

    // Before any update_membership() call, eligible_peers() is empty.
    BOOST_CHECK_EQUAL(r.eligible_peers().size(), 0u);

    // Member 4 has no address_book entry; address_book entry 3 is not a member.
    std::move(r.update_membership({1, 2, 4})).get();
    auto eligible = r.eligible_peers();
    BOOST_REQUIRE_EQUAL(eligible.size(), 2u);
    std::vector<std::uint64_t> ids;
    for (const auto& p : eligible) {
        ids.push_back(p.node_id);
    }
    std::sort(ids.begin(), ids.end());
    BOOST_CHECK_EQUAL(ids[0], 1u);
    BOOST_CHECK_EQUAL(ids[1], 2u);
}

BOOST_AUTO_TEST_CASE(find_catch_up_source_excludes_self_and_non_members) {
    auto r = make_replicator(0);
    std::move(r.advertise_progress(1, "self:1", 1, 500)).get();
    r.merge({{2, "b:1", 1, 500, 9999999999}});

    // Only self and node2 known; membership excludes node2 — nothing offered.
    std::move(r.update_membership({1})).get();
    auto found = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    BOOST_CHECK(!found.has_value());  // self excluded, node2 not a member

    std::move(r.update_membership({1, 2})).get();
    found = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 2u);  // never itself
}

namespace {

auto epoch_now() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

auto make_capped_replicator(std::size_t max_entries) -> gossip_t {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.gossip_round_interval = std::chrono::milliseconds{100000};  // don't fire during the test
    cfg.freshness_interval = std::chrono::seconds{5};
    cfg.max_table_entries = max_entries;
    return gossip_t{cfg};
}

auto table_ids(const gossip_t& r) -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> ids;
    for (const auto& d : r.snapshot_table()) {
        ids.push_back(d.node_id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

// Requirement 6.6: a sender-chosen far-future fresh_until is clamped to
// now + freshness_interval, so the entry still expires on schedule.
BOOST_AUTO_TEST_CASE(merge_clamps_fresh_until_to_freshness_interval) {
    auto r = make_replicator(0);
    const auto before = epoch_now();
    r.merge({{1, "a:1", 1, 10, 9999999999}});
    const auto after = epoch_now();

    auto table = r.snapshot_table();
    BOOST_REQUIRE_EQUAL(table.size(), 1u);
    BOOST_CHECK_GE(table[0].fresh_until, before + 5);
    BOOST_CHECK_LE(table[0].fresh_until, after + 5);
}

// Requirement 6.4 still holds below the clamp: an earlier deadline is kept.
BOOST_AUTO_TEST_CASE(merge_keeps_earlier_fresh_until) {
    auto r = make_replicator(0);
    const auto deadline = epoch_now() + 2;
    r.merge({{1, "a:1", 1, 10, deadline}});

    auto table = r.snapshot_table();
    BOOST_REQUIRE_EQUAL(table.size(), 1u);
    BOOST_CHECK_EQUAL(table[0].fresh_until, deadline);
}

// Requirement 6.5: digests for unknown ids stop being added once the table is
// full, however many a single message carries.
BOOST_AUTO_TEST_CASE(merge_caps_table_size) {
    auto r = make_capped_replicator(4);
    std::vector<kythira::gossip_digest<std::uint64_t, std::string, std::uint64_t>> flood;
    for (std::uint64_t id = 100; id < 1100; ++id) {
        flood.push_back({id, "x:1", 1, 1, 9999999999});
    }
    r.merge(flood);
    BOOST_CHECK_EQUAL(r.snapshot_table().size(), 4u);
}

// Requirement 6.5: a full table still accepts newer progress for ids it holds.
BOOST_AUTO_TEST_CASE(full_table_still_updates_known_ids) {
    auto r = make_capped_replicator(2);
    r.merge({{1, "a:1", 1, 10, 9999999999}, {2, "b:1", 1, 10, 9999999999}});
    r.merge({{1, "a:1", 2, 50, 9999999999}});

    std::move(r.update_membership({1})).get();
    auto found = std::move(r.find_catch_up_source(40, 50, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 1u);
}

// Requirement 6.5: a current member evicts a non-member from a full table;
// a non-member cannot evict anything.
BOOST_AUTO_TEST_CASE(full_table_member_evicts_non_member) {
    auto r = make_capped_replicator(2);
    r.merge({{100, "x:1", 1, 1, 9999999999}, {101, "y:1", 1, 1, 9999999999}});
    std::move(r.update_membership({1})).get();

    r.merge({{102, "z:1", 1, 1, 9999999999}});
    BOOST_CHECK((table_ids(r) == std::vector<std::uint64_t>{100, 101}));

    r.merge({{1, "a:1", 1, 500, 9999999999}});
    auto ids = table_ids(r);
    BOOST_REQUIRE_EQUAL(ids.size(), 2u);
    BOOST_CHECK_EQUAL(ids[0], 1u);

    auto found = std::move(r.find_catch_up_source(0, 0, std::chrono::milliseconds{10})).get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 1u);
}

// Requirement 6.5: once the table holds only members, a member's new digest
// does not evict another member.
BOOST_AUTO_TEST_CASE(full_table_of_members_keeps_members) {
    auto r = make_capped_replicator(1);
    std::move(r.update_membership({1, 2})).get();
    r.merge({{1, "a:1", 1, 1, 9999999999}});
    r.merge({{2, "b:1", 1, 1, 9999999999}});
    BOOST_CHECK((table_ids(r) == std::vector<std::uint64_t>{1}));
}

// Requirement 6.5: an expired entry gives up its slot to anyone.
BOOST_AUTO_TEST_CASE(full_table_reuses_expired_slot) {
    auto r = make_capped_replicator(1);
    r.merge({{100, "x:1", 1, 1, epoch_now() - 100}});
    r.merge({{101, "y:1", 1, 1, 9999999999}});
    BOOST_CHECK((table_ids(r) == std::vector<std::uint64_t>{101}));
}

BOOST_AUTO_TEST_CASE(zero_max_table_entries_rejected) {
    kythira::tcp_gossip_config<std::uint64_t, std::string> cfg;
    cfg.max_table_entries = 0;
    BOOST_CHECK_THROW(gossip_t{cfg}, std::invalid_argument);
}
