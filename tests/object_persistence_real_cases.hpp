// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_persistence_real_cases.hpp
/// @brief The checks every provider's real tier runs, written once
///        (cloud-object-persistence task 16; cases 6-11 are
///        object-store-batched-layout task 10).
///
/// Each is a free function taking a store, a bucket and a prefix, so a
/// provider's suite supplies only authentication and a bucket name. See
/// `object_persistence_real_support.hpp` for why these are shared rather than
/// copied per provider.
///
/// What this tier proves that the mock tier cannot: that the engine's
/// guarantees hold against **real object semantics** — real eventual-consistency
/// behaviour on LIST, real conditional-write evaluation, real error codes —
/// rather than against an in-memory map that agrees with the client by
/// construction.

#include "object_persistence_real_support.hpp"

#include <raft/key_object_store.hpp>
#include <raft/object_store_backup.hpp>
#include <raft/object_store_persistence.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace kythira::object_real {

/// The engine instantiation these cases exercise. Defaults throughout, because
/// the point is the *shipped* configuration against a real service.
template<key_object_store Store> using engine_for = object_store_persistence_engine<Store>;

template<conditional_key_object_store Store>
using fenced_engine_for =
    object_store_persistence_engine<Store, std::uint64_t, std::uint64_t, std::uint64_t,
                                    fencing_mode::compare_and_swap>;

/// @brief Case 1 — a returned write is readable by a **fresh** engine.
///
/// The durability claim, and the one a local mirror could otherwise fake: the
/// reader shares no memory with the writer, so everything it sees came back
/// from the service.
template<key_object_store Store>
auto case_fresh_engine_read_back(Store store, const std::string& bucket, const std::string& prefix)
    -> void {
    {
        engine_for<Store> writer{store, bucket, prefix};
        writer.save_current_term(7);
        writer.save_voted_for(42);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            log_entry<> entry;
            entry._term = 7;
            entry._index = i;
            const std::string payload = "command-" + std::to_string(i);
            entry._command.assign(
                reinterpret_cast<const std::byte*>(payload.data()),
                reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
            writer.append_log_entry(entry);
        }
    }

    engine_for<Store> reader{store, bucket, prefix};
    BOOST_TEST(reader.load_current_term() == 7U);
    BOOST_REQUIRE(reader.load_voted_for().has_value());
    BOOST_TEST(reader.load_voted_for().value() == 42U);
    BOOST_TEST(reader.get_last_log_index() == 5U);
    const auto entries = reader.get_log_entries(1, 5);
    BOOST_REQUIRE_EQUAL(entries.size(), 5U);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        BOOST_TEST(entries[i].index() == i + 1);
    }
}

/// @brief Case 2 — measured p50/p99 for the operations a Raft node waits on.
///
/// Reported through `report_latency`, which records **where** the measurement
/// was taken. Sample counts are deliberately small: each sample is a paid round
/// trip, and the useful signal here is order of magnitude and provider-to-
/// provider comparison, not a distribution.
template<key_object_store Store>
auto case_measured_latency(Store store, const std::string& bucket, const std::string& prefix,
                           const std::string& provider, std::size_t samples = 20) -> void {
    engine_for<Store> engine{store, bucket, prefix};

    // `term` is a single-slot key, and GCS throttles mutations of one object to
    // about 1/s (see `measure`). Spaced, and with fewer samples, because each
    // one now costs a second of wall clock — the figure is still the thing a
    // Raft node waits for when it advances a term.
    report_latency(provider, measure(
                                 "save_current_term", std::min<std::size_t>(samples, 8),
                                 [&](std::size_t i) {
                                     engine.save_current_term(static_cast<std::uint64_t>(i + 1));
                                 },
                                 std::chrono::milliseconds{1100}));

    report_latency(provider, measure("append_log_entry", samples, [&](std::size_t i) {
                       log_entry<> entry;
                       entry._term = 1;
                       entry._index = static_cast<std::uint64_t>(i + 1);
                       engine.append_log_entry(entry);
                   }));

    report_latency(provider, measure("get_log_entry", samples, [&](std::size_t i) {
                       (void)engine.get_log_entry(static_cast<std::uint64_t>(i + 1));
                   }));
}

