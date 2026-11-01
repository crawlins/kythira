// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_chunk_store_unit_test.cpp
/// @brief `cache_chunk_store` and `chunk_refcounts`
///        (.kiro/specs/disk-backed-build-cache/ task 2; Requirements 2.1, 2.5,
///        3.3, 6.7, 6.8 and 10.1).

#define BOOST_TEST_MODULE cache_chunk_store_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/cache_chunk_store.hpp>

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

#include <unistd.h>

namespace fs = std::filesystem;
using namespace kythira;

namespace {

#ifdef FIU_ENABLE
/// libfiu needs `fiu_init` before any fault point is armed.
struct fiu_init_fixture {
    fiu_init_fixture() { ::fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(fiu_init_fixture);
#endif

/// A fresh directory per test case, removed afterwards.
struct temp_dir {
    fs::path _path;
    temp_dir() {
        static std::atomic<int> counter{0};
        _path = fs::temp_directory_path() / ("kythira_chunk_store_" + std::to_string(::getpid()) +
                                             "_" + std::to_string(counter.fetch_add(1)));
        fs::remove_all(_path);
        fs::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
};

/// Deterministic bytes, distinct per seed.
auto make_bytes(std::size_t size, std::uint32_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> out(size);
    std::uint32_t x = seed * 2654435761U + 1;
    for (auto& b : out) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<std::byte>(x);
    }
    // Make the first bytes carry the seed outright, so that two seeds never
    // produce the same chunk even at size 0..4 (where the generator could).
    for (std::size_t i = 0; i < std::min<std::size_t>(size, 4); ++i) {
        out[i] = static_cast<std::byte>(seed >> (8U * i));
    }
    return out;
}

auto options_for(const fs::path& dir) -> cache_chunk_store_options {
    cache_chunk_store_options o;
    o._data_dir = dir;
    o._pack_threshold_bytes = 1024;
    o._pack_max_bytes = std::size_t{64} * 1024;
    return o;
}

/// Regular files anywhere under `dir`.
auto count_files(const fs::path& dir) -> std::size_t {
    std::size_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        n += e.is_regular_file() ? 1 : 0;
    }
    return n;
}

auto pack_files(const fs::path& dir) -> std::vector<fs::path> {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir / "packs")) {
        const auto name = e.path().filename().string();
        if (name.starts_with("pack.") && !name.ends_with(".dead")) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(hex_round_trip, *boost::unit_test::timeout(10)) {
    const auto sha = sha256_of(make_bytes(10, 1));
    const auto hex = to_hex(sha);
    BOOST_CHECK_EQUAL(hex.size(), 64U);
    BOOST_CHECK(chunk_sha_from_hex(hex) == sha);
    BOOST_CHECK(!chunk_sha_from_hex(hex.substr(1)));
    BOOST_CHECK(!chunk_sha_from_hex(std::string(64, 'g')));
    // Known vector: SHA-256 of the empty string.
    BOOST_CHECK_EQUAL(to_hex(sha256_of({})),
                      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

BOOST_AUTO_TEST_CASE(put_read_remove_loose_and_packed, *boost::unit_test::timeout(30)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    const auto small = make_bytes(100, 1);
    const auto large = make_bytes(4096, 2);
    const auto small_sha = sha256_of(small);
    const auto large_sha = sha256_of(large);

    BOOST_CHECK(store.put(small_sha, small));
    BOOST_CHECK(store.put(large_sha, large));
    // A replayed put is a no-op.
    BOOST_CHECK(!store.put(small_sha, small));
    BOOST_CHECK(!store.put(large_sha, large));

    BOOST_CHECK(store.contains(small_sha));
    BOOST_CHECK(store.contains(large_sha));
    BOOST_CHECK(fs::exists(store.loose_path(large_sha)));
    BOOST_CHECK(!fs::exists(store.loose_path(small_sha)));
    BOOST_CHECK(store.read(small_sha) == small);
    BOOST_CHECK(store.read(large_sha) == large);
    BOOST_CHECK(store.verify(small_sha));
    BOOST_CHECK(store.verify(large_sha));

    auto s = store.stats();
    BOOST_CHECK_EQUAL(s._loose_files, 1U);
    BOOST_CHECK_EQUAL(s._loose_bytes, 4096U);
    BOOST_CHECK_EQUAL(s._packed_chunks, 1U);
    BOOST_CHECK_EQUAL(s._pack_files, 1U);
    BOOST_CHECK_EQUAL(s._pack_bytes, 100U + cache_chunk_store::pack_record_overhead);

    BOOST_CHECK(store.remove(small_sha));
    BOOST_CHECK(store.remove(large_sha));
    BOOST_CHECK(!store.remove(large_sha));
    BOOST_CHECK(!store.contains(small_sha));
    BOOST_CHECK(!store.contains(large_sha));
    BOOST_CHECK(!store.read(small_sha));
    BOOST_CHECK(!store.verify(large_sha));
    s = store.stats();
    BOOST_CHECK_EQUAL(s._loose_files, 0U);
    BOOST_CHECK_EQUAL(s._packed_chunks, 0U);
    BOOST_CHECK_EQUAL(s._pack_dead_bytes, 100U + cache_chunk_store::pack_record_overhead);
}

BOOST_AUTO_TEST_CASE(threshold_boundary_and_bad_input, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    const auto below = make_bytes(1023, 3);
    const auto at = make_bytes(1024, 4);
    store.put(sha256_of(below), below);
    store.put(sha256_of(at), at);
    BOOST_CHECK(!fs::exists(store.loose_path(sha256_of(below))));
    BOOST_CHECK(fs::exists(store.loose_path(sha256_of(at))));

    // Bytes that do not hash to the name are refused, as is an oversize chunk.
    BOOST_CHECK_THROW(store.put(sha256_of(below), at), std::invalid_argument);
    const std::vector<std::byte> huge(store.options()._max_chunk_bytes + 1);
    BOOST_CHECK_THROW(store.put(sha256_of(huge), huge), std::invalid_argument);

    // An empty chunk is legal (an empty value) and packs.
    const std::vector<std::byte> empty;
    BOOST_CHECK(store.put(sha256_of(empty), empty));
    BOOST_CHECK(store.read(sha256_of(empty)) == empty);
}

BOOST_AUTO_TEST_CASE(state_survives_reopen, *boost::unit_test::timeout(30)) {
    temp_dir dir;
    std::vector<std::vector<std::byte>> values;
    for (std::uint32_t i = 0; i < 20; ++i) {
        values.push_back(make_bytes(i % 2 == 0 ? 200 : 2000, 100 + i));
    }
    {
        cache_chunk_store store(options_for(dir._path));
        for (const auto& v : values) {
            store.put(sha256_of(v), v);
        }
        // Remove a few of each kind; a restart must not bring them back.
        for (std::size_t i = 0; i < 4; ++i) {
            store.remove(sha256_of(values[i]));
        }
    }
    cache_chunk_store store(options_for(dir._path));
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto got = store.read(sha256_of(values[i]));
        if (i < 4) {
            BOOST_CHECK_MESSAGE(!got, "removed chunk " << i << " came back after reopen");
        } else {
            BOOST_CHECK_MESSAGE(got == values[i], "chunk " << i << " lost on reopen");
        }
    }
    const auto s = store.stats();
    BOOST_CHECK_EQUAL(s._packed_chunks, 8U);
    BOOST_CHECK_EQUAL(s._loose_files, 8U);
    BOOST_CHECK_EQUAL(s._loose_bytes, 8U * 2000U);
    // The two removed packed records are still in the file, counted dead.
    BOOST_CHECK_EQUAL(s._pack_dead_bytes, 2U * (200U + cache_chunk_store::pack_record_overhead));

    std::size_t seen = 0;
    store.for_each_chunk([&](const chunk_sha&, std::uint64_t) { ++seen; });
    BOOST_CHECK_EQUAL(seen, 16U);
}

BOOST_AUTO_TEST_CASE(removed_then_stored_again_in_same_pack, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    const auto v = make_bytes(50, 7);
    const auto sha = sha256_of(v);
    {
        cache_chunk_store store(options_for(dir._path));
        store.put(sha, v);
        store.remove(sha);
        BOOST_CHECK(store.put(sha, v));
    }
    // Two records for the same sha in one pack, the first dead: the dead
    // sidecar names offsets, so the second stays live.
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(store.read(sha) == v);
}

BOOST_AUTO_TEST_CASE(concurrent_put_of_the_same_sha, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    for (const std::size_t size : {std::size_t{100}, std::size_t{100000}}) {
        const auto v = make_bytes(size, static_cast<std::uint32_t>(size));
        const auto sha = sha256_of(v);
        std::atomic<int> created{0};
        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 20; ++i) {
                    created += store.put(sha, v) ? 1 : 0;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        BOOST_CHECK_EQUAL(created.load(), 1);
        BOOST_CHECK(store.read(sha) == v);
    }
    const auto s = store.stats();
    BOOST_CHECK_EQUAL(s._loose_files, 1U);
    BOOST_CHECK_EQUAL(s._packed_chunks, 1U);
    BOOST_CHECK_EQUAL(s._loose_bytes, 100000U);
    // No temporary file left behind by the writers that lost the race.
    BOOST_CHECK(fs::is_empty(dir._path / "chunks" / "tmp"));
}

BOOST_AUTO_TEST_CASE(leftover_temporary_files_are_wiped_on_open, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    {
        cache_chunk_store store(options_for(dir._path));
    }
    // What a crash between write and rename leaves: bytes under tmp/, no name.
    {
        std::ofstream f(dir._path / "chunks" / "tmp" / "123.456.0", std::ios::binary);
        f << std::string(5000, 'x');
    }
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(fs::is_empty(dir._path / "chunks" / "tmp"));
    BOOST_CHECK_EQUAL(store.stats()._loose_files, 0U);
}

#ifdef FIU_ENABLE
BOOST_AUTO_TEST_CASE(crash_between_write_and_rename_leaves_no_named_chunk,
                     *boost::unit_test::timeout(10)) {
    temp_dir dir;
    const auto v = make_bytes(5000, 9);
    const auto sha = sha256_of(v);
    {
        cache_chunk_store store(options_for(dir._path));
        ::fiu_enable("raft/cache/chunk_store/before_rename", 1, nullptr, 0);
        BOOST_CHECK_THROW(store.put(sha, v), cache_chunk_store_error);
        ::fiu_disable("raft/cache/chunk_store/before_rename");
        BOOST_CHECK(!fs::exists(store.loose_path(sha)));
        BOOST_CHECK(!store.contains(sha));
        // The "crashed" write's bytes are still under tmp/.
        BOOST_CHECK(!fs::is_empty(dir._path / "chunks" / "tmp"));
    }
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(fs::is_empty(dir._path / "chunks" / "tmp"));
    BOOST_CHECK(!store.contains(sha));
    BOOST_CHECK(store.put(sha, v));
    BOOST_CHECK(store.read(sha) == v);
}

BOOST_AUTO_TEST_CASE(write_failure_is_an_error_and_leaves_nothing, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    ::fiu_enable("raft/cache/chunk_store/write", 1, nullptr, 0);
    const auto small = make_bytes(10, 10);
    const auto large = make_bytes(5000, 11);
    BOOST_CHECK_THROW(store.put(sha256_of(small), small), cache_chunk_store_error);
    BOOST_CHECK_THROW(store.put(sha256_of(large), large), cache_chunk_store_error);
    ::fiu_disable("raft/cache/chunk_store/write");
    BOOST_CHECK(!store.contains(sha256_of(small)));
    BOOST_CHECK(!store.contains(sha256_of(large)));
    BOOST_CHECK(fs::is_empty(dir._path / "chunks" / "tmp"));
    // The pack has no partial record for the next append to follow.
    BOOST_CHECK(store.put(sha256_of(small), small));
    BOOST_CHECK(store.read(sha256_of(small)) == small);
}

BOOST_AUTO_TEST_CASE(statvfs_fault_points_zero_one_field, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    ::fiu_enable("raft/cache/statvfs_low_inodes", 1, nullptr, 0);
    auto s = store.space();
    BOOST_CHECK_EQUAL(s._free_inodes, 0U);
    BOOST_CHECK_GT(s._free_bytes, 0U);
    ::fiu_disable("raft/cache/statvfs_low_inodes");
    ::fiu_enable("raft/cache/statvfs_low_bytes", 1, nullptr, 0);
    s = store.space();
    BOOST_CHECK_EQUAL(s._free_bytes, 0U);
    ::fiu_disable("raft/cache/statvfs_low_bytes");
}
#endif

BOOST_AUTO_TEST_CASE(statvfs_probe_reports_bytes_and_inodes, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    const auto s = probe_filesystem_space(dir._path);
    BOOST_CHECK_GT(s._free_bytes, 0U);
    // Some filesystems (btrfs) report no inode limit as f_favail == 0; tmp
    // dirs in CI are ext4/overlay/tmpfs, all of which report a real count.
    BOOST_CHECK_GT(s._free_inodes, 0U);
    BOOST_CHECK_THROW((void)probe_filesystem_space(dir._path / "missing"), cache_chunk_store_error);
}

BOOST_AUTO_TEST_CASE(corrupted_chunks_fail_verify_and_read, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    const auto large = make_bytes(4096, 12);
    const auto small = make_bytes(64, 13);
    store.put(sha256_of(large), large);
    store.put(sha256_of(small), small);

    // Flip one byte of the loose file.
    {
        std::fstream f(store.loose_path(sha256_of(large)),
                       std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(100);
        f.put('\x55');
    }
    BOOST_CHECK(store.contains(sha256_of(large)));
    BOOST_CHECK(!store.verify(sha256_of(large)));
    BOOST_CHECK(!store.read(sha256_of(large)));

    // Flip one byte inside the packed record's body.
    {
        const auto pack = pack_files(dir._path).at(0);
        std::fstream f(pack, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(cache_chunk_store::pack_header_bytes + 10));
        f.put('\x55');
    }
    BOOST_CHECK(!store.verify(sha256_of(small)));
    BOOST_CHECK(!store.read(sha256_of(small)));
    BOOST_CHECK_EQUAL(store.stats()._hash_failures, 4U);

    // A truncated loose file fails too.
    fs::resize_file(store.loose_path(sha256_of(large)), 10);
    BOOST_CHECK(!store.verify(sha256_of(large)));
}

BOOST_AUTO_TEST_CASE(torn_pack_tail_is_truncated_on_open, *boost::unit_test::timeout(30)) {
    temp_dir dir;
    std::vector<std::vector<std::byte>> values;
    for (std::uint32_t i = 0; i < 3; ++i) {
        values.push_back(make_bytes(300, 20 + i));
    }
    std::uintmax_t good_size = 0;
    {
        cache_chunk_store store(options_for(dir._path));
        for (const auto& v : values) {
            store.put(sha256_of(v), v);
        }
        good_size = fs::file_size(pack_files(dir._path).at(0));
    }
    const auto pack = pack_files(dir._path).at(0);

    // Case 1: a partial header.
    {
        std::ofstream f(pack, std::ios::binary | std::ios::app);
        f << std::string(20, '\x01');
    }
    {
        cache_chunk_store store(options_for(dir._path));
        BOOST_CHECK_EQUAL(fs::file_size(pack), good_size);
        for (const auto& v : values) {
            BOOST_CHECK(store.read(sha256_of(v)) == v);
        }
    }

    // Case 2: a header promising more bytes than were written.
    {
        std::ofstream f(pack, std::ios::binary | std::ios::app);
        f << std::string(32, '\x02');
        f << '\0' << '\0' << '\x03' << '\xE8';  // len 1000
        f << std::string(100, '\x03');
    }
    {
        cache_chunk_store store(options_for(dir._path));
        BOOST_CHECK_EQUAL(fs::file_size(pack), good_size);
    }

    // Case 3: a complete record whose CRC does not match (a torn write that
    // reached the right length with the wrong bytes). Then a new put lands
    // where the torn record was and survives another reopen.
    {
        const auto extra = make_bytes(40, 99);
        std::ofstream f(pack, std::ios::binary | std::ios::app);
        const auto sha = sha256_of(extra);
        f.write(reinterpret_cast<const char*>(sha.data()), 32);
        f << '\0' << '\0' << '\0' << '\x28';
        f.write(reinterpret_cast<const char*>(extra.data()), 40);
        f << "\xde\xad\xbe\xef";
    }
    const auto later = make_bytes(500, 30);
    {
        cache_chunk_store store(options_for(dir._path));
        BOOST_CHECK_EQUAL(fs::file_size(pack), good_size);
        BOOST_CHECK(!store.contains(sha256_of(make_bytes(40, 99))));
        store.put(sha256_of(later), later);
    }
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(store.read(sha256_of(later)) == later);
    for (const auto& v : values) {
        BOOST_CHECK(store.read(sha256_of(v)) == v);
    }
}

namespace {

/// Fill pack 1 to its maximum, then remove most of it, so it is a closed
/// pack below half live. Returns the survivors and the removed values.
struct compaction_setup {
    std::vector<std::vector<std::byte>> _kept;
    std::vector<std::vector<std::byte>> _removed;
};

auto fill_and_hollow_first_pack(cache_chunk_store& store) -> compaction_setup {
    compaction_setup out;
    // 64 KiB packs, 1 KiB threshold: ~110 records of 500 bytes overflow one.
    std::uint32_t seed = 1000;
    while (pack_files(store.data_dir()).size() < 2) {
        auto v = make_bytes(500, seed++);
        store.put(sha256_of(v), v);
        (seed % 4 == 0 ? out._kept : out._removed).push_back(std::move(v));
    }
    // Everything put so far except the last value is in pack 1; the last
    // opened pack 2. Remove three quarters (the `_removed` set) wherever
    // they are.
    for (const auto& v : out._removed) {
        store.remove(sha256_of(v));
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(compaction_rewrites_live_records_and_frees_the_pack,
                     *boost::unit_test::timeout(60)) {
    temp_dir dir;
    compaction_setup setup;
    {
        cache_chunk_store store(options_for(dir._path));
        setup = fill_and_hollow_first_pack(store);
        const auto before = store.stats();
        BOOST_CHECK_EQUAL(before._pack_files, 2U);
        BOOST_CHECK_EQUAL(store.compact(), 1U);
        const auto after = store.stats();
        BOOST_CHECK_EQUAL(after._compactions, 1U);
        BOOST_CHECK(!fs::exists(store.pack_path(1)));
        BOOST_CHECK(!fs::exists(store.dead_path(1)));
        BOOST_CHECK_LT(after._pack_bytes, before._pack_bytes);
        // Nothing left to do: the open pack is never compacted.
        BOOST_CHECK_EQUAL(store.compact(), 0U);
        for (const auto& v : setup._kept) {
            BOOST_CHECK(store.read(sha256_of(v)) == v);
        }
        for (const auto& v : setup._removed) {
            BOOST_CHECK(!store.contains(sha256_of(v)));
        }
    }
    cache_chunk_store store(options_for(dir._path));
    for (const auto& v : setup._kept) {
        BOOST_CHECK(store.read(sha256_of(v)) == v);
    }
    for (const auto& v : setup._removed) {
        BOOST_CHECK(!store.contains(sha256_of(v)));
    }
    BOOST_CHECK_EQUAL(store.stats()._packed_chunks, setup._kept.size());
}

/// A crash after compaction copied the live records and before it unlinked
/// the old pack. Reproduced without libfiu by saving the old pack and its
/// sidecar, compacting, and putting them back, which is the directory the
/// crash leaves.
BOOST_AUTO_TEST_CASE(crash_mid_compaction_loses_and_resurrects_nothing,
                     *boost::unit_test::timeout(60)) {
    temp_dir dir;
    compaction_setup setup;
    const auto saved = dir._path / "saved";
    fs::create_directories(saved);
    {
        cache_chunk_store store(options_for(dir._path));
        setup = fill_and_hollow_first_pack(store);
        fs::copy_file(store.pack_path(1), saved / "pack.1");
        fs::copy_file(store.dead_path(1), saved / "pack.1.dead");
        BOOST_CHECK_EQUAL(store.compact(), 1U);
    }
    fs::copy_file(saved / "pack.1", dir._path / "packs" / "pack.1");
    fs::copy_file(saved / "pack.1.dead", dir._path / "packs" / "pack.1.dead");

    BOOST_REQUIRE(!setup._kept.empty());
    const auto victim = setup._kept.front();
    {
        cache_chunk_store store(options_for(dir._path));
        // No live chunk lost, none of the removed ones back, and each live
        // chunk indexed once even though two packs hold it.
        for (const auto& v : setup._kept) {
            BOOST_CHECK(store.read(sha256_of(v)) == v);
        }
        for (const auto& v : setup._removed) {
            BOOST_CHECK(!store.contains(sha256_of(v)));
        }
        BOOST_CHECK_EQUAL(store.stats()._packed_chunks, setup._kept.size());
        // Removing a duplicated chunk now must stick across a restart.
        BOOST_CHECK(store.remove(sha256_of(victim)));
        // Compaction can finish the interrupted job.
        BOOST_CHECK_GE(store.compact(), 1U);
    }
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(!store.contains(sha256_of(victim)));
    for (std::size_t i = 1; i < setup._kept.size(); ++i) {
        BOOST_CHECK(store.read(sha256_of(setup._kept[i])) == setup._kept[i]);
    }
    for (const auto& v : setup._removed) {
        BOOST_CHECK(!store.contains(sha256_of(v)));
    }
}

#ifdef FIU_ENABLE
BOOST_AUTO_TEST_CASE(injected_crash_before_compaction_unlink, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    compaction_setup setup;
    {
        cache_chunk_store store(options_for(dir._path));
        setup = fill_and_hollow_first_pack(store);
        ::fiu_enable("raft/cache/chunk_store/compact_before_unlink", 1, nullptr, 0);
        BOOST_CHECK_THROW(store.compact(), cache_chunk_store_error);
        ::fiu_disable("raft/cache/chunk_store/compact_before_unlink");
        BOOST_CHECK(fs::exists(store.pack_path(1)));
    }
    cache_chunk_store store(options_for(dir._path));
    for (const auto& v : setup._kept) {
        BOOST_CHECK(store.read(sha256_of(v)) == v);
    }
    for (const auto& v : setup._removed) {
        BOOST_CHECK(!store.contains(sha256_of(v)));
    }
    BOOST_CHECK_EQUAL(store.stats()._packed_chunks, setup._kept.size());
}
#endif

BOOST_AUTO_TEST_CASE(orphan_sidecar_is_deleted_on_open, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    {
        cache_chunk_store store(options_for(dir._path));
    }
    {
        std::ofstream f(dir._path / "packs" / "pack.7.dead", std::ios::binary);
        f << std::string(8, '\0');
    }
    cache_chunk_store store(options_for(dir._path));
    BOOST_CHECK(!fs::exists(dir._path / "packs" / "pack.7.dead"));
}

/// Requirement 6.7: the file count follows the disk budget, not the key
/// count. 100,000 small values must not mean 100,000 inodes.
BOOST_AUTO_TEST_CASE(hundred_thousand_small_puts_create_few_files,
                     *boost::unit_test::timeout(600)) {
    temp_dir dir;
    auto options = options_for(dir._path);
    options._pack_max_bytes = std::size_t{64} * 1024 * 1024;
    cache_chunk_store store(options);
    constexpr std::uint32_t count = 100000;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto v = make_bytes(64, i);
        store.put(sha256_of(v), v);
    }
    const auto s = store.stats();
    BOOST_CHECK_EQUAL(s._packed_chunks, count);
    BOOST_CHECK_EQUAL(s._loose_files, 0U);
    // 100,000 x 104 bytes is ~10 MB: one pack.
    BOOST_CHECK_EQUAL(s._pack_files, 1U);
    BOOST_CHECK_LE(count_files(dir._path), 4U);
    BOOST_CHECK(store.read(sha256_of(make_bytes(64, 77777))) == make_bytes(64, 77777));
}

BOOST_AUTO_TEST_CASE(refcounts_remove_at_zero, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    chunk_refcounts refs(store);
    const auto v = make_bytes(3000, 40);
    const auto sha = sha256_of(v);

    BOOST_CHECK(refs.put_and_acquire(sha, v));
    BOOST_CHECK(!refs.put_and_acquire(sha, v));
    BOOST_CHECK_EQUAL(refs.count(sha), 2U);
    BOOST_CHECK_EQUAL(refs.acquire(sha), 3U);
    BOOST_CHECK(!refs.release(sha));
    BOOST_CHECK(!refs.release(sha));
    BOOST_CHECK(store.contains(sha));
    BOOST_CHECK(refs.release(sha));
    BOOST_CHECK(!store.contains(sha));
    BOOST_CHECK_EQUAL(refs.size(), 0U);
    BOOST_CHECK_THROW(refs.release(sha), std::logic_error);

    // A reference to a chunk not on disk yet (restored from a snapshot,
    // still to be fetched) releases cleanly.
    const auto absent = sha256_of(make_bytes(10, 41));
    refs.acquire(absent);
    BOOST_CHECK(!refs.release(absent));
}

BOOST_AUTO_TEST_CASE(refcounts_race_between_shards, *boost::unit_test::timeout(60)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    chunk_refcounts refs(store);
    const auto v = make_bytes(5000, 50);
    const auto sha = sha256_of(v);
    // Shard A holds a reference throughout; shards B and C repeatedly take
    // and drop theirs. The chunk must never disappear while A holds it.
    refs.put_and_acquire(sha, v);
    std::atomic<bool> lost{false};
    std::vector<std::thread> threads;
    threads.reserve(2);
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                refs.put_and_acquire(sha, v);
                refs.release(sha);
                if (!store.contains(sha)) {
                    lost = true;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    BOOST_CHECK(!lost.load());
    BOOST_CHECK_EQUAL(refs.count(sha), 1U);
    BOOST_CHECK(refs.release(sha));
    BOOST_CHECK(!store.contains(sha));
}

BOOST_AUTO_TEST_CASE(refcounts_in_bulk_and_forget, *boost::unit_test::timeout(10)) {
    temp_dir dir;
    cache_chunk_store store(options_for(dir._path));
    chunk_refcounts refs(store);
    const auto v = make_bytes(2000, 60);
    const auto sha = sha256_of(v);

    // A caller that has hashed the bytes itself skips the store's check.
    BOOST_CHECK(refs.put_and_acquire(sha, v, true));
    BOOST_CHECK_EQUAL(refs.acquire(sha, 4), 5U);
    BOOST_CHECK(!refs.release(sha, 3));
    BOOST_CHECK_THROW(refs.release(sha, 3), std::logic_error);
    BOOST_CHECK_EQUAL(refs.count(sha), 2U);

    // Forgetting drops the count without deleting the file: a replica going
    // away has not deleted its keys.
    refs.forget(sha, 2);
    BOOST_CHECK_EQUAL(refs.count(sha), 0U);
    BOOST_CHECK(store.contains(sha));
    BOOST_CHECK_THROW(refs.forget(sha), std::logic_error);

    // A referenced chunk whose file went missing is written back.
    refs.acquire(sha);
    BOOST_CHECK(store.remove(sha));
    BOOST_CHECK(refs.put_referenced(sha, v));
    BOOST_CHECK(store.read(sha) == v);
    BOOST_CHECK(!refs.put_referenced(sha, v));
}
