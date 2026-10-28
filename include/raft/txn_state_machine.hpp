// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file txn_state_machine.hpp
/// @brief `transactional_state_machine`: the wrapper that turns any state
///        machine into a participant in cross-shard transactions.
///
/// See `.kiro/specs/multi-raft-transactions/` design "State machine wrapper"
/// (tasks 2-4).
///
/// ### What it holds
///
/// The wrapped state machine's committed data, plus three maps that are part
/// of the replicated state and so ride in every snapshot, every
/// `InstallSnapshot` and every split and merge (Requirement 6.1):
///
/// | Map | Keyed by | Holds |
/// |---|---|---|
/// | `_intents` | the written key | the lock table: a prepared, uncommitted write |
/// | `_records` | the primary key, then `txn_id` | each transaction's fate on its primary shard |
/// | `_settled` | the written key, then `txn_id` | how this shard finished a transaction's write |
///
/// A follower that caught up by snapshot without the lock table would serve a
/// key as unlocked that its leader holds locked, which is why the locks live
/// here rather than in the host.
///
/// ### Why `_settled` exists
///
/// Every op is idempotent by `txn_id` (task 3), and for most of them the
/// intent or the record is the memory that makes it so. The exception is a
/// prepare that arrives *after* its transaction has finished on this shard: a
/// coordinator that retried a prepare after a timeout, say. With no intent
/// left and, on a secondary, no record either, that prepare would install a
/// fresh intent, and the resolver would later roll it forward and apply the
/// write a second time. `_settled` remembers, per key, that this shard already
/// committed or aborted the transaction's write there, so the late prepare is
/// answered from memory and installs nothing. A one-phase commit uses the same
/// memory, so a retried 1PC entry does not apply twice either. Entries are
/// dropped by the leader-proposed `txn_cleanup`, with the record.
///
/// ### Determinism
///
/// `apply()` reads no clock and draws no randomness: every deadline and `now`
/// comes in the entry. Every map is ordered, so `get_state()` and
/// `split_state()` produce the same bytes on every replica whatever order the
/// writes arrived in.
///
/// ### Atomicity and the wrapped state machine
///
/// A one-phase commit and a commit that rolls several intents forward call the
/// wrapped `apply()` once per write. If the wrapped state machine throws part
/// way through, the earlier writes have applied. That is the same on every
/// replica (a command that throws, throws everywhere), so replicas do not
/// diverge, but the transaction is no longer all-or-nothing. The coordinator
/// therefore only buffers commands the application has already validated; the
/// wrapper cannot pre-check an opaque command.

#include <raft/exceptions.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// @brief Whether the wrapped state machine keeps versions (tier 2).
///
/// A compile-time choice, because under `mvcc::on` every write, plain ones
/// included, costs an oracle round trip (design "all writes carry
/// timestamps").
enum class mvcc : bool {
    off = false,
    on = true
};

/// @brief Per-transaction options (Requirement 10.1).
///
/// Here rather than with the coordinator because `multi_raft_config` carries
/// the defaults (Requirement 10.3), and `multi_raft.hpp` includes this header.
struct txn_options {
    txn_isolation isolation{txn_isolation::read_committed};
    /// How long a prepared intent lives without a heartbeat before a resolver
    /// may abort its transaction.
    std::chrono::milliseconds lock_ttl{std::chrono::seconds{3}};
    /// The whole transaction's budget, `begin()` to the commit point.
    std::chrono::milliseconds timeout{std::chrono::seconds{10}};
    /// Automatic retries after a conflict; zero hands every conflict back.
    std::size_t max_retries{0};
};

/// @brief An intent as a read path sees it (Requirement 5.5).
template<shard_key Key> struct intent_view {
    txn_id _txn{};
    Key _primary{};
    /// The prepare's TTL deadline, for the resolver (task 7).
    std::uint64_t _deadline_ms{0};

    [[nodiscard]] auto operator==(const intent_view&) const -> bool = default;
};

/// @brief A transaction record as `transaction_status()` sees it.
struct txn_record_view {
    txn_record_status _status{txn_record_status::pending};
    std::uint64_t _deadline_ms{0};
    std::uint64_t _commit_ts{0};