/// @brief Case 3 — the fencing race, **with its negative control**.
///
/// ## What `compare_and_swap` actually promises, and what this case had wrong
///
/// The first version of this case asserted that a takeover alone makes the
/// previous engine's next write fail. Against a real bucket it failed in the
/// most useful possible way: the *stale* writer succeeded and the *new owner*
/// was refused — the exact opposite. The code was right and the test was wrong.
///
/// `fencing_mode::compare_and_swap` **detects** a second writer; it does not
/// arbitrate one. The fence is a per-object `If-Match`, so an engine's write is
/// refused only once someone else has actually written **that object** and
/// moved its version on. Taking ownership at a higher epoch does not by itself
/// invalidate anything the previous engine still holds a current version for.
/// That is the documented design — "no lease, no arbitration, no takeover" —
/// and this case now tests it rather than a more comfortable story about it.
///
/// So the race is staged the way it actually happens: the new owner writes
/// first, and only then is the old engine genuinely stale.
///
/// The negative control is the half that makes this a test rather than an
/// assertion: it is not enough that the stale writer was rejected — the
/// **current** owner's writes must still be accepted. A fence that refused
/// everything would pass the first half and be useless, and that is exactly the
/// failure mode a live service can produce (a misread precondition, a
/// mis-mapped status) while a mock cannot.
template<conditional_key_object_store Store>
auto case_fencing_race_with_negative_control(Store store, const std::string& bucket,
                                             const std::string& prefix) -> void {
    object_persistence_options first_opts;
    first_opts.owner_id = "node-first";
    fenced_engine_for<Store> first{store, bucket, prefix, first_opts};
    first.save_current_term(1);

    object_persistence_options second_opts;
    second_opts.owner_id = "node-second";
    second_opts.takeover_epoch = first.owner_epoch() + 1;
    fenced_engine_for<Store> second{store, bucket, prefix, second_opts};

    // The new owner writes `term`, which is what actually moves that object's
    // version out from under `first`.
    BOOST_CHECK_NO_THROW(second.save_current_term(2));

    // NOW the first engine is genuinely stale, and the real service must refuse
    // it. This is the fence doing its job: the loser of the race finds out.
    BOOST_CHECK_THROW(first.save_current_term(99), std::exception);

    // NEGATIVE CONTROL: the current owner keeps writing. Without this the check
    // above is satisfied by a fence that rejects everything.
    BOOST_CHECK_NO_THROW(second.save_current_term(3));

    engine_for<Store> reader{store, bucket, prefix};
    BOOST_TEST(reader.load_current_term() == 3U);
}

/// @brief Case 4 — backup → verify → clone-restore → fresh-engine read-back.
///
/// End to end against the real service: the tooling an operator uses in a
/// recovery window, exercised on the same object semantics they will meet.
template<key_object_store Store>
auto case_backup_verify_restore_read_back(Store store, const std::string& bucket,
                                          const std::string& source_prefix,
                                          const std::string& backup_prefix,
                                          const std::string& restore_prefix) -> void {
    {
        engine_for<Store> writer{store, bucket, source_prefix};
        writer.save_current_term(11);
        writer.save_voted_for(3);
        for (std::uint64_t i = 1; i <= 4; ++i) {
            log_entry<> entry;
            entry._term = 11;
            entry._index = i;
            writer.append_log_entry(entry);
        }
    }

    const object_store_backup<Store> backup{store};
    const backup_source src{bucket, source_prefix};
    const backup_destination dst{bucket, backup_prefix};

    const auto manifest = backup.create(src, dst, {.source_quiesced = true});
    BOOST_TEST(manifest.objects.size() >= 6U);
    BOOST_REQUIRE(manifest.current_term.has_value());
    BOOST_TEST(*manifest.current_term == 11U);

    const auto report = backup.verify(dst, manifest.backup_id);
    for (const auto& problem : report.problems) {
        BOOST_TEST_MESSAGE("verify problem: " << problem.kind << " " << problem.subject << " — "
                                              << problem.detail);
    }
    BOOST_TEST(report.ok);

    std::ignore =
        backup.restore_clone(dst, manifest.backup_id, backup_source{bucket, restore_prefix});

    // The restored prefix is opened by an engine that never saw the original.
    engine_for<Store> restored{store, bucket, restore_prefix};
    BOOST_TEST(restored.load_current_term() == 11U);
    BOOST_REQUIRE(restored.load_voted_for().has_value());
    BOOST_TEST(restored.load_voted_for().value() == 3U);
    BOOST_TEST(restored.get_last_log_index() == 4U);
}

