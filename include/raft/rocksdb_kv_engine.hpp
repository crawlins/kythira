// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file rocksdb_kv_engine.hpp
/// @brief A `kv_engine` on RocksDB (.kiro/specs/persistent-state-machine/
///        Requirement 7.2, task 5).
///
/// Built only with `CONFIG_ENGINE_ROCKSDB=y` (vcpkg feature `rocksdb`), which
/// defines `KYTHIRA_ENGINE_ROCKSDB_AVAILABLE` and links the
/// `raft_rocksdb_engine` library. This header names no RocksDB type, so a
/// build without the option never sees a RocksDB header (Requirement 7.3).
///
/// One RocksDB database per node, with a column family for each of the
/// engine's column families (`data`, `txn`, `meta`) next to RocksDB's
/// `default`, which stays empty.
///
/// - `write` is one `WriteBatch`; `erase_range` becomes `DeleteRange`. An
///   unbounded end is bounded at the column family's last key, read under
///   the write lock, so nothing can land past it in between.
/// - Sequences are RocksDB's own: a batch's sequence is the database's
///   latest sequence number after it, read under the write lock. Writes are
///   serialized by that lock, which keeps the sequence a batch gets exactly
///   its own.
/// - Writes go to the WAL without fsync. `durable_sequence()` is the latest
///   sequence a `sync` (`SyncWAL`) has covered; everything recovered at open
///   counts as durable.
/// - `checkpoint` writes the range's keys, read at one RocksDB snapshot, as
///   one SST file per non-empty column family (`data.sst`, `txn.sst`,
///   `meta.sst`). RocksDB's own `Checkpoint` copies the whole database,
///   which holds every group on the node, so it cannot be ingested into one
///   range.
/// - `ingest` checks the manifest and every file's SHA-256, then writes a
///   range-tombstone SST per column family and hands it and the checkpoint's
///   file to one `IngestExternalFiles` call across all column families. That
///   call is atomic, so the range is replaced all at once or not at all: a
///   crash never leaves it half deleted. The checkpoint's files are moved
///   into the database where the filesystem allows (hard links), so the
///   directory is spent afterwards.
/// - `drop` is a `DeleteRange` per column family.

#include <raft/persistent_state_machine.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kythira {

struct rocksdb_kv_engine_options {
    /// Create the database (and its column families) if missing.
    bool create_if_missing = true;
    /// Fsync the WAL on every write, making every write durable at once.
    /// Off by default: the node syncs when it compacts (Requirement 3.3).
    bool sync_every_write = false;
};

class rocksdb_kv_engine {
public:
    static constexpr std::string_view k_format_tag = "kythira-rocksdb/1";

    /// Opens (or creates) the database in `path`. Throws
    /// `persistent_storage_error` on failure.
    explicit rocksdb_kv_engine(const std::filesystem::path& path,
                               const rocksdb_kv_engine_options& options = {});
    ~rocksdb_kv_engine();
    rocksdb_kv_engine(const rocksdb_kv_engine&) = delete;
    auto operator=(const rocksdb_kv_engine&) -> rocksdb_kv_engine& = delete;
    rocksdb_kv_engine(rocksdb_kv_engine&&) = delete;
    auto operator=(rocksdb_kv_engine&&) -> rocksdb_kv_engine& = delete;

    // -- kv_engine ----------------------------------------------------------

    [[nodiscard]] auto get(column_family cf, const std::string& key) const
        -> std::optional<std::string>;
    // The visit count is informational; callers act in the visitor.
    // NOLINTNEXTLINE(modernize-use-nodiscard)
    auto scan(column_family cf, const key_range& range, scan_direction direction,
              const scan_visitor& visit) const -> std::size_t;
    auto write(const write_batch& batch) -> engine_sequence;
    [[nodiscard]] auto latest_sequence() const -> engine_sequence;
    [[nodiscard]] auto durable_sequence() const -> engine_sequence;
    auto sync(engine_sequence seq) -> void;
    auto checkpoint(const key_range& range, const std::filesystem::path& dir) -> snapshot_manifest;
    auto ingest(const key_range& range, const std::filesystem::path& dir,
                const snapshot_manifest& manifest) -> engine_sequence;
    auto drop(const key_range& range) -> engine_sequence;
    [[nodiscard]] auto format_tag() const noexcept -> std::string_view { return k_format_tag; }

    [[nodiscard]] auto path() const -> const std::filesystem::path&;

private:
    struct impl;
    std::unique_ptr<impl> _impl;
};

static_assert(kv_engine<rocksdb_kv_engine>);

}  // namespace kythira