    [[nodiscard]] auto operator==(const txn_record_view&) const -> bool = default;
};

/// @brief Wraps `SM` so its host can take part in cross-shard transactions.
///
/// @tparam SM       The application's state machine. Must satisfy
///                  `state_machine<SM, std::uint64_t>`.
/// @tparam Key      The host's shard key.
/// @tparam P        The host's partitioner: `P{}.key_of(command)` gives the
///                  key a plain command writes. Stated as a requires-clause
///                  rather than with `partitioner<P, Key>` from
///                  `multi_raft.hpp`, which will include this header.
/// @tparam Mvcc     `mvcc::on` arrives with tier 2 (tasks 11-12).
/// @tparam KeyCodec How keys are written into envelopes and snapshots.
template<typename SM, shard_key Key, typename P, mvcc Mvcc = mvcc::off,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
requires state_machine<SM, std::uint64_t> && std::default_initializable<P> &&
         requires(const P& p, const std::vector<std::byte>& command) {
             { p.key_of(command) } -> std::same_as<Key>;
         }
class transactional_state_machine {
    static_assert(Mvcc == mvcc::off,
                  "transactional_state_machine: mvcc::on needs the multi-version state "
                  "machine of multi-raft-transactions tasks 11-12");

public:
    using inner_type = SM;
    using key_type = Key;
    using key_codec_type = KeyCodec;
    using log_index_type = std::uint64_t;
    using envelope_type = txn_envelope<Key>;
    using result_type = txn_result<Key>;

    /// @brief The snapshot header, distinct from any unwrapped format
    /// (Requirement 1.4).
    static constexpr std::string_view k_snapshot_magic = "kythira-txn-sm";
    static constexpr std::uint8_t k_snapshot_version = 1;

    transactional_state_machine() = default;
    explicit transactional_state_machine(SM inner, P partitioner = P{}, KeyCodec codec = KeyCodec{})
        : _inner(std::move(inner)),
          _partitioner(std::move(partitioner)),
          _codec(std::move(codec)) {}

    // ── state_machine ────────────────────────────────────────────────────────

    /// @brief Decode one envelope and apply it.
    ///
    /// @return An encoded `txn_result`.
    /// @throws serialization_exception if the entry is not an envelope.
    auto apply(const std::vector<std::byte>& entry, log_index_type index)
        -> std::vector<std::byte> {
        const auto env = decode_txn_envelope<Key>(entry, _codec);
        const auto res = std::visit([&](const auto& op) { return apply_op(op, index); }, env);
        return encode_txn_result<Key>(res, _codec);
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        return encode_state(_intents, _records, _settled, _oracle_hwm, _inner.get_state());
    }

    /// @throws serialization_exception for a snapshot in any other format,
    ///         naming both, so a host restored from an unwrapped snapshot (or
    ///         the reverse) fails at once rather than misreading the bytes.
    auto restore_from_snapshot(const std::vector<std::byte>& data, log_index_type index) -> void {
        auto decoded = decode_state(data);
        _inner.restore_from_snapshot(decoded._inner, index);
        _intents = std::move(decoded._intents);
        _records = std::move(decoded._records);
        _settled = std::move(decoded._settled);
        _oracle_hwm = decoded._oracle_hwm;
    }

    // ── splittable_state_machine, exactly when SM is (Requirement 1.3) ──────

    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t
    requires splittable_state_machine<SM, Key>
    {
        std::size_t locks = 0;
        for (const auto& [key, intent] : _intents) {
            locks += _codec.encode(key).size() + intent._command.size() + 64;
        }
        return _inner.approximate_size_bytes() + locks;
    }

    [[nodiscard]] auto approximate_key_count() const -> std::size_t
    requires splittable_state_machine<SM, Key>
    {
        return _inner.approximate_key_count();
    }

    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<Key>
    requires splittable_state_machine<SM, Key>
    {
        return _inner.suggest_split_keys(max);
    }

