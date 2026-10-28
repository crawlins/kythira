// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file txn_state_machine_test.cpp
/// @brief `transactional_state_machine` (tasks 2-3 of
///        `.kiro/specs/multi-raft-transactions/`).
///
/// Drives the wrapper the way a host would: every input is an encoded
/// envelope and every output is decoded from the encoded result, so the tests
/// cover the same bytes a log entry carries. The wrapped state machine is the
/// splittable key/value test machine; a tiny non-splittable counter checks
/// that the wrapper is splittable exactly when what it wraps is.
///
/// The idempotency cases deliver each op twice, and out of order, because a
/// coordinator that timed out retries, and a retried entry lands in the log
/// after whatever happened in between.

#define BOOST_TEST_MODULE txn_state_machine_test
#include <boost/test/unit_test.hpp>

#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#include "multi_raft_kv_workload.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_result_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_record_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_id)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::string>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

using kythira::serialization_exception;
using kythira::txn_id;
using kythira::txn_record_status;
using kythira::txn_result_status;
using kythira::testing::kv_partitioner;

namespace {

using kv_sm = kythira::test_key_value_state_machine<>;
using txn_sm = kythira::transactional_state_machine<kv_sm, std::string, kv_partitioner>;
using env = kythira::txn_envelope<std::string>;
using result = kythira::txn_result<std::string>;
using write = kythira::txn_write<std::string>;

/// A state machine with no split support, for the negative static_assert.
struct counter_sm {
    std::uint64_t _n{0};
    auto apply(const std::vector<std::byte>&, std::uint64_t) -> std::vector<std::byte> {
        ++_n;
        return {};
    }
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return {static_cast<std::byte>(_n)};
    }
    auto restore_from_snapshot(const std::vector<std::byte>& s, std::uint64_t) -> void {
        _n = s.empty() ? 0 : static_cast<std::uint64_t>(s[0]);
    }
};

struct first_byte_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& c) const -> std::string {
        return c.empty() ? std::string{} : std::string(1, static_cast<char>(c[0]));
    }
};

using counter_txn_sm =
    kythira::transactional_state_machine<counter_sm, std::string, first_byte_partitioner>;

static_assert(kythira::state_machine<txn_sm, std::uint64_t>);
static_assert(kythira::splittable_state_machine<txn_sm, std::string>);
static_assert(kythira::state_machine<counter_sm, std::uint64_t>);
static_assert(kythira::state_machine<counter_txn_sm, std::uint64_t>);
static_assert(!kythira::splittable_state_machine<counter_txn_sm, std::string>);
static_assert(kythira::is_transactional_state_machine_v<txn_sm>);
static_assert(!kythira::is_transactional_state_machine_v<kv_sm>);

const txn_id t1{0, 1};
const txn_id t2{0, 2};

auto put(const std::string& k, const std::string& v) -> std::vector<std::byte> {
    return kv_sm::make_put_command(k, v);
}

auto w(const std::string& k, const std::string& v) -> write {
    return write{k, put(k, v)};
}

struct harness {
    txn_sm sm;
    std::uint64_t index{0};

    auto run(const env& e) -> result {
        return kythira::decode_txn_result<std::string>(
            sm.apply(kythira::encode_txn_envelope<std::string>(e), ++index));
    }

    auto plain(const std::vector<std::byte>& command) -> result {
        return run(kythira::txn_plain{command});
    }

    auto prepare(const txn_id& id, const std::string& primary, std::vector<write> writes,
                 bool is_primary, std::uint64_t deadline = 1000) -> result {
        return run(kythira::txn_prepare<std::string>{
            id, primary, std::move(writes), {}, 0, deadline, is_primary});
    }

    auto commit_primary(const txn_id& id, const std::string& primary, std::uint64_t ts = 7)
        -> result {
        return run(kythira::txn_commit_primary<std::string>{id, primary, ts});
    }

    auto commit_secondary(const txn_id& id, std::vector<std::string> keys) -> result {
        return run(kythira::txn_commit_secondary<std::string>{id, std::move(keys), 7});
    }

    auto abort(const txn_id& id, const std::string& primary, std::vector<std::string> keys)
        -> result {
        return run(kythira::txn_abort<std::string>{id, primary, std::move(keys)});
    }

    auto check(const txn_id& id, const std::string& primary, std::uint64_t now) -> result {
        return run(kythira::txn_check_and_resolve<std::string>{id, primary, now});
    }

    [[nodiscard]] auto value(const std::string& k) const -> std::optional<std::string> {
        return sm.inner().get_value(k);
    }
};

