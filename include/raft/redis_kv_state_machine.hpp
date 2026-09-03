// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_kv_state_machine.hpp
/// @brief The replicated data behind the Redis-compatible gateway
///        (.kiro/specs/redis-compatible-kv/ design Component 3).
///
/// One instance per shard. It satisfies both `kythira::state_machine` and
/// `kythira::splittable_state_machine<_, std::string>` so that `multi_raft`
/// can split and merge shards without knowing what a Redis value is.
///
/// Two rules shape everything here:
///
///  1. `apply()` is a pure function of the log. It never reads a clock, the
///     environment or a random source, because the same entry is applied on
///     every replica and any of those would let two replicas diverge. Expiry
///     is therefore an absolute `expire_at_ms` stamped by the leader before
///     the entry was proposed, and expired keys are removed by a `sweep`
///     entry the leader proposes, never by `apply` noticing a deadline.
///
///  2. Values are shared, never copied out under the node's lock. `lookup()`
///     returns a `shared_ptr<const value_entry>` so the gateway can take the
///     handle inside `node::with_state_machine` (which runs under the node
///     mutex) and write the bytes to the socket after the lock is released.
///     An 8 MiB compiler artefact copied under that mutex would stall every
///     AppendEntries for the shard.

#include <raft/redis_kv_commands.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// A stored value plus its deadline. Immutable once published: a SET that
/// replaces a key installs a fresh entry rather than mutating this one, so a
/// reader still holding the old handle keeps reading consistent bytes.
struct redis_kv_value_entry {
    std::vector<std::byte> _value;
    /// ms since the Unix epoch; 0 = never expires.
    std::uint64_t _expire_at_ms = 0;
};

