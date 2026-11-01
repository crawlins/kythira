// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file cache_kv_state_machine.hpp
/// @brief The disk-backed build cache's replicated state: an index in RAM,
///        value bytes in the node's chunk store
///        (.kiro/specs/disk-backed-build-cache/ design Components 2 and 3).
///
/// It has the same public surface as `redis_kv_state_machine` (`lookup`,
/// `collect_expired`, `for_each_key`, the split and absorb hooks), so the
/// gateway can run on either. What differs is what a key maps to: a
/// `cache_manifest` naming the value's chunks by SHA-256, not the bytes.
///
/// **What is replicated.** The index (key → manifest) and the pending table
/// (chunks stored by `put_chunk` that no `commit` has bound yet, with the log
/// index that stored them). `get_state()` serializes exactly those, so a
/// snapshot costs about the size of the index, not of the values
/// (Requirement 4.1). Which chunks are on this node's disk is *not*
/// replicated, and never changes the index (Requirement 2.3): a replica that
/// is missing a chunk still binds a `commit` that names it, as long as the
/// chunk is pending or referenced, and simply cannot serve that key until the
/// fetcher brings the bytes (Requirement 4.4, the `has_all` bit).
///
/// **References.** Every occurrence of a chunk in a committed manifest, and
/// every pending chunk, holds one reference in the node-wide
/// `chunk_refcounts`, which all replicas on the node share. When the count
/// reaches zero the chunk file is deleted (Requirement 3.5). A replica keeps
/// no per-chunk count of its own (that would cost a second hash-map node per
/// chunk against Requirement 2.2); a restore computes its difference from the
/// index when it needs one.
///
/// **The orphan grace** is counted in log entries
/// (`KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES`), so every replica drops the same
/// pending chunks at the same entry (Requirement 3.6). A pending chunk stays
/// pending for the whole grace period even after a `commit` binds it: another
/// upload of the same bytes may be in flight, and the manifest's own reference
/// keeps it alive afterwards anyway.
///
/// **`set`** (the RESP path, format 0x01) is one implicit `put_chunk` per
/// 4 MiB piece plus a commit, in one entry, so RESP `SET` stays one proposal.
/// The piece size is a constant, not the configured chunk size: the chunk
/// list is replicated state, and a node configured differently would
/// otherwise build a different manifest from the same entry.
///
/// **Errors.** A chunk that cannot be written throws out of `apply()`
/// (`cache_chunk_store_error`); the node must stop rather than skip the entry
/// (Requirement 2.4). A `put_chunk` whose bytes do not hash to its name, and
/// a `commit` naming a chunk that is not pending (the grace period expired
/// mid-upload), are deterministic no-ops that `apply` reports with a zero
/// status byte.

#include <raft/cache_chunk_store.hpp>
#include <raft/redis_kv_commands.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// RESP `SET` values are cut into pieces of this size. Fixed, because the
/// resulting chunk list is replicated state (see the file comment).
inline constexpr std::size_t cache_set_piece_bytes = std::size_t{4} * 1024 * 1024;

class cache_manifest_ptr;

/// One key's value: its chunks in order, plus what a reader checks them
/// against. Immutable once published, like `redis_kv_value_entry`, except for
/// the `has_all` bit, which is local to this node and not replicated.
///
/// Laid out for Requirement 2.2's 256 bytes per key (56 bytes for a
/// one-chunk value, one allocation, no control block):
///
/// - Most values are one chunk, and a one-chunk value's chunk *is* the value,
///   with the same hash, so that case stores no chunk list: `chunks()`
///   returns the value hash as a one-element span. A longer list is stored
///   in the same allocation, after the object.
/// - The reference count is intrusive (`cache_manifest_ptr`), which saves the
///   separate control block a `shared_ptr` would add.
/// - The `has_all` bit and the stored-list bit share a word with the chunk
///   count.
///
/// Built only by `make_cache_manifest`.
class cache_manifest {
public:
    /// ms since the Unix epoch; 0 = never expires.
    std::uint64_t _expire_at_ms = 0;
    std::uint64_t _total_len = 0;
    chunk_sha _value_sha{};

