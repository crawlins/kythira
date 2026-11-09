// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file persistent_kv_state_machine.hpp
/// @brief The Redis gateway's command set on a `kv_engine`
///        (.kiro/specs/persistent-state-machine/ task 2, Requirements 2.1-2.4
///        and 9.1).
///
/// `redis_kv_state_machine` keeps a shard in a `std::map` and moves it as one
/// buffer on every snapshot, restart and split. This class applies the same
/// commands (`redis_kv_commands.hpp`) to a range of the node's engine instead,
/// with the same results, so the gateway can run on either one.
///
/// Layout, for a host namespace `ns` and a shard range `[start, end)`:
///
/// - `data` column family: `ns + key` → `u64 BE expire_at_ms` then the value.
/// - `meta` column family: one record at `ns + start`, the first key of the
///   group's own range, holding the applied index and the counters
///   (`approximate_size_bytes`, key count, expiring count).
///
/// Keeping the meta record inside the group's range (in its own column
/// family, so it never collides with data) means the engine's range
/// operations treat it as part of the group: a checkpoint carries the
/// leader's applied index and counters, an ingest replaces them, a drop
/// removes them, and no other group's range can contain them, since ranges
/// on one engine are disjoint. A split writes a record at each child's start;
/// a merge erases the source's record in the batch that widens the target.
/// The design sketched `<ns>/group/<id>/applied_index` instead, which needs a
/// group id this concept's `open(engine, range)` does not carry and would sit
/// inside whichever group's range happens to cover that string.
///
/// Every `apply` writes its effects and the new meta record in one batch
/// (Requirement 2.1) without syncing (2.4): the Raft log is durable, so a
/// batch lost in a power cut is replayed, and `durable_applied_index()` maps
/// the engine's durable sequence back to the highest applied index it covers
/// so the node never compacts past it (Requirement 3.1).
///
/// `apply` stays a pure function of the log, as in the in-memory machine:
/// expiry is a deadline stamped by the leader and removed by a `sweep` entry,
/// never by `apply` reading a clock.

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <kythira/redis/redis_kv_commands.hpp>
#include <kythira/redis/redis_kv_state_machine.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace persistent_kv_detail {

inline auto put_u64(std::string& out, std::uint64_t v) -> void {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
    }
}

inline auto get_u64(std::string_view in, std::size_t at) -> std::uint64_t {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8U) | static_cast<unsigned char>(in[at + i]);
    }
    return v;
}

/// The smallest string greater than every string starting with `prefix`, or
/// nullopt when there is none (empty, or all 0xFF bytes).
inline auto prefix_successor(std::string prefix) -> std::optional<std::string> {
    while (!prefix.empty()) {
        auto& last = prefix.back();
        if (static_cast<unsigned char>(last) != 0xFFU) {
            last = static_cast<char>(static_cast<unsigned char>(last) + 1U);
            return prefix;
        }
        prefix.pop_back();
    }
    return std::nullopt;
}

/// A stored data value: the deadline, then the bytes.
inline auto encode_value(std::uint64_t expire_at_ms, const std::vector<std::byte>& value)
    -> std::string {
    std::string out;
    out.reserve(8 + value.size());
    put_u64(out, expire_at_ms);
    out.append(reinterpret_cast<const char*>(value.data()), value.size());
    return out;
}

inline auto value_expiry(std::string_view stored) -> std::uint64_t {
    if (stored.size() < 8) {
        throw persistent_storage_error("persistent_kv_state_machine: truncated value record");
    }
    return get_u64(stored, 0);
}

inline auto value_size(std::string_view stored) -> std::size_t {
    return stored.size() - 8;
}

inline auto decode_value(std::string_view stored) -> std::shared_ptr<const redis_kv_value_entry> {
    auto entry = std::make_shared<redis_kv_value_entry>();
    entry->_expire_at_ms = value_expiry(stored);
    const auto body = stored.substr(8);
    entry->_value.resize(body.size());
    std::memcpy(entry->_value.data(), body.data(), body.size());
    return entry;
}

/// The per-group meta record.
struct meta_record {
    std::uint64_t applied = 0;
    std::uint64_t keys = 0;
    std::uint64_t bytes = 0;
    std::uint64_t expiring = 0;

    static constexpr std::size_t k_encoded_size = 32;

    [[nodiscard]] auto encode() const -> std::string {
        std::string out;
        out.reserve(k_encoded_size);
        put_u64(out, applied);
        put_u64(out, keys);
        put_u64(out, bytes);
        put_u64(out, expiring);
        return out;
    }

