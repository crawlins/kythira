// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file persistent_state_machine_test.cpp
/// @brief .kiro/specs/persistent-state-machine/ task 1: the concepts, the
///        snapshot manifest, and `memory_kv_engine` (Requirements 1.1-1.5,
///        7.1 and 7.4).
///
/// - The engine conformance suite on `memory_kv_engine` (the RocksDB engine
///   instantiates the same suite in task 5).
/// - `memory_kv_engine`'s held-back durable sequence: what a simulated crash
///   keeps and loses.
/// - `snapshot_manifest` encoding, its bounds, and the file-name checks a
///   follower relies on.
/// - Static checks that every existing state machine still selects the
///   byte-buffer path, and that a type satisfying both kinds selects the
///   persistent one.
/// - A small persistent state machine written against the concepts, to show
///   the contract supports atomic apply, replay after a crash and a durable
///   applied index (the real one is task 2).

#define BOOST_TEST_MODULE persistent_state_machine_test
#include <boost/test/unit_test.hpp>

#include "kv_engine_conformance.hpp"

#include <raft/ca_state_machine.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>
#include <raft/types.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using kythira::column_family;
using kythira::key_range;
using kythira::memory_kv_engine;
using kythira::snapshot_manifest;
using kythira::write_batch;

namespace {

/// A directory under the system temp dir, removed on destruction.
class temp_dir {
public:
    temp_dir() {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        _path = std::filesystem::temp_directory_path() /
                ("kythira-psm-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

struct memory_engine_harness {
    using engine_t = memory_kv_engine;
    temp_dir dir;
    static auto make_engine() -> std::unique_ptr<engine_t> { return std::make_unique<engine_t>(); }
    [[nodiscard]] auto scratch() const -> const std::filesystem::path& { return dir.path(); }
};

}  // namespace

KYTHIRA_KV_ENGINE_CONFORMANCE(memory_engine_harness, memory)

// ---------------------------------------------------------------------------
// memory_kv_engine: held-back durability
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(memory_engine_durability)

BOOST_AUTO_TEST_CASE(without_a_hold_every_write_is_durable_and_a_crash_loses_nothing) {
    memory_kv_engine e;
    const auto s = e.write(write_batch{}.put(column_family::data, "a", "1"));
    BOOST_TEST(e.durable_sequence() == s);
    e.simulate_crash();
    BOOST_TEST(e.get(column_family::data, "a").value_or("") == "1");
    BOOST_TEST(e.latest_sequence() == s);
}

BOOST_AUTO_TEST_CASE(a_crash_loses_exactly_the_writes_after_the_durable_sequence) {
    memory_kv_engine e;
    const auto s1 = e.write(write_batch{}.put(column_family::data, "a", "1"));
    e.hold_durability();
    const auto s2 = e.write(write_batch{}.put(column_family::data, "b", "2"));
    const auto s3 = e.write(
        write_batch{}.put(column_family::data, "a", "1-new").erase(column_family::data, "b"));
    e.write(write_batch{}.put(column_family::meta, "m", "x"));
    BOOST_TEST(e.durable_sequence() == s1);
    BOOST_TEST(e.latest_sequence() > s3);

    e.sync(s2);
    BOOST_TEST(e.durable_sequence() == s2);
    e.simulate_crash();

    // State as of s2: b written, the s3 overwrite and erase and the meta put lost.
    BOOST_TEST(e.latest_sequence() == s2);
    BOOST_TEST(e.get(column_family::data, "a").value_or("") == "1");
    BOOST_TEST(e.get(column_family::data, "b").value_or("") == "2");
    BOOST_TEST(!e.get(column_family::meta, "m").has_value());

    // The hold survives the crash; sequences continue from the recovered one.
    const auto s4 = e.write(write_batch{}.put(column_family::data, "c", "3"));
    BOOST_TEST(s4 == s2 + 1);
    BOOST_TEST(e.durable_sequence() == s2);
    e.simulate_crash();
    BOOST_TEST(!e.get(column_family::data, "c").has_value());
}

BOOST_AUTO_TEST_CASE(release_makes_everything_durable) {
    memory_kv_engine e;
    e.hold_durability();
    const auto s = e.write(write_batch{}.put(column_family::data, "a", "1"));
    BOOST_TEST(e.durable_sequence() == 0U);
    e.release_durability();
    BOOST_TEST(e.durable_sequence() == s);
    e.simulate_crash();
    BOOST_TEST(e.get(column_family::data, "a").value_or("") == "1");
    // Back to durable-on-write.
    const auto s2 = e.write(write_batch{}.put(column_family::data, "b", "2"));
    BOOST_TEST(e.durable_sequence() == s2);
}

BOOST_AUTO_TEST_CASE(ingest_and_drop_are_writes_a_crash_can_lose) {
    temp_dir dir;
    memory_kv_engine src;
    src.write(write_batch{}.put(column_family::data, "g/a", "from-checkpoint"));
    const key_range r{"g/", "g0"};
    const auto m = src.checkpoint(r, dir.path() / "cp");

    memory_kv_engine e;
    e.write(write_batch{}
                .put(column_family::data, "g/a", "old")
                .put(column_family::data, "g/b", "old"));
    e.hold_durability();
    e.ingest(r, dir.path() / "cp", m);
    BOOST_TEST(e.get(column_family::data, "g/a").value_or("") == "from-checkpoint");
    e.simulate_crash();
    BOOST_TEST(e.get(column_family::data, "g/a").value_or("") == "old");
    e.drop(r);
    BOOST_TEST(e.key_count(column_family::data) == 0U);
    e.simulate_crash();
    BOOST_TEST(e.key_count(column_family::data) == 2U);
}

BOOST_AUTO_TEST_CASE(an_empty_batch_takes_no_sequence) {
    memory_kv_engine e;
    const auto s = e.write(write_batch{}.put(column_family::data, "a", "1"));
    BOOST_TEST(e.write(write_batch{}) == s);
    BOOST_TEST(e.latest_sequence() == s);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// snapshot_manifest
// ---------------------------------------------------------------------------

namespace {

auto sample_manifest() -> snapshot_manifest {
    snapshot_manifest m;
    m.engine_format = "kythira-memory-kv/1";
    m.range = key_range{std::string("ns\0a", 4), std::string("ns\0m", 4)};
    for (int i = 0; i < 3; ++i) {
        kythira::checkpoint_file f;
        f.name = "file-" + std::to_string(i) + ".sst";
        f.size = 1000U * static_cast<std::uint64_t>(i);
        f.sha256 = kythira::sha256_of(f.name);
        m.files.push_back(f);
    }
    return m;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(manifest)

BOOST_AUTO_TEST_CASE(encoding_round_trips_with_and_without_a_range_end) {
    auto m = sample_manifest();
    BOOST_TEST((snapshot_manifest::decode(m.encode()) == m));
    BOOST_TEST(m.total_bytes() == 3000U);
    m.range.end.reset();
    m.files.clear();
    BOOST_TEST((snapshot_manifest::decode(m.encode()) == m));
}

BOOST_AUTO_TEST_CASE(the_digest_is_stable_and_covers_every_field) {
    const auto m = sample_manifest();
    BOOST_TEST((m.digest() == sample_manifest().digest()));
    auto changed = m;
    changed.files[1].size += 1;
    BOOST_TEST((changed.digest() != m.digest()));
    changed = m;
    changed.range.end = "other";
    BOOST_TEST((changed.digest() != m.digest()));
    changed = m;
    changed.files[2].sha256[31] ^= 0x80U;
    BOOST_TEST((changed.digest() != m.digest()));
}

BOOST_AUTO_TEST_CASE(every_truncation_of_an_encoding_is_refused) {
    const auto bytes = sample_manifest().encode();
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        BOOST_CHECK_THROW((void)snapshot_manifest::decode(std::string_view(bytes).substr(0, n)),
                          kythira::snapshot_manifest_error);
    }
}

BOOST_AUTO_TEST_CASE(trailing_bytes_bad_magic_and_unknown_versions_are_refused) {
    const auto bytes = sample_manifest().encode();
    BOOST_CHECK_THROW((void)snapshot_manifest::decode(bytes + "x"),
                      kythira::snapshot_manifest_error);
    auto bad_magic = bytes;
    bad_magic[0] = 'X';
    BOOST_CHECK_THROW((void)snapshot_manifest::decode(bad_magic), kythira::snapshot_manifest_error);
    auto bad_version = bytes;
    bad_version[4] = '\x02';
    BOOST_CHECK_THROW((void)snapshot_manifest::decode(bad_version),
                      kythira::snapshot_manifest_error);
}

BOOST_AUTO_TEST_CASE(a_file_count_the_input_cannot_hold_is_refused_before_allocating) {
    // Magic, version, format "f", empty start, no end, count 0xFFFFFFFF.
    std::string bytes(
        "KYMF\x01\x00\x01\x00"
        "f"
        "\x00\x00\x00\x00"
        "\x00"
        "\xff\xff\xff\xff",
        18);
    BOOST_CHECK_THROW((void)snapshot_manifest::decode(bytes), kythira::snapshot_manifest_error);
}

BOOST_AUTO_TEST_CASE(file_names_that_could_leave_the_staging_directory_are_refused) {
    for (const std::string& bad :
         {std::string(""), std::string("."), std::string(".."), std::string("../escape"),
          std::string("a/b"), std::string("a\\b"), std::string("/abs"), std::string("nul\0byte", 8),
          std::string(256, 'n')}) {
        BOOST_TEST(!snapshot_manifest::valid_file_name(bad));
        auto m = sample_manifest();
        m.files[0].name = bad;
        BOOST_CHECK_THROW((void)m.encode(), kythira::snapshot_manifest_error);
    }
    BOOST_TEST(snapshot_manifest::valid_file_name("000123.sst"));
    BOOST_TEST(snapshot_manifest::valid_file_name("..hidden"));
    BOOST_TEST(snapshot_manifest::valid_file_name(std::string(255, 'n')));

    // A name smuggled past encode() is still refused on decode.
    auto m = sample_manifest();
    auto bytes = m.encode();
    const auto at = bytes.find("file-0.sst");
    BOOST_REQUIRE(at != std::string::npos);
    bytes.replace(at, 10, "../../x.ss");
    BOOST_CHECK_THROW((void)snapshot_manifest::decode(bytes), kythira::snapshot_manifest_error);
}

BOOST_AUTO_TEST_CASE(duplicate_names_and_oversized_fields_are_refused) {
    auto dup = sample_manifest();
    dup.files[1].name = dup.files[0].name;
    BOOST_CHECK_THROW(dup.validate(), kythira::snapshot_manifest_error);

    auto no_format = sample_manifest();
    no_format.engine_format.clear();
    BOOST_CHECK_THROW(no_format.validate(), kythira::snapshot_manifest_error);

    auto long_format = sample_manifest();
    long_format.engine_format.assign(snapshot_manifest::k_max_format_bytes + 1, 'f');
    BOOST_CHECK_THROW(long_format.validate(), kythira::snapshot_manifest_error);

    auto long_key = sample_manifest();
    long_key.range.start.assign(snapshot_manifest::k_max_range_key_bytes + 1, 'k');
    BOOST_CHECK_THROW(long_key.validate(), kythira::snapshot_manifest_error);

    auto many = sample_manifest();
    many.files.resize(snapshot_manifest::k_max_files + 1);
    for (std::size_t i = 0; i < many.files.size(); ++i) {
        many.files[i].name = std::to_string(i);
    }
    BOOST_CHECK_THROW(many.validate(), kythira::snapshot_manifest_error);
}

BOOST_AUTO_TEST_CASE(key_range_containment) {
    const key_range r{"b", "d"};
    BOOST_TEST(r.contains("b"));
    BOOST_TEST(r.contains("c\xff"));
    BOOST_TEST(!r.contains("d"));
    BOOST_TEST(!r.contains("a"));
    BOOST_TEST((key_range{}.contains(std::string("\0", 1))));
    BOOST_TEST(r.contains(key_range{"b", "c"}));
    BOOST_TEST(!r.contains(key_range{"b", std::nullopt}));
    const key_range open_ended{"b", std::nullopt};
    BOOST_TEST(open_ended.contains(key_range{"c", "z"}));
    BOOST_TEST(!r.contains(key_range{"a", "c"}));
    BOOST_TEST((key_range{"d", "d"}.empty()));
    BOOST_TEST(!open_ended.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Concept selection
// ---------------------------------------------------------------------------

namespace {

/// A persistent state machine written only against the concepts: commands
/// are "key=value", the applied index lives in the meta column family next
/// to the group's range start, and `durable_applied_index` maps engine
/// sequences back to log indices, as the design's "No fsync on apply"
/// section describes. Task 2's `persistent_kv_state_machine` is the real
/// one.
class toy_persistent_sm {
public:
    using engine_type = memory_kv_engine;

    auto open(engine_type& engine, const key_range& range) -> void {
        _engine = &engine;
        _range = range;
        const auto stored = engine.get(column_family::meta, meta_key());
        _applied = stored.has_value() ? std::stoull(*stored) : 0U;
        // What open() reads back was recovered, so it is durable.
        _seqs.clear();
        _durable_floor = _applied;
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        if (index <= _applied) {
            return {};  // replay of an entry already in the engine
        }
        const std::string text(reinterpret_cast<const char*>(command.data()), command.size());
        const auto eq = text.find('=');
        write_batch b;
        b.put(column_family::data, _range.start + text.substr(0, eq), text.substr(eq + 1));
        b.put(column_family::meta, meta_key(), std::to_string(index));
        _seqs.emplace_back(index, _engine->write(b));
        _applied = index;
        return {};
    }

    [[nodiscard]] auto applied_index() const -> std::uint64_t { return _applied; }

    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t {
        const auto durable = _engine->durable_sequence();
        auto best = _durable_floor;
        for (const auto& [index, seq] : _seqs) {
            if (seq <= durable) {
                best = index;
            }
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> snapshot_manifest {
        return _engine->checkpoint(_range, dir);
    }

    auto ingest(const std::filesystem::path& dir, const snapshot_manifest& manifest,
                std::uint64_t index) -> void {
        _engine->ingest(_range, dir, manifest);
        _seqs.emplace_back(index, _engine->write(write_batch{}.put(column_family::meta, meta_key(),
                                                                   std::to_string(index))));
        _applied = index;
    }

    auto drop() -> void {
        _engine->drop(_range);
        _engine->write(write_batch{}.erase(column_family::meta, meta_key()));
        _applied = 0;
        _seqs.clear();
        _durable_floor = 0;
    }

    auto split_at(const std::vector<std::string>& keys) -> std::vector<key_range> {
        std::vector<key_range> children;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            children.push_back(key_range{keys[i], i + 1 < keys.size()
                                                      ? std::optional<std::string>(keys[i + 1])
                                                      : _range.end});
        }
        if (!keys.empty()) {
            _range.end = keys.front();
        }
        return children;
    }

    auto adopt(const key_range& range) -> void { _range.end = range.end; }

    [[nodiscard]] auto value(const std::string& key) const -> std::optional<std::string> {
        return _engine->get(column_family::data, _range.start + key);
    }

private:
    [[nodiscard]] auto meta_key() const -> std::string { return "applied/" + _range.start; }

    engine_type* _engine = nullptr;
    key_range _range;
    std::uint64_t _applied = 0;
    std::uint64_t _durable_floor = 0;
    std::vector<std::pair<std::uint64_t, kythira::engine_sequence>> _seqs;
};

/// Offers the byte-buffer pair as well; the persistent path must win.
class both_kinds_sm : public toy_persistent_sm {
public:
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> { return {}; }
    auto restore_from_snapshot(const std::vector<std::byte>&, std::uint64_t) -> void {}
};

/// Persistent but without split_at / adopt.
class unsplittable_sm {
public:
    using engine_type = memory_kv_engine;
    auto open(engine_type&, const key_range&) -> void {}
    auto apply(const std::vector<std::byte>&, std::uint64_t) -> std::vector<std::byte> {
        return {};
    }
    [[nodiscard]] auto applied_index() const -> std::uint64_t { return 0; }
    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t { return 0; }
    auto checkpoint(const std::filesystem::path&) -> snapshot_manifest { return {}; }
    auto ingest(const std::filesystem::path&, const snapshot_manifest&, std::uint64_t) -> void {}
    auto drop() -> void {}
};

using kythira::byte_buffer_state_machine;
using kythira::persistent_splittable_state_machine;
using kythira::persistent_state_machine;
using kythira::state_machine_storage;
using kythira::storage_kind_of;

template<typename SM>
constexpr bool selects_byte_buffer =
    byte_buffer_state_machine<SM, std::uint64_t> && !persistent_state_machine<SM> &&
    storage_kind_of<SM> == state_machine_storage::byte_buffer;

// Requirement 1.4: every existing state machine keeps the byte-buffer path.
// The Redis and build-cache state machines are checked in
// services/redis/tests/cache_kv_state_machine_unit_test.cpp, with the module.
/// The transaction wrapper needs a partitioner; any will do for a type check.
struct whole_command_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        return {reinterpret_cast<const char*>(command.data()), command.size()};
    }
};

static_assert(selects_byte_buffer<kythira::test_key_value_state_machine<std::uint64_t>>);
static_assert(selects_byte_buffer<kythira::capacity_ledger_state_machine<std::uint64_t>>);
static_assert(selects_byte_buffer<raft::testing::ca_state_machine>);
static_assert(
    selects_byte_buffer<
        kythira::transactional_state_machine<kythira::test_key_value_state_machine<std::uint64_t>,
                                             std::string, whole_command_partitioner>>);

// The new kinds.
static_assert(kythira::kv_engine<memory_kv_engine>);
static_assert(persistent_state_machine<toy_persistent_sm>);
static_assert(persistent_splittable_state_machine<toy_persistent_sm>);
static_assert(!kythira::state_machine<toy_persistent_sm, std::uint64_t>);
static_assert(storage_kind_of<toy_persistent_sm> == state_machine_storage::persistent);
static_assert(persistent_state_machine<unsplittable_sm>);
static_assert(!persistent_splittable_state_machine<unsplittable_sm>);

// Requirement 1.5: a type that is both selects the persistent path.
static_assert(kythira::state_machine<both_kinds_sm, std::uint64_t>);
static_assert(persistent_state_machine<both_kinds_sm>);
static_assert(storage_kind_of<both_kinds_sm> == state_machine_storage::persistent);
static_assert(!byte_buffer_state_machine<both_kinds_sm, std::uint64_t>);

// A byte-buffer state machine is not a persistent one, and the engine is
// not a state machine.
static_assert(!persistent_state_machine<kythira::test_key_value_state_machine<std::uint64_t>>);
static_assert(!kythira::kv_engine<toy_persistent_sm>);

auto cmd(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(contract)

BOOST_AUTO_TEST_CASE(selection_is_decided_at_compile_time) {
    // The static_asserts above are the test; this case records that they ran.
    BOOST_TEST((storage_kind_of<toy_persistent_sm> == state_machine_storage::persistent));
    BOOST_TEST((storage_kind_of<kythira::test_key_value_state_machine<std::uint64_t>> ==
                state_machine_storage::byte_buffer));
}

BOOST_AUTO_TEST_CASE(apply_replay_and_the_durable_applied_index_survive_a_crash) {
    memory_kv_engine engine;
    const key_range range{"g1/", "g10"};
    toy_persistent_sm sm;
    sm.open(engine, range);
    sm.apply(cmd("a=1"), 1);
    sm.apply(cmd("b=2"), 2);
    BOOST_TEST(sm.durable_applied_index() == 2U);

    engine.hold_durability();
    sm.apply(cmd("a=3"), 3);
    sm.apply(cmd("c=4"), 4);
    BOOST_TEST(sm.applied_index() == 4U);
    BOOST_TEST(sm.durable_applied_index() == 2U);  // compaction would stop here

    engine.sync(engine.latest_sequence() - 1);  // entry 3's batch, not 4's
    BOOST_TEST(sm.durable_applied_index() == 3U);

    engine.simulate_crash();
    toy_persistent_sm restarted;
    restarted.open(engine, range);
    // Effects and applied index were one batch, so they agree after the crash.
    BOOST_TEST(restarted.applied_index() == 3U);
    BOOST_TEST(restarted.value("a").value_or("") == "3");
    BOOST_TEST(!restarted.value("c").has_value());

    // The node replays from the log; entries at or below the applied index
    // are no-ops, entry 4 applies again.
    for (std::uint64_t i = 1; i <= 4; ++i) {
        restarted.apply(cmd(i == 1 ? "a=1" : i == 2 ? "b=2" : i == 3 ? "a=3" : "c=4"), i);
    }
    BOOST_TEST(restarted.applied_index() == 4U);
    BOOST_TEST(restarted.value("a").value_or("") == "3");
    BOOST_TEST(restarted.value("c").value_or("") == "4");
}

BOOST_AUTO_TEST_CASE(a_checkpoint_moves_a_group_to_another_engine) {
    temp_dir dir;
    memory_kv_engine leader_engine;
    memory_kv_engine follower_engine;
    const key_range range{"g1/", "g10"};
    toy_persistent_sm leader;
    leader.open(leader_engine, range);
    for (std::uint64_t i = 1; i <= 50; ++i) {
        leader.apply(cmd("k" + std::to_string(i) + "=" + std::to_string(i * i)), i);
    }
    const auto m = leader.checkpoint(dir.path() / "snap");
    const auto encoded = m.encode();  // what the wire would carry

    toy_persistent_sm follower;
    follower.open(follower_engine, range);
    follower.apply(cmd("stale=1"), 1);
    follower.ingest(dir.path() / "snap", snapshot_manifest::decode(encoded), 50);
    BOOST_TEST(follower.applied_index() == 50U);
    BOOST_TEST(follower.value("k7").value_or("") == "49");
    BOOST_TEST(!follower.value("stale").has_value());
}

BOOST_AUTO_TEST_CASE(split_and_adopt_change_ranges_without_touching_keys) {
    memory_kv_engine engine;
    toy_persistent_sm sm;
    sm.open(engine, key_range{"", std::nullopt});
    sm.apply(cmd("a=1"), 1);
    sm.apply(cmd("m=2"), 2);
    const auto keys_before = engine.key_count(column_family::data);
    const auto seq_before = engine.latest_sequence();

    const auto children = sm.split_at({"h", "p"});
    BOOST_REQUIRE_EQUAL(children.size(), 2U);
    BOOST_TEST((children[0] == key_range{"h", "p"}));
    BOOST_TEST((children[1] == key_range{"p", std::nullopt}));
    sm.adopt(children[0]);
    BOOST_TEST(engine.key_count(column_family::data) == keys_before);
    BOOST_TEST(engine.latest_sequence() == seq_before);
}

BOOST_AUTO_TEST_SUITE_END()
