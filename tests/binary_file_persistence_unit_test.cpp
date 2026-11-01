// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file binary_file_persistence_unit_test.cpp
/// @brief `binary_file_persistence_engine` (.kiro/specs/disk-backed-build-cache/
///        task 4; Requirements 5.1 and 5.3).
///
/// The first half is `file_persistence_unit_test.cpp`'s cases, ported
/// unchanged apart from the engine type, so both engines are held to the same
/// contract. The second half covers what only this engine has: segments,
/// torn tails, the contiguous-log rules, the barrier, and a process killed
/// mid-append.

#define BOOST_TEST_MODULE binary_file_persistence_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/binary_file_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#include <fiu.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using engine_t = kythira::binary_file_persistence_engine<>;
using log_entry_t = kythira::log_entry<>;
using snapshot_t = kythira::snapshot<>;

#ifdef FIU_ENABLE
namespace {
/// libfiu needs `fiu_init` before any fault point is armed.
struct fiu_init_fixture {
    fiu_init_fixture() { ::fiu_init(0); }
};
}  // namespace
BOOST_GLOBAL_FIXTURE(fiu_init_fixture);
#endif

namespace {

/// A fresh directory per test case, removed afterwards.
struct temp_dir {
    fs::path path;
    temp_dir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / ("kythira_bfp_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(counter.fetch_add(1)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
};

auto make_entry(std::uint64_t term, std::uint64_t index,
                std::vector<std::byte> cmd = {std::byte{0xAB}}) -> log_entry_t {
    log_entry_t e;
    e._term = term;
    e._index = index;
    e._command = std::move(cmd);
    return e;
}

auto payload(std::size_t size, std::uint64_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>((seed * 131U) + i);
    }
    return out;
}

auto segments_in(const fs::path& dir) -> std::vector<fs::path> {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().starts_with("log.")) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return std::stoull(a.extension().string().substr(1)) <
               std::stoull(b.extension().string().substr(1));
    });
    return out;
}

auto small_segments() -> kythira::binary_file_persistence_options {
    kythira::binary_file_persistence_options o;
    o._segment_max_bytes = 256;
    return o;
}

}  // namespace

// ── Concept ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_concept_satisfied) {
    static_assert(kythira::persistence_engine<engine_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              log_entry_t, snapshot_t>,
                  "binary_file_persistence_engine must satisfy persistence_engine concept");
}

// ── Term ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_initial_term_is_zero, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    BOOST_TEST(eng.load_current_term() == 0u);
}

BOOST_AUTO_TEST_CASE(test_term_round_trip, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    eng.save_current_term(42);
    BOOST_TEST(eng.load_current_term() == 42u);
    eng.save_current_term(100);
    BOOST_TEST(eng.load_current_term() == 100u);
}

BOOST_AUTO_TEST_CASE(test_term_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        eng.save_current_term(77);
    }
    engine_t eng2(d.path);
    BOOST_TEST(eng2.load_current_term() == 77u);
}

// ── votedFor ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_initial_voted_for_empty, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    BOOST_TEST(!eng.load_voted_for().has_value());
}

BOOST_AUTO_TEST_CASE(test_voted_for_round_trip, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    eng.save_voted_for(3);
    BOOST_REQUIRE(eng.load_voted_for().has_value());
    BOOST_TEST(*eng.load_voted_for() == 3u);
}

BOOST_AUTO_TEST_CASE(test_voted_for_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        eng.save_voted_for(99);
    }
    engine_t eng2(d.path);
    BOOST_REQUIRE(eng2.load_voted_for().has_value());
    BOOST_TEST(*eng2.load_voted_for() == 99u);
}

// ── Log ──────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_empty_log, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    BOOST_TEST(eng.get_last_log_index() == 0u);
    BOOST_TEST(!eng.get_log_entry(1).has_value());
}