    /// Intents and records move with their keys, so the wrapper adds no veto.
    [[nodiscard]] auto can_split_at(const Key& k) const -> bool
    requires splittable_state_machine<SM, Key>
    {
        return _inner.can_split_at(k);
    }

    /// @brief Cut every map at the same keys as the wrapped state machine
    /// (Requirement 6.1). Intents and settled entries go with their own key,
    /// records with their primary key, and the oracle high-water mark with
    /// the leftmost child (it is zero outside the oracle group).
    [[nodiscard]] auto split_state(const std::vector<Key>& keys)
        -> std::vector<std::vector<std::byte>>
    requires splittable_state_machine<SM, Key>
    {
        std::vector<Key> cuts = keys;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        auto inner_parts = _inner.split_state(keys);
        if (inner_parts.size() != cuts.size() + 1) {
            throw serialization_exception(
                "transactional_state_machine::split_state: wrapped state machine returned " +
                std::to_string(inner_parts.size()) + " parts for " + std::to_string(cuts.size()) +
                " cut(s)");
        }

        std::vector<std::vector<std::byte>> out;
        out.reserve(cuts.size() + 1);
        for (std::size_t i = 0; i <= cuts.size(); ++i) {
            const Key* lo = i == 0 ? nullptr : &cuts[i - 1];
            const Key* hi = i == cuts.size() ? nullptr : &cuts[i];
            out.push_back(encode_state(slice(_intents, lo, hi), slice(_records, lo, hi),
                                       slice(_settled, lo, hi), i == 0 ? _oracle_hwm : 0,
                                       inner_parts[i]));
        }
        return out;
    }

    /// @brief Take on another shard's state, which covered `range`. The exact
    /// inverse of `split_state` (Requirement 6.2).
    auto absorb(const std::vector<std::byte>& other, const shard_range<Key>& range) -> void
    requires splittable_state_machine<SM, Key>
    {
        auto decoded = decode_state(other);
        _inner.absorb(decoded._inner, range);
        for (auto& [k, v] : decoded._intents) {
            _intents.insert_or_assign(k, std::move(v));
        }
        for (auto& [k, v] : decoded._records) {
            auto& mine = _records[k];
            for (auto& [id, rec] : v) {
                mine.insert_or_assign(id, rec);
            }
        }
        for (auto& [k, v] : decoded._settled) {
            auto& mine = _settled[k];
            for (auto& [id, status] : v) {
                mine.insert_or_assign(id, status);
            }
        }
        _oracle_hwm = std::max(_oracle_hwm, decoded._oracle_hwm);
    }

    // ── Read paths (Requirement 5.5) ─────────────────────────────────────────

    [[nodiscard]] auto inner() -> SM& { return _inner; }
    [[nodiscard]] auto inner() const -> const SM& { return _inner; }

    [[nodiscard]] auto intent_on(const Key& k) const -> std::optional<intent_view<Key>> {
        const auto it = _intents.find(k);
        if (it == _intents.end()) {
            return std::nullopt;
        }
        return intent_view<Key>{it->second._txn, it->second._primary, it->second._deadline_ms};
    }

    [[nodiscard]] auto record_of(const Key& primary, const txn_id& txn) const
        -> std::optional<txn_record_view> {
        const auto* rec = find_record(primary, txn);
        if (rec == nullptr) {
            return std::nullopt;
        }
        return *rec;
    }

    [[nodiscard]] auto intent_count() const -> std::size_t { return _intents.size(); }
    [[nodiscard]] auto oracle_high_water_mark() const -> std::uint64_t { return _oracle_hwm; }

private:
    struct intent {
        txn_id _txn{};
        Key _primary{};
        std::vector<std::byte> _command{};
        std::uint64_t _deadline_ms{0};
    };

    using record_map = std::map<Key, std::map<txn_id, txn_record_view>>;
    using settled_map = std::map<Key, std::map<txn_id, txn_record_status>>;
    using intent_map = std::map<Key, intent>;

    // ── apply, one overload per op ───────────────────────────────────────────