    cache_manifest(const cache_manifest&) = delete;
    auto operator=(const cache_manifest&) -> cache_manifest& = delete;
    cache_manifest(cache_manifest&&) = delete;
    auto operator=(cache_manifest&&) -> cache_manifest& = delete;
    ~cache_manifest() = default;

    /// The chunks, in order. Valid while the manifest is.
    [[nodiscard]] auto chunks() const noexcept -> std::span<const chunk_sha> {
        const auto n = chunk_count();
        if (n == 1 && (_count_and_flag.load(std::memory_order_relaxed) & stored_bit) == 0) {
            return {&_value_sha, 1};
        }
        return {trailing(), n};
    }

    /// Whether every chunk is on this node's disk. Set when the manifest is
    /// bound or restored, and by the fetcher as chunks arrive. A read that
    /// finds it false forwards to the leader or answers a miss (Requirement
    /// 4.4); the chunks are hash-checked on read regardless.
    [[nodiscard]] auto has_all() const noexcept -> bool {
        return (_count_and_flag.load(std::memory_order_acquire) & has_all_bit) != 0;
    }
    void set_has_all(bool all) const noexcept {
        if (all) {
            _count_and_flag.fetch_or(has_all_bit, std::memory_order_acq_rel);
        } else {
            _count_and_flag.fetch_and(~has_all_bit, std::memory_order_acq_rel);
        }
    }

private:
    friend class cache_manifest_ptr;
    friend auto make_cache_manifest(std::uint64_t, std::uint64_t, const chunk_sha&,
                                    std::span<const chunk_sha>) -> cache_manifest_ptr;

    static constexpr std::uint32_t has_all_bit = 0x80000000U;
    /// The list is stored after the object (clear only for the one-chunk
    /// value whose chunk is the value).
    static constexpr std::uint32_t stored_bit = 0x40000000U;
    static constexpr std::uint32_t count_mask = 0x3FFFFFFFU;

    cache_manifest() = default;

    [[nodiscard]] auto chunk_count() const noexcept -> std::uint32_t {
        return _count_and_flag.load(std::memory_order_relaxed) & count_mask;
    }
    [[nodiscard]] auto trailing() const noexcept -> const chunk_sha* {
        return reinterpret_cast<const chunk_sha*>(this + 1);
    }
    [[nodiscard]] auto trailing() noexcept -> chunk_sha* {
        return reinterpret_cast<chunk_sha*>(this + 1);
    }

    mutable std::atomic<std::uint32_t> _refs{0};
    /// Chunk count, `stored_bit` and `has_all_bit`.
    mutable std::atomic<std::uint32_t> _count_and_flag{0};
};

static_assert(alignof(cache_manifest) >= alignof(chunk_sha));
static_assert(sizeof(cache_manifest) <= 56,
              "one malloc size class above 56 bytes costs 16 per key");