auto is_record(const result& r, txn_record_status s) -> bool {
    return r._status == txn_result_status::record && r._record == s;
}

}  // namespace

BOOST_AUTO_TEST_CASE(plain_commands_pass_through) {
    harness h;
    BOOST_TEST(h.plain(put("a", "1"))._status == txn_result_status::ok);
    BOOST_TEST(h.value("a") == std::optional<std::string>("1"));
    // The wrapped machine's own result comes back as the payload.
    const auto got = h.plain(kv_sm::make_get_command("a"));
    BOOST_TEST(got._status == txn_result_status::ok);
    BOOST_TEST(std::string(reinterpret_cast<const char*>(got._payload.data()),  // NOLINT
                           got._payload.size()) == "1");
}

BOOST_AUTO_TEST_CASE(plain_command_on_a_locked_key_is_refused) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "x")}, true)._status == txn_result_status::ok);
    const auto r = h.plain(put("a", "plain"));
    BOOST_TEST(r._status == txn_result_status::key_locked);
    BOOST_TEST(r._key == "a");
    BOOST_REQUIRE(r._holder.has_value());
    BOOST_TEST(r._holder->_txn == t1);
    BOOST_TEST(r._holder->_primary == "a");
    BOOST_TEST(!h.value("a").has_value());
    // Another key is unaffected.
    BOOST_TEST(h.plain(put("b", "1"))._status == txn_result_status::ok);
}

BOOST_AUTO_TEST_CASE(one_phase_commit_applies_every_write) {
    harness h;
    const auto r = h.run(kythira::txn_one_phase<std::string>{t1, {w("b", "2"), w("a", "1")}, 0, 5});
    BOOST_TEST(r._status == txn_result_status::ok);
    BOOST_TEST(h.value("a") == std::optional<std::string>("1"));
    BOOST_TEST(h.value("b") == std::optional<std::string>("2"));
    BOOST_TEST(h.sm.intent_count() == 0U);
}

BOOST_AUTO_TEST_CASE(retried_one_phase_commit_applies_once) {
    harness h;
    const env op = kythira::txn_one_phase<std::string>{t1, {w("a", "1")}, 0, 5};
    BOOST_TEST(h.run(op)._status == txn_result_status::ok);
    BOOST_TEST(h.plain(put("a", "later"))._status == txn_result_status::ok);
    // The retry must not overwrite the later plain write.
    BOOST_TEST(h.run(op)._status == txn_result_status::ok);
    BOOST_TEST(h.value("a") == std::optional<std::string>("later"));
}

BOOST_AUTO_TEST_CASE(one_phase_commit_conflicts_and_applies_nothing) {
    harness h;
    BOOST_TEST(h.prepare(t1, "b", {w("b", "x")}, true)._status == txn_result_status::ok);
    const auto r = h.run(kythira::txn_one_phase<std::string>{t2, {w("a", "1"), w("b", "2")}, 0, 5});
    BOOST_TEST(r._status == txn_result_status::write_conflict);
    BOOST_TEST(r._key == "b");
    BOOST_REQUIRE(r._holder.has_value());
    BOOST_TEST(r._holder->_txn == t1);
    BOOST_TEST(!h.value("a").has_value());
}

BOOST_AUTO_TEST_CASE(prepare_conflict_installs_nothing) {
    harness h;
    BOOST_TEST(h.prepare(t1, "b", {w("b", "x")}, true)._status == txn_result_status::ok);
    const auto r = h.prepare(t2, "a", {w("a", "1"), w("b", "2")}, true);
    BOOST_TEST(r._status == txn_result_status::write_conflict);
    BOOST_TEST(!h.sm.intent_on("a").has_value());
    BOOST_TEST(!h.sm.record_of("a", t2).has_value());
    BOOST_TEST(h.sm.intent_on("b")->_txn == t1);
}

BOOST_AUTO_TEST_CASE(two_phase_commit_on_one_shard) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1"), w("b", "2")}, true, 500)._status ==
               txn_result_status::ok);
    BOOST_TEST(h.sm.intent_count() == 2U);
    const auto rec = h.sm.record_of("a", t1);
    BOOST_REQUIRE(rec.has_value());
    BOOST_TEST(rec->_status == txn_record_status::pending);
    BOOST_TEST(rec->_deadline_ms == 500U);
    BOOST_TEST(!h.value("a").has_value());

    BOOST_TEST(h.commit_primary(t1, "a", 42)._status == txn_result_status::ok);
    BOOST_TEST(h.sm.record_of("a", t1)->_status == txn_record_status::committed);
    BOOST_TEST(h.sm.record_of("a", t1)->_commit_ts == 42U);
    BOOST_TEST(h.sm.intent_count() == 0U);
    BOOST_TEST(h.value("a") == std::optional<std::string>("1"));
    BOOST_TEST(h.value("b") == std::optional<std::string>("2"));
}

