// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file txn_state_machine_persistent_test.cpp
/// @brief `transactional_state_machine` over a persistent state machine
///        (.kiro/specs/persistent-state-machine/ task 6, Requirements 8.1-8.3).
///
/// The wrapper's three maps must live in the engine's `txn` column family,
/// written in the same batch as the inner effects. The tests hold it to the
/// in-memory wrapper, which is the reference:
///
/// - A random workload runs on both; every reply and every view agrees, and a
///   wrapper reopened on the engine at any point sees what the live one does.
/// - A power cut loses an entry's inner writes and its transaction state
///   together, never one without the other.
/// - A command that throws part way through an entry leaves nothing behind.
/// - A split and a merge move transaction state with the range, as
///   `split_state` and `absorb` do.

#define BOOST_TEST_MODULE txn_state_machine_persistent_test
#include <boost/test/unit_test.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_test_state_machine.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#include "multi_raft_kv_workload.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_result_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_record_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::string>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

using kythira::column_family;
using kythira::key_range;
using kythira::txn_id;
using kythira::txn_record_status;
using kythira::txn_result_status;
using kythira::testing::kv_partitioner;

namespace {

using kv_sm = kythira::test_key_value_state_machine<>;
using pkv_sm = kythira::persistent_test_kv_state_machine<>;
using mem_txn = kythira::transactional_state_machine<kv_sm, std::string, kv_partitioner>;
using disk_txn = kythira::transactional_state_machine<pkv_sm, std::string, kv_partitioner>;
using env = kythira::txn_envelope<std::string>;
using result = kythira::txn_result<std::string>;
using txn_write = kythira::txn_write<std::string>;

static_assert(disk_txn::k_persistent);
static_assert(!mem_txn::k_persistent);
static_assert(kythira::persistent_splittable_state_machine<disk_txn>);
static_assert(!kythira::state_machine<disk_txn, std::uint64_t>);
static_assert(kythira::storage_kind_of<disk_txn> == kythira::state_machine_storage::persistent);
static_assert(kythira::split_policy_state_machine<disk_txn, std::string>);
static_assert(kythira::is_transactional_state_machine_v<disk_txn>);

auto put(const std::string& k, const std::string& v) -> std::vector<std::byte> {
    return kv_sm::make_put_command(k, v);
}

auto encode(const env& e) -> std::vector<std::byte> {
    return kythira::encode_txn_envelope<std::string>(e);
}

auto decode(const std::vector<std::byte>& b) -> result {
    return kythira::decode_txn_result<std::string>(b);
}

const std::string k_ns = "ns/";

auto open_disk(kythira::memory_kv_engine& engine, const key_range& range = {}) -> disk_txn {
    disk_txn sm{pkv_sm{k_ns}};
    sm.open(engine, range);
    return sm;
}

const std::vector<std::string> k_keys{"a", "b", "c", "d", "e", "f", "g", "h"};

/// Everything a caller can see of a wrapper, for comparing two of them.
template<typename SM> auto views(const SM& sm, std::size_t max_txn) -> std::string {
    std::string out;
    for (const auto& k : k_keys) {
        out += k + "=" + sm.inner().get_value(k).value_or("-") + ";";
        if (const auto in = sm.intent_on(k)) {
            out += "i(" + std::to_string(in->_txn._lo) + "," + in->_primary + "," +
                   std::to_string(in->_deadline_ms) + ");";
        }
        for (std::uint64_t t = 1; t <= max_txn; ++t) {
            if (const auto r = sm.record_of(k, txn_id{0, t})) {
                out += "r" + std::to_string(t) + "(" +
                       std::to_string(static_cast<int>(r->_status)) + "," +
                       std::to_string(r->_deadline_ms) + "," + std::to_string(r->_commit_ts) + ");";
            }
        }
    }
    for (const auto& o : sm.open_transactions()) {
        out += "open" + std::to_string(o._txn._lo) + ";";
    }
    for (const auto& f : sm.finished_transactions()) {
        out += "fin" + std::to_string(f._txn._lo) + f._primary + ";";
    }
    out += "hwm" + std::to_string(sm.oracle_high_water_mark());
    return out;
}

/// A random envelope over `k_keys` and transactions 1..`max_txn`.
auto random_envelope(std::mt19937_64& rng, std::size_t max_txn) -> env {
    auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
    const auto key = [&] { return k_keys[pick(k_keys.size())]; };
    const txn_id id{0, 1 + pick(max_txn)};
    auto keys = [&] {
        std::vector<std::string> out;
        const auto n = 1 + pick(3);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(key());
        }
        return out;
    };
    auto writes = [&] {
        std::vector<txn_write> out;
        for (const auto& k : keys()) {
            if (std::none_of(out.begin(), out.end(),
                             [&](const txn_write& w) { return w._key == k; })) {
                out.push_back(txn_write{k, put(k, std::to_string(rng() % 1000))});
            }
        }
        return out;
    };
    switch (pick(10)) {
        case 0:
            return kythira::txn_plain{put(key(), std::to_string(rng() % 1000))};
        case 1:
            return kythira::txn_one_phase<std::string>{id, writes(), 0, 0};
        case 2:
        case 3: {
            auto ws = writes();
            const auto primary = ws.front()._key;
            return kythira::txn_prepare<std::string>{id, primary,           std::move(ws), {},
                                                     0,  100 + rng() % 100, pick(2) == 0};
        }
        case 4:
            return kythira::txn_commit_primary<std::string>{id, key(), 7};
        case 5:
            return kythira::txn_commit_secondary<std::string>{id, keys(), 7};
        case 6:
            return kythira::txn_abort<std::string>{id, key(), keys()};
        case 7:
            return kythira::txn_check_and_resolve<std::string>{id, key(), rng() % 300};
        case 8:
            return kythira::txn_heartbeat<std::string>{id, key(), 100 + rng() % 400};
        default: {
            kythira::txn_cleanup<std::string> c;
            c._items.push_back(kythira::txn_cleanup_item<std::string>{key(), id});
            return c;
        }
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_random_workload_matches_the_in_memory_wrapper_and_survives_reopen) {
    constexpr std::size_t k_txns = 6;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        std::mt19937_64 rng(seed);
        kythira::memory_kv_engine engine;
        auto disk = open_disk(engine);
        mem_txn mem;
        for (std::uint64_t index = 1; index <= 300; ++index) {
            auto e = random_envelope(rng, k_txns);
            if (index % 37 == 0) {
                e = kythira::txn_oracle_reserve{index * 10};
            }
            const auto bytes = encode(e);
            const auto want = decode(mem.apply(bytes, index));
            const auto got = decode(disk.apply(bytes, index));
            BOOST_REQUIRE_MESSAGE(want == got, "seed " << seed << " index " << index);
            BOOST_REQUIRE_EQUAL(views(mem, k_txns), views(disk, k_txns));
            if (index % 25 == 0) {
                // Everything the maps hold must be in the engine.
                const auto reopened = open_disk(engine);
                BOOST_REQUIRE_EQUAL(views(disk, k_txns), views(reopened, k_txns));
                BOOST_REQUIRE_EQUAL(reopened.applied_index(), index);
            }
        }
        // A finished workload leaves no txn value for a key with no state.
        const auto reopened = open_disk(engine);
        BOOST_REQUIRE_EQUAL(views(disk, k_txns), views(reopened, k_txns));
    }
}

