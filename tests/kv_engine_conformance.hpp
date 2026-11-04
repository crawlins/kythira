// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kv_engine_conformance.hpp
/// @brief One test body for the `kv_engine` concept, instantiated per engine
///        (.kiro/specs/persistent-state-machine/ Requirement 7.4).
///
/// `memory_kv_engine` instantiates it now; the RocksDB engine (task 5)
/// instantiates the same cases, so both engines are held to one contract.
///
/// ## Instantiating it
///
/// ```cpp
/// KYTHIRA_KV_ENGINE_CONFORMANCE(my_harness, my_engine)
/// ```
///
/// ### The harness contract
///
/// | Member | Meaning |
/// |---|---|
/// | `using engine_t` | the engine type under test |
/// | `make_engine()` | a fresh, empty engine as `std::unique_ptr<engine_t>`; may be called more
/// than once per case (the second is the ingest target) | | `scratch()` | a directory unique to the
/// harness instance that the cases create checkpoint directories under; the harness removes it |
///
/// A harness is default-constructed once per case.

#include <raft/persistent_state_machine.hpp>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::kv_conformance {

using kythira::column_family;
using kythira::key_range;
using kythira::scan_direction;
using kythira::write_batch;

/// Every (key, value) a scan visits, in visit order.
template<typename Engine>
auto scan_all(const Engine& e, column_family cf, const key_range& range,
              scan_direction direction = scan_direction::forward)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    e.scan(cf, range, direction, [&](std::string_view k, std::string_view v) {
        out.emplace_back(std::string(k), std::string(v));
        return true;
    });
    return out;
}

template<typename Engine> auto keys_of(const Engine& e, column_family cf, const key_range& range) {
    std::vector<std::string> out;
    for (auto& [k, v] : scan_all(e, cf, range)) {
        out.push_back(k);
    }
    return out;
}

inline auto everything() -> key_range {
    return key_range{};
}