BOOST_AUTO_TEST_CASE(test_log_append_and_get, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    eng.append_log_entry(make_entry(1, 1));
    eng.append_log_entry(make_entry(1, 2));
    eng.append_log_entry(make_entry(2, 3, {std::byte{0x01}, std::byte{0x02}}));

    BOOST_TEST(eng.get_last_log_index() == 3u);
    auto e1 = eng.get_log_entry(1);
    BOOST_REQUIRE(e1.has_value());
    BOOST_TEST(e1->term() == 1u);
    BOOST_TEST(e1->index() == 1u);
    auto e3 = eng.get_log_entry(3);
    BOOST_REQUIRE(e3.has_value());
    BOOST_TEST(e3->term() == 2u);
    BOOST_TEST(e3->command().size() == 2u);
}

BOOST_AUTO_TEST_CASE(test_log_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        eng.append_log_entry(make_entry(1, 1, {std::byte{0xAA}}));
        eng.append_log_entry(make_entry(2, 2, {std::byte{0xBB}}));
    }
    engine_t eng2(d.path);
    BOOST_TEST(eng2.get_last_log_index() == 2u);
    auto e1 = eng2.get_log_entry(1);
    BOOST_REQUIRE(e1.has_value());
    BOOST_TEST(std::to_integer<int>(e1->command()[0]) == 0xAA);
    auto e2 = eng2.get_log_entry(2);
    BOOST_REQUIRE(e2.has_value());
    BOOST_TEST(std::to_integer<int>(e2->command()[0]) == 0xBB);
}

BOOST_AUTO_TEST_CASE(test_binary_command_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    // All 256 byte values including null bytes.
    std::vector<std::byte> all_bytes(256);
    for (int i = 0; i < 256; ++i) {
        all_bytes[i] = static_cast<std::byte>(i);
    }
    {
        engine_t eng(d.path);
        eng.append_log_entry(make_entry(1, 1, all_bytes));
    }
    engine_t eng2(d.path);
    auto e = eng2.get_log_entry(1);
    BOOST_REQUIRE(e.has_value());
    BOOST_CHECK(e->command() == all_bytes);
}

BOOST_AUTO_TEST_CASE(test_log_entries_range, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i, {static_cast<std::byte>(i)}));
    }
    auto r = eng.get_log_entries(2, 4);
    BOOST_REQUIRE_EQUAL(r.size(), 3u);
    BOOST_TEST(r[0].index() == 2u);
    BOOST_TEST(r[1].index() == 3u);
    BOOST_TEST(r[2].index() == 4u);
}

BOOST_AUTO_TEST_CASE(test_truncate_log, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i));
    }
    eng.truncate_log(3);
    BOOST_TEST(eng.get_last_log_index() == 2u);
    BOOST_TEST(!eng.get_log_entry(3).has_value());
    BOOST_TEST(!eng.get_log_entry(5).has_value());
    BOOST_TEST(eng.get_log_entry(1).has_value());
    BOOST_TEST(eng.get_log_entry(2).has_value());
}

BOOST_AUTO_TEST_CASE(test_truncate_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.truncate_log(3);
    }
    engine_t eng2(d.path);
    BOOST_TEST(eng2.get_last_log_index() == 2u);
    BOOST_TEST(!eng2.get_log_entry(3).has_value());
    BOOST_TEST(eng2.get_log_entry(2).has_value());
}

BOOST_AUTO_TEST_CASE(test_delete_log_entries_before, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i));
    }
    eng.delete_log_entries_before(3);
    BOOST_TEST(!eng.get_log_entry(1).has_value());
    BOOST_TEST(!eng.get_log_entry(2).has_value());
    BOOST_TEST(eng.get_log_entry(3).has_value());
    BOOST_TEST(eng.get_last_log_index() == 5u);
}