template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class redis_kv_state_machine {
public:
    using key_type = std::string;
    using entry_handle = std::shared_ptr<const redis_kv_value_entry>;

    redis_kv_state_machine() = default;

    // ---- kythira::state_machine -------------------------------------------

    /// Apply one committed entry. The reply bytes are unused by the gateway
    /// (it answers from its own knowledge of what it proposed) but the
    /// concept requires them; a single status byte is returned so a caller
    /// through `node::read_state`-style paths can tell a no-op from a change.
    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        _last_applied_index = index;
        auto decoded = decode_redis_kv_command(command);
        bool changed = std::visit([this](auto& c) { return apply_one(c); }, decoded);
        return {changed ? std::byte{1} : std::byte{0}};
    }

    /// Serialize the whole store in key order: for each key `u32 BE keylen,
    /// key, u64 BE expire_at_ms, u32 BE value len, value`, preceded by a
    /// `u32 BE` entry count. This is also the format of every blob
    /// `split_state` produces, so the split/absorb law holds by construction.
    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return serialize_store(_store.begin(), _store.end(), _store.size());
    }

    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index)
        -> void {
        store_type incoming;
        std::size_t bytes = 0;
        deserialize_store(snapshot_data, incoming, bytes);
        _store = std::move(incoming);
        _bytes = bytes;
        _expiring = 0;
        for (const auto& [key, entry] : _store) {
            _expiring += entry->_expire_at_ms != 0 ? 1 : 0;
        }
        _last_applied_index = index;
    }

    [[nodiscard]] auto last_applied_index() const noexcept -> LogIndex {
        return _last_applied_index;
    }

    // ---- kythira::splittable_state_machine ---------------------------------

    /// Keys plus values, maintained incrementally so the policy tick that
    /// polls it does not walk the map.
    [[nodiscard]] auto approximate_size_bytes() const noexcept -> std::size_t { return _bytes; }
    [[nodiscard]] auto approximate_key_count() const noexcept -> std::size_t {
        return _store.size();
    }
    /// Keys carrying a deadline, swept or not; INFO's `expires=`.
    [[nodiscard]] auto expiring_key_count() const noexcept -> std::size_t { return _expiring; }

    /// Evenly spaced by key count, never the first key (an empty left child
    /// is a legal shard and a useless one). sccache keys are content hashes,
    /// so count and bytes are close enough that a size-weighted walk would
    /// buy nothing.
    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<key_type> {
        std::vector<key_type> out;
        if (max == 0 || _store.size() < 2) {
            return out;
        }
        const std::size_t wanted = std::min(max, _store.size() - 1);
        const std::size_t stride = _store.size() / (wanted + 1);
        if (stride == 0) {
            return out;
        }
        std::size_t index = 0;
        std::size_t taken = 0;
        for (const auto& [key, entry] : _store) {
            ++index;
            if (taken < wanted && index % stride == 0 && index < _store.size()) {
                out.push_back(key);
                ++taken;
            }
        }
        return out;
    }

    /// Always true. Redis keys are independent of one another: there is no
    /// row/index relationship, no multi-key command in the closure, and no
    /// per-range metadata, so there is never a key between which and its
    /// neighbour a cut would be wrong. The concept requires the veto to exist
    /// because other state machines need it; this one has nothing to protect.
    [[nodiscard]] auto can_split_at([[maybe_unused]] const key_type& key) const -> bool {
        return true;
    }

    /// Cut the store at `keys`, returning `keys.size() + 1` blobs in key
    /// order, each in `get_state()`'s format. Cuts are sorted and deduplicated
    /// first so a caller's ordering mistake cannot yield overlapping children.
    [[nodiscard]] auto split_state(const std::vector<key_type>& keys)
        -> std::vector<std::vector<std::byte>> {
        std::vector<key_type> cuts = keys;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        std::vector<std::vector<std::byte>> out;
        out.reserve(cuts.size() + 1);
        auto it = _store.begin();
        for (const auto& cut : cuts) {
            auto begin = it;
            std::size_t n = 0;
            while (it != _store.end() && it->first < cut) {
                ++it;
                ++n;
            }
            out.push_back(serialize_store(begin, it, n));
        }
        std::size_t n = 0;
        for (auto tail = it; tail != _store.end(); ++tail) {
            ++n;
        }
        out.push_back(serialize_store(it, _store.end(), n));
        return out;
    }

    /// Take on a sibling's state. `range` is unused: there is no range-scoped
    /// structure to update. Incoming entries win on a key collision, which
    /// cannot happen between siblings of one split and is the documented
    /// behaviour if it ever does.
    auto absorb(const std::vector<std::byte>& other_state,
                [[maybe_unused]] const shard_range<key_type>& range) -> void {
        store_type incoming;
        std::size_t bytes = 0;
        deserialize_store(other_state, incoming, bytes);
        for (auto& [key, entry] : incoming) {
            insert_or_replace(key, std::move(entry));
        }
    }

    // ---- read path ---------------------------------------------------------

    /// O(1)-ish handle to a value; null if absent. Does NOT consult the
    /// deadline: the gateway compares `_expire_at_ms` to its own clock, which
    /// keeps this method and everything else here clock-free.
    [[nodiscard]] auto lookup(std::string_view key) const -> entry_handle {
        auto it = _store.find(key);
        return it == _store.end() ? nullptr : it->second;
    }

    /// Keys whose deadline is non-zero and `<= now_ms`, oldest deadline first,
    /// at most `max`. The leader calls this on its policy tick to build a
    /// `sweep` proposal; replicas never call it.
    [[nodiscard]] auto collect_expired(std::uint64_t now_ms, std::size_t max) const
        -> std::vector<redis_kv_sweep_entry> {
        std::vector<redis_kv_sweep_entry> out;
        for (const auto& [key, entry] : _store) {
            if (out.size() >= max) {
                break;
            }
            if (entry->_expire_at_ms != 0 && entry->_expire_at_ms <= now_ms) {
                out.push_back({key, entry->_expire_at_ms});
            }
        }
        return out;
    }

    /// Every key in order; for INFO/DBSIZE-style diagnostics and the leader's
    /// eviction bookkeeping when it has to seed an advisory LRU.
    template<typename F> auto for_each_key(F&& f) const -> void {
        for (const auto& [key, entry] : _store) {
            f(key, *entry);
        }
    }

    // ---- codec, public so tests can check the split/absorb law -------------

    using store_type = std::map<key_type, entry_handle, std::less<>>;

    template<typename It>
    [[nodiscard]] static auto serialize_store(It begin, It end, std::size_t count)
        -> std::vector<std::byte> {
        std::vector<std::byte> out;
        redis_kv_detail::put_u32(out, static_cast<std::uint32_t>(count));
        for (auto it = begin; it != end; ++it) {
            const auto& [key, entry] = *it;
            redis_kv_detail::put_u32(out, static_cast<std::uint32_t>(key.size()));
            redis_kv_detail::put_string(out, key);
            redis_kv_detail::put_u64(out, entry->_expire_at_ms);
            redis_kv_detail::put_u32(out, static_cast<std::uint32_t>(entry->_value.size()));
            redis_kv_detail::put_bytes(out, entry->_value);
        }
        return out;
    }

    static auto deserialize_store(const std::vector<std::byte>& blob, store_type& into,
                                  std::size_t& bytes) -> void {
        redis_kv_detail::reader in(blob);
        auto count = in.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            auto key = in.string(in.u32());
            auto entry = std::make_shared<redis_kv_value_entry>();
            entry->_expire_at_ms = in.u64();
            auto v = in.bytes(in.u32());
            entry->_value.assign(v.begin(), v.end());
            bytes += key.size() + entry->_value.size();
            into[std::move(key)] = std::move(entry);
        }
        if (in.remaining() != 0) {
            throw redis_kv_codec_error("trailing bytes after redis kv state blob");
        }
    }