BOOST_AUTO_TEST_CASE(two_phase_commit_across_shards) {
    harness primary;
    harness secondary;
    BOOST_TEST(primary.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(secondary.prepare(t1, "a", {w("m", "2")}, false)._status == txn_result_status::ok);
    BOOST_TEST(!secondary.sm.record_of("a", t1).has_value());
    BOOST_TEST(primary.commit_primary(t1, "a")._status == txn_result_status::ok);
    BOOST_TEST(secondary.commit_secondary(t1, {"m"})._status == txn_result_status::ok);
    BOOST_TEST(primary.value("a") == std::optional<std::string>("1"));
    BOOST_TEST(secondary.value("m") == std::optional<std::string>("2"));
    BOOST_TEST(secondary.sm.intent_count() == 0U);
}

BOOST_AUTO_TEST_CASE(every_op_is_idempotent_when_delivered_twice) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.sm.intent_count() == 1U);
    BOOST_TEST(h.run(kythira::txn_heartbeat<std::string>{t1, "a", 900})._status ==
               txn_result_status::record);
    BOOST_TEST(h.run(kythira::txn_heartbeat<std::string>{t1, "a", 900})._status ==
               txn_result_status::record);
    BOOST_TEST(h.sm.record_of("a", t1)->_deadline_ms == 1000U);  // never shortened

    BOOST_TEST(h.commit_primary(t1, "a")._status == txn_result_status::ok);
    BOOST_TEST(h.plain(put("a", "later"))._status == txn_result_status::ok);
    BOOST_TEST(h.commit_primary(t1, "a")._status == txn_result_status::ok);
    BOOST_TEST(h.commit_secondary(t1, {"a"})._status == txn_result_status::ok);
    // A prepare retried after its commit: answered, installs nothing.
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.sm.intent_count() == 0U);
    BOOST_TEST(h.value("a") == std::optional<std::string>("later"));

    const env oracle = kythira::txn_oracle_reserve{100};
    BOOST_TEST(h.run(oracle)._status == txn_result_status::ok);
    BOOST_TEST(h.run(kythira::txn_oracle_reserve{50})._status == txn_result_status::ok);
    BOOST_TEST(h.sm.oracle_high_water_mark() == 100U);
    BOOST_TEST(h.run(kythira::txn_gc{3})._status == txn_result_status::ok);
}

BOOST_AUTO_TEST_CASE(late_secondary_prepare_after_commit_installs_nothing) {
    harness secondary;
    BOOST_TEST(secondary.prepare(t1, "a", {w("m", "1")}, false)._status == txn_result_status::ok);
    BOOST_TEST(secondary.commit_secondary(t1, {"m"})._status == txn_result_status::ok);
    BOOST_TEST(secondary.plain(put("m", "later"))._status == txn_result_status::ok);
    // A coordinator retry of the prepare lands after the commit.
    BOOST_TEST(secondary.prepare(t1, "a", {w("m", "1")}, false)._status == txn_result_status::ok);
    BOOST_TEST(secondary.sm.intent_count() == 0U);
    BOOST_TEST(secondary.commit_secondary(t1, {"m"})._status == txn_result_status::ok);
    BOOST_TEST(secondary.value("m") == std::optional<std::string>("later"));
}

BOOST_AUTO_TEST_CASE(late_secondary_prepare_after_abort_installs_nothing) {
    harness secondary;
    BOOST_TEST(secondary.abort(t1, "a", {"m"})._status == txn_result_status::ok);
    BOOST_TEST(secondary.prepare(t1, "a", {w("m", "1")}, false)._status ==
               txn_result_status::already_aborted);
    BOOST_TEST(secondary.sm.intent_count() == 0U);
}

BOOST_AUTO_TEST_CASE(commit_before_prepare_leaves_a_rollback_marker) {
    harness h;
    BOOST_TEST(is_record(h.commit_primary(t1, "a"), txn_record_status::aborted));
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status ==
               txn_result_status::already_aborted);
    BOOST_TEST(h.sm.intent_count() == 0U);
    BOOST_TEST(!h.value("a").has_value());
}