BOOST_AUTO_TEST_CASE(a_replayed_entry_changes_nothing) {
    kythira::memory_kv_engine engine;
    auto sm = open_disk(engine);
    const auto prepare = encode(kythira::txn_prepare<std::string>{
        txn_id{0, 1}, "a", {txn_write{"a", put("a", "1")}}, {}, 0, 100, true});
    std::ignore = sm.apply(prepare, 1);
    const auto before = engine.latest_sequence();
    std::ignore = sm.apply(prepare, 1);
    BOOST_TEST(engine.latest_sequence() == before);
    BOOST_TEST(sm.intent_on("a").has_value());
}

BOOST_AUTO_TEST_CASE(a_power_cut_loses_the_writes_and_the_lock_release_together) {
    kythira::memory_kv_engine engine;
    auto sm = open_disk(engine);
    const txn_id t{0, 1};
    std::ignore = sm.apply(encode(kythira::txn_prepare<std::string>{
                               t,
                               "a",
                               {txn_write{"a", put("a", "1")}, txn_write{"b", put("b", "2")}},
                               {},
                               0,
                               100,
                               true}),
                           1);
    engine.hold_durability();
    const auto res =
        decode(sm.apply(encode(kythira::txn_commit_primary<std::string>{t, "a", 7}), 2));
    BOOST_TEST(res._status == txn_result_status::ok);
    BOOST_TEST(sm.inner().get_value("b") == std::optional<std::string>("2"));
    BOOST_TEST(!sm.intent_on("b").has_value());

    engine.simulate_crash();
    auto after = open_disk(engine);
    // Back at the prepare: both intents held, neither write applied, the
    // record pending. Never a write without its release, or the reverse.
    BOOST_TEST(after.applied_index() == 1U);
    BOOST_TEST(after.intent_on("a").has_value());
    BOOST_TEST(after.intent_on("b").has_value());
    BOOST_TEST(!after.inner().get_value("a").has_value());
    BOOST_TEST(!after.inner().get_value("b").has_value());
    BOOST_TEST(after.record_of("a", t)->_status == txn_record_status::pending);

    // The log replays the commit.
    engine.release_durability();
    std::ignore = after.apply(encode(kythira::txn_commit_primary<std::string>{t, "a", 7}), 2);
    BOOST_TEST(after.inner().get_value("a") == std::optional<std::string>("1"));
    BOOST_TEST(after.inner().get_value("b") == std::optional<std::string>("2"));
    BOOST_TEST(after.open_transactions().empty());
}