/// @brief Case 5 — list-after-write, empirically (closes task 0.1's cells).
///
/// Writes @p count objects and immediately LISTs, three rounds. The engine's
/// recovery reads the whole log from one prefixed LIST, so a listing that is
/// short by even one entry is silent data loss — and unlike every other failure
/// here, it is one the engine **cannot** detect at runtime.
///
/// A short listing fails rather than warns. If a provider genuinely cannot
/// offer read-after-write on LIST, that is a finding about the provider that
/// belongs in the record, not a tolerance to build in.
template<key_object_store Store>
auto case_list_after_write(Store store, const std::string& bucket, const std::string& prefix,
                           const std::string& provider, std::size_t count = 25,
                           std::size_t rounds = 3) -> void {
    for (std::size_t round = 0; round < rounds; ++round) {
        const std::string round_prefix = prefix + "/law-" + std::to_string(round);
        for (std::size_t i = 0; i < count; ++i) {
            store.put_object(bucket, round_prefix + "/" + std::to_string(i), "x");
        }
        const auto listed = store.list_keys(bucket, round_prefix + "/");
        std::cout << "KYTHIRA_LIST_AFTER_WRITE provider=" << provider << " round=" << round
                  << " wrote=" << count << " listed=" << listed.size() << "\n";
        std::cout.flush();
        BOOST_TEST(listed.size() == count);
    }
}

// ── Batched layouts (object-store-batched-layout task 10) ────────────────────

/// Spacing between two mutations of one object in the cases below. GCS throttles
/// mutations of a single object to about one a second (see `measure`), and
/// `term` and `voted_for` are each written more than once per case. Applied on
/// every provider, so the cases do the same thing everywhere; it costs a few
/// seconds of wall clock and no requests.
inline constexpr std::chrono::milliseconds k_same_object_spacing{1100};

inline auto space_same_object_writes() -> void {
    std::this_thread::sleep_for(k_same_object_spacing);
}

/// The opt-in layouts, together: `hard_state_layout::combined` and
/// `log_layout::segmented`. @p owner_id set makes the options fenced.
[[nodiscard]] inline auto batched_options(std::string owner_id = {}) -> object_persistence_options {
    object_persistence_options opts;
    opts.hard_state = hard_state_layout::combined;
    opts.log = log_layout::segmented;
    opts.owner_id = std::move(owner_id);
    return opts;
}

/// Entries `first..last` in @p term, each carrying a payload of @p payload_bytes.
[[nodiscard]] inline auto make_run(std::uint64_t first, std::uint64_t last, std::uint64_t term,
                                   std::size_t payload_bytes = 16) -> std::vector<log_entry<>> {
    std::vector<log_entry<>> run;
    for (std::uint64_t i = first; i <= last; ++i) {
        log_entry<> entry;
        entry._term = term;
        entry._index = i;
        entry._command.assign(payload_bytes, static_cast<std::byte>('a' + (i % 26)));
        run.push_back(std::move(entry));
    }
    return run;
}

