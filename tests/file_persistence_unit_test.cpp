#define BOOST_TEST_MODULE file_persistence_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/file_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;
using engine_t = kythira::file_persistence_engine<>;
using log_entry_t = kythira::log_entry<>;
using snapshot_t = kythira::snapshot<>;

// RAII temp directory unique per test process
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("kythira_fp_" + std::to_string(static_cast<unsigned>(::getpid())));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static auto make_entry(std::uint64_t term, std::uint64_t index,
                       std::vector<std::byte> cmd = {std::byte{0xAB}}) -> log_entry_t {
    log_entry_t e;
    e._term = term;
    e._index = index;
    e._command = std::move(cmd);
    return e;
}

// ── Concept ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_concept_satisfied) {
    static_assert(kythira::persistence_engine<engine_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              log_entry_t, snapshot_t>,
                  "file_persistence_engine must satisfy persistence_engine concept");
}

// ── Term ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_initial_term_is_zero, *boost::unit_test::timeout(10)) {
    TempDir d;
    engine_t eng(d.path);
    BOOST_TEST(eng.load_current_term() == 0u);
}

BOOST_AUTO_TEST_CASE(test_term_round_trip, *boost::unit_test::timeout(10)) {
    TempDir d;
    engine_t eng(d.path);
    eng.save_current_term(42);
    BOOST_TEST(eng.load_current_term() == 42u);
    eng.save_current_term(100);
    BOOST_TEST(eng.load_current_term() == 100u);
}

BOOST_AUTO_TEST_CASE(test_term_survives_reload, *boost::unit_test::timeout(10)) {
    TempDir d;
    {
        engine_t eng(d.path);
        eng.save_current_term(77);
    }
    engine_t eng2(d.path);
    BOOST_TEST(eng2.load_current_term() == 77u);
}

// ── votedFor ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_initial_voted_for_empty, *boost::unit_test::timeout(10)) {
    TempDir d;
    engine_t eng(d.path);
    BOOST_TEST(!eng.load_voted_for().has_value());
}

BOOST_AUTO_TEST_CASE(test_voted_for_round_trip, *boost::unit_test::timeout(10)) {
    TempDir d;
    engine_t eng(d.path);
    eng.save_voted_for(3);
    BOOST_REQUIRE(eng.load_voted_for().has_value());
    BOOST_TEST(*eng.load_voted_for() == 3u);
}

BOOST_AUTO_TEST_CASE(test_voted_for_survives_reload, *boost::unit_test::timeout(10)) {
    TempDir d;
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
    TempDir d;
    engine_t eng(d.path);
    BOOST_TEST(eng.get_last_log_index() == 0u);
    BOOST_TEST(!eng.get_log_entry(1).has_value());
}

BOOST_AUTO_TEST_CASE(test_log_append_and_get, *boost::unit_test::timeout(10)) {
    TempDir d;
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
    TempDir d;
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
    TempDir d;
    // All 256 byte values including null bytes — exercises the base64 codec end-to-end
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
    TempDir d;
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
    TempDir d;
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
    TempDir d;
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
    TempDir d;
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
    TempDir d;
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
    TempDir d;
    engine_t eng(d.path);
    BOOST_TEST(!eng.load_snapshot().has_value());
}

BOOST_AUTO_TEST_CASE(test_snapshot_round_trip, *boost::unit_test::timeout(10)) {
    TempDir d;
    engine_t eng(d.path);
    kythira::cluster_configuration<> cfg{{1, 2, 3}, false, std::nullopt};
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
    TempDir d;
    {
        engine_t eng(d.path);
        kythira::cluster_configuration<> cfg{{1, 2}, false, std::nullopt};
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
    TempDir d;
    engine_t eng(d.path);
    kythira::cluster_configuration<> cfg{{1}, false, std::nullopt};
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
    TempDir d;
    engine_t eng(d.path);
    eng.save_current_term(5);
    eng.append_log_entry(make_entry(1, 1));

    engine_t eng2(std::move(eng));
    BOOST_TEST(eng2.load_current_term() == 5u);
    BOOST_TEST(eng2.get_log_entry(1).has_value());
}