/// A shared, intrusively counted handle to an immutable manifest. The gateway
/// takes one under the node lock and streams the chunks after releasing it,
/// exactly as it does with `redis_kv_state_machine`'s `shared_ptr` handles.
class cache_manifest_ptr {
public:
    cache_manifest_ptr() noexcept = default;
    cache_manifest_ptr(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    cache_manifest_ptr(const cache_manifest_ptr& o) noexcept : _m(o._m) { add_ref(); }
    cache_manifest_ptr(cache_manifest_ptr&& o) noexcept : _m(std::exchange(o._m, nullptr)) {}
    auto operator=(cache_manifest_ptr o) noexcept -> cache_manifest_ptr& {
        std::swap(_m, o._m);
        return *this;
    }
    ~cache_manifest_ptr() { drop(); }

    [[nodiscard]] auto get() const noexcept -> const cache_manifest* { return _m; }
    auto operator->() const noexcept -> const cache_manifest* { return _m; }
    auto operator*() const noexcept -> const cache_manifest& { return *_m; }
    explicit operator bool() const noexcept { return _m != nullptr; }
    friend auto operator==(const cache_manifest_ptr& a, std::nullptr_t) noexcept -> bool {
        return a._m == nullptr;
    }
    friend auto operator==(const cache_manifest_ptr& a, const cache_manifest_ptr& b) noexcept
        -> bool {
        return a._m == b._m;
    }

private:
    friend auto make_cache_manifest(std::uint64_t, std::uint64_t, const chunk_sha&,
                                    std::span<const chunk_sha>) -> cache_manifest_ptr;
    /// Adopt a manifest whose count is still zero.
    explicit cache_manifest_ptr(cache_manifest* m) noexcept : _m(m) { add_ref(); }

    void add_ref() const noexcept {
        if (_m != nullptr) {
            _m->_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void drop() noexcept {
        if (_m != nullptr && _m->_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            _m->~cache_manifest();
            ::operator delete(static_cast<void*>(_m));
        }
        _m = nullptr;
    }
    cache_manifest* _m = nullptr;
};

/// Build a manifest: one allocation holding the object and, unless the value
/// is its own single chunk, the chunk list after it.
[[nodiscard]] inline auto make_cache_manifest(std::uint64_t expire_at_ms, std::uint64_t total_len,
                                              const chunk_sha& value_sha,
                                              std::span<const chunk_sha> chunks)
    -> cache_manifest_ptr {
    if (chunks.size() > cache_manifest::count_mask) {
        throw std::length_error("cache_manifest: too many chunks");
    }
    const bool stored = chunks.size() != 1 || chunks[0] != value_sha;
    const std::size_t extra = stored ? chunks.size() * sizeof(chunk_sha) : 0;
    void* raw = ::operator new(sizeof(cache_manifest) + extra);
    auto* m = new (raw) cache_manifest();
    m->_expire_at_ms = expire_at_ms;
    m->_total_len = total_len;
    m->_value_sha = value_sha;
    m->_count_and_flag.store(
        static_cast<std::uint32_t>(chunks.size()) | (stored ? cache_manifest::stored_bit : 0U),
        std::memory_order_relaxed);
    if (stored && !chunks.empty()) {
        std::memcpy(static_cast<void*>(m->trailing()), chunks.data(), extra);
    }
    return cache_manifest_ptr(m);
}

struct cache_kv_options {
    /// Log entries an unbound chunk survives (`KYTHIRA_CACHE_ORPHAN_GRACE_ENTRIES`).
    std::uint64_t _orphan_grace_entries = 100000;
};

/// Read a whole value back from the store, checking each chunk against its
/// name and the result against `_value_sha`. `nullopt` if any chunk is
/// missing or corrupt. Used by tests and small RESP reads; the HTTP front end
/// streams chunk by chunk instead.
[[nodiscard]] inline auto read_cache_value(const cache_chunk_store& store,
                                           const cache_manifest& manifest)
    -> std::optional<std::vector<std::byte>> {
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(manifest._total_len));
    for (const auto& sha : manifest.chunks()) {
        auto bytes = store.read(sha);
        if (!bytes) {
            return std::nullopt;
        }
        out.insert(out.end(), bytes->begin(), bytes->end());
    }
    if (out.size() != manifest._total_len || sha256_of(out) != manifest._value_sha) {
        return std::nullopt;
    }
    return out;
}

template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class cache_kv_state_machine {
public:
    using key_type = std::string;
    using entry_handle = cache_manifest_ptr;
    using index_type = std::map<key_type, entry_handle, std::less<>>;

    /// `refs` is the node-wide count over the node's one chunk store, shared
    /// by every replica on the node.
    explicit cache_kv_state_machine(std::shared_ptr<chunk_refcounts> refs,
                                    cache_kv_options options = {})
        : _refs(std::move(refs)), _options(options) {
        if (!_refs) {
            throw std::invalid_argument("cache_kv_state_machine: no chunk_refcounts");
        }
    }

    cache_kv_state_machine(const cache_kv_state_machine&) = delete;
    auto operator=(const cache_kv_state_machine&) -> cache_kv_state_machine& = delete;

    cache_kv_state_machine(cache_kv_state_machine&& other) noexcept
        : _refs(std::move(other._refs)),
          _options(other._options),
          _index(std::move(other._index)),
          _pending(std::move(other._pending)),
          _pending_by_age(std::move(other._pending_by_age)),
          _bytes(other._bytes),
          _expiring(other._expiring),
          _last_applied_index(other._last_applied_index) {
        other.clear_after_move();
    }

    auto operator=(cache_kv_state_machine&& other) noexcept -> cache_kv_state_machine& {
        if (this != &other) {
            forget_all();
            _refs = std::move(other._refs);
            _options = other._options;
            _index = std::move(other._index);
            _pending = std::move(other._pending);
            _pending_by_age = std::move(other._pending_by_age);
            _bytes = other._bytes;
            _expiring = other._expiring;
            _last_applied_index = other._last_applied_index;
            other.clear_after_move();
        }
        return *this;
    }

    /// Lets go of this replica's references without deleting any chunk: a
    /// replica destroyed on shutdown, or after a merge, has not deleted its
    /// keys.
    ~cache_kv_state_machine() { forget_all(); }

    // ---- kythira::state_machine -------------------------------------------

    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        _last_applied_index = index;
        expire_pending(index);
        auto decoded = decode_redis_kv_command(command);
        const bool changed =
            std::visit([this, index](auto& c) { return apply_one(c, index); }, decoded);
        return {changed ? std::byte{1} : std::byte{0}};
    }

    /// The index and pending table in key order, in design.md Component 3's
    /// format. Also the format of every `split_state` blob.
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return serialize(_index.begin(), _index.end(), _index.size(), _pending);
    }

