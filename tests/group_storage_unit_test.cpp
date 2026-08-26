// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file group_storage_unit_test.cpp
/// @brief Unit tests for per-group durable state (task 7) and the tombstone
///        set (task 8) of `.kiro/specs/multi-raft/`.
///
/// The isolation cases use a real `file_persistence_engine` over a real
/// temporary directory rather than a mock, because the property under test is
/// precisely that two groups' *paths* do not collide — a mock would be
/// asserting the test's own idea of the layout back at itself.
///
/// The batching case does use a counting mock, for the opposite reason: the
/// claim is "N appends produce ONE durability barrier", and a barrier is not
/// observable from the filesystem after the fact.

#define BOOST_TEST_MODULE group_storage_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/file_persistence.hpp>
#include <raft/group_storage.hpp>
#include <raft/persistence.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using kythira::batched_persistence_engine;
using kythira::destroy_group_data;
using kythira::file_persistence_engine;
using kythira::group_data_dir;
using kythira::group_key_prefix;
using kythira::group_path_component;
using kythira::group_scoped_persistence;
using kythira::log_entry;
using kythira::memory_persistence_engine;
using kythira::tombstone_reason;
using kythira::tombstone_set;

namespace {

using file_engine = file_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
using memory_engine = memory_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
using entry_type_alias = log_entry<std::uint64_t, std::uint64_t>;

auto entry(std::uint64_t term, std::uint64_t index) -> entry_type_alias {
    return entry_type_alias{._term = term,
                            ._index = index,
                            ._command = {std::byte{static_cast<unsigned char>(index)}},
                            ._type = kythira::entry_type::normal};
}

/// RAII temporary directory. Named after the test so a leftover on a failed run
/// says which one leaked it.
class temp_dir {
public:
    explicit temp_dir(const std::string& label)
        : _path(std::filesystem::temp_directory_path() /
                ("kythira-group-storage-" + label + "-" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

/// A store that counts durability barriers, which the filesystem cannot report.
class counting_store {
public:
    using log_entry_t = entry_type_alias;
    using snapshot_t = kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t>;

    auto save_current_term(std::uint64_t term) -> void { _term = term; }
    auto load_current_term() -> std::uint64_t { return _term; }
    auto save_voted_for(std::uint64_t node) -> void { _voted_for = node; }
    auto load_voted_for() -> std::optional<std::uint64_t> { return _voted_for; }

    auto append_log_entry(const log_entry_t& e) -> void {
        ++_appends;
        _log[e.index()] = e;
    }
    auto get_log_entry(std::uint64_t i) -> std::optional<log_entry_t> {
        auto it = _log.find(i);
        return it == _log.end() ? std::nullopt : std::optional{it->second};
    }
    auto get_log_entries(std::uint64_t a, std::uint64_t b) -> std::vector<log_entry_t> {
        std::vector<log_entry_t> out;
        for (auto i = a; i <= b; ++i) {
            if (auto e = get_log_entry(i)) {
                out.push_back(*e);
            }
        }
        return out;
    }
    auto get_last_log_index() -> std::uint64_t {
        std::uint64_t max = 0;
        for (const auto& [i, _] : _log) {
            max = i > max ? i : max;
        }
        return max;
    }
    auto truncate_log(std::uint64_t i) -> void {
        std::erase_if(_log, [i](const auto& kv) { return kv.first >= i; });
    }
    auto save_snapshot(const snapshot_t& s) -> void { _snapshot = s; }
    auto load_snapshot() -> std::optional<snapshot_t> { return _snapshot; }
    auto delete_log_entries_before(std::uint64_t i) -> void {
        std::erase_if(_log, [i](const auto& kv) { return kv.first < i; });
    }

    auto begin_batch() -> void { ++_begins; }
    auto commit_batch() -> void { ++_barriers; }
    auto abort_batch() -> void { ++_aborts; }

    int _appends{0};
    int _begins{0};
    int _barriers{0};
    int _aborts{0};

private:
    std::uint64_t _term{0};
    std::optional<std::uint64_t> _voted_for;
    std::unordered_map<std::uint64_t, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(group_storage_unit)

// ── naming ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_group_path_component_cannot_escape_its_own_subtree) {
    BOOST_CHECK_EQUAL(group_path_component<std::uint64_t>(42), "42");
    BOOST_CHECK_EQUAL(group_path_component<std::string>("tenant-a"), "tenant-a");
    // A group id is chosen by the application; a slash in it must not be
    // allowed to place the group's data outside `groups/<id>`.
    BOOST_CHECK_EQUAL(group_path_component<std::string>("../../etc"), ".._.._etc");
    BOOST_CHECK_EQUAL(group_path_component<std::string>(""), "_");
}

BOOST_AUTO_TEST_CASE(group_locations_nest_under_a_groups_level) {
    const std::filesystem::path root{"/var/lib/kythira"};
    BOOST_CHECK_EQUAL(group_data_dir<std::uint64_t>(root, 7).string(), "/var/lib/kythira/groups/7");
    BOOST_CHECK_EQUAL(group_key_prefix<std::uint64_t>("cluster-a", 7), "cluster-a/groups/7");
    BOOST_CHECK_EQUAL(group_key_prefix<std::uint64_t>("", 7), "groups/7");
}

// ── isolation ────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(two_groups_over_one_directory_tree_do_not_observe_each_other) {
    const temp_dir root{"isolation"};

    group_scoped_persistence<file_engine, std::uint64_t> g1{
        1, file_engine{group_data_dir<std::uint64_t>(root.path(), 1)}};
    group_scoped_persistence<file_engine, std::uint64_t> g2{
        2, file_engine{group_data_dir<std::uint64_t>(root.path(), 2)}};

    g1.save_current_term(11);
    g1.save_voted_for(std::uint64_t{101});
    g1.append_log_entry(entry(11, 1));
    g1.append_log_entry(entry(11, 2));

    g2.save_current_term(22);
    g2.save_voted_for(std::uint64_t{202});
    g2.append_log_entry(entry(22, 1));

    BOOST_CHECK_EQUAL(g1.load_current_term(), 11u);
    BOOST_CHECK_EQUAL(g2.load_current_term(), 22u);
    BOOST_CHECK_EQUAL(g1.load_voted_for().value(), 101u);
    BOOST_CHECK_EQUAL(g2.load_voted_for().value(), 202u);
    BOOST_CHECK_EQUAL(g1.get_last_log_index(), 2u);
    BOOST_CHECK_EQUAL(g2.get_last_log_index(), 1u);

    kythira::snapshot<std::uint64_t, std::uint64_t, std::uint64_t> snap{};
    snap._last_included_index = 2;
    snap._last_included_term = 11;
    g1.save_snapshot(snap);
    BOOST_CHECK(g1.load_snapshot().has_value());
    BOOST_CHECK(!g2.load_snapshot().has_value());
}

BOOST_AUTO_TEST_CASE(destroying_one_groups_tree_leaves_the_other_intact) {
    const temp_dir root{"destroy"};
    {
        group_scoped_persistence<file_engine, std::uint64_t> g1{
            1, file_engine{group_data_dir<std::uint64_t>(root.path(), 1)}};
        group_scoped_persistence<file_engine, std::uint64_t> g2{
            2, file_engine{group_data_dir<std::uint64_t>(root.path(), 2)}};
        g1.save_current_term(11);
        g2.save_current_term(22);
    }

    BOOST_CHECK(destroy_group_data<std::uint64_t>(root.path(), 1));
    BOOST_CHECK(!std::filesystem::exists(group_data_dir<std::uint64_t>(root.path(), 1)));
    BOOST_CHECK(std::filesystem::exists(group_data_dir<std::uint64_t>(root.path(), 2)));

    // Re-opening group 2 still finds its term; re-opening group 1 starts fresh.
    file_engine reopened_2{group_data_dir<std::uint64_t>(root.path(), 2)};
    BOOST_CHECK_EQUAL(reopened_2.load_current_term(), 22u);
    file_engine reopened_1{group_data_dir<std::uint64_t>(root.path(), 1)};
    BOOST_CHECK_EQUAL(reopened_1.load_current_term(), 0u);

    // Destroying a group with no data is not an error, and reports nothing removed.
    BOOST_CHECK(!destroy_group_data<std::uint64_t>(root.path(), 99));
}

// ── batching ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_batching_extension_is_detected_structurally) {
    static_assert(batched_persistence_engine<counting_store>);
    static_assert(batched_persistence_engine<file_engine>);
    // memory_persistence_engine has no batching, and must not be made to
    // pretend otherwise — the tick's `if constexpr` is what keeps it working.
    static_assert(!batched_persistence_engine<memory_engine>);

    // And the property propagates through the group-scoped wrapper in both
    // directions, which is the wrapper's main reason to exist.
    static_assert(
        batched_persistence_engine<group_scoped_persistence<counting_store, std::uint64_t>>);
    static_assert(
        !batched_persistence_engine<group_scoped_persistence<memory_engine, std::uint64_t>>);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(n_appends_in_one_batch_produce_one_barrier) {
    group_scoped_persistence<counting_store, std::uint64_t> store{1, counting_store{}};

    store.begin_batch();
    for (std::uint64_t i = 1; i <= 10; ++i) {
        store.append_log_entry(entry(1, i));
    }
    store.commit_batch();

    BOOST_CHECK_EQUAL(store.engine()._appends, 10);
    BOOST_CHECK_EQUAL(store.engine()._begins, 1);
    BOOST_CHECK_EQUAL(store.engine()._barriers, 1);
    BOOST_CHECK_EQUAL(store.engine()._aborts, 0);
}

BOOST_AUTO_TEST_CASE(a_committed_batch_survives_a_reopen) {
    const temp_dir root{"batch-commit"};
    const auto dir = group_data_dir<std::uint64_t>(root.path(), 1);
    {
        file_engine store{dir};
        store.begin_batch();
        BOOST_CHECK(store.batch_open());
        store.append_log_entry(entry(3, 1));
        store.append_log_entry(entry(3, 2));
        store.append_log_entry(entry(3, 3));
        store.commit_batch();
        BOOST_CHECK(!store.batch_open());
        BOOST_CHECK_EQUAL(store.get_last_log_index(), 3u);
    }
    file_engine reopened{dir};
    BOOST_CHECK_EQUAL(reopened.get_last_log_index(), 3u);
    BOOST_REQUIRE(reopened.get_log_entry(2).has_value());
    BOOST_CHECK_EQUAL(reopened.get_log_entry(2)->term(), 3u);
}

BOOST_AUTO_TEST_CASE(an_aborted_batch_leaves_neither_memory_nor_disk_changed) {
    const temp_dir root{"batch-abort"};
    const auto dir = group_data_dir<std::uint64_t>(root.path(), 1);
    {
        file_engine store{dir};
        store.append_log_entry(entry(1, 1));  // before the batch, so it must survive

        store.begin_batch();
        store.append_log_entry(entry(2, 1));  // overwrites index 1 inside the batch
        store.append_log_entry(entry(2, 2));
        store.abort_batch();

        BOOST_CHECK_EQUAL(store.get_last_log_index(), 1u);
        BOOST_REQUIRE(store.get_log_entry(1).has_value());
        // Rolled back to the PRE-batch value, not to the intermediate one.
        BOOST_CHECK_EQUAL(store.get_log_entry(1)->term(), 1u);
        BOOST_CHECK(!store.get_log_entry(2).has_value());
    }
    file_engine reopened{dir};
    BOOST_CHECK_EQUAL(reopened.get_last_log_index(), 1u);
    BOOST_CHECK_EQUAL(reopened.get_log_entry(1)->term(), 1u);
}

BOOST_AUTO_TEST_CASE(nested_and_unmatched_batch_calls_are_refused) {
    const temp_dir root{"batch-misuse"};
    file_engine store{group_data_dir<std::uint64_t>(root.path(), 1)};

    BOOST_CHECK_THROW(store.commit_batch(), std::runtime_error);
    BOOST_CHECK_THROW(store.abort_batch(), std::runtime_error);
    store.begin_batch();
    BOOST_CHECK_THROW(store.begin_batch(), std::runtime_error);
    store.commit_batch();
}

BOOST_AUTO_TEST_CASE(term_and_vote_stay_synchronous_inside_a_batch) {
    // Raft requires currentTerm and votedFor durable BEFORE the node responds
    // to the RPC that changed them. Deferring them into a batch to save a
    // syscall would break the algorithm's own ordering requirement, so the
    // batch covers appends only.
    const temp_dir root{"batch-term"};
    const auto dir = group_data_dir<std::uint64_t>(root.path(), 1);
    {
        file_engine store{dir};
        store.begin_batch();
        store.save_current_term(7);
        store.save_voted_for(std::uint64_t{3});
        store.append_log_entry(entry(7, 1));
        // Deliberately abandoned: the process "crashes" here.
    }
    file_engine reopened{dir};
    BOOST_CHECK_EQUAL(reopened.load_current_term(), 7u);
    BOOST_CHECK_EQUAL(reopened.load_voted_for().value(), 3u);
    // The uncommitted append did not reach the disk.
    BOOST_CHECK_EQUAL(reopened.get_last_log_index(), 0u);
}

// ── tombstones ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_tombstone_records_the_reason_and_the_time) {
    tombstone_set<std::uint64_t> dead;
    const auto t0 = std::chrono::system_clock::time_point{std::chrono::milliseconds{1'000'000}};

    BOOST_CHECK(dead.empty());
    dead.insert(7, tombstone_reason::merged_away, t0);
    BOOST_CHECK(dead.contains(7));
    BOOST_CHECK(!dead.contains(8));
    BOOST_CHECK_EQUAL(dead.size(), 1u);

    const auto rec = dead.find(7);
    BOOST_REQUIRE(rec.has_value());
    BOOST_CHECK(rec->_reason == tombstone_reason::merged_away);
    BOOST_CHECK_EQUAL(rec->_destroyed_at_ms, 1'000'000);
}

BOOST_AUTO_TEST_CASE(reinserting_a_tombstone_keeps_the_earliest_record) {
    // A redelivered destroy must not push the garbage-collection horizon out,
    // or a group could stay tombstoned indefinitely under a retry loop.
    tombstone_set<std::uint64_t> dead;
    const std::chrono::system_clock::time_point early{std::chrono::milliseconds{1000}};
    const std::chrono::system_clock::time_point late{std::chrono::milliseconds{9000}};

    dead.insert(7, tombstone_reason::merged_away, late);
    dead.insert(7, tombstone_reason::merged_away, early);
    BOOST_CHECK_EQUAL(dead.find(7)->_destroyed_at_ms, 1000);

    dead.insert(7, tombstone_reason::merged_away, late);
    BOOST_CHECK_EQUAL(dead.find(7)->_destroyed_at_ms, 1000);
}

BOOST_AUTO_TEST_CASE(gc_removes_an_entry_past_the_horizon_and_not_before) {
    tombstone_set<std::uint64_t> dead;
    const std::chrono::system_clock::time_point t0{std::chrono::milliseconds{0}};
    dead.insert(1, tombstone_reason::merged_away, t0);
    dead.insert(2, tombstone_reason::replica_removed, t0 + std::chrono::milliseconds{5000});

    const auto horizon = std::chrono::milliseconds{1000};

    // At t = 500 with a 1 s horizon, nothing is old enough yet.
    BOOST_CHECK_EQUAL(dead.gc(t0 + std::chrono::milliseconds{500}, horizon), 0u);
    BOOST_CHECK_EQUAL(dead.size(), 2u);

    // At t = 2000, group 1 (destroyed at 0) is past it; group 2 (at 5000) is not.
    BOOST_CHECK_EQUAL(dead.gc(t0 + std::chrono::milliseconds{2000}, horizon), 1u);
    BOOST_CHECK(!dead.contains(1));
    BOOST_CHECK(dead.contains(2));

    BOOST_CHECK_EQUAL(dead.gc(t0 + std::chrono::milliseconds{7000}, horizon), 1u);
    BOOST_CHECK(dead.empty());
}

BOOST_AUTO_TEST_CASE(a_tombstone_survives_a_simulated_restart) {
    const temp_dir root{"tombstones"};
    const auto path = root.path() / "tombstones";
    const std::chrono::system_clock::time_point t0{std::chrono::milliseconds{123456}};

    {
        tombstone_set<std::uint64_t> dead;
        dead.insert(7, tombstone_reason::merged_away, t0);
        dead.insert(8, tombstone_reason::replica_removed, t0);
        dead.insert(9, tombstone_reason::admin, t0);
        dead.save_to_file(path);
    }

    const auto reloaded = tombstone_set<std::uint64_t>::load_from_file(path);
    BOOST_CHECK_EQUAL(reloaded.size(), 3u);
    BOOST_CHECK(reloaded.contains(7));
    BOOST_CHECK(reloaded.find(8)->_reason == tombstone_reason::replica_removed);
    BOOST_CHECK(reloaded.find(9)->_reason == tombstone_reason::admin);
    BOOST_CHECK_EQUAL(reloaded.find(7)->_destroyed_at_ms, 123456);
}

BOOST_AUTO_TEST_CASE(a_missing_tombstone_file_loads_as_an_empty_set) {
    const temp_dir root{"tombstones-missing"};
    const auto reloaded =
        tombstone_set<std::uint64_t>::load_from_file(root.path() / "does-not-exist");
    BOOST_CHECK(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(a_truncated_tombstone_file_loses_records_rather_than_the_node) {
    // A node that refuses to start because its tombstone file was truncated by
    // a crash is worse than one that loses a tombstone: the lost tombstone
    // re-opens a resurrection window the epoch check still closes, while the
    // refusal costs the whole replica.
    const auto good = tombstone_set<std::uint64_t>::from_text("7\t0\t1000\n8\t1\t2000\n");
    BOOST_CHECK_EQUAL(good.size(), 2u);

    const auto truncated = tombstone_set<std::uint64_t>::from_text("7\t0\t1000\n8\t1");
    BOOST_CHECK_EQUAL(truncated.size(), 1u);
    BOOST_CHECK(truncated.contains(7));

    const auto garbage = tombstone_set<std::uint64_t>::from_text("not a record\n\n7\t0\t1000\n");
    BOOST_CHECK_EQUAL(garbage.size(), 1u);
    BOOST_CHECK(garbage.contains(7));
}

BOOST_AUTO_TEST_CASE(string_group_ids_round_trip_through_the_tombstone_file) {
    tombstone_set<std::string> dead;
    dead.insert("tenant-a/shard-04", tombstone_reason::merged_away,
                std::chrono::system_clock::time_point{std::chrono::milliseconds{5}});
    const auto reloaded = tombstone_set<std::string>::from_text(dead.to_text());
    BOOST_CHECK(reloaded.contains("tenant-a/shard-04"));
}

BOOST_AUTO_TEST_SUITE_END()