    [[nodiscard]] static auto decode(std::string_view in) -> meta_record {
        if (in.size() != k_encoded_size) {
            throw persistent_storage_error(
                "persistent_kv_state_machine: meta record has the wrong size");
        }
        return {get_u64(in, 0), get_u64(in, 8), get_u64(in, 16), get_u64(in, 24)};
    }
};

}  // namespace persistent_kv_detail

template<typename Engine = memory_kv_engine, typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class persistent_kv_state_machine {
public:
    using engine_type = Engine;
    using key_type = std::string;
    using entry_handle = std::shared_ptr<const redis_kv_value_entry>;

    /// `ns` prefixes every engine key, keeping two hosts in one process apart
    /// (design "One engine per node, groups as key ranges").
    explicit persistent_kv_state_machine(std::string ns = {}) : _ns(std::move(ns)) {}

    // ---- persistent_state_machine -----------------------------------------

    /// Attaches to `range` of `engine` (in shard-key space) and reads its meta
    /// record back. Nothing else is read (Requirement 4.1). What was recovered
    /// is durable, so the durable applied index starts there.
    auto open(engine_type& engine, const key_range& range) -> void {
        _engine = &engine;
        _range = range;
        _seqs.clear();
        discard_staged();
        const auto stored = engine.get(column_family::meta, meta_key());
        _meta = stored.has_value() ? persistent_kv_detail::meta_record::decode(*stored)
                                   : persistent_kv_detail::meta_record{};
        _durable_floor = static_cast<LogIndex>(_meta.applied);
    }

    /// Applies one committed entry and records `index` in the same batch
    /// (Requirement 2.1). An index at or below `applied_index()` is a replay
    /// of something the engine already holds and changes nothing (2.3). The
    /// reply is the in-memory machine's: one byte, 1 if the entry changed
    /// anything.
    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        require_open();
        if (static_cast<std::uint64_t>(index) <= _meta.applied) {
            return {std::byte{0}};
        }
        write_batch batch;
        auto reply = stage(command, batch);
        commit_staged(batch, index);
        return reply;
    }

    // ---- persistent_staged_state_machine ----------------------------------

    /// Adds `command`'s effects to `batch` (Requirement 8.1). Later `stage`
    /// calls of the same entry read what this one wrote, and the counters
    /// carry over until `commit_staged`.
    auto stage(const std::vector<std::byte>& command, write_batch& batch)
        -> std::vector<std::byte> {
        require_open();
        if (!_staged.has_value()) {
            _pending.clear();
            _staged = _meta;
        }
        auto decoded = decode_redis_kv_command(command);
        const bool changed =
            std::visit([&](auto& c) { return apply_one(c, batch, *_staged); }, decoded);
        return {changed ? std::byte{1} : std::byte{0}};
    }

    /// Writes `batch` with `index` as the applied index and the staged
    /// counters, in one write.
    auto commit_staged(write_batch& batch, LogIndex index) -> void {
        require_open();
        auto next = _staged.value_or(_meta);
        _staged.reset();
        next.applied = static_cast<std::uint64_t>(index);
        batch.put(column_family::meta, meta_key(), next.encode());
        commit(batch, next);
    }

    auto discard_staged() -> void {
        _staged.reset();
        _pending.clear();
    }

    [[nodiscard]] auto key_prefix() const noexcept -> const std::string& { return _ns; }

    [[nodiscard]] auto applied_index() const -> LogIndex {
        return static_cast<LogIndex>(_meta.applied);
    }

    /// The highest applied index whose batch the engine has made durable.
    [[nodiscard]] auto durable_applied_index() const -> LogIndex {
        if (_engine == nullptr) {
            return _durable_floor;
        }
        const auto durable = _engine->durable_sequence();
        while (!_seqs.empty() && _seqs.front().second <= durable) {
            _durable_floor = _seqs.front().first;
            _seqs.pop_front();
        }
        return _durable_floor;
    }

    /// Makes every batch applied so far durable. The node calls it before
    /// compacting when the durable index lags by more than
    /// `raft_configuration::_persistent_sync_lag_entries`; without it an
    /// engine that syncs only when asked (RocksDB) never advances the
    /// durable index, and the group's log is never compacted.
    auto sync_durable() -> void {
        require_open();
        if (!_seqs.empty()) {
            _engine->sync(_seqs.back().second);
        }
    }

    /// A checkpoint of the whole range at `applied_index()`, meta record
    /// included, for a follower (file-install-snapshot).
    auto checkpoint(const std::filesystem::path& dir) -> snapshot_manifest {
        require_open();
        return _engine->checkpoint(engine_range(), dir);
    }

    /// Replaces the range with a leader's checkpoint taken at `index`. The
    /// checkpoint brought the leader's meta record; the applied index is set
    /// to `index` in a second batch, and a crash between the two leaves the
    /// leader's own applied index, which is the checkpoint's point too.
    auto ingest(const std::filesystem::path& dir, const snapshot_manifest& manifest, LogIndex index)
        -> void {
        require_open();
        _engine->ingest(engine_range(), dir, manifest);
        const auto stored = _engine->get(column_family::meta, meta_key());
        auto next = stored.has_value() ? persistent_kv_detail::meta_record::decode(*stored)
                                       : persistent_kv_detail::meta_record{};
        next.applied = static_cast<std::uint64_t>(index);
        write_batch batch;
        batch.put(column_family::meta, meta_key(), next.encode());
        commit(batch, next);
    }

    /// Deletes every key of the range, meta record included.
    auto drop() -> void {
        require_open();
        _engine->drop(engine_range());
        _meta = {};
        _seqs.clear();
        _durable_floor = LogIndex{};
    }

    // ---- persistent_splittable_state_machine -------------------------------

    /// Narrows this range to the part below the first cut and returns the
    /// child ranges in key order. One batch writes a meta record at each
    /// child's start, carrying this group's applied index (a child's applied
    /// index starts at the split entry's, as a derived child's does today)
    /// and the counters of its part. No data key moves; the counters are
    /// recounted by a scan of each part.
    auto split_at(const std::vector<std::string>& keys) -> std::vector<key_range> {
        require_open();
        std::vector<std::string> cuts = keys;
        std::ranges::sort(cuts);
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::erase_if(cuts, [&](const std::string& k) { return !_range.contains(k); });
        if (cuts.empty() || cuts.front() == _range.start) {
            if (!cuts.empty()) {
                throw std::invalid_argument(
                    "persistent_kv_state_machine: split at the range start");
            }
            return {};
        }

        std::vector<key_range> children;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            children.push_back(key_range{cuts[i], i + 1 < cuts.size()
                                                      ? std::optional<std::string>(cuts[i + 1])
                                                      : _range.end});
        }
        write_batch batch;
        for (const auto& child : children) {
            auto m = count(child);
            m.applied = _meta.applied;
            batch.put(column_family::meta, _ns + child.start, m.encode());
        }
        _range.end = cuts.front();
        auto next = count(_range);
        next.applied = _meta.applied;
        batch.put(column_family::meta, meta_key(), next.encode());
        commit(batch, next);
        return children;
    }

    /// Widens this range over the adjacent `range`, above or below it,
    /// already in the same engine. The source's meta record is folded into
    /// this one in one batch; no data key moves. Adopting from below moves
    /// this group's record to the new start, which is the source's key, so
    /// the source's record is overwritten rather than erased. Adopting a
    /// range already covered is a no-op (design "Split and merge",
    /// idempotence).
    auto adopt(const key_range& range) -> void {
        require_open();
        if (_range.contains(range)) {
            return;
        }
        const bool above = _range.end.has_value() && range.start == *_range.end;
        const bool below = range.end.has_value() && *range.end == _range.start;
        if (!above && !below) {
            throw std::invalid_argument(
                "persistent_kv_state_machine: adopt of a range that is not adjacent");
        }
        const auto source_key = _ns + range.start;
        const auto stored = _engine->get(column_family::meta, source_key);
        const auto source = stored.has_value() ? persistent_kv_detail::meta_record::decode(*stored)
                                               : persistent_kv_detail::meta_record{};
        auto next = _meta;
        next.keys += source.keys;
        next.bytes += source.bytes;
        next.expiring += source.expiring;
        write_batch batch;
        if (above) {
            batch.erase(column_family::meta, source_key);
            _range.end = range.end;
        } else {
            batch.erase(column_family::meta, meta_key());
            _range.start = range.start;
        }
        batch.put(column_family::meta, meta_key(), next.encode());
        commit(batch, next);
    }

    // ---- splittable_state_machine's size hints -----------------------------

    [[nodiscard]] auto approximate_size_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>(_meta.bytes);
    }
    [[nodiscard]] auto approximate_key_count() const noexcept -> std::size_t {
        return static_cast<std::size_t>(_meta.keys);
    }
    [[nodiscard]] auto expiring_key_count() const noexcept -> std::size_t {
        return static_cast<std::size_t>(_meta.expiring);
    }

    /// The in-memory machine's choice: evenly spaced by key count, never the
    /// first key.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) const -> std::vector<key_type> {
        std::vector<key_type> out;
        const auto n = approximate_key_count();
        if (max == 0 || n < 2) {
            return out;
        }
        const std::size_t wanted = std::min(max, n - 1);
        const std::size_t stride = n / (wanted + 1);
        if (stride == 0) {
            return out;
        }
        std::size_t index = 0;
        for_each_engine_key([&](std::string_view key, std::string_view) {
            ++index;
            if (out.size() < wanted && index % stride == 0 && index < n) {
                out.emplace_back(key);
            }
            return out.size() < wanted;
        });
        return out;
    }

    [[nodiscard]] auto can_split_at([[maybe_unused]] const key_type& key) const -> bool {
        return true;
    }

    // ---- read path (the in-memory machine's API) ---------------------------

    /// A handle to a value, read from the engine; null if absent. Like the
    /// in-memory machine it ignores the deadline.
    [[nodiscard]] auto lookup(std::string_view key) const -> entry_handle {
        require_open();
        const auto stored = _engine->get(column_family::data, _ns + std::string(key));
        return stored.has_value() ? persistent_kv_detail::decode_value(*stored) : nullptr;
    }

    /// Keys whose deadline is non-zero and at or before `now_ms`, in key
    /// order, at most `max`.
    [[nodiscard]] auto collect_expired(std::uint64_t now_ms, std::size_t max) const
        -> std::vector<redis_kv_sweep_entry> {
        std::vector<redis_kv_sweep_entry> out;
        if (max == 0) {
            return out;
        }
        for_each_engine_key([&](std::string_view key, std::string_view stored) {
            const auto expire = persistent_kv_detail::value_expiry(stored);
            if (expire != 0 && expire <= now_ms) {
                out.push_back({std::string(key), expire});
            }
            return out.size() < max;
        });
        return out;
    }

    /// Every key in order with its entry.
    template<typename F> auto for_each_key(F&& f) const -> void {
        for_each_engine_key([&](std::string_view key, std::string_view stored) {
            const auto entry = persistent_kv_detail::decode_value(stored);
            f(std::string(key), *entry);
            return true;
        });
    }

    [[nodiscard]] auto range() const noexcept -> const key_range& { return _range; }