BOOST_AUTO_TEST_CASE(test_delete_before_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.delete_log_entries_before(4);
    }
    engine_t eng2(d.path);
    BOOST_TEST(!eng2.get_log_entry(1).has_value());
    BOOST_TEST(!eng2.get_log_entry(3).has_value());
    BOOST_TEST(eng2.get_log_entry(4).has_value());
    BOOST_TEST(eng2.get_log_entry(5).has_value());
}

// ── Snapshot ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_initial_snapshot_empty, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    BOOST_TEST(!eng.load_snapshot().has_value());
}

BOOST_AUTO_TEST_CASE(test_snapshot_round_trip, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    kythira::cluster_configuration<> cfg{{1, 2, 3}, false, std::nullopt, {}};
    snapshot_t snap;
    snap._last_included_index = 10;
    snap._last_included_term = 3;
    snap._configuration = cfg;
    snap._state_machine_state = {std::byte{0xCA}, std::byte{0xFE}, std::byte{0x00},
                                 std::byte{0xFF}};
    eng.save_snapshot(snap);
    auto loaded = eng.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 10u);
    BOOST_TEST(loaded->last_included_term() == 3u);
    BOOST_CHECK(loaded->state_machine_state() == snap._state_machine_state);
    BOOST_TEST(loaded->configuration().nodes() == cfg.nodes());
}

BOOST_AUTO_TEST_CASE(test_snapshot_survives_reload, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        kythira::cluster_configuration<> cfg{{1, 2}, false, std::nullopt, {}};
        snapshot_t snap;
        snap._last_included_index = 20;
        snap._last_included_term = 5;
        snap._configuration = cfg;
        snap._state_machine_state = {std::byte{0x42}};
        eng.save_snapshot(snap);
    }
    engine_t eng2(d.path);
    auto loaded = eng2.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 20u);
    BOOST_TEST(loaded->last_included_term() == 5u);
    BOOST_TEST(std::to_integer<int>(loaded->state_machine_state()[0]) == 0x42);
}

BOOST_AUTO_TEST_CASE(test_snapshot_overwrites_previous, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    kythira::cluster_configuration<> cfg{{1}, false, std::nullopt, {}};
    snapshot_t s1;
    s1._last_included_index = 5;
    s1._last_included_term = 1;
    s1._configuration = cfg;
    snapshot_t s2;
    s2._last_included_index = 10;
    s2._last_included_term = 2;
    s2._configuration = cfg;
    s1._state_machine_state = {std::byte{0x01}};
    s2._state_machine_state = {std::byte{0x02}};
    eng.save_snapshot(s1);
    eng.save_snapshot(s2);
    auto loaded = eng.load_snapshot();
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loaded->last_included_index() == 10u);
    BOOST_TEST(std::to_integer<int>(loaded->state_machine_state()[0]) == 0x02);
}

// ── Move constructor ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_move_constructor, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    eng.save_current_term(5);
    eng.append_log_entry(make_entry(1, 1));

    engine_t eng2(std::move(eng));
    BOOST_TEST(eng2.load_current_term() == 5u);
    BOOST_TEST(eng2.get_log_entry(1).has_value());
}

// ── Engine-specific cases ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_optional_concepts_satisfied) {
    static_assert(kythira::barriered_persistence_engine<engine_t>);
    static_assert(kythira::hard_state_persistence_engine<engine_t>);
    static_assert(kythira::persistence_engine<kythira::binary_file_persistence_engine<std::string>,
                                              std::string, std::uint64_t, std::uint64_t,
                                              log_entry_t, kythira::snapshot<std::string>>);
}

