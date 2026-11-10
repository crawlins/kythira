// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// rocksdb_kv_engine (.kiro/specs/persistent-state-machine/ task 5). See the
// header for the design; this file is the only one that includes RocksDB.

#include <raft/rocksdb_kv_engine.hpp>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/sst_file_writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace {

constexpr std::array<std::string_view, 3> k_cf_names{"data", "txn", "meta"};

auto cf_index(column_family cf) -> std::size_t {
    return static_cast<std::size_t>(cf);
}

auto file_name_of(column_family cf) -> std::string {
    return std::string(k_cf_names.at(cf_index(cf))) + ".sst";
}

auto check(const rocksdb::Status& s, std::string_view what) -> void {
    if (!s.ok()) {
        throw persistent_storage_error("rocksdb_kv_engine: " + std::string(what) + ": " +
                                       s.ToString());
    }
}

auto to_slice(std::string_view s) -> rocksdb::Slice {
    return {s.data(), s.size()};
}

}  // namespace

struct rocksdb_kv_engine::impl {
    std::filesystem::path path;
    rocksdb_kv_engine_options options;
    std::unique_ptr<rocksdb::DB> db;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;  // default, data, txn, meta
    rocksdb::ColumnFamilyOptions cf_options;
    rocksdb::DBOptions db_options;
    std::mutex write_mu;
    std::atomic<engine_sequence> durable{0};

    ~impl() {
        if (db) {
            for (auto* h : handles) {
                (void)db->DestroyColumnFamilyHandle(h);
            }
            (void)db->Close();
        }
    }

    [[nodiscard]] auto handle(column_family cf) const -> rocksdb::ColumnFamilyHandle* {
        return handles.at(cf_index(cf) + 1);
    }
    [[nodiscard]] auto default_handle() const -> rocksdb::ColumnFamilyHandle* {
        return handles.at(0);
    }

    // The column family's last key at or above `start`, read live; callers
    // hold write_mu so nothing lands after it in between.
    [[nodiscard]] auto last_key_from(column_family cf, const std::string& start) const
        -> std::optional<std::string> {
        rocksdb::ReadOptions ro;
        const rocksdb::Slice lower(start);
        ro.iterate_lower_bound = &lower;
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(ro, handle(cf)));
        it->SeekToLast();
        if (!it->Valid()) {
            check(it->status(), "seek to last key");
            return std::nullopt;
        }
        return it->key().ToString();
    }

    // The end of a range for DeleteRange: its own end, or just past the
    // last key that could be in it. Empty when nothing can be in it.
    [[nodiscard]] auto delete_end(column_family cf, const key_range& range,
                                  const std::optional<std::string>& also_past) const
        -> std::optional<std::string> {
        if (range.empty()) {
            return std::nullopt;
        }
        if (range.end.has_value()) {
            return range.end;
        }
        auto last = last_key_from(cf, range.start);
        if (also_past.has_value() && (!last.has_value() || *last < *also_past)) {
            last = also_past;
        }
        if (!last.has_value()) {
            return std::nullopt;
        }
        return *last + std::string(1, '\0');
    }

    auto refuse_if_secondary(std::string_view what) const -> void {
        if (!options.secondary_path.empty()) {
            throw persistent_storage_error("rocksdb_kv_engine: " + std::string(what) + " on " +
                                           path.string() + ", opened read-only as a secondary");
        }
    }

    auto write_locked(const write_batch& batch) -> engine_sequence {
        refuse_if_secondary("write");
        if (batch.empty()) {
            return db->GetLatestSequenceNumber();
        }
        rocksdb::WriteBatch wb;
        // The highest key put so far in this batch, per column family: an
        // unbounded erase_range later in the batch must cover it too.
        std::array<std::optional<std::string>, 3> max_put{};
        for (const auto& op : batch.operations()) {
            if (const auto* put = std::get_if<write_batch::put_op>(&op)) {
                check(wb.Put(handle(put->cf), to_slice(put->key), to_slice(put->value)), "put");
                auto& m = max_put.at(cf_index(put->cf));
                if (!m.has_value() || *m < put->key) {
                    m = put->key;
                }
            } else if (const auto* er = std::get_if<write_batch::erase_op>(&op)) {
                check(wb.Delete(handle(er->cf), to_slice(er->key)), "delete");
            } else {
                const auto& rg = std::get<write_batch::erase_range_op>(op);
                const auto& m = max_put.at(cf_index(rg.cf));
                const auto past = (m.has_value() && rg.range.contains(*m)) ? m : std::nullopt;
                if (const auto end = delete_end(rg.cf, rg.range, past); end.has_value()) {
                    check(wb.DeleteRange(handle(rg.cf), to_slice(rg.range.start), to_slice(*end)),
                          "delete range");
                }
            }
        }
        if (wb.Count() == 0) {
            // Every operation was a no-op (an erase_range over nothing); the
            // batch still takes a sequence, as the in-memory engine's does.
            check(wb.Put(default_handle(), rocksdb::Slice(), rocksdb::Slice()), "sequence marker");
        }
        rocksdb::WriteOptions wo;
        wo.sync = options.sync_every_write;
        check(db->Write(wo, &wb), "write");
        const auto seq = db->GetLatestSequenceNumber();
        if (options.sync_every_write) {
            raise_durable(seq);
        }
        return seq;
    }

    auto raise_durable(engine_sequence seq) -> void {
        auto cur = durable.load();
        while (cur < seq && !durable.compare_exchange_weak(cur, seq)) {
        }
    }
};