BOOST_AUTO_TEST_CASE(abort_releases_locks_and_reports_the_record) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1"), w("b", "2")}, true)._status ==
               txn_result_status::ok);
    BOOST_TEST(is_record(h.abort(t1, "a", {"a", "b"}), txn_record_status::aborted));
    BOOST_TEST(h.sm.intent_count() == 0U);
    BOOST_TEST(h.sm.record_of("a", t1)->_status == txn_record_status::aborted);
    BOOST_TEST(is_record(h.abort(t1, "a", {"a", "b"}), txn_record_status::aborted));
    // The commit point never passes after an abort.
    BOOST_TEST(is_record(h.commit_primary(t1, "a"), txn_record_status::aborted));
    BOOST_TEST(!h.value("a").has_value());
    BOOST_TEST(h.plain(put("a", "free"))._status == txn_result_status::ok);
}

BOOST_AUTO_TEST_CASE(abort_after_commit_changes_nothing) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.commit_primary(t1, "a")._status == txn_result_status::ok);
    BOOST_TEST(is_record(h.abort(t1, "a", {"a"}), txn_record_status::committed));
    BOOST_TEST(h.value("a") == std::optional<std::string>("1"));
    BOOST_TEST(h.sm.record_of("a", t1)->_status == txn_record_status::committed);
}

BOOST_AUTO_TEST_CASE(check_and_resolve_follows_the_ttl_table) {
    // Absent: write the rollback marker.
    {
        harness h;
        BOOST_TEST(is_record(h.check(t1, "a", 10), txn_record_status::aborted));
        BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status ==
                   txn_result_status::already_aborted);
    }
    // Pending, deadline not reached: stays pending, locks held.
    {
        harness h;
        BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true, 100)._status == txn_result_status::ok);
        BOOST_TEST(is_record(h.check(t1, "a", 99), txn_record_status::pending));
        BOOST_TEST(h.sm.intent_count() == 1U);
        // Pending, deadline reached: aborted, the primary's locks released.
        BOOST_TEST(is_record(h.check(t1, "a", 100), txn_record_status::aborted));
        BOOST_TEST(h.sm.intent_count() == 0U);
        BOOST_TEST(is_record(h.commit_primary(t1, "a"), txn_record_status::aborted));
    }
    // A heartbeat pushes the deadline out.
    {
        harness h;
        BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true, 100)._status == txn_result_status::ok);
        BOOST_TEST(is_record(h.run(kythira::txn_heartbeat<std::string>{t1, "a", 300}),
                             txn_record_status::pending));
        BOOST_TEST(is_record(h.check(t1, "a", 200), txn_record_status::pending));
    }
    // Committed: reported with its timestamp, nothing changes.
    {
        harness h;
        BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true, 100)._status == txn_result_status::ok);
        BOOST_TEST(h.commit_primary(t1, "a", 9)._status == txn_result_status::ok);
        const auto r = h.check(t1, "a", 1000);
        BOOST_TEST(is_record(r, txn_record_status::committed));
        BOOST_TEST(r._commit_ts == 9U);
    }
}

BOOST_AUTO_TEST_CASE(heartbeat_on_an_unknown_txn_reports_aborted) {
    harness h;
    BOOST_TEST(is_record(h.run(kythira::txn_heartbeat<std::string>{t1, "a", 300}),
                         txn_record_status::aborted));
}

BOOST_AUTO_TEST_CASE(cleanup_forgets_records_and_settled_entries) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.commit_primary(t1, "a")._status == txn_result_status::ok);
    BOOST_TEST(h.run(kythira::txn_cleanup<std::string>{{{"a", t1}}})._status ==
               txn_result_status::ok);
    BOOST_TEST(!h.sm.record_of("a", t1).has_value());
    // Only the wrapped data is left.
    harness fresh;
    BOOST_TEST(fresh.plain(put("a", "1"))._status == txn_result_status::ok);
    BOOST_TEST(h.sm.get_state() == fresh.sm.get_state());
}