    /// Swap in a snapshot's index. References are adjusted by the difference
    /// (new ones taken before old ones dropped, so a chunk both use is never
    /// deleted in between). Chunks the snapshot names that are not on disk
    /// are left to the fetcher (`missing_chunks()`); this returns without
    /// waiting for them (Requirement 4.3).
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index)
        -> void {
        index_type incoming;
        pending_type pending;
        deserialize(snapshot_data, incoming, pending);

        const auto before = count_references(_index, _pending);
        const auto after = count_references(incoming, pending);
        for (const auto& [sha, n] : after) {
            auto it = before.find(sha);
            const std::uint64_t had = it == before.end() ? 0 : it->second;
            if (n > had) {
                _refs->acquire(sha, n - had);
            }
        }
        for (const auto& [sha, n] : before) {
            auto it = after.find(sha);
            const std::uint64_t keeps = it == after.end() ? 0 : it->second;
            if (n > keeps) {
                _refs->release(sha, n - keeps);
            }
        }

        _index = std::move(incoming);
        _pending = std::move(pending);
        _pending_by_age.clear();
        for (const auto& [sha, applied] : _pending) {
            _pending_by_age.emplace(applied, sha);
        }
        _bytes = 0;
        _expiring = 0;
        for (const auto& [key, m] : _index) {
            _bytes += static_cast<std::size_t>(m->_total_len);
            _expiring += m->_expire_at_ms != 0 ? 1 : 0;
            refresh_has_all(*m);
        }
        _last_applied_index = index;
    }

    [[nodiscard]] auto last_applied_index() const noexcept -> LogIndex {
        return _last_applied_index;
    }

    // ---- kythira::splittable_state_machine ---------------------------------

    /// The sum of the values' lengths (design Component 3): what drives
    /// splitting and the disk budget.
    [[nodiscard]] auto approximate_size_bytes() const noexcept -> std::size_t { return _bytes; }
    [[nodiscard]] auto approximate_key_count() const noexcept -> std::size_t {
        return _index.size();
    }
    [[nodiscard]] auto expiring_key_count() const noexcept -> std::size_t { return _expiring; }

    /// Evenly spaced by key count, never the first key; as in
    /// `redis_kv_state_machine`.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<key_type> {
        std::vector<key_type> out;
        if (max == 0 || _index.size() < 2) {
            return out;
        }
        const std::size_t wanted = std::min(max, _index.size() - 1);
        const std::size_t stride = _index.size() / (wanted + 1);
        if (stride == 0) {
            return out;
        }
        std::size_t position = 0;
        std::size_t taken = 0;
        for (const auto& [key, m] : _index) {
            ++position;
            if (taken < wanted && position % stride == 0 && position < _index.size()) {
                out.push_back(key);
                ++taken;
            }
        }
        return out;
    }

    /// Keys are independent of one another, so every cut is legal.
    [[nodiscard]] auto can_split_at([[maybe_unused]] const key_type& key) const -> bool {
        return true;
    }

    /// Cut the index at `keys`. Pending chunks belong to no key yet, so every
    /// child receives the whole pending table: whichever child the eventual
    /// `commit` lands in still finds its chunks, and the others drop theirs
    /// when the grace period passes.
    [[nodiscard]] auto split_state(const std::vector<key_type>& keys)
        -> std::vector<std::vector<std::byte>> {
        std::vector<key_type> cuts = keys;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::vector<std::vector<std::byte>> out;
        out.reserve(cuts.size() + 1);
        auto it = _index.begin();
        for (const auto& cut : cuts) {
            auto begin = it;
            std::size_t n = 0;
            while (it != _index.end() && it->first < cut) {
                ++it;
                ++n;
            }
            out.push_back(serialize(begin, it, n, _pending));
        }
        out.push_back(serialize(
            it, _index.end(), static_cast<std::size_t>(std::distance(it, _index.end())), _pending));
        return out;
    }

    /// Take on a sibling's keys and pending chunks. Incoming keys win on a
    /// collision (which siblings of one split cannot have); a pending chunk
    /// both hold keeps the later index.
    auto absorb(const std::vector<std::byte>& other_state,
                [[maybe_unused]] const shard_range<key_type>& range) -> void {
        index_type incoming;
        pending_type pending;
        deserialize(other_state, incoming, pending);
        for (const auto& [sha, applied] : pending) {
            auto it = _pending.find(sha);
            if (it == _pending.end()) {
                _pending.emplace(sha, applied);
                _pending_by_age.emplace(applied, sha);
                _refs->acquire(sha);
            } else if (applied > it->second) {
                _pending_by_age.erase({it->second, sha});
                it->second = applied;
                _pending_by_age.emplace(applied, sha);
            }
        }
        for (auto& [key, m] : incoming) {
            for (const auto& sha : m->chunks()) {
                _refs->acquire(sha);
            }
            refresh_has_all(*m);
            bind(key, std::move(m));
        }
    }

    // ---- read path ---------------------------------------------------------

    /// The manifest handle, or null. Like `redis_kv_state_machine::lookup` it
    /// ignores the deadline; the caller checks it against its own clock. The
    /// caller also checks `has_all()` before serving the bytes itself.
    [[nodiscard]] auto lookup(std::string_view key) const -> entry_handle {
        auto it = _index.find(key);
        return it == _index.end() ? nullptr : it->second;
    }

    [[nodiscard]] auto collect_expired(std::uint64_t now_ms, std::size_t max) const
        -> std::vector<redis_kv_sweep_entry> {
        std::vector<redis_kv_sweep_entry> out;
        for (const auto& [key, m] : _index) {
            if (out.size() >= max) {
                break;
            }
            if (m->_expire_at_ms != 0 && m->_expire_at_ms <= now_ms) {
                out.push_back({key, m->_expire_at_ms});
            }
        }
        return out;
    }

    template<typename F> auto for_each_key(F&& f) const -> void {
        for (const auto& [key, m] : _index) {
            f(key, *m);
        }
    }

    // ---- chunk bookkeeping, for the fetcher and diagnostics ----------------

    /// Chunks this replica references whose files are not on disk.
    [[nodiscard]] auto missing_chunks() const -> std::vector<chunk_sha> {
        std::vector<chunk_sha> out;
        for (const auto& [sha, n] : count_references(_index, _pending)) {
            if (!_refs->store().contains(sha)) {
                out.push_back(sha);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /// Recompute every manifest's `has_all()` (after the fetcher stored some
    /// chunks, or a repair dropped one). Returns how many keys are servable.
    auto refresh_has_all() -> std::size_t {
        std::size_t servable = 0;
        for (const auto& [key, m] : _index) {
            servable += refresh_has_all(*m) ? 1 : 0;
        }
        return servable;
    }

    /// Distinct chunks this replica references (pending or committed). Walks
    /// the index; for diagnostics and tests.
    [[nodiscard]] auto referenced_chunk_count() const -> std::size_t {
        return count_references(_index, _pending).size();
    }
    [[nodiscard]] auto pending_chunk_count() const noexcept -> std::size_t {
        return _pending.size();
    }

    [[nodiscard]] auto chunk_store() const -> const cache_chunk_store& { return _refs->store(); }
    [[nodiscard]] auto refcounts() const -> const std::shared_ptr<chunk_refcounts>& {
        return _refs;
    }

private:
    using pending_type = std::map<chunk_sha, LogIndex>;
    using count_type = std::unordered_map<chunk_sha, std::uint64_t, chunk_sha_hash>;

    /// This replica's references per chunk: one per occurrence in a manifest,
    /// plus one while pending. Not kept in RAM between calls (it would cost
    /// another hash-map node per chunk, Requirement 2.2); the node-wide
    /// counts are updated occurrence by occurrence instead, and this is
    /// rebuilt when a restore needs the difference.
    [[nodiscard]] static auto count_references(const index_type& index, const pending_type& pending)
        -> count_type {
        count_type out;
        for (const auto& [key, m] : index) {
            for (const auto& sha : m->chunks()) {
                ++out[sha];
            }
        }
        for (const auto& [sha, applied] : pending) {
            ++out[sha];
        }
        return out;
    }

    void forget_all() noexcept {
        if (!_refs) {
            return;
        }
        try {
            for (const auto& [sha, n] : count_references(_index, _pending)) {
                _refs->forget(sha, n);
            }
        } catch (...) {
            // A count that disagrees with ours is a bookkeeping bug, but a
            // destructor is no place to report it; the next start rebuilds
            // the counts from the snapshots.
        }
    }

    void clear_after_move() noexcept {
        _index.clear();
        _pending.clear();
        _pending_by_age.clear();
        _bytes = 0;
        _expiring = 0;
    }

    // ---- pending -----------------------------------------------------------

    /// Drop pending chunks stored more than the grace period of entries ago.
    void expire_pending(LogIndex now) {
        const auto grace = _options._orphan_grace_entries;
        while (!_pending_by_age.empty()) {
            const auto [applied, sha] = *_pending_by_age.begin();
            if (static_cast<std::uint64_t>(now) <= static_cast<std::uint64_t>(applied) ||
                static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(applied) <= grace) {
                break;
            }
            _pending_by_age.erase(_pending_by_age.begin());
            _pending.erase(sha);
            _refs->release(sha);
        }
    }

    // ---- index -------------------------------------------------------------

    // NOLINTNEXTLINE(modernize-use-nodiscard): callers may only want the side effect
    auto refresh_has_all(const cache_manifest& m) const -> bool {
        const auto& store = _refs->store();
        const auto chunks = m.chunks();
        const bool all = std::all_of(chunks.begin(), chunks.end(), [&store](const chunk_sha& sha) {
            return store.contains(sha);
        });
        m.set_has_all(all);
        return all;
    }

    /// Install `m` under `key`, releasing the old manifest's references. The
    /// caller has already taken `m`'s, so chunks both values share survive.
    void bind(const key_type& key, entry_handle m) {
        auto [it, inserted] = _index.try_emplace(key, nullptr);
        if (!inserted) {
            unbind_counters(*it->second);
            const auto old = std::move(it->second);
            it->second = std::move(m);
            for (const auto& sha : old->chunks()) {
                _refs->release(sha);
            }
        } else {
            it->second = std::move(m);
        }
        _bytes += static_cast<std::size_t>(it->second->_total_len);
        _expiring += it->second->_expire_at_ms != 0 ? 1 : 0;
    }

    void unbind_counters(const cache_manifest& m) {
        _bytes -= static_cast<std::size_t>(m._total_len);
        _expiring -= m._expire_at_ms != 0 ? 1 : 0;
    }

    auto erase_key(typename index_type::iterator it) -> void {
        unbind_counters(*it->second);
        const auto old = std::move(it->second);
        _index.erase(it);
        for (const auto& sha : old->chunks()) {
            _refs->release(sha);
        }
    }

    auto erase_key(std::string_view key) -> bool {
        auto it = _index.find(key);
        if (it == _index.end()) {
            return false;
        }
        erase_key(it);
        return true;
    }

    // ---- apply -------------------------------------------------------------

    auto apply_one(redis_kv_set_command& c, LogIndex) -> bool {
        const std::span<const std::byte> value(c._value);
        const chunk_sha value_sha = sha256_of(value);
        std::vector<chunk_sha> chunks;
        chunks.reserve((value.size() / cache_set_piece_bytes) + 1);
        for (std::size_t off = 0; off < value.size(); off += cache_set_piece_bytes) {
            const auto piece =
                value.subspan(off, std::min(cache_set_piece_bytes, value.size() - off));
            // A value of one piece is its own chunk: no second hash.
            const chunk_sha sha = piece.size() == value.size() ? value_sha : sha256_of(piece);
            // Stores the bytes if they are not on disk, and takes the
            // reference, atomically with any other shard's release.
            _refs->put_and_acquire(sha, piece, true);
            chunks.push_back(sha);
        }
        auto m = make_cache_manifest(c._expire_at_ms, value.size(), value_sha, chunks);
        m->set_has_all(true);
        bind(c._key, std::move(m));
        return true;
    }

    auto apply_one(const redis_kv_del_command& c, LogIndex) -> bool { return erase_key(c._key); }

    auto apply_one(const redis_kv_sweep_command& c, LogIndex) -> bool {
        bool changed = false;
        for (const auto& e : c._entries) {
            auto it = _index.find(e._key);
            if (it != _index.end() && e._expire_at_ms != 0 &&
                it->second->_expire_at_ms == e._expire_at_ms) {
                erase_key(it);
                changed = true;
            }
        }
        return changed;
    }

    auto apply_one(const redis_kv_evict_command& c, LogIndex) -> bool {
        bool changed = false;
        for (const auto& k : c._keys) {
            changed = erase_key(k) || changed;
        }
        return changed;
    }

    auto apply_one(const redis_kv_put_chunk_command& c, LogIndex index) -> bool {
        // A pure function of the entry: the hash of its bytes is the same on
        // every replica, so refusing a mismatch cannot make two diverge.
        if (sha256_of(c._bytes) != c._sha) {
            return false;
        }
        auto it = _pending.find(c._sha);
        if (it != _pending.end()) {
            // Stored again (a retried upload): the grace restarts, and the
            // bytes are written if a fetch for them was still outstanding.
            _pending_by_age.erase({it->second, c._sha});
            it->second = index;
            _pending_by_age.emplace(index, c._sha);
            _refs->put_referenced(c._sha, c._bytes, true);
            return true;
        }
        _refs->put_and_acquire(c._sha, c._bytes, true);
        _pending.emplace(c._sha, index);
        _pending_by_age.emplace(index, c._sha);
        return true;
    }

    auto apply_one(const redis_kv_commit_command& c, LogIndex) -> bool {
        // Every chunk must be pending here. The front end proposes a
        // `put_chunk` for every chunk of every upload, even one whose bytes
        // are already stored, so a missing one means the grace expired
        // mid-upload; binding the key to chunks nobody keeps would publish a
        // value that can never be read. Pending-ness is replicated state,
        // unlike which files are on disk, so this check cannot diverge.
        for (const auto& sha : c._chunks) {
            if (!_pending.contains(sha)) {
                return false;
            }
        }
        auto m = make_cache_manifest(c._expire_at_ms, c._total_len, c._value_sha, c._chunks);
        for (const auto& sha : m->chunks()) {
            _refs->acquire(sha);
        }
        refresh_has_all(*m);
        bind(c._key, std::move(m));
        return true;
    }

    /// Advisory only; never proposed. A no-op if one ever is.
    auto apply_one(const redis_kv_touch_command&, LogIndex) -> bool { return false; }

    // ---- snapshot format ---------------------------------------------------

    static constexpr std::uint32_t state_version = 2;

    template<typename It>
    [[nodiscard]] static auto serialize(It begin, It end, std::size_t count,
                                        const pending_type& pending) -> std::vector<std::byte> {
        using namespace redis_kv_detail;
        std::vector<std::byte> out;
        put_u32(out, state_version);
        put_u32(out, static_cast<std::uint32_t>(count));
        for (auto it = begin; it != end; ++it) {
            const auto& [key, m] = *it;
            put_u32(out, static_cast<std::uint32_t>(key.size()));
            put_string(out, key);
            put_u64(out, m->_expire_at_ms);
            put_u64(out, m->_total_len);
            put_bytes(out, m->_value_sha);
            const auto chunks = m->chunks();
            put_u32(out, static_cast<std::uint32_t>(chunks.size()));
            for (const auto& sha : chunks) {
                put_bytes(out, sha);
            }
        }
        put_u32(out, static_cast<std::uint32_t>(pending.size()));
        for (const auto& [sha, applied] : pending) {
            put_bytes(out, sha);
            put_u64(out, static_cast<std::uint64_t>(applied));
        }
        return out;
    }

    static void read_sha(redis_kv_detail::reader& in, chunk_sha& out) {
        auto b = in.bytes(out.size());
        std::copy(b.begin(), b.end(), out.begin());
    }

    static void deserialize(const std::vector<std::byte>& blob, index_type& index,
                            pending_type& pending) {
        redis_kv_detail::reader in(blob);
        if (in.u32() != state_version) {
            throw redis_kv_codec_error("unknown cache kv state version");
        }
        const auto count = in.u32();
        std::vector<chunk_sha> chunks;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto key = in.string(in.u32());
            const auto expire = in.u64();
            const auto total = in.u64();
            chunk_sha value_sha{};
            read_sha(in, value_sha);
            const auto n = in.u32();
            if (static_cast<std::size_t>(n) > in.remaining() / 32) {
                throw redis_kv_codec_error("cache kv manifest chunk count exceeds buffer");
            }
            chunks.resize(n);
            for (auto& sha : chunks) {
                read_sha(in, sha);
            }
            index.insert_or_assign(std::move(key),
                                   make_cache_manifest(expire, total, value_sha, chunks));
        }
        const auto npending = in.u32();
        if (static_cast<std::size_t>(npending) > in.remaining() / 40) {
            throw redis_kv_codec_error("cache kv pending count exceeds buffer");
        }
        for (std::uint32_t i = 0; i < npending; ++i) {
            chunk_sha sha{};
            read_sha(in, sha);
            pending[sha] = static_cast<LogIndex>(in.u64());
        }
        if (in.remaining() != 0) {
            throw redis_kv_codec_error("trailing bytes after cache kv state blob");
        }
    }

    std::shared_ptr<chunk_refcounts> _refs;
    cache_kv_options _options;
    index_type _index;
    pending_type _pending;
    std::set<std::pair<LogIndex, chunk_sha>> _pending_by_age;
    std::size_t _bytes = 0;
    std::size_t _expiring = 0;
    LogIndex _last_applied_index{};
};

static_assert(state_machine<cache_kv_state_machine<std::uint64_t>, std::uint64_t>,
              "cache_kv_state_machine must satisfy the state_machine concept");
static_assert(splittable_state_machine<cache_kv_state_machine<std::uint64_t>, std::string>,
              "cache_kv_state_machine must satisfy the splittable_state_machine concept");

}  // namespace kythira