/// Every entry `1..last` is present and carries @p term_of(index).
template<typename Engine, typename TermOf>
auto check_log(Engine& engine, std::uint64_t first, std::uint64_t last, TermOf term_of) -> void {
    BOOST_TEST(engine.get_last_log_index() == last);
    const auto entries = engine.get_log_entries(first, last);
    BOOST_REQUIRE_EQUAL(entries.size(), last - first + 1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        BOOST_TEST(entries[i].index() == first + i);
        BOOST_TEST(entries[i].term() == term_of(first + i));
    }
}

/// @brief Case 6 — in-place migration from a prefix a default engine wrote,
///        then fresh read-back under both opt-in layouts (Requirement 16.1).
///
/// A default engine leaves a legacy pair (decimal `term`, `voted_for`) and five
/// singleton log objects. An engine with both opt-in layouts opens the prefix
/// without writing, then its first hard-state write migrates it (m1, then m2
/// retiring the vote slot) and its next run lands as one segment. A reader that
/// shares no memory with either sees the whole state, and the raw vote slot
/// says `retired` — which is what the real service returned, not the mirror.
template<key_object_store Store>
auto case_layout_migration_read_back(Store store, const std::string& bucket,
                                     const std::string& prefix) -> void {
    {
        engine_for<Store> legacy{store, bucket, prefix};
        legacy.save_current_term(4);
        legacy.save_voted_for(2);
        space_same_object_writes();
        for (const auto& entry : make_run(1, 5, 4)) {
            legacy.append_log_entry(entry);
        }
    }

    {
        engine_for<Store> upgraded{store, bucket, prefix, batched_options()};
        BOOST_TEST(upgraded.load_current_term() == 4U);
        BOOST_REQUIRE(upgraded.load_voted_for().has_value());
        BOOST_TEST(upgraded.load_voted_for().value() == 2U);
        BOOST_TEST(upgraded.get_last_log_index() == 5U);
        upgraded.save_hard_state(5, 3);
        const auto run = make_run(6, 30, 5);
        upgraded.append_log_entries(std::span<const log_entry<>>{run});
    }

    engine_for<Store> reader{store, bucket, prefix, batched_options()};
    BOOST_TEST(reader.load_current_term() == 5U);
    BOOST_REQUIRE(reader.load_voted_for().has_value());
    BOOST_TEST(reader.load_voted_for().value() == 3U);
    check_log(reader, 1, 30, [](std::uint64_t i) { return i <= 5 ? 4U : 5U; });

    const auto vote_slot = store.get_object(bucket, prefix + "/voted_for");
    BOOST_REQUIRE(vote_slot.has_value());
    BOOST_TEST(vote_slot->body == "retired");
    // Five singletons and one segment for 6..30.
    BOOST_TEST(store.list_keys(bucket, reader.log_prefix()).size() == 6U);
}

/// @brief Case 7 — truncation and compaction across a real segment
///        (Requirement 16.1), unfenced or fenced by @p Engine.
///
/// One 30-entry segment. Truncation from 21 overwrites it in place (`If-Match`
/// under fencing, the commit point); compaction below 11 PUTs the surviving
/// suffix under `log/11` and then deletes the segment (the commit point). The
/// listing after each step is the service's, so a lost overwrite or a
/// surviving straddler shows here and not just in a mirror.
template<typename Engine, key_object_store Store>
auto case_segment_truncate_compact(Store store, const std::string& bucket,
                                   const std::string& prefix,
                                   const object_persistence_options& opts) -> void {
    {
        Engine writer{store, bucket, prefix, opts};
        writer.save_hard_state(3, std::nullopt);
        const auto run = make_run(1, 30, 3);
        writer.append_log_entries(std::span<const log_entry<>>{run});
        BOOST_TEST(store.list_keys(bucket, writer.log_prefix()) ==
                       std::vector<std::string>{writer.log_key(1)},
                   boost::test_tools::per_element());

        writer.truncate_log(21);
        BOOST_TEST(store.list_keys(bucket, writer.log_prefix()) ==
                       std::vector<std::string>{writer.log_key(1)},
                   boost::test_tools::per_element());

        writer.delete_log_entries_before(11);
        BOOST_TEST(store.list_keys(bucket, writer.log_prefix()) ==
                       std::vector<std::string>{writer.log_key(11)},
                   boost::test_tools::per_element());

        // The log still continues: a new run after the truncated tail.
        const auto tail = make_run(21, 25, 4);
        writer.append_log_entries(std::span<const log_entry<>>{tail});
    }

    engine_for<Store> reader{store, bucket, prefix, batched_options()};
    BOOST_TEST(!reader.get_log_entry(10).has_value());
    check_log(reader, 11, 25, [](std::uint64_t i) { return i <= 20 ? 3U : 4U; });
}