BOOST_AUTO_TEST_CASE(snapshot_round_trips_locks_records_and_data) {
    harness h;
    BOOST_TEST(h.plain(put("z", "0"))._status == txn_result_status::ok);
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1"), w("b", "2")}, true)._status ==
               txn_result_status::ok);
    BOOST_TEST(h.abort(t2, "q", {"q"})._status == txn_result_status::record);
    BOOST_TEST(h.run(kythira::txn_oracle_reserve{77})._status == txn_result_status::ok);
    const auto state = h.sm.get_state();

    txn_sm restored;
    restored.restore_from_snapshot(state, 10);
    BOOST_TEST(restored.get_state() == state);
    BOOST_TEST(restored.intent_on("a")->_txn == t1);
    BOOST_TEST(restored.record_of("q", t2)->_status == txn_record_status::aborted);
    BOOST_TEST(restored.oracle_high_water_mark() == 77U);
    BOOST_TEST(restored.inner().get_value("z") == std::optional<std::string>("0"));
    // The lock is live in the restored copy: a follower that caught up by
    // snapshot must not serve the key as unlocked.
    BOOST_TEST(kythira::decode_txn_result<std::string>(
                   restored.apply(kythira::encode_txn_envelope<std::string>(
                                      env{kythira::txn_plain{put("a", "p")}}),
                                  11))
                   ._status == txn_result_status::key_locked);
}

BOOST_AUTO_TEST_CASE(unwrapped_snapshot_is_refused_by_name) {
    kv_sm plain;
    std::ignore = plain.apply(put("a", "1"), 1);
    txn_sm wrapped;
    try {
        wrapped.restore_from_snapshot(plain.get_state(), 1);
        BOOST_FAIL("an unwrapped snapshot was accepted");
    } catch (const serialization_exception& e) {
        const std::string what = e.what();
        BOOST_TEST(what.find("kythira-txn-sm") != std::string::npos);
        BOOST_TEST(what.find("unwrapped") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(truncated_and_corrupt_snapshots_are_refused) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1")}, true)._status == txn_result_status::ok);
    BOOST_TEST(h.plain(put("b", "2"))._status == txn_result_status::ok);
    const auto state = h.sm.get_state();
    for (std::size_t n = 0; n < state.size(); ++n) {
        const std::vector<std::byte> cut(state.begin(),
                                         state.begin() + static_cast<std::ptrdiff_t>(n));
        txn_sm fresh;
        BOOST_CHECK_THROW(fresh.restore_from_snapshot(cut, 1), std::exception);
    }
    auto trailing = state;
    trailing.push_back(std::byte{0});
    txn_sm fresh;
    BOOST_CHECK_THROW(fresh.restore_from_snapshot(trailing, 1), serialization_exception);

    auto bad_version = state;
    bad_version[txn_sm::k_snapshot_magic.size()] = std::byte{9};
    BOOST_CHECK_THROW(fresh.restore_from_snapshot(bad_version, 1), serialization_exception);
}

BOOST_AUTO_TEST_CASE(non_envelope_entry_is_refused) {
    txn_sm sm;
    BOOST_CHECK_THROW(std::ignore = sm.apply(put("a", "1"), 1), serialization_exception);
}

BOOST_AUTO_TEST_CASE(non_splittable_inner_still_wraps) {
    counter_txn_sm sm;
    const auto r = kythira::decode_txn_result<std::string>(sm.apply(
        kythira::encode_txn_envelope<std::string>(env{kythira::txn_plain{{std::byte{'k'}}}}), 1));
    BOOST_TEST(r._status == txn_result_status::ok);
    BOOST_TEST(sm.inner()._n == 1U);
    counter_txn_sm copy;
    copy.restore_from_snapshot(sm.get_state(), 1);
    BOOST_TEST(copy.inner()._n == 1U);
}

BOOST_AUTO_TEST_CASE(split_moves_locks_with_their_keys) {
    harness h;
    BOOST_TEST(h.prepare(t1, "a", {w("a", "1"), w("m", "2")}, true)._status ==
               txn_result_status::ok);
    BOOST_TEST(h.plain(put("z", "3"))._status == txn_result_status::ok);
    const auto parts = h.sm.split_state({"k"});
    BOOST_REQUIRE_EQUAL(parts.size(), 2U);
    txn_sm left;
    left.restore_from_snapshot(parts[0], 1);
    txn_sm right;
    right.restore_from_snapshot(parts[1], 1);
    BOOST_TEST(left.intent_on("a").has_value());
    BOOST_TEST(!left.intent_on("m").has_value());
    BOOST_TEST(left.record_of("a", t1).has_value());
    BOOST_TEST(right.intent_on("m")->_primary == "a");
    BOOST_TEST(!right.record_of("a", t1).has_value());
    BOOST_TEST(right.inner().get_value("z") == std::optional<std::string>("3"));

    left.absorb(parts[1], kythira::shard_range<std::string>{std::string("k"), std::nullopt});
    BOOST_TEST(left.get_state() == h.sm.get_state());
}