template<typename Harness> struct cases {
    static void a_fresh_engine_is_empty() {
        Harness h;
        auto e = h.make_engine();
        for (const auto cf : kythira::k_all_column_families) {
            BOOST_TEST(!e->get(cf, "a").has_value());
            BOOST_TEST(scan_all(*e, cf, everything()).empty());
        }
        BOOST_TEST(e->durable_sequence() <= e->latest_sequence());
    }

    static void put_get_and_erase_are_per_column_family() {
        Harness h;
        auto e = h.make_engine();
        e->write(write_batch{}.put(column_family::data, "k", "data-v"));
        e->write(write_batch{}.put(column_family::txn, "k", "txn-v"));
        BOOST_TEST(e->get(column_family::data, "k").value_or("") == "data-v");
        BOOST_TEST(e->get(column_family::txn, "k").value_or("") == "txn-v");
        BOOST_TEST(!e->get(column_family::meta, "k").has_value());

        e->write(write_batch{}.put(column_family::data, "k", "data-v2"));
        BOOST_TEST(e->get(column_family::data, "k").value_or("") == "data-v2");

        e->write(write_batch{}.erase(column_family::data, "k"));
        BOOST_TEST(!e->get(column_family::data, "k").has_value());
        BOOST_TEST(e->get(column_family::txn, "k").value_or("") == "txn-v");

        // Erasing an absent key is not an error.
        e->write(write_batch{}.erase(column_family::meta, "absent"));
    }

    static void a_batch_applies_in_order_and_takes_one_increasing_sequence() {
        Harness h;
        auto e = h.make_engine();
        const auto s0 = e->latest_sequence();
        const auto s1 = e->write(write_batch{}
                                     .put(column_family::data, "a", "1")
                                     .erase(column_family::data, "a")
                                     .put(column_family::data, "b", "1")
                                     .put(column_family::data, "b", "2"));
        BOOST_TEST(s1 > s0);
        BOOST_TEST(e->latest_sequence() == s1);
        BOOST_TEST(!e->get(column_family::data, "a").has_value());
        BOOST_TEST(e->get(column_family::data, "b").value_or("") == "2");

        const auto s2 = e->write(write_batch{}
                                     .erase(column_family::data, "c")
                                     .put(column_family::data, "c", "x")
                                     .put(column_family::meta, "applied", "7"));
        BOOST_TEST(s2 > s1);
        BOOST_TEST(e->get(column_family::data, "c").value_or("") == "x");
        BOOST_TEST(e->get(column_family::meta, "applied").value_or("") == "7");
    }

    static void erase_range_is_half_open_and_honours_unbounded_ends() {
        Harness h;
        auto e = h.make_engine();
        write_batch seed;
        for (const auto* k : {"a", "b", "c", "d", "e"}) {
            seed.put(column_family::data, k, k);
            seed.put(column_family::txn, k, k);
        }
        e->write(seed);

        e->write(write_batch{}.erase_range(column_family::data, key_range{"b", "d"}));
        BOOST_TEST((keys_of(*e, column_family::data, everything()) ==
                    std::vector<std::string>{"a", "d", "e"}));
        // Other column families are untouched.
        BOOST_TEST(keys_of(*e, column_family::txn, everything()).size() == 5U);

        e->write(write_batch{}.erase_range(column_family::data, key_range{"d", std::nullopt}));
        BOOST_TEST(
            (keys_of(*e, column_family::data, everything()) == std::vector<std::string>{"a"}));

        e->write(write_batch{}.erase_range(column_family::txn, key_range{"", "c"}));
        BOOST_TEST((keys_of(*e, column_family::txn, everything()) ==
                    std::vector<std::string>{"c", "d", "e"}));

        // An empty range erases nothing.
        e->write(write_batch{}.erase_range(column_family::txn, key_range{"d", "d"}));
        e->write(write_batch{}.erase_range(column_family::txn, key_range{"e", "c"}));
        BOOST_TEST(keys_of(*e, column_family::txn, everything()).size() == 3U);
    }

    static void scans_visit_the_range_in_order_both_ways_and_stop_on_request() {
        Harness h;
        auto e = h.make_engine();
        write_batch seed;
        // More keys than one internal chunk of any reasonable engine.
        for (int i = 0; i < 700; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "k%04d", i);
            seed.put(column_family::data, buf, std::to_string(i));
        }
        seed.put(column_family::txn, "k0005", "other-cf");
        e->write(seed);

        const key_range r{"k0100", "k0600"};
        const auto fwd = scan_all(*e, column_family::data, r);
        BOOST_REQUIRE_EQUAL(fwd.size(), 500U);
        BOOST_TEST(fwd.front().first == "k0100");
        BOOST_TEST(fwd.back().first == "k0599");
        for (std::size_t i = 1; i < fwd.size(); ++i) {
            BOOST_TEST(fwd[i - 1].first < fwd[i].first);
        }

        const auto rev = scan_all(*e, column_family::data, r, scan_direction::reverse);
        BOOST_REQUIRE_EQUAL(rev.size(), 500U);
        BOOST_TEST(rev.front().first == "k0599");
        BOOST_TEST(rev.back().first == "k0100");

        // Unbounded both ways, reversed.
        const auto all_rev =
            scan_all(*e, column_family::data, everything(), scan_direction::reverse);
        BOOST_REQUIRE_EQUAL(all_rev.size(), 700U);
        BOOST_TEST(all_rev.front().first == "k0699");

        // The visitor bounds the scan; the count includes the key that stopped it.
        int seen = 0;
        const auto n = e->scan(column_family::data, everything(), scan_direction::forward,
                               [&](std::string_view, std::string_view) { return ++seen < 3; });
        BOOST_TEST(n == 3U);
        BOOST_TEST(seen == 3);

        BOOST_TEST(scan_all(*e, column_family::data, key_range{"k0300", "k0300"}).empty());
        BOOST_TEST(scan_all(*e, column_family::txn, everything()).size() == 1U);
    }

    static void a_scan_visitor_may_read_the_engine() {
        Harness h;
        auto e = h.make_engine();
        write_batch seed;
        for (int i = 0; i < 300; ++i) {
            seed.put(column_family::data, "d" + std::to_string(1000 + i), "v");
            seed.put(column_family::meta, "m" + std::to_string(1000 + i), "meta");
        }
        e->write(seed);
        std::size_t matched = 0;
        e->scan(column_family::data, everything(), scan_direction::forward,
                [&](std::string_view k, std::string_view) {
                    const auto m = e->get(column_family::meta, "m" + std::string(k.substr(1)));
                    matched += m.has_value() ? 1U : 0U;
                    return true;
                });
        BOOST_TEST(matched == 300U);
    }

    static void keys_order_as_unsigned_bytes() {
        Harness h;
        auto e = h.make_engine();
        const std::string zero("a\0b", 3);
        const std::string high("a\xff", 2);
        const std::string plain("ab");
        e->write(write_batch{}
                     .put(column_family::data, high, "high")
                     .put(column_family::data, plain, "plain")
                     .put(column_family::data, zero, "zero")
                     .put(column_family::data, "a", "a"));
        BOOST_TEST((keys_of(*e, column_family::data, everything()) ==
                    std::vector<std::string>{"a", zero, plain, high}));
        BOOST_TEST(e->get(column_family::data, zero).value_or("") == "zero");
        // A range ending at "a\xff" excludes it and includes "ab".
        BOOST_TEST((keys_of(*e, column_family::data, key_range{"a\x01", high}) ==
                    std::vector<std::string>{plain}));
    }

    static void sync_makes_the_requested_sequence_durable() {
        Harness h;
        auto e = h.make_engine();
        const auto s1 = e->write(write_batch{}.put(column_family::data, "a", "1"));
        const auto s2 = e->write(write_batch{}.put(column_family::data, "b", "2"));
        BOOST_TEST(e->durable_sequence() <= e->latest_sequence());
        e->sync(s1);
        BOOST_TEST(e->durable_sequence() >= s1);
        e->sync(s2 + 1000);  // past the latest: syncs everything
        BOOST_TEST(e->durable_sequence() == s2);
        BOOST_TEST(e->durable_sequence() == e->latest_sequence());
    }

    static void checkpoint_then_ingest_reproduces_the_range_in_every_column_family() {
        Harness h;
        auto src = h.make_engine();
        write_batch seed;
        for (const auto* k : {"g/a", "g/b", "g/c", "h/a", "f/z"}) {
            seed.put(column_family::data, k, std::string("data:") + k);
            seed.put(column_family::txn, k, std::string("txn:") + k);
        }
        seed.put(column_family::meta, "g/meta", "meta-in-range");
        src->write(seed);

        const key_range r{"g/", "g0"};
        const auto dir = h.scratch() / "cp1";
        const auto m = src->checkpoint(r, dir);
        BOOST_TEST(m.engine_format == std::string(src->format_tag()));
        BOOST_TEST((m.range == r));
        BOOST_REQUIRE(!m.files.empty());
        for (const auto& f : m.files) {
            BOOST_TEST(kythira::snapshot_manifest::valid_file_name(f.name));
            const auto [digest, size] = kythira::sha256_of_file(dir / f.name);
            BOOST_TEST(size == f.size);
            BOOST_TEST((digest == f.sha256));
        }
        // The manifest survives its own encoding.
        BOOST_TEST((kythira::snapshot_manifest::decode(m.encode()) == m));

        auto dst = h.make_engine();
        dst->write(write_batch{}
                       .put(column_family::data, "g/stale", "replaced")
                       .put(column_family::txn, "g/a", "old")
                       .put(column_family::data, "z/outside", "kept"));
        const auto before = dst->latest_sequence();
        const auto seq = dst->ingest(r, dir, m);
        BOOST_TEST(seq > before);

        BOOST_TEST(
            (scan_all(*dst, column_family::data, r) == scan_all(*src, column_family::data, r)));
        BOOST_TEST(
            (scan_all(*dst, column_family::txn, r) == scan_all(*src, column_family::txn, r)));
        BOOST_TEST(dst->get(column_family::meta, "g/meta").value_or("") == "meta-in-range");
        BOOST_TEST(!dst->get(column_family::data, "g/stale").has_value());
        BOOST_TEST(dst->get(column_family::data, "z/outside").value_or("") == "kept");
        BOOST_TEST(!dst->get(column_family::data, "h/a").has_value());
    }

    static void a_checkpoint_is_a_point_in_time_copy() {
        Harness h;
        auto src = h.make_engine();
        src->write(write_batch{}.put(column_family::data, "k", "before"));
        const auto dir = h.scratch() / "cp";
        const auto m = src->checkpoint(everything(), dir);
        src->write(write_batch{}
                       .put(column_family::data, "k", "after")
                       .put(column_family::data, "new", "x"));
        auto dst = h.make_engine();
        dst->ingest(everything(), dir, m);
        BOOST_TEST(dst->get(column_family::data, "k").value_or("") == "before");
        BOOST_TEST(!dst->get(column_family::data, "new").has_value());
    }

    static void an_empty_range_checkpoints_and_ingests_as_empty() {
        Harness h;
        auto src = h.make_engine();
        src->write(write_batch{}.put(column_family::data, "a", "1"));
        const key_range r{"m", "n"};
        const auto dir = h.scratch() / "empty";
        const auto m = src->checkpoint(r, dir);
        auto dst = h.make_engine();
        dst->write(write_batch{}
                       .put(column_family::data, "m1", "gone")
                       .put(column_family::data, "a", "kept"));
        dst->ingest(r, dir, m);
        BOOST_TEST(!dst->get(column_family::data, "m1").has_value());
        BOOST_TEST(dst->get(column_family::data, "a").value_or("") == "kept");
    }

    static void ingest_refuses_a_bad_checkpoint_and_leaves_the_range_alone() {
        Harness h;
        auto src = h.make_engine();
        src->write(write_batch{}.put(column_family::data, "g/a", "fresh"));
        const key_range r{"g/", "g0"};
        const auto dir = h.scratch() / "cp";
        const auto m = src->checkpoint(r, dir);

        auto dst = h.make_engine();
        dst->write(write_batch{}.put(column_family::data, "g/a", "live"));
        const auto untouched = [&] {
            BOOST_TEST(dst->get(column_family::data, "g/a").value_or("") == "live");
        };

        // Another engine's format.
        auto other_format = m;
        other_format.engine_format = "someone-else/9";
        BOOST_CHECK_THROW(dst->ingest(r, dir, other_format), std::exception);
        untouched();

        // A different range from the one checkpointed.
        BOOST_CHECK_THROW(dst->ingest(key_range{"g/", "g/zz"}, dir, m), std::exception);
        untouched();

        // A checksum that does not match.
        auto wrong_sum = m;
        wrong_sum.files.front().sha256[0] ^= 0x01U;
        BOOST_CHECK_THROW(dst->ingest(r, dir, wrong_sum), std::exception);
        untouched();

        // A file changed on disk after the checkpoint.
        {
            std::ofstream out(dir / m.files.front().name, std::ios::binary | std::ios::app);
            out << "tampered";
        }
        BOOST_CHECK_THROW(dst->ingest(r, dir, m), std::exception);
        untouched();

        // A missing file.
        std::filesystem::remove(dir / m.files.front().name);
        BOOST_CHECK_THROW(dst->ingest(r, dir, m), std::exception);
        untouched();
    }

    static void checkpoint_refuses_an_existing_directory() {
        Harness h;
        auto e = h.make_engine();
        const auto dir = h.scratch() / "exists";
        std::filesystem::create_directories(dir);
        BOOST_CHECK_THROW(e->checkpoint(everything(), dir), std::exception);
    }

    static void drop_removes_the_range_from_every_column_family_only() {
        Harness h;
        auto e = h.make_engine();
        write_batch seed;
        for (const auto cf : kythira::k_all_column_families) {
            seed.put(cf, "g/a", "in");
            seed.put(cf, "h/a", "out");
        }
        e->write(seed);
        const auto before = e->latest_sequence();
        BOOST_TEST(e->drop(key_range{"g/", "g0"}) > before);
        for (const auto cf : kythira::k_all_column_families) {
            BOOST_TEST(!e->get(cf, "g/a").has_value());
            BOOST_TEST(e->get(cf, "h/a").value_or("") == "out");
        }
    }

    static void concurrent_writers_each_get_a_distinct_sequence() {
        Harness h;
        auto e = h.make_engine();
        constexpr int k_threads = 8;
        constexpr int k_writes = 200;
        std::vector<std::vector<kythira::engine_sequence>> seqs(k_threads);
        std::vector<std::thread> threads;
        threads.reserve(k_threads);
        for (int t = 0; t < k_threads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < k_writes; ++i) {
                    const auto key = "t" + std::to_string(t) + "/" + std::to_string(i);
                    seqs[static_cast<std::size_t>(t)].push_back(
                        e->write(write_batch{}.put(column_family::data, key, "v")));
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        std::set<kythira::engine_sequence> all;
        for (const auto& per : seqs) {
            for (std::size_t i = 1; i < per.size(); ++i) {
                BOOST_TEST(per[i - 1] < per[i]);
            }
            all.insert(per.begin(), per.end());
        }
        BOOST_TEST(all.size() == static_cast<std::size_t>(k_threads * k_writes));
        BOOST_TEST(scan_all(*e, column_family::data, everything()).size() ==
                   static_cast<std::size_t>(k_threads * k_writes));
    }
};

}  // namespace kythira::kv_conformance

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_KVC_CASE(harness_type, case_name)                     \
    BOOST_AUTO_TEST_CASE(case_name, *boost::unit_test::timeout(60)) { \
        kythira::kv_conformance::cases<harness_type>::case_name();    \
    }

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_KV_ENGINE_CONFORMANCE(harness_type, suite_prefix)                                \
    BOOST_AUTO_TEST_SUITE(suite_prefix##_kv_engine_conformance)                                  \
    KYTHIRA_KVC_CASE(harness_type, a_fresh_engine_is_empty)                                      \
    KYTHIRA_KVC_CASE(harness_type, put_get_and_erase_are_per_column_family)                      \
    KYTHIRA_KVC_CASE(harness_type, a_batch_applies_in_order_and_takes_one_increasing_sequence)   \
    KYTHIRA_KVC_CASE(harness_type, erase_range_is_half_open_and_honours_unbounded_ends)          \
    KYTHIRA_KVC_CASE(harness_type, scans_visit_the_range_in_order_both_ways_and_stop_on_request) \
    KYTHIRA_KVC_CASE(harness_type, a_scan_visitor_may_read_the_engine)                           \
    KYTHIRA_KVC_CASE(harness_type, keys_order_as_unsigned_bytes)                                 \
    KYTHIRA_KVC_CASE(harness_type, sync_makes_the_requested_sequence_durable)                    \
    KYTHIRA_KVC_CASE(harness_type,                                                               \
                     checkpoint_then_ingest_reproduces_the_range_in_every_column_family)         \
    KYTHIRA_KVC_CASE(harness_type, a_checkpoint_is_a_point_in_time_copy)                         \
    KYTHIRA_KVC_CASE(harness_type, an_empty_range_checkpoints_and_ingests_as_empty)              \
    KYTHIRA_KVC_CASE(harness_type, ingest_refuses_a_bad_checkpoint_and_leaves_the_range_alone)   \
    KYTHIRA_KVC_CASE(harness_type, checkpoint_refuses_an_existing_directory)                     \
    KYTHIRA_KVC_CASE(harness_type, drop_removes_the_range_from_every_column_family_only)         \
    KYTHIRA_KVC_CASE(harness_type, concurrent_writers_each_get_a_distinct_sequence)              \
    BOOST_AUTO_TEST_SUITE_END()