    auto apply_op(const txn_plain& op, log_index_type index) -> result_type {
        const Key key = _partitioner.key_of(op._command);
        if (auto locked = conflict_on(key, nullptr); locked.has_value()) {
            locked->_status = txn_result_status::key_locked;
            return *locked;
        }
        result_type res;
        res._payload = _inner.apply(op._command, index);
        return res;
    }

    auto apply_op(const txn_one_phase<Key>& op, log_index_type index) -> result_type {
        // A retried 1PC entry: the first one already applied every write.
        for (const auto& w : op._writes) {
            if (const auto s = settled_status(w._key, op._txn); s.has_value()) {
                return *s == txn_record_status::committed
                           ? result_type{}
                           : status_only(txn_result_status::already_aborted);
            }
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            if (auto conflict = conflict_on(w._key, &op._txn); conflict.has_value()) {
                conflict->_status = txn_result_status::write_conflict;
                return *conflict;
            }
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            std::ignore = _inner.apply(w._command, index);
            _settled[w._key][op._txn] = txn_record_status::committed;
        }
        return {};
    }

    auto apply_op(const txn_prepare<Key>& op, log_index_type) -> result_type {
        // Tier 3's read keys never arrive under mvcc::off: a serializable
        // transaction is refused at begin() on such a host (Requirement 10.2).
        if (op._is_primary) {
            if (const auto* rec = find_record(op._primary, op._txn); rec != nullptr) {
                if (rec->_status == txn_record_status::aborted) {
                    return status_only(txn_result_status::already_aborted);
                }
                if (rec->_status == txn_record_status::committed) {
                    return {};  // a prepare retried after its commit
                }
            }
        }
        for (const auto& w : op._writes) {
            if (const auto s = settled_status(w._key, op._txn); s.has_value()) {
                return *s == txn_record_status::committed
                           ? result_type{}
                           : status_only(txn_result_status::already_aborted);
            }
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            if (auto conflict = conflict_on(w._key, &op._txn); conflict.has_value()) {
                conflict->_status = txn_result_status::write_conflict;
                return *conflict;
            }
        }
        for (const auto& w : op._writes) {
            _intents.insert_or_assign(
                w._key, intent{op._txn, op._primary, w._command, op._ttl_deadline_ms});
        }
        if (op._is_primary) {
            auto& rec = _records[op._primary][op._txn];
            rec._deadline_ms = std::max(rec._deadline_ms, op._ttl_deadline_ms);
        }
        return {};
    }

    auto apply_op(const txn_commit_primary<Key>& op, log_index_type index) -> result_type {
        auto* rec = find_record(op._primary, op._txn);
        if (rec == nullptr) {
            // Never prepared here, or cleaned up long after it finished. Leave
            // a rollback marker, as check_and_resolve would: a prepare that
            // arrives later must not then succeed (Requirement 4.4).
            _records[op._primary][op._txn]._status = txn_record_status::aborted;
            return record_result({txn_record_status::aborted, 0, 0});
        }
        if (rec->_status == txn_record_status::aborted) {
            return record_result(*rec);
        }
        if (rec->_status == txn_record_status::pending) {
            rec->_status = txn_record_status::committed;
            rec->_commit_ts = op._commit_ts;
            roll_forward_all(op._txn, index);
        }
        return {};
    }

    auto apply_op(const txn_commit_secondary<Key>& op, log_index_type index) -> result_type {
        for (const auto& k : sorted_keys(op._keys)) {
            roll_forward(k, op._txn, index);
        }
        return {};
    }

    auto apply_op(const txn_abort<Key>& op, log_index_type) -> result_type {
        const bool on_primary =
            std::find(op._keys.begin(), op._keys.end(), op._primary) != op._keys.end();
        if (on_primary) {
            auto& rec = _records[op._primary][op._txn];
            if (rec._status == txn_record_status::committed) {
                // Too late: the commit point has passed, and the intents here
                // were applied with it. Report it; change nothing.
                return record_result(rec);
            }
            rec._status = txn_record_status::aborted;
        }
        for (const auto& k : op._keys) {
            roll_back(k, op._txn);
        }
        return on_primary ? record_result({txn_record_status::aborted, 0, 0}) : result_type{};
    }