private:
    auto apply_one(redis_kv_set_command& c) -> bool {
        auto entry = std::make_shared<redis_kv_value_entry>();
        entry->_value = std::move(c._value);
        entry->_expire_at_ms = c._expire_at_ms;
        insert_or_replace(c._key, std::move(entry));
        return true;
    }

    auto apply_one(const redis_kv_del_command& c) -> bool { return erase(c._key); }

    /// Delete each named key only if its stored deadline still equals the
    /// one the sweep was built from: a SET that landed between the sweep's
    /// proposal and its apply replaced the deadline and must survive.
    auto apply_one(const redis_kv_sweep_command& c) -> bool {
        bool changed = false;
        for (const auto& e : c._entries) {
            auto it = _store.find(e._key);
            if (it != _store.end() && it->second->_expire_at_ms == e._expire_at_ms &&
                e._expire_at_ms != 0) {
                forget(it);
                _store.erase(it);
                changed = true;
            }
        }
        return changed;
    }

    auto apply_one(const redis_kv_evict_command& c) -> bool {
        bool changed = false;
        for (const auto& k : c._keys) {
            changed = erase(k) || changed;
        }
        return changed;
    }

    auto insert_or_replace(const key_type& key, entry_handle entry) -> void {
        auto [it, inserted] = _store.try_emplace(key, nullptr);
        if (!inserted) {
            forget(it);
        }
        _bytes += key.size() + entry->_value.size();
        _expiring += entry->_expire_at_ms != 0 ? 1 : 0;
        it->second = std::move(entry);
    }

    /// Undo an entry's contribution to the incremental counters.
    auto forget(store_type::const_iterator it) -> void {
        _bytes -= it->first.size() + it->second->_value.size();
        _expiring -= it->second->_expire_at_ms != 0 ? 1 : 0;
    }

    auto erase(const key_type& key) -> bool {
        auto it = _store.find(key);
        if (it == _store.end()) {
            return false;
        }
        forget(it);
        _store.erase(it);
        return true;
    }

    store_type _store;
    std::size_t _bytes = 0;
    std::size_t _expiring = 0;
    LogIndex _last_applied_index{};
};

static_assert(state_machine<redis_kv_state_machine<std::uint64_t>, std::uint64_t>,
              "redis_kv_state_machine must satisfy the state_machine concept");
static_assert(splittable_state_machine<redis_kv_state_machine<std::uint64_t>, std::string>,
              "redis_kv_state_machine must satisfy the splittable_state_machine concept");

}  // namespace kythira
