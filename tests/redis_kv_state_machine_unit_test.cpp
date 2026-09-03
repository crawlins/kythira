// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_kv_state_machine_unit_test.cpp
/// @brief State machine tests (.kiro/specs/redis-compatible-kv/ task 3,
///        Requirements 8.1-8.6, 5.2, 5.5, 6.2).

#define BOOST_TEST_MODULE redis_kv_state_machine_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/redis_kv_state_machine.hpp>

#include <cstddef>
#include <string>
#include <vector>

using namespace kythira;

namespace {

using sm_type = redis_kv_state_machine<std::uint64_t>;

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto set(sm_type& sm, std::uint64_t idx, const std::string& k, const std::string& v,
         std::uint64_t exp = 0) -> std::vector<std::byte> {
    return sm.apply(encode_redis_kv_command(redis_kv_set_command{k, bytes_of(v), exp}), idx);
}

auto del(sm_type& sm, std::uint64_t idx, const std::string& k) -> std::vector<std::byte> {
    return sm.apply(encode_redis_kv_command(redis_kv_del_command{k}), idx);
}

auto value_of(const sm_type& sm, const std::string& k) -> std::string {
    auto h = sm.lookup(k);
    if (!h) {
        return "<missing>";
    }
    return std::string(reinterpret_cast<const char*>(h->_value.data()), h->_value.size());
}

}  // namespace

BOOST_AUTO_TEST_CASE(set_get_del_and_accounting, *boost::unit_test::timeout(10)) {
    sm_type sm;
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 0u);
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 0u);

    set(sm, 1, "alpha", "12345");
    BOOST_CHECK_EQUAL(value_of(sm, "alpha"), "12345");
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 1u);
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 5u + 5u);

    // Replacing shrinks the accounting by the old value and grows by the new.
    set(sm, 2, "alpha", "12");
    BOOST_CHECK_EQUAL(value_of(sm, "alpha"), "12");
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 5u + 2u);

    auto changed = del(sm, 3, "alpha");
    BOOST_CHECK(changed == std::vector<std::byte>{std::byte{1}});
    BOOST_CHECK(sm.lookup("alpha") == nullptr);
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 0u);
    BOOST_CHECK_EQUAL(sm.last_applied_index(), 3u);

    auto unchanged = del(sm, 4, "alpha");
    BOOST_CHECK(unchanged == std::vector<std::byte>{std::byte{0}});
}

BOOST_AUTO_TEST_CASE(lookup_handle_survives_replacement, *boost::unit_test::timeout(10)) {
    sm_type sm;
    set(sm, 1, "k", "old");
    auto handle = sm.lookup("k");
    set(sm, 2, "k", "new");
    BOOST_REQUIRE(handle != nullptr);
    BOOST_CHECK(handle->_value == bytes_of("old"));
    BOOST_CHECK_EQUAL(value_of(sm, "k"), "new");
}

BOOST_AUTO_TEST_CASE(apply_stores_deadline_without_consulting_clock,
                     *boost::unit_test::timeout(10)) {
    sm_type sm;
    // A deadline far in the past is stored verbatim: only a sweep removes it.
    set(sm, 1, "k", "v", 1);
    auto h = sm.lookup("k");
    BOOST_REQUIRE(h != nullptr);
    BOOST_CHECK_EQUAL(h->_expire_at_ms, 1u);
}

BOOST_AUTO_TEST_CASE(sweep_deletes_only_matching_deadline, *boost::unit_test::timeout(10)) {
    sm_type sm;
    set(sm, 1, "a", "1", 100);
    set(sm, 2, "b", "2", 100);
    set(sm, 3, "c", "3", 0);
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 2u);

    auto expired = sm.collect_expired(150, 10);
    BOOST_REQUIRE_EQUAL(expired.size(), 2u);

    // Between collect and apply, `b` is re-set with a later deadline.
    set(sm, 4, "b", "2b", 500);
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 2u);

    redis_kv_sweep_command sweep;
    sweep._entries = expired;
    sm.apply(encode_redis_kv_command(sweep), 5);

    BOOST_CHECK(sm.lookup("a") == nullptr);
    BOOST_CHECK_EQUAL(value_of(sm, "b"), "2b");
    BOOST_CHECK_EQUAL(value_of(sm, "c"), "3");
    BOOST_CHECK_EQUAL(sm.approximate_key_count(), 2u);
    BOOST_CHECK_EQUAL(sm.approximate_size_bytes(), 1u + 2u + 1u + 1u);
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 1u);
    // Overwriting with no deadline, then deleting, both release the count.
    set(sm, 6, "b", "2c", 0);
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 0u);
    set(sm, 7, "d", "4", 900);
    sm.apply(encode_redis_kv_command(redis_kv_del_command{"d"}), 8);
    BOOST_CHECK_EQUAL(sm.expiring_key_count(), 0u);

    // A snapshot round trip recounts from the restored store.
    set(sm, 9, "e", "5", 900);
    sm_type restored;
    restored.restore_from_snapshot(sm.get_state(), 9);
    BOOST_CHECK_EQUAL(restored.expiring_key_count(), 1u);
}

BOOST_AUTO_TEST_CASE(collect_expired_is_bounded, *boost::unit_test::timeout(10)) {
    sm_type sm;
    for (int i = 0; i < 10; ++i) {
        set(sm, static_cast<std::uint64_t>(i + 1), "k" + std::to_string(i), "v", 10);
    }
    BOOST_CHECK_EQUAL(sm.collect_expired(20, 4).size(), 4u);
    BOOST_CHECK_EQUAL(sm.collect_expired(5, 4).size(), 0u);
}