rocksdb_kv_engine::rocksdb_kv_engine(const std::filesystem::path& path,
                                     const rocksdb_kv_engine_options& options)
    : _impl(std::make_unique<impl>()) {
    _impl->path = path;
    _impl->options = options;
    _impl->db_options.create_if_missing = options.create_if_missing;
    _impl->db_options.create_missing_column_families = options.create_if_missing;
    _impl->db_options.max_total_wal_size = options.max_total_wal_bytes;
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, _impl->cf_options);
    for (const auto name : k_cf_names) {
        descriptors.emplace_back(std::string(name), _impl->cf_options);
    }
    rocksdb::DB* raw = nullptr;
    if (!options.secondary_path.empty()) {
        // A secondary keeps every file it reads open, since the primary may
        // delete it (RocksDB requires this setting for secondaries).
        _impl->db_options.create_if_missing = false;
        _impl->db_options.create_missing_column_families = false;
        _impl->db_options.max_open_files = -1;
        std::filesystem::create_directories(options.secondary_path);
        check(rocksdb::DB::OpenAsSecondary(_impl->db_options, path.string(),
                                           options.secondary_path.string(), descriptors,
                                           &_impl->handles, &raw),
              "open " + path.string() + " as a secondary");
    } else {
        check(
            rocksdb::DB::Open(_impl->db_options, path.string(), descriptors, &_impl->handles, &raw),
            "open " + path.string());
    }
    _impl->db.reset(raw);
    // What survived the open is durable by definition.
    _impl->durable = _impl->db->GetLatestSequenceNumber();
}

rocksdb_kv_engine::~rocksdb_kv_engine() = default;

auto rocksdb_kv_engine::path() const -> const std::filesystem::path& {
    return _impl->path;
}

auto rocksdb_kv_engine::get(column_family cf, const std::string& key) const
    -> std::optional<std::string> {
    std::string value;
    const auto s = _impl->db->Get(rocksdb::ReadOptions(), _impl->handle(cf), key, &value);
    if (s.IsNotFound()) {
        return std::nullopt;
    }
    check(s, "get");
    return value;
}

auto rocksdb_kv_engine::scan(column_family cf, const key_range& range, scan_direction direction,
                             const scan_visitor& visit) const -> std::size_t {
    if (range.empty()) {
        return 0;
    }
    rocksdb::ReadOptions ro;
    const rocksdb::Slice lower(range.start);
    rocksdb::Slice upper;
    ro.iterate_lower_bound = &lower;
    if (range.end.has_value()) {
        upper = rocksdb::Slice(*range.end);
        ro.iterate_upper_bound = &upper;
    }
    std::unique_ptr<rocksdb::Iterator> it(_impl->db->NewIterator(ro, _impl->handle(cf)));
    std::size_t visited = 0;
    if (direction == scan_direction::forward) {
        for (it->Seek(lower); it->Valid(); it->Next()) {
            ++visited;
            if (!visit(std::string_view(it->key().data(), it->key().size()),
                       std::string_view(it->value().data(), it->value().size()))) {
                return visited;
            }
        }
    } else {
        for (it->SeekToLast(); it->Valid(); it->Prev()) {
            ++visited;
            if (!visit(std::string_view(it->key().data(), it->key().size()),
                       std::string_view(it->value().data(), it->value().size()))) {
                return visited;
            }
        }
    }
    check(it->status(), "scan");
    return visited;
}