/// @brief Case 8 — the retired vote slot refuses a pre-migration writer, with
///        its negative control (Requirement 16.2; fencing providers only).
///
/// The design's fencing argument rests on one service behaviour: once m2 has
/// replaced `<prefix>/voted_for` with `retired`, a fenced legacy-layout engine
/// still holding the slot's old version is refused at its next `If-Match` write
/// to it. The mock agrees by construction; this asks the service. The
/// negative control is the migrated engine writing on afterwards: a fence that
/// refused everything would pass the first half.
template<conditional_key_object_store Store>
auto case_retired_vote_slot_with_negative_control(Store store, const std::string& bucket,
                                                  const std::string& prefix) -> void {
    const std::string owner = "node-migrating";
    object_persistence_options legacy_opts;
    legacy_opts.owner_id = owner;
    {
        fenced_engine_for<Store> seed{store, bucket, prefix, legacy_opts};
        seed.save_current_term(4);
        seed.save_voted_for(1);
    }
    space_same_object_writes();

    // Both load the legacy pair's versions before the migration runs.
    fenced_engine_for<Store> stale{store, bucket, prefix, legacy_opts};
    fenced_engine_for<Store> migrating{store, bucket, prefix, batched_options(owner)};

    BOOST_CHECK_NO_THROW(migrating.save_hard_state(5, 2));
    const auto vote_slot = store.get_object(bucket, prefix + "/voted_for");
    BOOST_REQUIRE(vote_slot.has_value());
    BOOST_TEST(vote_slot->body == "retired");
    space_same_object_writes();

    // The pre-migration writer is now stale at the vote slot.
    BOOST_CHECK_THROW(stale.save_voted_for(9), persistence_fenced_error);
    BOOST_TEST(stale.is_fenced());
    space_same_object_writes();

    // NEGATIVE CONTROL: the migrated engine keeps writing.
    BOOST_TEST(!migrating.is_fenced());
    BOOST_CHECK_NO_THROW(migrating.save_hard_state(6, 3));

    engine_for<Store> reader{store, bucket, prefix, batched_options()};
    BOOST_TEST(reader.load_current_term() == 6U);
    BOOST_REQUIRE(reader.load_voted_for().has_value());
    BOOST_TEST(reader.load_voted_for().value() == 3U);
    const auto after = store.get_object(bucket, prefix + "/voted_for");
    BOOST_REQUIRE(after.has_value());
    BOOST_TEST(after->body == "retired");
}