    auto apply_op(const txn_heartbeat<Key>& op, log_index_type) -> result_type {
        auto* rec = find_record(op._primary, op._txn);
        if (rec == nullptr) {
            return record_result({txn_record_status::aborted, 0, 0});
        }
        if (rec->_status == txn_record_status::pending) {
            rec->_deadline_ms = std::max(rec->_deadline_ms, op._new_deadline_ms);
        }
        return record_result(*rec);
    }

    /// Requirement 4.3's table.
    auto apply_op(const txn_check_and_resolve<Key>& op, log_index_type) -> result_type {
        auto* rec = find_record(op._primary, op._txn);
        if (rec == nullptr) {
            _records[op._primary][op._txn]._status = txn_record_status::aborted;
            return record_result({txn_record_status::aborted, 0, 0});
        }
        if (rec->_status == txn_record_status::pending && rec->_deadline_ms <= op._now_ms) {
            rec->_status = txn_record_status::aborted;
            // The primary's own intents are on this shard; release them now.
            roll_back_all(op._txn);
        }
        return record_result(*rec);
    }

    auto apply_op(const txn_cleanup<Key>& op, log_index_type) -> result_type {
        for (const auto& item : op._items) {
            if (auto it = _records.find(item._primary); it != _records.end()) {
                it->second.erase(item._txn);
                if (it->second.empty()) {
                    _records.erase(it);
                }
            }
            for (auto it = _settled.begin(); it != _settled.end();) {
                it->second.erase(item._txn);
                it = it->second.empty() ? _settled.erase(it) : std::next(it);
            }
        }
        return {};
    }

    /// Tier 2. Nothing is versioned under mvcc::off, so there is nothing to
    /// collect.
    auto apply_op(const txn_gc&, log_index_type) -> result_type { return {}; }