BOOST_AUTO_TEST_CASE(test_hard_state_and_string_ids_survive_reload,
                     *boost::unit_test::timeout(10)) {
    temp_dir d;
    using string_engine = kythira::binary_file_persistence_engine<std::string>;
    {
        string_engine eng(d.path);
        eng.save_hard_state(7, std::string("node-b"));
        kythira::snapshot<std::string> snap{};
        snap._last_included_index = 4;
        snap._last_included_term = 2;
        snap._configuration._nodes = {"a", "b", "c"};
        snap._configuration._is_joint_consensus = true;
        snap._configuration._old_nodes = std::vector<std::string>{"a", "b"};
        snap._configuration._learners = {"d"};
        eng.save_snapshot(snap);
    }
    string_engine eng(d.path);
    BOOST_TEST(eng.load_current_term() == 7U);
    BOOST_REQUIRE(eng.load_voted_for().has_value());
    BOOST_TEST(*eng.load_voted_for() == "node-b");
    auto snap = eng.load_snapshot();
    BOOST_REQUIRE(snap.has_value());
    BOOST_TEST(snap->configuration().nodes() == (std::vector<std::string>{"a", "b", "c"}));
    BOOST_TEST(snap->configuration().is_joint_consensus());
    BOOST_REQUIRE(snap->configuration().old_nodes().has_value());
    BOOST_TEST(*snap->configuration().old_nodes() == (std::vector<std::string>{"a", "b"}));
    BOOST_TEST(snap->configuration().learners() == (std::vector<std::string>{"d"}));

    // Clearing the vote is a hard-state change too.
    eng.save_hard_state(8, std::nullopt);
    string_engine again(d.path);
    BOOST_TEST(again.load_current_term() == 8U);
    BOOST_TEST(!again.load_voted_for().has_value());
}

BOOST_AUTO_TEST_CASE(test_entry_type_and_large_command_round_trip, *boost::unit_test::timeout(10)) {
    temp_dir d;
    const auto big = payload(std::size_t{4} * 1024 * 1024 + 3, 9);
    {
        engine_t eng(d.path);
        auto e = make_entry(1, 1, {});
        e._type = kythira::entry_type::configuration;
        eng.append_log_entry(e);
        eng.append_log_entry(make_entry(1, 2, big));
    }
    engine_t eng(d.path);
    auto e1 = eng.get_log_entry(1);
    BOOST_REQUIRE(e1.has_value());
    BOOST_TEST(static_cast<int>(e1->type()) ==
               static_cast<int>(kythira::entry_type::configuration));
    BOOST_TEST(e1->command().empty());
    auto e2 = eng.get_log_entry(2);
    BOOST_REQUIRE(e2.has_value());
    BOOST_CHECK(e2->command() == big);
}

BOOST_AUTO_TEST_CASE(test_append_rules_follow_raft, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i, payload(10, i)));
    }
    // Same index and term again: the same entry, a no-op that keeps 4 and 5.
    eng.append_log_entry(make_entry(1, 3, payload(10, 3)));
    BOOST_TEST(eng.get_last_log_index() == 5U);
    // Same index, newer term: a conflict, which drops 3..5 first.
    eng.append_log_entry(make_entry(2, 3, payload(10, 33)));
    BOOST_TEST(eng.get_last_log_index() == 3U);
    BOOST_TEST(eng.get_log_entry(3)->term() == 2U);
    // A gap is a caller bug.
    BOOST_CHECK_THROW(eng.append_log_entry(make_entry(2, 5)), kythira::persistence_exception);

    engine_t reloaded(d.path);
    BOOST_TEST(reloaded.get_last_log_index() == 3U);
    BOOST_TEST(reloaded.get_log_entry(3)->term() == 2U);
    BOOST_CHECK(reloaded.get_log_entry(2)->command() == payload(10, 2));
}