BOOST_AUTO_TEST_CASE(evict_removes_named_keys, *boost::unit_test::timeout(10)) {
    sm_type sm;
    set(sm, 1, "a", "1");
    set(sm, 2, "b", "2");
    sm.apply(encode_redis_kv_command(redis_kv_evict_command{{"a", "zzz"}}), 3);
    BOOST_CHECK(sm.lookup("a") == nullptr);
    BOOST_CHECK_EQUAL(value_of(sm, "b"), "2");
}

BOOST_AUTO_TEST_CASE(snapshot_round_trip_preserves_deadlines, *boost::unit_test::timeout(10)) {
    sm_type a;
    set(a, 1, "x", "1", 0);
    set(a, 2, "y", "22", 4242);
    set(a, 3, "z", "", 7);

    auto state = a.get_state();
    sm_type b;
    b.restore_from_snapshot(state, 3);
    BOOST_CHECK(b.get_state() == state);
    BOOST_CHECK_EQUAL(b.approximate_key_count(), 3u);
    BOOST_CHECK_EQUAL(b.approximate_size_bytes(), a.approximate_size_bytes());
    BOOST_CHECK_EQUAL(b.lookup("y")->_expire_at_ms, 4242u);
    BOOST_CHECK_EQUAL(b.lookup("z")->_value.size(), 0u);
    BOOST_CHECK_EQUAL(b.last_applied_index(), 3u);
}

BOOST_AUTO_TEST_CASE(get_state_is_key_ordered_and_deterministic, *boost::unit_test::timeout(10)) {
    sm_type a;
    set(a, 1, "b", "2");
    set(a, 2, "a", "1");
    sm_type b;
    set(b, 1, "a", "1");
    set(b, 2, "b", "2");
    BOOST_CHECK(a.get_state() == b.get_state());
}

// Requirement 8.3/8.4: split_state and absorb are exact inverses.
BOOST_AUTO_TEST_CASE(split_absorb_round_trip, *boost::unit_test::timeout(10)) {
    sm_type whole;
    for (char c = 'a'; c <= 'j'; ++c) {
        set(whole, static_cast<std::uint64_t>(c), std::string(1, c), std::string(3, c),
            c % 2 ? 0 : 1000);
    }
    auto before = whole.get_state();

    auto parts = whole.split_state({"d", "h"});
    BOOST_REQUIRE_EQUAL(parts.size(), 3u);

    sm_type left;
    sm_type mid;
    sm_type right;
    left.absorb(parts[0], shard_range<std::string>{std::nullopt, "d"});
    mid.absorb(parts[1], shard_range<std::string>{"d", "h"});
    right.absorb(parts[2], shard_range<std::string>{"h", std::nullopt});
    BOOST_CHECK_EQUAL(left.approximate_key_count(), 3u);
    BOOST_CHECK_EQUAL(mid.approximate_key_count(), 4u);
    BOOST_CHECK_EQUAL(right.approximate_key_count(), 3u);
    BOOST_CHECK(left.lookup("c") != nullptr);
    BOOST_CHECK(mid.lookup("d") != nullptr);
    BOOST_CHECK(right.lookup("h") != nullptr);

    // Each blob is in get_state() format.
    BOOST_CHECK(left.get_state() == parts[0]);

    sm_type merged;
    merged.absorb(parts[2], shard_range<std::string>{"h", std::nullopt});
    merged.absorb(parts[0], shard_range<std::string>{std::nullopt, "d"});
    merged.absorb(parts[1], shard_range<std::string>{"d", "h"});
    BOOST_CHECK(merged.get_state() == before);
    BOOST_CHECK_EQUAL(merged.approximate_size_bytes(), whole.approximate_size_bytes());
}

BOOST_AUTO_TEST_CASE(split_sorts_and_dedups_cuts, *boost::unit_test::timeout(10)) {
    sm_type sm;
    for (char c = 'a'; c <= 'e'; ++c) {
        set(sm, static_cast<std::uint64_t>(c), std::string(1, c), "v");
    }
    auto parts = sm.split_state({"d", "b", "d"});
    BOOST_REQUIRE_EQUAL(parts.size(), 3u);
    sm_type p0;
    sm_type p1;
    sm_type p2;
    p0.absorb(parts[0], {});
    p1.absorb(parts[1], {});
    p2.absorb(parts[2], {});
    BOOST_CHECK_EQUAL(p0.approximate_key_count(), 1u);  // a
    BOOST_CHECK_EQUAL(p1.approximate_key_count(), 2u);  // b c
    BOOST_CHECK_EQUAL(p2.approximate_key_count(), 2u);  // d e
}

BOOST_AUTO_TEST_CASE(suggest_split_keys_never_first_key, *boost::unit_test::timeout(10)) {
    sm_type sm;
    BOOST_CHECK(sm.suggest_split_keys(3).empty());
    for (int i = 0; i < 100; ++i) {
        set(sm, static_cast<std::uint64_t>(i + 1), "k" + std::to_string(1000 + i), "v");
    }
    auto keys = sm.suggest_split_keys(3);
    BOOST_REQUIRE_EQUAL(keys.size(), 3u);
    BOOST_CHECK(keys[0] != "k1000");
    BOOST_CHECK(keys[0] < keys[1] && keys[1] < keys[2]);
    for (const auto& k : keys) {
        BOOST_CHECK(sm.can_split_at(k));
    }
}

BOOST_AUTO_TEST_CASE(rejects_corrupt_entries_and_snapshots, *boost::unit_test::timeout(10)) {
    sm_type sm;
    BOOST_CHECK_THROW(sm.apply({std::byte{0x09}}, 1), redis_kv_codec_error);
    BOOST_CHECK_THROW(
        sm.restore_from_snapshot({std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5}}, 1),
        redis_kv_codec_error);
}