/// @brief Case 9 — p50/p99 of a combined hard-state write and of full-size
///        segment PUTs (Requirement 16.3).
///
/// Two full sizes, because a segment is bounded two ways: 100 small entries
/// (`max_segment_entries`), and seven 96 KiB entries, about 0.88 MiB of body
/// once base64-encoded, the most whole entries that fit under the default
/// 1 MiB `max_segment_bytes`. Each sample appends one run that is exactly one
/// segment, so one sample is one PUT plus the engine's bookkeeping. The
/// combined write is spaced 1100 ms for the same GCS per-object limit as
/// `save_current_term` in case 2.
template<key_object_store Store>
auto case_batched_latency(Store store, const std::string& bucket, const std::string& prefix,
                          const std::string& provider, std::size_t samples = 10) -> void {
    {
        engine_for<Store> hard{store, bucket, prefix + "/hard", batched_options()};
        report_latency(provider, measure(
                                     "save_hard_state_combined", std::min<std::size_t>(samples, 8),
                                     [&](std::size_t i) {
                                         hard.save_hard_state(static_cast<std::uint64_t>(i + 1), 1);
                                     },
                                     std::chrono::milliseconds{1100}));
    }

    {
        engine_for<Store> by_count{store, bucket, prefix + "/count", batched_options()};
        report_latency(provider, measure("segment_put_100_entries", samples, [&](std::size_t i) {
                           const auto first = static_cast<std::uint64_t>(i * 100 + 1);
                           const auto run = make_run(first, first + 99, 1);
                           by_count.append_log_entries(std::span<const log_entry<>>{run});
                       }));
        BOOST_TEST(store.list_keys(bucket, by_count.log_prefix()).size() == samples);
    }

    {
        auto opts = batched_options();
        opts.max_segment_entries = 1000;  // so the byte bound is the one that binds
        engine_for<Store> by_bytes{store, bucket, prefix + "/bytes", opts};
        constexpr std::size_t k_entries = 7;
        constexpr std::size_t k_payload = 96U * 1024U;
        report_latency(provider, measure("segment_put_max_bytes", samples, [&](std::size_t i) {
                           const auto first = static_cast<std::uint64_t>(i * k_entries + 1);
                           const auto run = make_run(first, first + k_entries - 1, 1, k_payload);
                           by_bytes.append_log_entries(std::span<const log_entry<>>{run});
                       }));
        BOOST_TEST(store.list_keys(bucket, by_bytes.log_prefix()).size() == samples);
    }
}

/// @brief Case 10 — backup → verify → clone-restore → read-back over both
///        opt-in layouts (Requirement 16.1).
///
/// The same path as case 4, on a prefix holding a combined `term` and log
/// segments, so the backup tool's segment reading (task 8) meets real listings
/// and real bodies.
template<key_object_store Store>
auto case_batched_backup_verify_restore(Store store, const std::string& bucket,
                                        const std::string& source_prefix,
                                        const std::string& backup_prefix,
                                        const std::string& restore_prefix) -> void {
    {
        engine_for<Store> writer{store, bucket, source_prefix, batched_options()};
        writer.save_hard_state(11, 3);
        const auto run = make_run(1, 40, 11);
        writer.append_log_entries(std::span<const log_entry<>>{run});
        const auto more = make_run(41, 45, 11);
        writer.append_log_entries(std::span<const log_entry<>>{more});
    }

    const object_store_backup<Store> backup{store};
    const backup_source src{bucket, source_prefix};
    const backup_destination dst{bucket, backup_prefix};

    const auto manifest = backup.create(src, dst, {.source_quiesced = true});
    BOOST_TEST(manifest.format_version == 2);
    BOOST_REQUIRE(manifest.current_term.has_value());
    BOOST_TEST(*manifest.current_term == 11U);
    BOOST_REQUIRE(manifest.last_log_index.has_value());
    BOOST_TEST(*manifest.last_log_index == 45U);

    const auto report = backup.verify(dst, manifest.backup_id);
    for (const auto& problem : report.problems) {
        BOOST_TEST_MESSAGE("verify problem: " << problem.kind << " " << problem.subject << " — "
                                              << problem.detail);
    }
    BOOST_TEST(report.ok);

    std::ignore =
        backup.restore_clone(dst, manifest.backup_id, backup_source{bucket, restore_prefix});

    engine_for<Store> restored{store, bucket, restore_prefix, batched_options()};
    BOOST_TEST(restored.load_current_term() == 11U);
    BOOST_REQUIRE(restored.load_voted_for().has_value());
    BOOST_TEST(restored.load_voted_for().value() == 3U);
    check_log(restored, 1, 45, [](std::uint64_t) { return 11U; });
}