BOOST_AUTO_TEST_CASE(test_segments_roll_and_prefix_deletion_frees_them,
                     *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path, small_segments());
        for (std::uint64_t i = 1; i <= 40; ++i) {
            eng.append_log_entry(make_entry(1, i, payload(50, i)));
        }
        // 75-byte records, 256-byte segments: four records per segment.
        BOOST_TEST(eng.segment_count() == 10U);
        eng.delete_log_entries_before(23);
        // 1..20 lived in the first five segments; 21..24 share the sixth.
        BOOST_TEST(eng.segment_count() == 5U);
        BOOST_TEST(!eng.get_log_entry(22).has_value());
        BOOST_TEST(eng.get_log_entry(23).has_value());
    }
    BOOST_TEST(segments_in(d.path).size() == 5U);
    engine_t eng(d.path, small_segments());
    BOOST_TEST(!eng.get_log_entry(22).has_value());
    BOOST_CHECK(eng.get_log_entry(23)->command() == payload(50, 23));
    BOOST_TEST(eng.get_last_log_index() == 40U);
    BOOST_TEST(eng.get_log_entries(1, 100).size() == 18U);
}

BOOST_AUTO_TEST_CASE(test_truncate_across_segments, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path, small_segments());
        for (std::uint64_t i = 1; i <= 20; ++i) {
            eng.append_log_entry(make_entry(1, i, payload(50, i)));
        }
        eng.truncate_log(7);
        BOOST_TEST(eng.segment_count() == 2U);
        eng.append_log_entry(make_entry(2, 7, payload(50, 70)));
    }
    engine_t eng(d.path, small_segments());
    BOOST_TEST(eng.get_last_log_index() == 7U);
    BOOST_TEST(eng.get_log_entry(7)->term() == 2U);
    BOOST_CHECK(eng.get_log_entry(6)->command() == payload(50, 6));
}

/// After a snapshot install: the whole log goes (`truncate_log(1)`) and the
/// next append starts past the snapshot, or an emptied log restarts lower.
BOOST_AUTO_TEST_CASE(test_empty_log_restarts_anywhere, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path, small_segments());
        for (std::uint64_t i = 1; i <= 10; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.delete_log_entries_before(50);  // a snapshot well past the log
        BOOST_TEST(eng.get_last_log_index() == 0U);
        eng.append_log_entry(make_entry(3, 50));
        eng.append_log_entry(make_entry(3, 51));
    }
    {
        engine_t eng(d.path, small_segments());
        BOOST_TEST(eng.get_last_log_index() == 51U);
        BOOST_TEST(!eng.get_log_entry(10).has_value());
        eng.truncate_log(1);
        BOOST_TEST(eng.get_last_log_index() == 0U);
        // Lower than the recorded first index: no dead record may come back.
        eng.append_log_entry(make_entry(4, 20));
    }
    engine_t eng(d.path, small_segments());
    BOOST_TEST(eng.get_last_log_index() == 20U);
    BOOST_TEST(eng.get_log_entries(1, 100).size() == 1U);
    BOOST_TEST(eng.get_log_entry(20)->term() == 4U);
}

BOOST_AUTO_TEST_CASE(test_torn_tail_is_truncated, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        for (std::uint64_t i = 1; i <= 3; ++i) {
            eng.append_log_entry(make_entry(1, i, payload(100, i)));
        }
    }
    const auto seg = segments_in(d.path).back();
    const auto good = fs::file_size(seg);
    // A partial header.
    {
        std::ofstream f(seg, std::ios::binary | std::ios::app);
        f << std::string(9, '\x01');
    }
    {
        engine_t eng(d.path);
        BOOST_TEST(eng.get_last_log_index() == 3U);
        BOOST_TEST(fs::file_size(seg) == good);
    }
    // A whole record's length whose CRC is wrong, then appends continue.
    fs::resize_file(seg, good - 2);
    {
        engine_t eng(d.path);
        BOOST_TEST(eng.get_last_log_index() == 2U);
        eng.append_log_entry(make_entry(1, 3, payload(100, 33)));
    }
    engine_t eng(d.path);
    BOOST_TEST(eng.get_last_log_index() == 3U);
    BOOST_CHECK(eng.get_log_entry(3)->command() == payload(100, 33));
}