auto rocksdb_kv_engine::write(const write_batch& batch) -> engine_sequence {
    std::lock_guard lock(_impl->write_mu);
    return _impl->write_locked(batch);
}

auto rocksdb_kv_engine::latest_sequence() const -> engine_sequence {
    return _impl->db->GetLatestSequenceNumber();
}

auto rocksdb_kv_engine::durable_sequence() const -> engine_sequence {
    return std::min(_impl->durable.load(), latest_sequence());
}

auto rocksdb_kv_engine::sync(engine_sequence seq) -> void {
    _impl->refuse_if_secondary("sync");
    if (seq <= _impl->durable.load()) {
        return;
    }
    engine_sequence target = 0;
    {
        // Every write up to `target` has returned, so its WAL record is
        // written; SyncWAL makes it durable.
        std::lock_guard lock(_impl->write_mu);
        target = _impl->db->GetLatestSequenceNumber();
    }
    check(_impl->db->SyncWAL(), "sync WAL");
    _impl->raise_durable(target);
}

auto rocksdb_kv_engine::checkpoint(const key_range& range, const std::filesystem::path& dir)
    -> snapshot_manifest {
    std::error_code ec;
    if (!std::filesystem::create_directory(dir, ec)) {
        throw persistent_storage_error("rocksdb_kv_engine: checkpoint directory " + dir.string() +
                                       (ec ? ": " + ec.message() : " already exists"));
    }
    const auto* snapshot = _impl->db->GetSnapshot();
    const auto release =
        std::unique_ptr<const rocksdb::Snapshot, std::function<void(const rocksdb::Snapshot*)>>(
            snapshot, [this](const rocksdb::Snapshot* s) { _impl->db->ReleaseSnapshot(s); });

    snapshot_manifest m;
    m.engine_format = std::string(k_format_tag);
    m.range = range;
    if (range.empty()) {
        return m;
    }
    const rocksdb::Options sst_options(_impl->db_options, _impl->cf_options);
    for (const auto cf : k_all_column_families) {
        rocksdb::ReadOptions ro;
        ro.snapshot = snapshot;
        // A checkpoint reads the whole range once: keep it out of the block
        // cache, where it would evict what the group reads and grow the
        // process by the cache's size during a snapshot transfer
        // (file-install-snapshot Requirement 7.2).
        ro.fill_cache = false;
        const rocksdb::Slice lower(range.start);
        rocksdb::Slice upper;
        ro.iterate_lower_bound = &lower;
        if (range.end.has_value()) {
            upper = rocksdb::Slice(*range.end);
            ro.iterate_upper_bound = &upper;
        }
        std::unique_ptr<rocksdb::Iterator> it(_impl->db->NewIterator(ro, _impl->handle(cf)));
        it->Seek(lower);
        if (!it->Valid()) {
            check(it->status(), "checkpoint scan");
            continue;
        }
        const auto name = file_name_of(cf);
        const auto file = dir / name;
        rocksdb::SstFileWriter writer(rocksdb::EnvOptions(), sst_options, _impl->handle(cf));
        check(writer.Open(file.string()), "open " + file.string());
        for (; it->Valid(); it->Next()) {
            check(writer.Put(it->key(), it->value()), "checkpoint put");
        }
        check(it->status(), "checkpoint scan");
        check(writer.Finish(), "finish " + file.string());
        const auto [digest, size] = sha256_of_file(file);
        m.files.push_back(checkpoint_file{name, size, digest});
    }
    return m;
}