// The wrapper forwards sync_durable(), so the node can compact a
// transactional persistent group on an engine that syncs only when asked;
// syncing the inner machine covers the transaction state, which rides in
// the same batches.
BOOST_AUTO_TEST_CASE(sync_durable_covers_the_transaction_state) {
    static_assert(requires(disk_txn& s) { s.sync_durable(); });
    kythira::memory_kv_engine engine;
    auto sm = open_disk(engine);
    engine.hold_durability();
    const txn_id t{0, 1};
    std::ignore = sm.apply(encode(kythira::txn_prepare<std::string>{
                               t, "a", {txn_write{"a", put("a", "1")}}, {}, 0, 100, true}),
                           1);
    BOOST_TEST(sm.durable_applied_index() == 0U);
    sm.sync_durable();
    BOOST_TEST(sm.durable_applied_index() == 1U);

    engine.simulate_crash();
    auto after = open_disk(engine);
    BOOST_TEST(after.applied_index() == 1U);
    BOOST_TEST(after.intent_on("a").has_value());
}

BOOST_AUTO_TEST_CASE(a_command_that_throws_leaves_nothing_behind) {
    kythira::memory_kv_engine engine;
    auto sm = open_disk(engine);
    const txn_id t{0, 1};
    // "b"'s command is not a valid key/value command, so rolling it forward
    // throws after "a"'s write was staged.
    const std::vector<std::byte> bad{std::byte{0x7F}, std::byte{1}, std::byte{0},
                                     std::byte{0},    std::byte{0}, std::byte{'b'}};
    std::ignore = sm.apply(
        encode(kythira::txn_prepare<std::string>{
            t, "a", {txn_write{"a", put("a", "1")}, txn_write{"b", bad}}, {}, 0, 100, true}),
        1);
    const auto before = views(sm, 1);
    BOOST_CHECK_THROW(
        std::ignore = sm.apply(encode(kythira::txn_commit_primary<std::string>{t, "a", 7}), 2),
        std::exception);
    BOOST_TEST(sm.applied_index() == 1U);
    BOOST_TEST(!sm.inner().get_value("a").has_value());
    BOOST_TEST(views(sm, 1) == before);
    BOOST_TEST(views(open_disk(engine), 1) == before);
}