BOOST_AUTO_TEST_CASE(test_corruption_outside_the_tail_is_fatal, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path, small_segments());
        for (std::uint64_t i = 1; i <= 12; ++i) {
            eng.append_log_entry(make_entry(1, i, payload(50, i)));
        }
    }
    const auto first = segments_in(d.path).front();
    {
        std::fstream f(first, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(40);
        f.put('\x7F');
    }
    BOOST_CHECK_THROW(engine_t(d.path, small_segments()), kythira::persistence_exception);

    // A damaged meta is fatal too.
    temp_dir d2;
    {
        engine_t eng(d2.path);
        eng.save_current_term(3);
    }
    {
        std::fstream f(d2.path / "meta", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(10);
        f.put('\x7F');
    }
    BOOST_CHECK_THROW(engine_t{d2.path}, kythira::persistence_exception);
}

/// A crash after `meta` recorded the new first index and before the dead
/// segments were deleted: the load deletes them.
BOOST_AUTO_TEST_CASE(test_dead_segments_left_by_a_crash_are_deleted,
                     *boost::unit_test::timeout(10)) {
    temp_dir d;
    const auto saved = d.path / "saved";
    {
        engine_t eng(d.path / "g", small_segments());
        for (std::uint64_t i = 1; i <= 20; ++i) {
            eng.append_log_entry(make_entry(1, i, payload(50, i)));
        }
        fs::create_directories(saved);
        fs::copy_file(segments_in(d.path / "g").front(), saved / "log.1");
        eng.delete_log_entries_before(13);
    }
    fs::copy_file(saved / "log.1", d.path / "g" / "log.1");
    engine_t eng(d.path / "g", small_segments());
    BOOST_TEST(!fs::exists(d.path / "g" / "log.1"));
    BOOST_TEST(!eng.get_log_entry(4).has_value());
    BOOST_TEST(eng.get_log_entry(13).has_value());
    BOOST_TEST(eng.get_last_log_index() == 20U);
}

BOOST_AUTO_TEST_CASE(test_barrier_publishes_and_coalesces, *boost::unit_test::timeout(30)) {
    temp_dir d;
    engine_t eng(d.path);
    BOOST_TEST(static_cast<int>(eng.durability()) ==
               static_cast<int>(kythira::durability_class::barrier));
    BOOST_TEST(kythira::describes_durable_storage(eng));
    const auto s1 = eng.append_log_entry_sequenced(make_entry(1, 1));
    const auto s2 = eng.append_log_entry_sequenced(make_entry(1, 2));
    BOOST_TEST(eng.durable_through() == 0U);
    eng.barrier_through(s1);
    // One barrier covers everything written before it sampled.
    BOOST_TEST(eng.durable_through() >= s2);
    const auto issued = eng.barriers_issued();
    eng.barrier_through(s2);
    BOOST_TEST(eng.barriers_issued() == issued);
    BOOST_CHECK_THROW(eng.barrier_through(s2 + 10), kythira::persistence_exception);

    // Many threads, each appending then barriering: fewer fsyncs than appends.
    std::atomic<std::uint64_t> next{3};
    std::mutex append_order;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                kythira::write_sequence seq = 0;
                {
                    // Appends must stay contiguous, so they are ordered here;
                    // the barriers are what run concurrently.
                    const std::lock_guard lock(append_order);
                    seq = eng.append_log_entry_sequenced(make_entry(1, next++));
                }
                eng.barrier_through(seq);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    BOOST_TEST(eng.get_last_log_index() == 202U);
    BOOST_TEST(eng.durable_through() >= 202U);
    BOOST_TEST_MESSAGE("barriers for 200 appends: " << eng.barriers_issued() - issued);
    BOOST_TEST(eng.barriers_issued() - issued <= 200U);

    kythira::binary_file_persistence_options buffered;
    buffered._barriers_enabled = false;
    engine_t fast(d.path / "buffered", buffered);
    BOOST_TEST(!kythira::describes_durable_storage(fast));
}

/// A writer process appends and barriers in a loop, reporting each durable
/// index on a pipe, and is killed with SIGKILL at an arbitrary point. Every
/// index it reported must be there after a reload, and the log must be one
/// valid run.
BOOST_AUTO_TEST_CASE(test_kill_mid_append_keeps_every_acknowledged_entry,
                     *boost::unit_test::timeout(120)) {
    for (int round = 0; round < 5; ++round) {
        temp_dir d;
        int fds[2];
        BOOST_REQUIRE(::pipe(fds) == 0);
        const pid_t pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            ::close(fds[0]);
            engine_t eng(d.path, small_segments());
            for (std::uint64_t i = 1;; ++i) {
                const auto seq =
                    eng.append_log_entry_sequenced(make_entry(1, i, payload(30 + (i % 200), i)));
                eng.barrier_through(seq);
                if (::write(fds[1], &i, sizeof(i)) != sizeof(i)) {
                    ::_exit(1);
                }
            }
        }
        ::close(fds[1]);
        std::uint64_t acked = 0;
        std::uint64_t got = 0;
        const std::uint64_t kill_after = 50 + (static_cast<std::uint64_t>(round) * 37);
        while (::read(fds[0], &got, sizeof(got)) == sizeof(got)) {
            acked = got;
            if (acked >= kill_after) {
                break;
            }
        }
        // Let it get some way into further appends before the kill lands.
        std::this_thread::sleep_for(std::chrono::microseconds(200 * round));
        ::kill(pid, SIGKILL);
        // Drain whatever it acknowledged before dying.
        while (::read(fds[0], &got, sizeof(got)) == sizeof(got)) {
            acked = got;
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);

        engine_t eng(d.path, small_segments());
        const auto last = eng.get_last_log_index();
        BOOST_TEST_MESSAGE("round " << round << ": acked " << acked << ", reloaded " << last);
        BOOST_TEST(last >= acked);
        const auto all = eng.get_log_entries(1, last);
        BOOST_REQUIRE_EQUAL(all.size(), last);
        for (std::uint64_t i = 1; i <= last; ++i) {
            BOOST_CHECK(all[i - 1].command() == payload(30 + (i % 200), i));
        }
        // And it can carry on.
        eng.append_log_entry(make_entry(2, last + 1));
    }
}