private:
    // ---- commands ----------------------------------------------------------

    auto apply_one(redis_kv_set_command& c, write_batch& batch,
                   persistent_kv_detail::meta_record& m) -> bool {
        const auto key = _ns + c._key;
        forget(key, m);
        auto encoded = persistent_kv_detail::encode_value(c._expire_at_ms, c._value);
        batch.put(column_family::data, key, encoded);
        _pending[key] = std::move(encoded);
        m.keys += 1;
        m.bytes += c._key.size() + c._value.size();
        m.expiring += c._expire_at_ms != 0 ? 1 : 0;
        return true;
    }

    auto apply_one(const redis_kv_del_command& c, write_batch& batch,
                   persistent_kv_detail::meta_record& m) -> bool {
        return erase(c._key, batch, m);
    }

    /// Deletes each key only if its stored deadline still equals the one the
    /// sweep was built from, as the in-memory machine does.
    auto apply_one(const redis_kv_sweep_command& c, write_batch& batch,
                   persistent_kv_detail::meta_record& m) -> bool {
        bool changed = false;
        for (const auto& e : c._entries) {
            if (e._expire_at_ms == 0) {
                continue;
            }
            const auto stored = current(_ns + e._key);
            if (stored.has_value() &&
                persistent_kv_detail::value_expiry(*stored) == e._expire_at_ms) {
                changed = erase(e._key, batch, m) || changed;
            }
        }
        return changed;
    }

    /// The disk-backed store's records; no-ops here as in the in-memory
    /// machine.
    auto apply_one(const redis_kv_put_chunk_command&, write_batch&,
                   persistent_kv_detail::meta_record&) -> bool {
        return false;
    }
    auto apply_one(const redis_kv_commit_command&, write_batch&, persistent_kv_detail::meta_record&)
        -> bool {
        return false;
    }
    auto apply_one(const redis_kv_touch_command&, write_batch&, persistent_kv_detail::meta_record&)
        -> bool {
        return false;
    }

    auto apply_one(const redis_kv_evict_command& c, write_batch& batch,
                   persistent_kv_detail::meta_record& m) -> bool {
        bool changed = false;
        for (const auto& k : c._keys) {
            changed = erase(k, batch, m) || changed;
        }
        return changed;
    }

    // ---- helpers -----------------------------------------------------------

    /// The value of `engine_key` as this entry has left it so far. A batch
    /// is applied only at commit, so later commands of the same entry (a
    /// sweep or evict naming one key twice) must see earlier ones' effects.
    [[nodiscard]] auto current(const std::string& engine_key) const -> std::optional<std::string> {
        if (const auto it = _pending.find(engine_key); it != _pending.end()) {
            return it->second;
        }
        return _engine->get(column_family::data, engine_key);
    }

    /// Takes an existing value's contribution out of the counters.
    auto forget(const std::string& engine_key, persistent_kv_detail::meta_record& m) -> void {
        const auto stored = current(engine_key);
        if (stored.has_value()) {
            m.keys -= 1;
            m.bytes -= (engine_key.size() - _ns.size()) + persistent_kv_detail::value_size(*stored);
            m.expiring -= persistent_kv_detail::value_expiry(*stored) != 0 ? 1 : 0;
        }
    }

    auto erase(const std::string& key, write_batch& batch, persistent_kv_detail::meta_record& m)
        -> bool {
        const auto engine_key = _ns + key;
        const auto stored = current(engine_key);
        if (!stored.has_value()) {
            return false;
        }
        m.keys -= 1;
        m.bytes -= key.size() + persistent_kv_detail::value_size(*stored);
        m.expiring -= persistent_kv_detail::value_expiry(*stored) != 0 ? 1 : 0;
        batch.erase(column_family::data, engine_key);
        _pending[engine_key] = std::nullopt;
        return true;
    }

    auto commit(const write_batch& batch, const persistent_kv_detail::meta_record& next) -> void {
        _pending.clear();
        const auto seq = _engine->write(batch);
        _meta = next;
        _seqs.emplace_back(static_cast<LogIndex>(next.applied), seq);
    }

    /// Counts the keys, bytes and deadlines of a shard-key range.
    [[nodiscard]] auto count(const key_range& range) const -> persistent_kv_detail::meta_record {
        persistent_kv_detail::meta_record m;
        _engine->scan(column_family::data, to_engine(range), scan_direction::forward,
                      [&](std::string_view key, std::string_view stored) {
                          m.keys += 1;
                          m.bytes +=
                              (key.size() - _ns.size()) + persistent_kv_detail::value_size(stored);
                          m.expiring += persistent_kv_detail::value_expiry(stored) != 0 ? 1 : 0;
                          return true;
                      });
        return m;
    }

    /// Visits the data keys of this range in order, with the namespace
    /// stripped.
    template<typename F> auto for_each_engine_key(F&& f) const -> void {
        require_open();
        _engine->scan(column_family::data, engine_range(), scan_direction::forward,
                      [&](std::string_view key, std::string_view stored) {
                          return f(key.substr(_ns.size()), stored);
                      });
    }

    [[nodiscard]] auto to_engine(const key_range& range) const -> key_range {
        return key_range{_ns + range.start, range.end.has_value()
                                                ? std::optional<std::string>(_ns + *range.end)
                                                : persistent_kv_detail::prefix_successor(_ns)};
    }

    [[nodiscard]] auto engine_range() const -> key_range { return to_engine(_range); }
    [[nodiscard]] auto meta_key() const -> std::string { return _ns + _range.start; }

    auto require_open() const -> void {
        if (_engine == nullptr) {
            throw std::logic_error("persistent_kv_state_machine: used before open()");
        }
    }

    std::string _ns;
    engine_type* _engine = nullptr;
    key_range _range;
    persistent_kv_detail::meta_record _meta;
    mutable LogIndex _durable_floor{};
    /// (applied index, engine sequence) of each batch not yet known durable.
    mutable std::deque<std::pair<LogIndex, engine_sequence>> _seqs;
    /// Data keys this entry has already written (nullopt = erased), so later
    /// commands of the same entry read them; cleared at commit.
    std::map<std::string, std::optional<std::string>, std::less<>> _pending;
    /// The counters as the entry being staged has left them; empty between
    /// entries.
    std::optional<persistent_kv_detail::meta_record> _staged;
};

}  // namespace kythira