auto rocksdb_kv_engine::ingest(const key_range& range, const std::filesystem::path& dir,
                               const snapshot_manifest& manifest) -> engine_sequence {
    _impl->refuse_if_secondary("ingest");
    manifest.validate();
    if (manifest.engine_format != k_format_tag) {
        throw persistent_storage_error("rocksdb_kv_engine: checkpoint format '" +
                                       manifest.engine_format + "' is not '" +
                                       std::string(k_format_tag) + "'");
    }
    if (!(manifest.range == range)) {
        throw persistent_storage_error(
            "rocksdb_kv_engine: checkpoint range does not match the ingest range");
    }
    // Verify every file before touching the live range.
    std::array<std::optional<std::filesystem::path>, 3> files{};
    for (const auto& f : manifest.files) {
        std::optional<column_family> cf;
        for (const auto c : k_all_column_families) {
            if (f.name == file_name_of(c)) {
                cf = c;
            }
        }
        if (!cf.has_value()) {
            throw persistent_storage_error("rocksdb_kv_engine: unexpected checkpoint file " +
                                           f.name);
        }
        const auto path = dir / f.name;
        if (!std::filesystem::is_regular_file(path)) {
            throw persistent_storage_error("rocksdb_kv_engine: missing checkpoint file " +
                                           path.string());
        }
        const auto [digest, size] = sha256_of_file(path);
        if (size != f.size || digest != f.sha256) {
            throw persistent_storage_error("rocksdb_kv_engine: checksum mismatch on " +
                                           path.string());
        }
        files.at(cf_index(*cf)) = path;
    }

    // Writes made before the ingest must be as durable as the ingested
    // files, or a crash could keep the range and lose what preceded it.
    check(_impl->db->SyncWAL(), "sync WAL before ingest");

    const auto tombstones = dir / ".kythira-tombstones";
    std::error_code ec;
    std::filesystem::remove_all(tombstones, ec);
    std::filesystem::create_directory(tombstones);
    const auto cleanup = std::unique_ptr<const std::filesystem::path,
                                         std::function<void(const std::filesystem::path*)>>(
        &tombstones, [](const std::filesystem::path* p) {
            std::error_code e;
            std::filesystem::remove_all(*p, e);
        });

    std::lock_guard lock(_impl->write_mu);
    const rocksdb::Options sst_options(_impl->db_options, _impl->cf_options);
    std::vector<rocksdb::IngestExternalFileArg> args;
    for (const auto cf : k_all_column_families) {
        rocksdb::IngestExternalFileArg arg;
        arg.column_family = _impl->handle(cf);
        arg.options.move_files = true;
        arg.options.failed_move_fall_back_to_copy = true;
        arg.options.snapshot_consistency = true;
        arg.options.allow_global_seqno = true;
        arg.options.allow_blocking_flush = true;
        // First a tombstone over whatever this node holds in the range, then
        // the checkpoint's keys, which a later file in one ingestion wins
        // over.
        if (const auto end = _impl->delete_end(cf, range, std::nullopt); end.has_value()) {
            const auto file = tombstones / file_name_of(cf);
            rocksdb::SstFileWriter writer(rocksdb::EnvOptions(), sst_options, _impl->handle(cf));
            check(writer.Open(file.string()), "open " + file.string());
            check(writer.DeleteRange(to_slice(range.start), to_slice(*end)), "tombstone");
            check(writer.Finish(), "finish " + file.string());
            arg.external_files.push_back(file.string());
        }
        if (const auto& f = files.at(cf_index(cf)); f.has_value()) {
            arg.external_files.push_back(f->string());
        }
        if (!arg.external_files.empty()) {
            args.push_back(std::move(arg));
        }
    }
    if (args.empty()) {
        // Nothing here and nothing sent: still one step in the sequence.
        return _impl->write_locked(write_batch{}.erase_range(column_family::meta, range));
    }
    check(_impl->db->IngestExternalFiles(args), "ingest");
    const auto seq = _impl->db->GetLatestSequenceNumber();
    // Ingested files are synced, and the WAL was synced just before.
    _impl->raise_durable(seq);
    return seq;
}

auto rocksdb_kv_engine::drop(const key_range& range) -> engine_sequence {
    write_batch batch;
    for (const auto cf : k_all_column_families) {
        batch.erase_range(cf, range);
    }
    return write(batch);
}

}  // namespace kythira