#ifdef FIU_ENABLE
BOOST_AUTO_TEST_CASE(test_injected_torn_append, *boost::unit_test::timeout(10)) {
    temp_dir d;
    {
        engine_t eng(d.path);
        eng.append_log_entry(make_entry(1, 1, payload(100, 1)));
        ::fiu_enable("raft/binary_persistence/torn_append", 1, nullptr, 0);
        BOOST_CHECK_THROW(eng.append_log_entry(make_entry(1, 2, payload(100, 2))),
                          kythira::persistence_exception);
        ::fiu_disable("raft/binary_persistence/torn_append");
        // The failed append left nothing behind; the next one lands cleanly.
        eng.append_log_entry(make_entry(1, 2, payload(100, 22)));
    }
    engine_t eng(d.path);
    BOOST_TEST(eng.get_last_log_index() == 2U);
    BOOST_CHECK(eng.get_log_entry(2)->command() == payload(100, 22));
}

BOOST_AUTO_TEST_CASE(test_failed_barrier_throws_and_retries, *boost::unit_test::timeout(10)) {
    temp_dir d;
    engine_t eng(d.path);
    const auto seq = eng.append_log_entry_sequenced(make_entry(1, 1));
    ::fiu_enable("raft/persistence/barrier", 1, nullptr, 0);
    BOOST_CHECK_THROW(eng.barrier_through(seq), kythira::persistence_exception);
    ::fiu_disable("raft/persistence/barrier");
    BOOST_TEST(eng.durable_through() < seq);
    eng.barrier_through(seq);
    BOOST_TEST(eng.durable_through() >= seq);
}
#endif
