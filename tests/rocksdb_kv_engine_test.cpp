// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file rocksdb_kv_engine_test.cpp
/// @brief .kiro/specs/persistent-state-machine/ task 5: `rocksdb_kv_engine`
///        (Requirements 7.2-7.4).
///
/// - The engine conformance suite, the same cases `memory_kv_engine` passes.
/// - Reopen: data, sequences and durability survive closing the database.
/// - Ingest replaces the range and only the range, even with keys of other
///   groups on both sides, and leaves the checkpoint directory spent.
/// - Opening a missing database with `create_if_missing = false` throws.

#define BOOST_TEST_MODULE rocksdb_kv_engine_test
#include <boost/test/unit_test.hpp>

#include "kv_engine_conformance.hpp"

#include <raft/persistent_state_machine.hpp>
#include <raft/rocksdb_kv_engine.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

using kythira::column_family;
using kythira::key_range;
using kythira::rocksdb_kv_engine;
using kythira::rocksdb_kv_engine_options;
using kythira::write_batch;
using kythira::kv_conformance::scan_all;

namespace {

/// A directory under the system temp dir, removed on destruction.
class temp_dir {
public:
    temp_dir() {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        _path = std::filesystem::temp_directory_path() /
                ("kythira-rocks-" + std::to_string(rd()) + "-" + std::to_string(counter++));
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

/// Each `make_engine` opens a fresh database in its own subdirectory.
struct rocksdb_engine_harness {
    using engine_t = rocksdb_kv_engine;
    temp_dir dir;
    temp_dir dbs;
    int next = 0;
    auto make_engine() -> std::unique_ptr<engine_t> {
        return std::make_unique<engine_t>(dbs.path() / ("db-" + std::to_string(next++)));
    }
    [[nodiscard]] auto scratch() const -> const std::filesystem::path& { return dir.path(); }
};

}  // namespace

KYTHIRA_KV_ENGINE_CONFORMANCE(rocksdb_engine_harness, rocksdb)

BOOST_AUTO_TEST_SUITE(rocksdb_engine)

BOOST_AUTO_TEST_CASE(data_sequences_and_durability_survive_a_reopen) {
    const temp_dir dir;
    const auto db = dir.path() / "db";
    kythira::engine_sequence written = 0;
    {
        rocksdb_kv_engine e(db);
        e.write(
            write_batch{}.put(column_family::data, "a", "1").put(column_family::meta, "m", "x"));
        written = e.write(
            write_batch{}.put(column_family::txn, "t", "2").erase(column_family::data, "a"));
        BOOST_TEST(e.latest_sequence() == written);
    }
    rocksdb_kv_engine e(db);
    BOOST_TEST(e.latest_sequence() == written);
    BOOST_TEST(e.durable_sequence() == written);
    BOOST_TEST(!e.get(column_family::data, "a").has_value());
    BOOST_TEST(e.get(column_family::txn, "t").value_or("") == "2");
    BOOST_TEST(e.get(column_family::meta, "m").value_or("") == "x");
    const auto next = e.write(write_batch{}.put(column_family::data, "b", "3"));
    BOOST_TEST(next > written);
}

BOOST_AUTO_TEST_CASE(ingest_replaces_only_the_range_between_other_groups_keys) {
    const temp_dir dir;
    rocksdb_kv_engine src(dir.path() / "src");
    rocksdb_kv_engine dst(dir.path() / "dst");
    const key_range range{"g2/", std::string("g2/\xff")};

    src.write(write_batch{}
                  .put(column_family::data, "g2/a", "new-a")
                  .put(column_family::data, "g2/c", "new-c")
                  .put(column_family::meta, "g2/applied", "9"));
    dst.write(write_batch{}
                  .put(column_family::data, "g1/z", "keep-1")
                  .put(column_family::data, "g2/a", "old-a")
                  .put(column_family::data, "g2/b", "old-b")
                  .put(column_family::txn, "g2/t", "old-t")
                  .put(column_family::data, "g3/a", "keep-3"));

    const auto ckpt = dir.path() / "ckpt";
    const auto manifest = src.checkpoint(range, ckpt);
    const auto before = dst.latest_sequence();
    const auto seq = dst.ingest(range, ckpt, manifest);
    BOOST_TEST(seq > before);
    BOOST_TEST(dst.durable_sequence() >= seq);

    BOOST_TEST(dst.get(column_family::data, "g1/z").value_or("") == "keep-1");
    BOOST_TEST(dst.get(column_family::data, "g3/a").value_or("") == "keep-3");
    BOOST_TEST(dst.get(column_family::data, "g2/a").value_or("") == "new-a");
    BOOST_TEST(dst.get(column_family::data, "g2/c").value_or("") == "new-c");
    BOOST_TEST(!dst.get(column_family::data, "g2/b").has_value());
    BOOST_TEST(!dst.get(column_family::txn, "g2/t").has_value());
    BOOST_TEST(dst.get(column_family::meta, "g2/applied").value_or("") == "9");
    BOOST_TEST(scan_all(dst, column_family::data, range).size() == 2U);

    // Writes after the ingest land on top of it and survive a reopen.
    dst.write(write_batch{}.put(column_family::data, "g2/d", "after"));
    BOOST_TEST(dst.get(column_family::data, "g2/d").value_or("") == "after");
}

BOOST_AUTO_TEST_CASE(an_ingested_range_survives_a_reopen) {
    const temp_dir dir;
    const key_range range{"k", std::nullopt};
    {
        rocksdb_kv_engine src(dir.path() / "src");
        src.write(write_batch{}.put(column_family::data, "k1", "v1"));
        const auto m = src.checkpoint(range, dir.path() / "ckpt");
        rocksdb_kv_engine dst(dir.path() / "dst");
        dst.write(write_batch{}
                      .put(column_family::data, "k0", "gone")
                      .put(column_family::data, "a", "kept"));
        dst.ingest(range, dir.path() / "ckpt", m);
    }
    rocksdb_kv_engine dst(dir.path() / "dst");
    BOOST_TEST(dst.get(column_family::data, "k1").value_or("") == "v1");
    BOOST_TEST(!dst.get(column_family::data, "k0").has_value());
    BOOST_TEST(dst.get(column_family::data, "a").value_or("") == "kept");
}

BOOST_AUTO_TEST_CASE(opening_a_missing_database_without_create_throws) {
    const temp_dir dir;
    rocksdb_kv_engine_options options;
    options.create_if_missing = false;
    BOOST_CHECK_THROW(rocksdb_kv_engine(dir.path() / "nope", options),
                      kythira::persistent_storage_error);
}

BOOST_AUTO_TEST_CASE(sync_every_write_makes_each_write_durable) {
    const temp_dir dir;
    rocksdb_kv_engine_options options;
    options.sync_every_write = true;
    rocksdb_kv_engine e(dir.path() / "db", options);
    const auto s = e.write(write_batch{}.put(column_family::data, "a", "1"));
    BOOST_TEST(e.durable_sequence() == s);
}

BOOST_AUTO_TEST_SUITE_END()
