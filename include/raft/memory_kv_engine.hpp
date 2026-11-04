// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file memory_kv_engine.hpp
/// @brief An in-memory `kv_engine` for tests, with a durable sequence that
///        tests can hold back (.kiro/specs/persistent-state-machine/
///        Requirement 7.1, task 1).
///
/// Three `std::map`s, one per column family, behind one mutex. Every
/// non-empty batch takes the next sequence number.
///
/// Durability is simulated. By default every write is durable at once
/// (`durable_sequence() == latest_sequence()`). After `hold_durability()` the
/// engine keeps a copy of the state as of the durable sequence and stops
/// advancing it: `sync(seq)` moves it forward to `seq`, and
/// `simulate_crash()` throws away everything after it, as a power cut would
/// throw away writes RocksDB had not synced. That is what lets crash and
/// compaction-bound tests (Requirement 3.1) run deterministically without a
/// disk.
///
/// `scan` copies at most `k_scan_chunk` entries at a time under the mutex
/// and calls the visitor with the mutex released, resuming after the last
/// key it visited, so a visitor may call back into the engine. Like a
/// RocksDB iterator without a snapshot, it sees writes made between chunks.
///
/// A checkpoint is one file, `range.kvs`, holding every column family's keys
/// in the range: "KYKV", a u32 version, then records of u8 column family,
/// u32 key length, key, u32 value length, value (all little-endian).

#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

class memory_kv_engine {
public:
    static constexpr std::string_view k_format_tag = "kythira-memory-kv/1";
    static constexpr std::string_view k_checkpoint_file = "range.kvs";
    static constexpr std::size_t k_scan_chunk = 256;

    memory_kv_engine() = default;
    memory_kv_engine(const memory_kv_engine&) = delete;
    auto operator=(const memory_kv_engine&) -> memory_kv_engine& = delete;
    memory_kv_engine(memory_kv_engine&&) = delete;
    auto operator=(memory_kv_engine&&) -> memory_kv_engine& = delete;
    ~memory_kv_engine() = default;

    // -- kv_engine ----------------------------------------------------------

