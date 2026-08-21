// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_persistence_real_cases.hpp
/// @brief The five checks every provider's real tier runs, written once
///        (cloud-object-persistence task 16).
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
#include <chrono>
#include <cstdint>
#include <string>
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

}  // namespace kythira::object_real