/// @brief Case 11 — a leader's commit rate with `write_behind` beside the
///        synchronous one (object-store-group-commit Requirement 8.6).
///
/// The driver does what `node` does with each command, without the cluster:
/// under one mutex, append the next entry; synchronous, that append is the
/// PUT and the mutex is held across it (`durability_class::synchronous` never
/// releases it); write-behind, the append stages, the mutex is released, and
/// the caller waits at `barrier_through`. @p submitters threads share
/// @p commands commands. Both modes use the segmented log.
///
/// Reported per mode, on one greppable line:
///   - `commits_per_s`, the leader's commit rate (one replica; a follower's
///     PUT overlaps the leader's in a real cluster)
///   - `log_puts`, the flushes (write-behind) or appends (synchronous)
///   - `B`, commands per log PUT
///   - `mean_flush_ms`, wall time divided by flushes. Flushes are serial and
///     back to back while submitters are queued, so this is the flush
///     duration `p50(flush)` stands for; the engine exposes no per-flush
///     timing, and a percentile of command latency is reported beside it.
template<key_object_store Store>
auto case_write_behind_commit_rate(Store store, const std::string& bucket,
                                   const std::string& prefix, const std::string& provider,
                                   std::size_t submitters = 16, std::size_t commands = 64) -> void {
    for (const bool write_behind : {false, true}) {
        const std::string mode = write_behind ? "write_behind" : "synchronous";
        const std::string mode_prefix = prefix + "/" + mode;
        auto opts = batched_options();
        opts.write_behind = write_behind;
        engine_for<Store> engine{store, bucket, mode_prefix, opts};
        engine.save_hard_state(1, std::nullopt);

        std::mutex node_mutex;
        std::uint64_t next_index = 0;
        std::atomic<std::size_t> issued{0};
        std::vector<double> latencies_ms;
        std::mutex latencies_mutex;

        const auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        threads.reserve(submitters);
        for (std::size_t t = 0; t < submitters; ++t) {
            threads.emplace_back([&] {
                while (issued.fetch_add(1) < commands) {
                    const auto began = std::chrono::steady_clock::now();
                    std::optional<write_sequence> seq;
                    {
                        std::lock_guard lock{node_mutex};
                        const auto entry = make_run(next_index + 1, next_index + 1, 1).front();
                        if (write_behind) {
                            seq = engine.append_log_entry_sequenced(entry);
                        } else {
                            engine.append_log_entry(entry);
                        }
                        ++next_index;
                    }
                    if (seq.has_value()) {
                        engine.barrier_through(*seq);
                    }
                    const auto ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - began)
                                        .count();
                    std::lock_guard lock{latencies_mutex};
                    latencies_ms.push_back(ms);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        const std::size_t log_puts = write_behind ? engine.flushes_issued() : commands;
        std::cout << "KYTHIRA_COMMIT_RATE provider=" << provider << " mode=" << mode
                  << " submitters=" << submitters << " commands=" << commands
                  << " seconds=" << seconds
                  << " commits_per_s=" << static_cast<double>(commands) / seconds
                  << " log_puts=" << log_puts
                  << " B=" << static_cast<double>(commands) / static_cast<double>(log_puts)
                  << " mean_flush_ms=" << seconds * 1000.0 / static_cast<double>(log_puts)
                  << " measured=client-side-around-engine-call\n";
        std::cout.flush();

        std::ranges::sort(latencies_ms);
        latency_report report;
        report.operation = "commit_" + mode;
        report.samples = latencies_ms.size();
        if (!latencies_ms.empty()) {
            const auto pick = [&](double q) {
                const auto rank =
                    static_cast<std::size_t>(q * static_cast<double>(latencies_ms.size()));
                return latencies_ms[std::min(rank, latencies_ms.size() - 1)];
            };
            report.p50_ms = pick(0.50);
            report.p99_ms = pick(0.99);
        }
        report_latency(provider, report);

        BOOST_TEST(engine.durable_through() >= commands);
        if (write_behind) {
            // Sixteen submitters queue behind every flush; anything else means
            // the barrier is not coalescing against this service.
            BOOST_TEST(log_puts < commands);
        }
        engine_for<Store> reader{store, bucket, mode_prefix, batched_options()};
        check_log(reader, 1, commands, [](std::uint64_t) { return 1U; });
    }
}

}  // namespace kythira::object_real