    auto apply_op(const txn_oracle_reserve& op, log_index_type) -> result_type {
        _oracle_hwm = std::max(_oracle_hwm, op._new_hwm);
        return {};
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    static auto status_only(txn_result_status s) -> result_type {
        result_type res;
        res._status = s;
        return res;
    }

    static auto record_result(const txn_record_view& rec) -> result_type {
        result_type res;
        res._status = txn_result_status::record;
        res._record = rec._status;
        res._commit_ts = rec._commit_ts;
        return res;
    }

    /// The conflict on `key`, if another transaction (or, with `self` null,
    /// any transaction) holds an intent there. Status is the caller's to set.
    [[nodiscard]] auto conflict_on(const Key& key, const txn_id* self) const
        -> std::optional<result_type> {
        const auto it = _intents.find(key);
        if (it == _intents.end() || (self != nullptr && it->second._txn == *self)) {
            return std::nullopt;
        }
        result_type res;
        res._key = key;
        res._holder = txn_lock_holder<Key>{it->second._txn, it->second._primary};
        return res;
    }

    [[nodiscard]] auto settled_status(const Key& key, const txn_id& txn) const
        -> std::optional<txn_record_status> {
        const auto it = _settled.find(key);
        if (it == _settled.end()) {
            return std::nullopt;
        }
        const auto jt = it->second.find(txn);
        if (jt == it->second.end()) {
            return std::nullopt;
        }
        return jt->second;
    }

    [[nodiscard]] auto find_record(const Key& primary, const txn_id& txn) -> txn_record_view* {
        const auto it = _records.find(primary);
        if (it == _records.end()) {
            return nullptr;
        }
        const auto jt = it->second.find(txn);
        return jt == it->second.end() ? nullptr : &jt->second;
    }

    [[nodiscard]] auto find_record(const Key& primary, const txn_id& txn) const
        -> const txn_record_view* {
        return const_cast<transactional_state_machine*>(this)->find_record(primary, txn);
    }

    /// Apply and release `txn`'s intent on `key`, if it still holds one.
    auto roll_forward(const Key& key, const txn_id& txn, log_index_type index) -> void {
        const auto it = _intents.find(key);
        if (it == _intents.end() || it->second._txn != txn) {
            return;  // already resolved, or never prepared here
        }
        std::ignore = _inner.apply(it->second._command, index);
        _intents.erase(it);
        _settled[key][txn] = txn_record_status::committed;
    }

    auto roll_back(const Key& key, const txn_id& txn) -> void {
        const auto it = _intents.find(key);
        if (it != _intents.end() && it->second._txn == txn) {
            _intents.erase(it);
        }
        // Remembered even when no intent was here, so a prepare that arrives
        // after its abort installs nothing.
        _settled[key][txn] = txn_record_status::aborted;
    }

    [[nodiscard]] auto keys_held_by(const txn_id& txn) const -> std::vector<Key> {
        std::vector<Key> keys;
        for (const auto& [k, in] : _intents) {
            if (in._txn == txn) {
                keys.push_back(k);
            }
        }
        return keys;
    }

    auto roll_forward_all(const txn_id& txn, log_index_type index) -> void {
        for (const auto& k : keys_held_by(txn)) {
            roll_forward(k, txn, index);
        }
    }

    auto roll_back_all(const txn_id& txn) -> void {
        for (const auto& k : keys_held_by(txn)) {
            roll_back(k, txn);
        }
    }

    static auto sorted_by_key(const std::vector<txn_write<Key>>& writes)
        -> std::vector<txn_write<Key>> {
        auto out = writes;
        std::stable_sort(out.begin(), out.end(),
                         [](const auto& a, const auto& b) { return a._key < b._key; });
        return out;
    }

    static auto sorted_keys(const std::vector<Key>& keys) -> std::vector<Key> {
        auto out = keys;
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// Entries of `m` with `lo <= key < hi`; a null bound is unbounded.
    template<typename Map> static auto slice(const Map& m, const Key* lo, const Key* hi) -> Map {
        Map out;
        auto it = lo == nullptr ? m.begin() : m.lower_bound(*lo);
        const auto end = hi == nullptr ? m.end() : m.lower_bound(*hi);
        for (; it != end; ++it) {
            out.insert(*it);
        }
        return out;
    }

    // ── snapshot format ──────────────────────────────────────────────────────
    //
    // "kythira-txn-sm", a version byte, the oracle high-water mark, the three
    // maps in key order, then the wrapped state as an opaque blob. Counts here
    // are bounded by the bytes remaining rather than by the envelope's
    // per-list cap: a shard may legitimately hold more intents than one
    // transaction may name.

    struct decoded_state {
        intent_map _intents;
        record_map _records;
        settled_map _settled;
        std::uint64_t _oracle_hwm{0};
        std::vector<std::byte> _inner;
    };

    [[nodiscard]] auto encode_state(const intent_map& intents, const record_map& records,
                                    const settled_map& settled, std::uint64_t hwm,
                                    const std::vector<std::byte>& inner) const
        -> std::vector<std::byte> {
        using namespace txn_wire;
        std::vector<std::byte> out;
        for (char c : k_snapshot_magic) {
            out.push_back(static_cast<std::byte>(c));
        }
        write_u8(out, k_snapshot_version);
        write_u64(out, hwm);

        write_u64(out, intents.size());
        for (const auto& [k, in] : intents) {
            write_key(out, k, _codec);
            write_txn_id(out, in._txn);
            write_key(out, in._primary, _codec);
            write_blob(out, in._command, k_txn_max_command_bytes, "intent command");
            write_u64(out, in._deadline_ms);
        }

        std::size_t record_count = 0;
        for (const auto& [k, by_txn] : records) {
            record_count += by_txn.size();
        }
        write_u64(out, record_count);
        for (const auto& [k, by_txn] : records) {
            for (const auto& [id, rec] : by_txn) {
                write_key(out, k, _codec);
                write_txn_id(out, id);
                write_u8(out, static_cast<std::uint8_t>(rec._status));
                write_u64(out, rec._deadline_ms);
                write_u64(out, rec._commit_ts);
            }
        }

        std::size_t settled_count = 0;
        for (const auto& [k, by_txn] : settled) {
            settled_count += by_txn.size();
        }
        write_u64(out, settled_count);
        for (const auto& [k, by_txn] : settled) {
            for (const auto& [id, status] : by_txn) {
                write_key(out, k, _codec);
                write_txn_id(out, id);
                write_u8(out, static_cast<std::uint8_t>(status));
            }
        }

        write_u64(out, inner.size());
        out.insert(out.end(), inner.begin(), inner.end());
        return out;
    }

    [[nodiscard]] static auto has_snapshot_magic(const std::vector<std::byte>& data) -> bool {
        if (data.size() < k_snapshot_magic.size()) {
            return false;
        }
        for (std::size_t i = 0; i < k_snapshot_magic.size(); ++i) {
            if (static_cast<char>(data[i]) != k_snapshot_magic[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto decode_state(const std::vector<std::byte>& data) const -> decoded_state {
        using namespace txn_wire;
        if (!has_snapshot_magic(data)) {
            throw serialization_exception(
                "transactional_state_machine: expected a kythira-txn-sm v" +
                std::to_string(k_snapshot_version) +
                " snapshot, found an unwrapped state-machine snapshot (" +
                std::to_string(data.size()) +
                " bytes); a host cannot switch between wrapped and unwrapped state machines "
                "without rebuilding its data");
        }
        const std::vector<std::byte> body(
            data.begin() + static_cast<std::ptrdiff_t>(k_snapshot_magic.size()), data.end());
        reader r{body, "kythira-txn-sm snapshot"};
        if (const auto v = r.u8(); v != k_snapshot_version) {
            r.fail("unknown kythira-txn-sm snapshot version " + std::to_string(v));
        }

        decoded_state out;
        out._oracle_hwm = r.u64();

        const auto intents = r.count(4 * sizeof(std::uint64_t), "intent table", body.size());
        for (std::size_t i = 0; i < intents; ++i) {
            Key k = read_key<Key>(r, _codec);
            intent in;
            in._txn = read_txn_id(r);
            in._primary = read_key<Key>(r, _codec);
            in._command = r.blob(k_txn_max_command_bytes, "intent command");
            in._deadline_ms = r.u64();
            out._intents.insert_or_assign(std::move(k), std::move(in));
        }

        const auto records = r.count(4 * sizeof(std::uint64_t), "record table", body.size());
        for (std::size_t i = 0; i < records; ++i) {
            Key k = read_key<Key>(r, _codec);
            const auto id = read_txn_id(r);
            txn_record_view rec;
            rec._status = read_record_status(r);
            rec._deadline_ms = r.u64();
            rec._commit_ts = r.u64();
            out._records[std::move(k)][id] = rec;
        }

        const auto settled = r.count(3 * sizeof(std::uint64_t), "settled table", body.size());
        for (std::size_t i = 0; i < settled; ++i) {
            Key k = read_key<Key>(r, _codec);
            const auto id = read_txn_id(r);
            out._settled[std::move(k)][id] = read_record_status(r);
        }

        out._inner = r.blob(body.size(), "wrapped state");
        r.finish();
        return out;
    }

    static auto read_record_status(txn_wire::reader& r) -> txn_record_status {
        const auto v = r.u8();
        if (v > static_cast<std::uint8_t>(txn_record_status::aborted)) {
            r.fail("unknown record status " + std::to_string(v));
        }
        return static_cast<txn_record_status>(v);
    }

    SM _inner{};
    P _partitioner{};
    KeyCodec _codec{};
    intent_map _intents;
    record_map _records;
    settled_map _settled;
    std::uint64_t _oracle_hwm{0};
};

/// @brief Whether `SM` is a `transactional_state_machine`, for the host's
/// `if constexpr` (task 5).
template<typename SM> inline constexpr bool is_transactional_state_machine_v = false;

template<typename SM, shard_key Key, typename P, mvcc Mvcc, typename KeyCodec>
inline constexpr bool
    is_transactional_state_machine_v<transactional_state_machine<SM, Key, P, Mvcc, KeyCodec>> =
        true;

}  // namespace kythira