BOOST_AUTO_TEST_CASE(split_and_merge_move_transaction_state_with_the_range) {
    kythira::memory_kv_engine engine;
    auto parent = open_disk(engine);
    mem_txn mem;
    std::uint64_t index = 0;
    auto both = [&](const env& e) {
        ++index;
        const auto bytes = encode(e);
        BOOST_REQUIRE(decode(mem.apply(bytes, index)) == decode(parent.apply(bytes, index)));
    };
    both(kythira::txn_oracle_reserve{500});
    both(kythira::txn_prepare<std::string>{
        txn_id{0, 1}, "b", {txn_write{"b", put("b", "1")}}, {}, 0, 100, true});
    both(kythira::txn_prepare<std::string>{
        txn_id{0, 2}, "f", {txn_write{"f", put("f", "2")}}, {}, 0, 100, true});
    both(kythira::txn_abort<std::string>{txn_id{0, 3}, "g", {"g", "c"}});

    // Split at "e": the parent keeps [-inf, e), the child gets [e, +inf).
    const auto parts = mem.split_state({"e"});
    mem_txn mem_low;
    mem_txn mem_high;
    mem_low.restore_from_snapshot(parts[0], index);
    mem_high.restore_from_snapshot(parts[1], index);

    const auto children = parent.split_at({"e"});
    BOOST_REQUIRE_EQUAL(children.size(), 1U);
    auto child = open_disk(engine, children[0]);
    BOOST_TEST(views(parent, 3) == views(mem_low, 3));
    BOOST_TEST(views(child, 3) == views(mem_high, 3));
    BOOST_TEST(child.oracle_high_water_mark() == 0U);
    BOOST_TEST(parent.oracle_high_water_mark() == 500U);
    // Reopening each range reads the same back.
    BOOST_TEST(views(open_disk(engine, parent.range()), 3) == views(mem_low, 3));
    BOOST_TEST(views(open_disk(engine, children[0]), 3) == views(mem_high, 3));

    // Merge back: the parent adopts the child's range.
    mem_low.absorb(mem_high.get_state(), kythira::shard_range<std::string>{"e", std::nullopt});
    parent.adopt(children[0]);
    BOOST_TEST(views(parent, 3) == views(mem_low, 3));
    BOOST_TEST(views(open_disk(engine, parent.range()), 3) == views(mem_low, 3));
}

BOOST_AUTO_TEST_CASE(adopting_from_below_keeps_the_larger_high_water_mark) {
    kythira::memory_kv_engine engine;
    auto low = open_disk(engine, key_range{"", std::string("m")});
    auto high = open_disk(engine, key_range{"m", std::nullopt});
    std::ignore = low.apply(encode(kythira::txn_oracle_reserve{10}), 1);
    std::ignore = high.apply(encode(kythira::txn_oracle_reserve{90}), 1);
    std::ignore =
        high.apply(encode(kythira::txn_prepare<std::string>{
                       txn_id{0, 1}, "q", {txn_write{"q", put("q", "1")}}, {}, 0, 100, true}),
                   2);
    high.adopt(key_range{"", std::string("m")});
    BOOST_TEST(high.range().start.empty());
    BOOST_TEST(high.oracle_high_water_mark() == 90U);
    BOOST_TEST(high.intent_on("q").has_value());
    const auto reopened = open_disk(engine, high.range());
    BOOST_TEST(reopened.oracle_high_water_mark() == 90U);
    BOOST_TEST(reopened.intent_on("q").has_value());
}

BOOST_AUTO_TEST_CASE(drop_removes_transaction_state) {
    kythira::memory_kv_engine engine;
    auto sm = open_disk(engine);
    std::ignore =
        sm.apply(encode(kythira::txn_prepare<std::string>{
                     txn_id{0, 1}, "a", {txn_write{"a", put("a", "1")}}, {}, 0, 100, true}),
                 1);
    BOOST_TEST(engine.key_count(column_family::txn) == 1U);
    sm.drop();
    BOOST_TEST(engine.key_count(column_family::txn) == 0U);
    BOOST_TEST(sm.open_transactions().empty());
}