    [[nodiscard]] auto get(column_family cf, const std::string& key) const
        -> std::optional<std::string> {
        std::lock_guard lock(_mu);
        const auto& t = table_of(_live, cf);
        const auto it = t.find(key);
        if (it == t.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto scan(column_family cf, const key_range& range, scan_direction direction,
              const scan_visitor& visit) const -> std::size_t {
        std::size_t visited = 0;
        std::optional<std::string> resume;  // last key visited
        for (;;) {
            std::vector<std::pair<std::string, std::string>> chunk;
            {
                std::lock_guard lock(_mu);
                chunk = next_chunk(table_of(_live, cf), range, direction, resume);
            }
            if (chunk.empty()) {
                return visited;
            }
            for (const auto& [k, v] : chunk) {
                ++visited;
                if (!visit(k, v)) {
                    return visited;
                }
            }
            resume = chunk.back().first;
        }
    }

    auto write(const write_batch& batch) -> engine_sequence {
        std::lock_guard lock(_mu);
        return write_locked(batch);
    }

    [[nodiscard]] auto latest_sequence() const -> engine_sequence {
        std::lock_guard lock(_mu);
        return _latest;
    }

    [[nodiscard]] auto durable_sequence() const -> engine_sequence {
        std::lock_guard lock(_mu);
        return _durable_seq;
    }

    auto sync(engine_sequence seq) -> void {
        std::lock_guard lock(_mu);
        sync_locked(seq);
    }

    auto checkpoint(const key_range& range, const std::filesystem::path& dir) -> snapshot_manifest {
        std::string body;
        {
            std::lock_guard lock(_mu);
            body = encode_range_locked(range);
        }
        if (!std::filesystem::create_directory(dir)) {
            throw persistent_storage_error(
                "memory_kv_engine: checkpoint directory already exists: " + dir.string());
        }
        const auto path = dir / k_checkpoint_file;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            out.flush();
            if (!out) {
                throw persistent_storage_error("memory_kv_engine: cannot write " + path.string());
            }
        }
        snapshot_manifest m;
        m.engine_format = std::string(k_format_tag);
        m.range = range;
        m.files.push_back(checkpoint_file{std::string(k_checkpoint_file),
                                          static_cast<std::uint64_t>(body.size()),
                                          sha256_of(body)});
        return m;
    }

    auto ingest(const key_range& range, const std::filesystem::path& dir,
                const snapshot_manifest& manifest) -> engine_sequence {
        manifest.validate();
        if (manifest.engine_format != k_format_tag) {
            throw persistent_storage_error("memory_kv_engine: checkpoint format '" +
                                           manifest.engine_format + "' is not '" +
                                           std::string(k_format_tag) + "'");
        }
        if (!(manifest.range == range)) {
            throw persistent_storage_error(
                "memory_kv_engine: checkpoint range does not match the ingest range");
        }
        // Verify and parse every file before touching the live range.
        write_batch batch;
        for (const auto cf : k_all_column_families) {
            batch.erase_range(cf, range);
        }
        for (const auto& f : manifest.files) {
            const auto path = dir / f.name;
            const auto [digest, size] = sha256_of_file(path);
            if (size != f.size || digest != f.sha256) {
                throw persistent_storage_error("memory_kv_engine: checksum mismatch on " +
                                               path.string());
            }
            decode_records(read_file(path), range, batch);
        }
        std::lock_guard lock(_mu);
        return write_locked(batch);
    }

    auto drop(const key_range& range) -> engine_sequence {
        write_batch batch;
        for (const auto cf : k_all_column_families) {
            batch.erase_range(cf, range);
        }
        return write(batch);
    }

    [[nodiscard]] auto format_tag() const noexcept -> std::string_view { return k_format_tag; }

    // -- test controls ------------------------------------------------------

    /// Stop advancing the durable sequence on write. Idempotent.
    auto hold_durability() -> void {
        std::lock_guard lock(_mu);
        if (!_durable.has_value()) {
            _durable = _live;
        }
    }

    /// Make everything durable and go back to durable-on-write.
    auto release_durability() -> void {
        std::lock_guard lock(_mu);
        sync_locked(_latest);
        _durable.reset();
    }

    /// Lose every write after the durable sequence, as a power cut would.
    /// The engine keeps holding durability afterwards. Without a hold
    /// nothing is lost.
    auto simulate_crash() -> void {
        std::lock_guard lock(_mu);
        if (!_durable.has_value()) {
            return;
        }
        _live = *_durable;
        _latest = _durable_seq;
        _pending.clear();
    }

    [[nodiscard]] auto key_count(column_family cf) const -> std::size_t {
        std::lock_guard lock(_mu);
        return table_of(_live, cf).size();
    }

private:
    using table = std::map<std::string, std::string, std::less<>>;
    using tables = std::array<table, k_all_column_families.size()>;

    static auto table_of(tables& t, column_family cf) -> table& {
        return t.at(static_cast<std::size_t>(cf));
    }
    static auto table_of(const tables& t, column_family cf) -> const table& {
        return t.at(static_cast<std::size_t>(cf));
    }

    static auto apply_batch(tables& t, const write_batch& batch) -> void {
        for (const auto& op : batch.operations()) {
            if (const auto* put = std::get_if<write_batch::put_op>(&op)) {
                table_of(t, put->cf).insert_or_assign(put->key, put->value);
            } else if (const auto* er = std::get_if<write_batch::erase_op>(&op)) {
                table_of(t, er->cf).erase(er->key);
            } else {
                const auto& rg = std::get<write_batch::erase_range_op>(op);
                if (rg.range.empty()) {
                    continue;
                }
                auto& tb = table_of(t, rg.cf);
                const auto first = tb.lower_bound(rg.range.start);
                const auto last =
                    rg.range.end.has_value() ? tb.lower_bound(*rg.range.end) : tb.end();
                tb.erase(first, last);
            }
        }
    }

    auto write_locked(const write_batch& batch) -> engine_sequence {
        if (batch.empty()) {
            return _latest;
        }
        apply_batch(_live, batch);
        ++_latest;
        if (_durable.has_value()) {
            _pending.emplace_back(_latest, batch);
        } else {
            _durable_seq = _latest;
        }
        return _latest;
    }

    auto sync_locked(engine_sequence seq) -> void {
        const auto target = std::min(seq, _latest);
        if (!_durable.has_value()) {
            return;  // durable-on-write: already there
        }
        std::size_t done = 0;
        for (; done < _pending.size() && _pending[done].first <= target; ++done) {
            apply_batch(*_durable, _pending[done].second);
        }
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(done));
        _durable_seq = std::max(_durable_seq, target);
    }

    static auto next_chunk(const table& t, const key_range& range, scan_direction direction,
                           const std::optional<std::string>& resume)
        -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> out;
        if (range.empty()) {
            return out;
        }
        if (direction == scan_direction::forward) {
            auto it = resume.has_value() ? t.upper_bound(*resume) : t.lower_bound(range.start);
            for (; it != t.end() && out.size() < k_scan_chunk; ++it) {
                if (range.end.has_value() && it->first >= *range.end) {
                    break;
                }
                out.emplace_back(it->first, it->second);
            }
            return out;
        }
        // Reverse: start just below `resume`, or below the range end.
        auto it = resume.has_value()      ? t.lower_bound(*resume)
                  : range.end.has_value() ? t.lower_bound(*range.end)
                                          : t.end();
        while (it != t.begin() && out.size() < k_scan_chunk) {
            --it;
            if (it->first < range.start) {
                break;
            }
            out.emplace_back(it->first, it->second);
        }
        return out;
    }

    auto encode_range_locked(const key_range& range) const -> std::string {
        std::string out("KYKV");
        put_u32(out, 1);
        if (range.empty()) {
            return out;
        }
        for (const auto cf : k_all_column_families) {
            const auto& t = table_of(_live, cf);
            auto it = t.lower_bound(range.start);
            for (; it != t.end(); ++it) {
                if (range.end.has_value() && it->first >= *range.end) {
                    break;
                }
                out.push_back(static_cast<char>(cf));
                put_u32(out, static_cast<std::uint32_t>(it->first.size()));
                out.append(it->first);
                put_u32(out, static_cast<std::uint32_t>(it->second.size()));
                out.append(it->second);
            }
        }
        return out;
    }

    static auto decode_records(std::string_view in, const key_range& range, write_batch& batch)
        -> void {
        std::size_t pos = 0;
        const auto take = [&](std::size_t n) -> std::string_view {
            if (n > in.size() - pos) {
                throw persistent_storage_error("memory_kv_engine: truncated checkpoint file");
            }
            auto s = in.substr(pos, n);
            pos += n;
            return s;
        };
        const auto u32 = [&]() -> std::uint32_t {
            const auto b = take(4);
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << (8 * i);
            }
            return v;
        };
        if (take(4) != "KYKV" || u32() != 1) {
            throw persistent_storage_error("memory_kv_engine: not a checkpoint file");
        }
        while (pos < in.size()) {
            const auto cf_byte = static_cast<unsigned char>(take(1)[0]);
            if (cf_byte >= k_all_column_families.size()) {
                throw persistent_storage_error("memory_kv_engine: bad column family in checkpoint");
            }
            std::string key(take(u32()));
            std::string value(take(u32()));
            if (!range.contains(key)) {
                throw persistent_storage_error(
                    "memory_kv_engine: checkpoint key outside its range");
            }
            batch.put(static_cast<column_family>(cf_byte), std::move(key), std::move(value));
        }
    }

    static auto read_file(const std::filesystem::path& path) -> std::string {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw persistent_storage_error("memory_kv_engine: cannot open " + path.string());
        }
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    static auto put_u32(std::string& out, std::uint32_t v) -> void {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
        }
    }

    mutable std::mutex _mu;
    tables _live;
    std::optional<tables> _durable;  // state at _durable_seq while held
    std::vector<std::pair<engine_sequence, write_batch>> _pending;  // writes after _durable_seq
    engine_sequence _latest = 0;
    engine_sequence _durable_seq = 0;
};

static_assert(kv_engine<memory_kv_engine>);

}  // namespace kythira
