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
/// wrapper cannot pre-check an opaque command. A wrapped state machine that
/// can check its own commands supplies `validate`
/// (`txn_validating_state_machine`), which `prepare` and `one_phase` call
/// under the lock before installing or applying anything.
///
/// ### Persistent hosts
///
/// Over a `persistent_staged_state_machine` (persistent-state-machine
/// Requirement 8) the wrapper is itself a persistent state machine. Each
/// entry stages its inner writes and the changed keys' transaction state into
/// one batch, committed once with the applied index, so a crash cannot keep a
/// commit and lose its intent's release, or the reverse. A command that
/// throws part way through leaves nothing in the engine: the batch is
/// discarded and the maps are read back. The maps stay in memory as a cache
/// of the engine's `txn` column family, read back at `open`, `ingest` and
/// `adopt`, so a restart reads the transaction state held (locks and records
/// not yet cleaned up), never the data.

#include <raft/exceptions.hpp>
#include <raft/multi_version_state_machine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/shard_types.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
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
    /// `snapshot` and `serializable` only: the start timestamp to use, from
    /// an earlier oracle allocation (a Redis `WATCH` takes it when the watch
    /// begins). Zero takes a fresh one at `begin()`.
    std::uint64_t start_ts{0};
};

/// @brief An intent as a read path sees it (Requirement 5.5).
template<shard_key Key> struct intent_view {
    txn_id _txn{};
    Key _primary{};
    /// The prepare's TTL deadline, for the resolver (task 7).
    std::uint64_t _deadline_ms{0};
    /// The holder's start timestamp; zero below tier 2. A snapshot read at
    /// or above it must wait for or resolve the holder (Requirement 8.4).
    std::uint64_t _start_ts{0};

    [[nodiscard]] auto operator==(const intent_view&) const -> bool = default;
};

/// @brief A transaction record as `transaction_status()` sees it.
struct txn_record_view {
    txn_record_status _status{txn_record_status::pending};
    std::uint64_t _deadline_ms{0};
    std::uint64_t _commit_ts{0};

    [[nodiscard]] auto operator==(const txn_record_view&) const -> bool = default;
};

/// @brief A transaction holding intents on this shard, as the leader-side
///        resolver sees it (task 7).
template<shard_key Key> struct txn_open_view {
    txn_id _txn{};
    Key _primary{};
    /// Every key this shard holds locked for the transaction, in key order.
    std::vector<Key> _keys{};
    /// The earliest of their prepares' TTL deadlines. A heartbeat extends
    /// only the primary's record, so on a secondary this is a hint to ask the
    /// primary, not a verdict.
    std::uint64_t _deadline_ms{0};

    [[nodiscard]] auto operator==(const txn_open_view&) const -> bool = default;
};

/// @brief One write's verdict from `validate` (see
///        `txn_validating_state_machine`).
struct txn_validation {
    bool _ok{true};
    /// Why the write is refused, or what an accepted one observed (a
    /// lock-based read's value, say). Opaque to the wrapper: it reaches the
    /// coordinator's outcome as `txn_validation_entry::_detail`.
    std::vector<std::byte> _detail{};
};

/// @brief A wrapped state machine that checks each transactional write while
///        its intent is installed (dynamodb-compatible-api Requirement 12.1).
///
/// The wrapper cannot pre-check an opaque command (see "Atomicity and the
/// wrapped state machine" above), so an application whose writes carry
/// conditions supplies this hook. `prepare` and `one_phase` call it for every
/// write after the lock checks and before installing anything, against the
/// committed state: the key is not locked by anyone else at that point, so
/// what `validate` sees is what the write will apply to. A refusal of any
/// write installs nothing and applies nothing, and the result lists every
/// write's verdict, so the caller can say which actions failed.
///
/// `validate` must be deterministic in the command and the committed state,
/// as `apply` is. Plain writes are not validated: they apply their own
/// checks.
template<typename SM, typename Key>
concept txn_validating_state_machine =
    requires(const SM& sm, const Key& key, const std::vector<std::byte>& command) {
        { sm.validate(key, command) } -> std::same_as<txn_validation>;
    };

namespace txn_detail {
/// The wrapped machine's engine, or a placeholder for a byte-buffer one, so
/// the persistent members' signatures stay well formed on every host.
struct no_engine {};
template<typename SM> struct engine_of {
    using type = no_engine;
};
template<typename SM>
requires requires { typename SM::engine_type; }
struct engine_of<SM> {
    using type = typename SM::engine_type;
};
}  // namespace txn_detail

/// @brief Wraps `SM` so its host can take part in cross-shard transactions.
///
/// @tparam SM       The application's state machine. Must satisfy
///                  `state_machine<SM, std::uint64_t>`, or
///                  `persistent_staged_state_machine` for a host whose state
///                  lives in a `kv_engine` (see "Persistent hosts" below).
/// @tparam Key      The host's shard key.
/// @tparam P        The host's partitioner: `P{}.key_of(command)` gives the
///                  key a plain command writes. Stated as a requires-clause
///                  rather than with `partitioner<P, Key>` from
///                  `multi_raft.hpp`, which will include this header.
/// @tparam Mvcc     `mvcc::on` needs `SM` to be a
///                  `multi_version_state_machine` (task 12). Every write is
///                  then applied at its transaction's commit timestamp, and
///                  a prepare or one-phase commit with a start timestamp
///                  fails with `write_conflict` if a written key has a
///                  version newer than it (first committer wins,
///                  Requirement 8.3). Nothing sends a one-phase entry
///                  then: it carries no intent, so a reader that takes a
///                  later start timestamp could read the shard before the
///                  entry applies and then see it appear below its read
///                  point. Writes go through prepare, then a commit
///                  timestamp taken after it, then commit.
/// @tparam KeyCodec How keys are written into envelopes and snapshots.
template<typename SM, shard_key Key, typename P, mvcc Mvcc = mvcc::off,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
requires(state_machine<SM, std::uint64_t> || persistent_staged_state_machine<SM, std::uint64_t>) &&
        std::default_initializable<P> &&
        requires(const P& p, const std::vector<std::byte>& command) {
            { p.key_of(command) } -> std::same_as<Key>;
        } && (Mvcc == mvcc::off || multi_version_state_machine<SM, Key>)
class transactional_state_machine {
public:
    using inner_type = SM;
    using key_type = Key;
    using key_codec_type = KeyCodec;
    using log_index_type = std::uint64_t;
    using envelope_type = txn_envelope<Key>;
    using result_type = txn_result<Key>;
    static constexpr mvcc k_mvcc = Mvcc;

    /// True when the wrapped state machine keeps its state in a `kv_engine`.
    /// The three maps are then written to the engine's `txn` column family
    /// in the same batch as the inner effects (persistent-state-machine
    /// Requirement 8), and the wrapper is itself a persistent state machine.
    static constexpr bool k_persistent = persistent_staged_state_machine<SM, std::uint64_t>;
    static_assert(!k_persistent || std::same_as<Key, std::string>,
                  "a persistent transactional host needs std::string keys, the engine's");
    static_assert(!k_persistent || Mvcc == mvcc::off,
                  "versioned (mvcc::on) persistent hosts are not built yet");

    /// @brief The snapshot header, distinct from any unwrapped format
    /// (Requirement 1.4).
    static constexpr std::string_view k_snapshot_magic = "kythira-txn-sm";
    /// Version 2 adds each intent's start timestamp, and version 3 the
    /// garbage-collection safe point. `mvcc::on` writes version 3;
    /// `mvcc::off` still writes version 1, byte for byte as before. Every
    /// version reads.
    static constexpr std::uint8_t k_snapshot_version = Mvcc == mvcc::on ? 3 : 1;

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
        if constexpr (k_persistent) {
            return apply_persistent(env, index);
        } else {
            const auto res = std::visit([&](const auto& op) { return apply_op(op, index); }, env);
            return encode_txn_result<Key>(res, _codec);
        }
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte>
    requires(!k_persistent)
    {
        return encode_state(_intents, _records, _settled, _oracle_hwm, _safe_point,
                            _inner.get_state());
    }

    /// @throws serialization_exception for a snapshot in any other format,
    ///         naming both, so a host restored from an unwrapped snapshot (or
    ///         the reverse) fails at once rather than misreading the bytes.
    auto restore_from_snapshot(const std::vector<std::byte>& data, log_index_type index) -> void
    requires(!k_persistent)
    {
        auto decoded = decode_state(data);
        _inner.restore_from_snapshot(decoded._inner, index);
        _intents = std::move(decoded._intents);
        _records = std::move(decoded._records);
        _settled = std::move(decoded._settled);
        _oracle_hwm = decoded._oracle_hwm;
        _safe_point = decoded._safe_point;
    }

    // ── splittable_state_machine, exactly when SM is (Requirement 1.3) ──────

    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t
    requires split_policy_state_machine<SM, Key>
    {
        std::size_t locks = 0;
        for (const auto& [key, intent] : _intents) {
            locks += _codec.encode(key).size() + intent._command.size() + 64;
        }
        return _inner.approximate_size_bytes() + locks;
    }

    [[nodiscard]] auto approximate_key_count() const -> std::size_t
    requires split_policy_state_machine<SM, Key>
    {
        return _inner.approximate_key_count();
    }

    [[nodiscard]] auto suggest_split_keys(std::size_t max) -> std::vector<Key>
    requires split_policy_state_machine<SM, Key>
    {
        return _inner.suggest_split_keys(max);
    }

    /// Intents and records move with their keys, so the wrapper adds no veto.
    [[nodiscard]] auto can_split_at(const Key& k) const -> bool
    requires split_policy_state_machine<SM, Key>
    {
        return _inner.can_split_at(k);
    }

    /// @brief Cut every map at the same keys as the wrapped state machine
    /// (Requirement 6.1). Intents and settled entries go with their own key,
    /// records with their primary key, and the oracle high-water mark with
    /// the leftmost child (it is zero outside the oracle group). Every child
    /// keeps the safe point: their versions below it are gone too.
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
                                       _safe_point, inner_parts[i]));
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
        // Versions below either side's safe point may be gone from that
        // side, so the merged shard can only promise reads above both.
        _safe_point = std::max(_safe_point, decoded._safe_point);
    }

    // ── persistent_splittable_state_machine, exactly when SM is persistent ──
    //
    // persistent-state-machine Requirement 8. The maps stay in memory as a
    // cache of the engine's `txn` column family: `open`, `ingest` and
    // `adopt` read the group's range of it back, and every entry writes the
    // keys it changed in the batch that carries the inner effects and the
    // applied index. The snapshot is the engine checkpoint, which covers
    // every column family (8.2), and a split or merge moves transaction state
    // with its range exactly as `split_state`/`absorb` slice it (8.3).

    using engine_type = typename txn_detail::engine_of<SM>::type;

    auto open(engine_type& engine, const key_range& range) -> void
    requires k_persistent
    {
        _inner.open(engine, range);
        _engine = &engine;
        load_txn_state();
    }

    [[nodiscard]] auto applied_index() const -> log_index_type
    requires k_persistent
    {
        return _inner.applied_index();
    }

    [[nodiscard]] auto durable_applied_index() const -> log_index_type
    requires k_persistent
    {
        return _inner.durable_applied_index();
    }

    /// The inner machine's sync, when it has one: every batch this wrapper
    /// commits is the inner machine's batch, so syncing it covers the
    /// transaction state too.
    auto sync_durable() -> void
    requires k_persistent && requires(SM& i) { i.sync_durable(); }
    {
        _inner.sync_durable();
    }

    auto checkpoint(const std::filesystem::path& dir) -> snapshot_manifest
    requires k_persistent
    {
        return _inner.checkpoint(dir);
    }

    auto ingest(const std::filesystem::path& dir, const snapshot_manifest& manifest,
                log_index_type index) -> void
    requires k_persistent
    {
        _inner.ingest(dir, manifest, index);
        load_txn_state();
    }

    auto drop() -> void
    requires k_persistent
    {
        _inner.drop();
        clear_txn_state();
    }

    /// Cuts the range where the wrapped machine does. The children's
    /// transaction state is already in their ranges of the engine, so this
    /// only forgets it here. The oracle high-water mark stays with the
    /// parent, the leftmost part, as in `split_state`.
    auto split_at(const std::vector<std::string>& keys) -> std::vector<key_range>
    requires k_persistent
    {
        auto children = _inner.split_at(keys);
        const auto& kept = _inner.range();
        const auto outside = [&](const Key& k) { return !kept.contains(k); };
        std::erase_if(_intents, [&](const auto& kv) { return outside(kv.first); });
        std::erase_if(_records, [&](const auto& kv) { return outside(kv.first); });
        std::erase_if(_settled, [&](const auto& kv) { return outside(kv.first); });
        return children;
    }

    /// Widens the range over an adjacent one and reads its transaction state
    /// back. The two high-water marks and safe points merge by maximum, as
    /// in `absorb`, and are written at the widened range's start. That write
    /// is a second batch after the wrapped machine's: a crash between them
    /// leaves the older values at the start, and the source's at a key that
    /// is no longer any group's start. Both are harmless, because these
    /// values only ever merge by maximum and are read only at a group's
    /// start; a later split that starts a child there gives the child at most
    /// a high-water mark it never uses, outside the oracle group.
    auto adopt(const key_range& range) -> void
    requires k_persistent
    {
        if (_inner.range().contains(range)) {
            return;
        }
        const auto source = read_header(range.start);
        const auto hwm = std::max(_oracle_hwm, source.first);
        const auto safe_point = std::max(_safe_point, source.second);
        _inner.adopt(range);
        load_txn_state();
        if (hwm != _oracle_hwm || safe_point != _safe_point) {
            _oracle_hwm = hwm;
            _safe_point = safe_point;
            write_batch batch;
            write_key_state(batch, _inner.range().start);
            _engine->write(batch);
        }
    }

    [[nodiscard]] auto range() const -> const key_range&
    requires k_persistent
    {
        return _inner.range();
    }

    // ── Read paths (Requirement 5.5) ─────────────────────────────────────────

    [[nodiscard]] auto inner() -> SM& { return _inner; }
    [[nodiscard]] auto inner() const -> const SM& { return _inner; }

    /// @brief The write intent on `k`, if any. A read intent (tier 3) is not
    ///        reported: it holds no value a reader could miss, so no read
    ///        need wait for it. Writers still meet it as a conflict.
    [[nodiscard]] auto intent_on(const Key& k) const -> std::optional<intent_view<Key>> {
        const auto it = _intents.find(k);
        if (it == _intents.end() || it->second.is_read()) {
            return std::nullopt;
        }
        return intent_view<Key>{it->second._txn, it->second._primary, it->second._deadline_ms,
                                it->second._start_ts};
    }

    /// @brief What a snapshot read at `ts` sees on `key` (Requirement 8.4):
    ///        the newest version at or below `ts`, and any intent. The
    ///        caller must not use the value while the intent's start
    ///        timestamp is at or below `ts`: the holder may yet commit
    ///        below it.
    [[nodiscard]] auto read_at(const Key& k, std::uint64_t ts) const
    requires(Mvcc == mvcc::on)
    {
        return std::pair{_inner.read_at(k, ts), intent_on(k)};
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

    // ── Resolver views (task 7, Requirements 4.6 and 4.7) ────────────────────
    //
    // Read-only, and read by the shard leader only: what they return decides
    // which entries the leader proposes, and apply then re-checks everything
    // against the state at that point in the log.

    /// @brief Every transaction holding an intent here, ordered by txn id.
    [[nodiscard]] auto open_transactions() const -> std::vector<txn_open_view<Key>> {
        std::map<txn_id, txn_open_view<Key>> by_txn;
        for (const auto& [k, in] : _intents) {
            auto [it, fresh] = by_txn.try_emplace(in._txn);
            auto& v = it->second;
            if (fresh) {
                v._txn = in._txn;
                v._primary = in._primary;
                v._deadline_ms = in._deadline_ms;
            } else {
                v._deadline_ms = std::min(v._deadline_ms, in._deadline_ms);
            }
            v._keys.push_back(k);  // _intents is ordered, so _keys is too
        }
        std::vector<txn_open_view<Key>> out;
        out.reserve(by_txn.size());
        for (auto& [id, v] : by_txn) {
            out.push_back(std::move(v));
        }
        return out;
    }

    /// @brief Every transaction this shard has finished and still remembers:
    ///        a `committed` or `aborted` record, or settled entries, and no
    ///        intent left here. These are what `txn_cleanup` may drop once
    ///        the retention has passed.
    ///
    /// Each item names the record's primary key when there is a record, or
    /// else one of the settled keys; `txn_cleanup` drops the record at that
    /// key and the transaction's settled entries everywhere on the shard.
    [[nodiscard]] auto finished_transactions() const -> std::vector<txn_cleanup_item<Key>> {
        // Transactions that are not finished here: holding a lock, or with a
        // pending record.
        std::set<txn_id> open;
        for (const auto& [k, in] : _intents) {
            open.insert(in._txn);
        }
        for (const auto& [primary, by_txn] : _records) {
            for (const auto& [id, rec] : by_txn) {
                if (rec._status == txn_record_status::pending) {
                    open.insert(id);
                }
            }
        }
        std::map<txn_id, txn_cleanup_item<Key>> out_by_txn;
        for (const auto& [primary, by_txn] : _records) {
            for (const auto& [id, rec] : by_txn) {
                if (!open.contains(id)) {
                    out_by_txn.try_emplace(id, txn_cleanup_item<Key>{primary, id});
                }
            }
        }
        for (const auto& [k, by_txn] : _settled) {
            for (const auto& [id, status] : by_txn) {
                if (!open.contains(id)) {
                    out_by_txn.try_emplace(id, txn_cleanup_item<Key>{k, id});
                }
            }
        }
        std::vector<txn_cleanup_item<Key>> out;
        out.reserve(out_by_txn.size());
        for (auto& [id, item] : out_by_txn) {
            out.push_back(std::move(item));
        }
        return out;
    }
    [[nodiscard]] auto oracle_high_water_mark() const -> std::uint64_t { return _oracle_hwm; }

    /// @brief The highest safe point a `gc` entry has collected to: reads
    ///        and transactions below it fail with `snapshot_too_old`
    ///        (Requirement 8.6). Zero until the first `gc`.
    [[nodiscard]] auto safe_point() const -> std::uint64_t { return _safe_point; }

private:
    struct intent {
        txn_id _txn{};
        Key _primary{};
        /// Empty for a read intent (tier 3): no write is pending, but writers
        /// are blocked until the holder finishes (Requirement 9.2). A write
        /// command is never empty, so the snapshot format needs no flag.
        std::vector<std::byte> _command{};
        std::uint64_t _deadline_ms{0};
        std::uint64_t _start_ts{0};

        [[nodiscard]] auto is_read() const -> bool { return _command.empty(); }
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
        res._payload = apply_inner(op._command, 0, index);
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
        if (auto old = too_old(op._start_ts); old.has_value()) {
            return *old;
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            if (auto conflict = conflict_on(w._key, &op._txn); conflict.has_value()) {
                conflict->_status = txn_result_status::write_conflict;
                return *conflict;
            }
            if (auto newer = newer_version_on(w._key, op._start_ts); newer.has_value()) {
                return *newer;
            }
        }
        auto verdicts = validate_writes(op._writes);
        if (verdicts._status == txn_result_status::validation_failed) {
            return verdicts;
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            std::ignore = apply_inner(w._command, op._commit_ts, index);
            _settled[w._key][op._txn] = txn_record_status::committed;
            touch(w._key);
        }
        return verdicts;
    }

    auto apply_op(const txn_prepare<Key>& op, log_index_type) -> result_type {
        // Tier 3's read keys never arrive under mvcc::off: a serializable
        // transaction is refused at begin() on such a host (Requirement 10.2).
        // Refuse them anyway rather than lock keys no version can validate.
        if constexpr (Mvcc == mvcc::off) {
            if (!op._read_keys.empty()) {
                return status_only(txn_result_status::read_conflict);
            }
        }
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
        // Tier 3: only the keys read and not written; a written key's write
        // intent already holds it, and its newer-version check is the same.
        std::vector<Key> reads;
        for (const auto& k : op._read_keys) {
            if (std::none_of(op._writes.begin(), op._writes.end(),
                             [&](const auto& w) { return w._key == k; })) {
                reads.push_back(k);
            }
        }
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        for (const auto& k : reads) {
            if (const auto s = settled_status(k, op._txn); s.has_value()) {
                return *s == txn_record_status::committed
                           ? result_type{}
                           : status_only(txn_result_status::already_aborted);
            }
        }
        if (auto old = too_old(op._start_ts); old.has_value()) {
            return *old;
        }
        for (const auto& w : sorted_by_key(op._writes)) {
            if (auto conflict = conflict_on(w._key, &op._txn); conflict.has_value()) {
                conflict->_status = txn_result_status::write_conflict;
                return *conflict;
            }
            if (auto newer = newer_version_on(w._key, op._start_ts); newer.has_value()) {
                return *newer;
            }
        }
        // Requirement 9.2: a read key fails validation if a version committed
        // after the start timestamp, or another transaction holds it.
        for (const auto& k : reads) {
            if (auto conflict = conflict_on(k, &op._txn); conflict.has_value()) {
                conflict->_status = txn_result_status::read_conflict;
                return *conflict;
            }
            if (auto newer = newer_version_on(k, op._start_ts); newer.has_value()) {
                newer->_status = txn_result_status::read_conflict;
                return *newer;
            }
        }
        auto verdicts = validate_writes(op._writes);
        if (verdicts._status == txn_result_status::validation_failed) {
            return verdicts;
        }
        for (const auto& w : op._writes) {
            _intents.insert_or_assign(w._key, intent{op._txn, op._primary, w._command,
                                                     op._ttl_deadline_ms, op._start_ts});
            touch(w._key);
        }
        // A read intent is an intent with no command: it blocks writers as a
        // write intent does, and rolls forward to nothing. A key this
        // transaction already holds (a retried prepare) keeps its intent.
        for (const auto& k : reads) {
            if (const auto it = _intents.find(k); it == _intents.end()) {
                _intents.emplace(
                    k, intent{op._txn, op._primary, {}, op._ttl_deadline_ms, op._start_ts});
                touch(k);
            }
        }
        if (op._is_primary) {
            auto& rec = _records[op._primary][op._txn];
            rec._deadline_ms = std::max(rec._deadline_ms, op._ttl_deadline_ms);
            touch(op._primary);
        }
        return verdicts;
    }

    auto apply_op(const txn_commit_primary<Key>& op, log_index_type index) -> result_type {
        auto* rec = find_record(op._primary, op._txn);
        if (rec == nullptr) {
            // Never prepared here, or cleaned up long after it finished. Leave
            // a rollback marker, as check_and_resolve would: a prepare that
            // arrives later must not then succeed (Requirement 4.4).
            _records[op._primary][op._txn]._status = txn_record_status::aborted;
            touch(op._primary);
            return record_result({txn_record_status::aborted, 0, 0});
        }
        if (rec->_status == txn_record_status::aborted) {
            return record_result(*rec);
        }
        result_type res;
        if (rec->_status == txn_record_status::pending) {
            rec->_status = txn_record_status::committed;
            rec->_commit_ts = op._commit_ts;
            touch(op._primary);
            // The primary's own write first: its reply is the commit's
            // payload, which a host's plain write under mvcc::on (a one-key
            // transaction) hands back to its caller.
            if (auto payload = roll_forward(op._primary, op._txn, op._commit_ts, index);
                payload.has_value()) {
                res._payload = std::move(*payload);
            }
            roll_forward_all(op._txn, op._commit_ts, index);
        }
        return res;
    }

    auto apply_op(const txn_commit_secondary<Key>& op, log_index_type index) -> result_type {
        for (const auto& k : sorted_keys(op._keys)) {
            std::ignore = roll_forward(k, op._txn, op._commit_ts, index);
        }
        return {};
    }

    auto apply_op(const txn_abort<Key>& op, log_index_type) -> result_type {
        const bool on_primary =
            std::find(op._keys.begin(), op._keys.end(), op._primary) != op._keys.end();
        if (on_primary) {
            auto& rec = _records[op._primary][op._txn];
            touch(op._primary);
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
            touch(op._primary);
        }
        return record_result(*rec);
    }

    /// Requirement 4.3's table.
    auto apply_op(const txn_check_and_resolve<Key>& op, log_index_type) -> result_type {
        auto* rec = find_record(op._primary, op._txn);
        if (rec == nullptr) {
            _records[op._primary][op._txn]._status = txn_record_status::aborted;
            touch(op._primary);
            return record_result({txn_record_status::aborted, 0, 0});
        }
        if (rec->_status == txn_record_status::pending && rec->_deadline_ms <= op._now_ms) {
            rec->_status = txn_record_status::aborted;
            touch(op._primary);
            // The primary's own intents are on this shard; release them now.
            roll_back_all(op._txn);
        }
        return record_result(*rec);
    }

    /// Drops each item's record and settled entries. A `pending` record is
    /// never dropped, and its item is skipped whole: the leader proposes only
    /// finished transactions, so a pending one here means the log moved on
    /// between the proposal and this entry (Requirement 4.7: nothing is
    /// removed that is not known to be finished).
    auto apply_op(const txn_cleanup<Key>& op, log_index_type) -> result_type {
        for (const auto& item : op._items) {
            if (auto it = _records.find(item._primary); it != _records.end()) {
                if (const auto jt = it->second.find(item._txn);
                    jt != it->second.end() && jt->second._status == txn_record_status::pending) {
                    continue;
                }
                if (it->second.erase(item._txn) != 0) {
                    touch(item._primary);
                }
                if (it->second.empty()) {
                    _records.erase(it);
                }
            }
            for (auto it = _settled.begin(); it != _settled.end();) {
                if (it->second.erase(item._txn) != 0) {
                    touch(it->first);
                }
                it = it->second.empty() ? _settled.erase(it) : std::next(it);
            }
        }
        return {};
    }

    /// Tier 2: drop versions no read at or above the safe point can see.
    /// The payload is the number dropped, as eight little-endian bytes, for
    /// the host's metric. A safe point at or below the current one changes
    /// nothing. Nothing is versioned under mvcc::off, so there is nothing to
    /// collect.
    auto apply_op(const txn_gc& op, log_index_type) -> result_type {
        result_type res;
        if constexpr (Mvcc == mvcc::on) {
            if (op._safe_point > _safe_point) {
                _safe_point = op._safe_point;
                touch_header();
                const auto dropped =
                    static_cast<std::uint64_t>(_inner.collect_garbage(op._safe_point));
                for (int i = 0; i < 8; ++i) {
                    res._payload.push_back(static_cast<std::byte>((dropped >> (8 * i)) & 0xFF));
                }
            }
        }
        return res;
    }

    auto apply_op(const txn_oracle_reserve& op, log_index_type) -> result_type {
        if (op._new_hwm > _oracle_hwm) {
            _oracle_hwm = op._new_hwm;
            touch_header();
        }
        return {};
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    /// Every write's verdict from the wrapped state machine's `validate`,
    /// in key order: `validation_failed` if any write is refused,
    /// `validated` if all pass and some had something to say, else a plain
    /// `ok` with no list (also when `SM` does not validate).
    [[nodiscard]] auto validate_writes(const std::vector<txn_write<Key>>& writes) const
        -> result_type {
        result_type res;
        if constexpr (txn_validating_state_machine<SM, Key>) {
            bool failed = false;
            bool detail = false;
            for (const auto& w : sorted_by_key(writes)) {
                auto v = _inner.validate(w._key, w._command);
                failed = failed || !v._ok;
                detail = detail || !v._detail.empty();
                res._validations.push_back(
                    txn_validation_entry<Key>{w._key, v._ok, std::move(v._detail)});
            }
            if (failed) {
                res._status = txn_result_status::validation_failed;
            } else if (detail) {
                res._status = txn_result_status::validated;
            } else {
                res._validations.clear();
            }
        }
        return res;
    }

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

    /// Requirement 8.6: a transaction that started below the safe point may
    /// have read versions that are gone, so it may not write either.
    [[nodiscard]] auto too_old(std::uint64_t start_ts) const -> std::optional<result_type> {
        if constexpr (Mvcc == mvcc::on) {
            if (start_ts != 0 && start_ts < _safe_point) {
                result_type res = status_only(txn_result_status::snapshot_too_old);
                res._safe_point = _safe_point;
                return res;
            }
        }
        return std::nullopt;
    }

    /// First committer wins (Requirement 8.3): under `mvcc::on`, a write
    /// conflicts with any version of its key committed after the writer's
    /// start timestamp. A zero start timestamp (tier 1, or a plain write)
    /// asks for no such check.
    [[nodiscard]] auto newer_version_on(const Key& key, std::uint64_t start_ts) const
        -> std::optional<result_type> {
        if constexpr (Mvcc == mvcc::on) {
            if (start_ts != 0) {
                if (const auto ts = _inner.latest_commit_ts(key);
                    ts.has_value() && *ts > start_ts) {
                    // No holder: a version, not a lock, so retrying cannot
                    // help.
                    result_type res = status_only(txn_result_status::write_conflict);
                    res._key = key;
                    return res;
                }
            }
        }
        return std::nullopt;
    }

    /// Apply one write to the wrapped state machine: at `commit_ts` under
    /// `mvcc::on` when there is one, else as a plain apply.
    auto apply_inner(const std::vector<std::byte>& command, std::uint64_t commit_ts,
                     log_index_type index) -> std::vector<std::byte> {
        if constexpr (Mvcc == mvcc::on) {
            if (commit_ts != 0) {
                return _inner.apply_at(command, commit_ts, index);
            }
        }
        if constexpr (k_persistent) {
            // Every write of the entry goes into the one batch that
            // `apply_persistent` commits with the applied index.
            return _inner.stage(command, _batch);
        } else {
            return _inner.apply(command, index);
        }
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

    /// Apply and release `txn`'s intent on `key`, if it still holds one;
    /// the wrapped state machine's reply when it did.
    auto roll_forward(const Key& key, const txn_id& txn, std::uint64_t commit_ts,
                      log_index_type index) -> std::optional<std::vector<std::byte>> {
        const auto it = _intents.find(key);
        if (it == _intents.end() || it->second._txn != txn) {
            return std::nullopt;  // already resolved, or never prepared here
        }
        std::optional<std::vector<std::byte>> payload;
        if (!it->second.is_read()) {
            payload = apply_inner(it->second._command, commit_ts, index);
        }
        _intents.erase(it);
        _settled[key][txn] = txn_record_status::committed;
        touch(key);
        return payload;
    }

    auto roll_back(const Key& key, const txn_id& txn) -> void {
        const auto it = _intents.find(key);
        if (it != _intents.end() && it->second._txn == txn) {
            _intents.erase(it);
        }
        // Remembered even when no intent was here, so a prepare that arrives
        // after its abort installs nothing.
        _settled[key][txn] = txn_record_status::aborted;
        touch(key);
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

    auto roll_forward_all(const txn_id& txn, std::uint64_t commit_ts, log_index_type index)
        -> void {
        for (const auto& k : keys_held_by(txn)) {
            std::ignore = roll_forward(k, txn, commit_ts, index);
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

    // ── persistent storage (persistent-state-machine Requirement 8) ─────────
    //
    // One `txn` column family value per key, at the same engine key as the
    // key's data (`prefix + key`), holding everything the three maps keep
    // under that key: its intent, the records whose primary it is, and its
    // settled entries. The group's high-water mark and safe point ride in the
    // value at the range's start key. One value per key, rather than the
    // design's `<key>/i`, `<primary>/r/<txn_id>` and `<key>/s/<txn_id>`,
    // keeps every engine key inside the group's range whatever the range's
    // bounds are: a suffixed key can sort past an end bound that extends the
    // key (`[k, k + "\x00")` holds `k` and nothing longer).
    //
    // Value layout: u8 version (1), u8 flags (bit 0: header, bit 1: intent),
    // [u64 high-water mark, u64 safe point], [intent: txn id, primary,
    // command, deadline, start ts], u64 record count then (txn id, status,
    // deadline, commit ts) each, u64 settled count then (txn id, status) each.

    static constexpr std::uint8_t k_txn_value_version = 1;
    static constexpr std::uint8_t k_flag_header = 0x01;
    static constexpr std::uint8_t k_flag_intent = 0x02;

    auto touch(const Key& k) -> void {
        if constexpr (k_persistent) {
            _dirty.insert(k);
        }
    }

    auto touch_header() -> void {
        if constexpr (k_persistent) {
            _header_dirty = true;
        }
    }

    template<typename Envelope>
    auto apply_persistent(const Envelope& env, log_index_type index) -> std::vector<std::byte> {
        if (index <= _inner.applied_index()) {
            return encode_txn_result<Key>(result_type{}, _codec);  // already in the engine
        }
        _batch = write_batch{};
        _dirty.clear();
        _header_dirty = false;
        try {
            const auto res = std::visit([&](const auto& op) { return apply_op(op, index); }, env);
            for (const auto& k : _dirty) {
                write_key_state(_batch, k);
            }
            if (_header_dirty && !_dirty.contains(_inner.range().start)) {
                write_key_state(_batch, _inner.range().start);
            }
            _inner.commit_staged(_batch, index);
            _batch = write_batch{};
            return encode_txn_result<Key>(res, _codec);
        } catch (...) {
            // Nothing reached the engine; put the maps back as it has them.
            _inner.discard_staged();
            _batch = write_batch{};
            load_txn_state();
            throw;
        }
    }

    [[nodiscard]] auto engine_key(const Key& k) const -> std::string {
        return _inner.key_prefix() + k;
    }

    /// Puts `k`'s current value in `batch`, or erases it when nothing is left.
    auto write_key_state(write_batch& batch, const Key& k) const -> void {
        using namespace txn_wire;
        const bool header = k == _inner.range().start && (_oracle_hwm != 0 || _safe_point != 0);
        const auto in = _intents.find(k);
        const auto rec = _records.find(k);
        const auto set = _settled.find(k);
        const bool has_intent = in != _intents.end();
        const bool has_records = rec != _records.end() && !rec->second.empty();
        const bool has_settled = set != _settled.end() && !set->second.empty();
        if (!header && !has_intent && !has_records && !has_settled) {
            batch.erase(column_family::txn, engine_key(k));
            return;
        }
        std::vector<std::byte> out;
        write_u8(out, k_txn_value_version);
        write_u8(out, static_cast<std::uint8_t>((header ? k_flag_header : 0) |
                                                (has_intent ? k_flag_intent : 0)));
        if (header) {
            write_u64(out, _oracle_hwm);
            write_u64(out, _safe_point);
        }
        if (has_intent) {
            write_txn_id(out, in->second._txn);
            write_key(out, in->second._primary, _codec);
            write_blob(out, in->second._command, k_txn_max_command_bytes, "intent command");
            write_u64(out, in->second._deadline_ms);
            write_u64(out, in->second._start_ts);
        }
        write_u64(out, has_records ? rec->second.size() : 0);
        if (has_records) {
            for (const auto& [id, r] : rec->second) {
                write_txn_id(out, id);
                write_u8(out, static_cast<std::uint8_t>(r._status));
                write_u64(out, r._deadline_ms);
                write_u64(out, r._commit_ts);
            }
        }
        write_u64(out, has_settled ? set->second.size() : 0);
        if (has_settled) {
            for (const auto& [id, status] : set->second) {
                write_txn_id(out, id);
                write_u8(out, static_cast<std::uint8_t>(status));
            }
        }
        batch.put(column_family::txn, engine_key(k),
                  std::string(reinterpret_cast<const char*>(out.data()), out.size()));  // NOLINT
    }

    struct decoded_key_state {
        bool header{false};
        std::uint64_t hwm{0};
        std::uint64_t safe_point{0};
        std::optional<intent> in;
        std::map<txn_id, txn_record_view> records;
        std::map<txn_id, txn_record_status> settled;
    };

    [[nodiscard]] auto decode_key_state(std::string_view stored) const -> decoded_key_state {
        using namespace txn_wire;
        const std::vector<std::byte> bytes(
            reinterpret_cast<const std::byte*>(stored.data()),
            reinterpret_cast<const std::byte*>(stored.data()) + stored.size());  // NOLINT
        reader r{bytes, "kythira-txn-sm engine value"};
        if (const auto v = r.u8(); v != k_txn_value_version) {
            r.fail("unknown transaction state version " + std::to_string(v));
        }
        const auto flags = r.u8();
        decoded_key_state out;
        if ((flags & k_flag_header) != 0) {
            out.header = true;
            out.hwm = r.u64();
            out.safe_point = r.u64();
        }
        if ((flags & k_flag_intent) != 0) {
            intent in;
            in._txn = read_txn_id(r);
            in._primary = read_key<Key>(r, _codec);
            in._command = r.blob(k_txn_max_command_bytes, "intent command");
            in._deadline_ms = r.u64();
            in._start_ts = r.u64();
            out.in = std::move(in);
        }
        const auto records = r.count(2 * sizeof(std::uint64_t) + 1 + 2 * sizeof(std::uint64_t),
                                     "record list", bytes.size());
        for (std::size_t i = 0; i < records; ++i) {
            const auto id = read_txn_id(r);
            txn_record_view rec;
            rec._status = read_record_status(r);
            rec._deadline_ms = r.u64();
            rec._commit_ts = r.u64();
            out.records[id] = rec;
        }
        const auto settled = r.count(2 * sizeof(std::uint64_t) + 1, "settled list", bytes.size());
        for (std::size_t i = 0; i < settled; ++i) {
            const auto id = read_txn_id(r);
            out.settled[id] = read_record_status(r);
        }
        r.finish();
        return out;
    }

    auto clear_txn_state() -> void {
        _intents.clear();
        _records.clear();
        _settled.clear();
        _oracle_hwm = 0;
        _safe_point = 0;
    }

    /// The engine range holding this group's keys.
    [[nodiscard]] auto engine_range() const -> key_range {
        const auto& ns = _inner.key_prefix();
        const auto& r = _inner.range();
        std::optional<std::string> end;
        if (r.end.has_value()) {
            end = ns + *r.end;
        } else {
            std::string succ = ns;
            while (!succ.empty() && static_cast<unsigned char>(succ.back()) == 0xFFU) {
                succ.pop_back();
            }
            if (!succ.empty()) {
                succ.back() = static_cast<char>(static_cast<unsigned char>(succ.back()) + 1U);
                end = std::move(succ);
            }
        }
        return key_range{ns + r.start, std::move(end)};
    }

    /// Reads the group's range of the `txn` column family into the maps.
    /// Bounded by the transaction state held, not by the data.
    auto load_txn_state() -> void {
        clear_txn_state();
        const auto& ns = _inner.key_prefix();
        const auto start = _inner.range().start;
        _engine->scan(column_family::txn, engine_range(), scan_direction::forward,
                      [&](std::string_view key, std::string_view stored) {
                          Key k(key.substr(ns.size()));
                          auto d = decode_key_state(stored);
                          if (d.header && k == start) {
                              _oracle_hwm = d.hwm;
                              _safe_point = d.safe_point;
                          }
                          if (d.in.has_value()) {
                              _intents.insert_or_assign(k, std::move(*d.in));
                          }
                          if (!d.records.empty()) {
                              _records[k] = std::move(d.records);
                          }
                          if (!d.settled.empty()) {
                              _settled[k] = std::move(d.settled);
                          }
                          return true;
                      });
    }

    /// The high-water mark and safe point stored at `start`, or zeros.
    [[nodiscard]] auto read_header(const std::string& start) const
        -> std::pair<std::uint64_t, std::uint64_t> {
        const auto stored = _engine->get(column_family::txn, _inner.key_prefix() + start);
        if (!stored.has_value()) {
            return {0, 0};
        }
        const auto d = decode_key_state(*stored);
        return d.header ? std::pair{d.hwm, d.safe_point}
                        : std::pair<std::uint64_t, std::uint64_t>{};
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
        std::uint64_t _safe_point{0};
        std::vector<std::byte> _inner;
    };

    [[nodiscard]] auto encode_state(const intent_map& intents, const record_map& records,
                                    const settled_map& settled, std::uint64_t hwm,
                                    std::uint64_t safe_point,
                                    const std::vector<std::byte>& inner) const
        -> std::vector<std::byte> {
        using namespace txn_wire;
        std::vector<std::byte> out;
        for (char c : k_snapshot_magic) {
            out.push_back(static_cast<std::byte>(c));
        }
        write_u8(out, k_snapshot_version);
        write_u64(out, hwm);
        if constexpr (k_snapshot_version >= 3) {
            write_u64(out, safe_point);
        }

        write_u64(out, intents.size());
        for (const auto& [k, in] : intents) {
            write_key(out, k, _codec);
            write_txn_id(out, in._txn);
            write_key(out, in._primary, _codec);
            write_blob(out, in._command, k_txn_max_command_bytes, "intent command");
            write_u64(out, in._deadline_ms);
            if constexpr (k_snapshot_version >= 2) {
                write_u64(out, in._start_ts);
            }
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
        const auto version = r.u8();
        if (version < 1 || version > 3) {
            r.fail("unknown kythira-txn-sm snapshot version " + std::to_string(version));
        }

        decoded_state out;
        out._oracle_hwm = r.u64();
        if (version >= 3) {
            out._safe_point = r.u64();
        }

        const auto intents = r.count(4 * sizeof(std::uint64_t), "intent table", body.size());
        for (std::size_t i = 0; i < intents; ++i) {
            Key k = read_key<Key>(r, _codec);
            intent in;
            in._txn = read_txn_id(r);
            in._primary = read_key<Key>(r, _codec);
            in._command = r.blob(k_txn_max_command_bytes, "intent command");
            in._deadline_ms = r.u64();
            if (version >= 2) {
                in._start_ts = r.u64();
            }
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
    /// Tier 2: the highest `gc` safe point applied.
    std::uint64_t _safe_point{0};

    // Persistent hosts only: the engine, and the entry being applied.
    engine_type* _engine{nullptr};
    write_batch _batch;
    std::set<Key> _dirty;
    bool _header_dirty{false};
};

/// @brief Whether `SM` is a `transactional_state_machine`, for the host's
/// `if constexpr` (task 5).
template<typename SM> inline constexpr bool is_transactional_state_machine_v = false;

template<typename SM, shard_key Key, typename P, mvcc Mvcc, typename KeyCodec>
inline constexpr bool
    is_transactional_state_machine_v<transactional_state_machine<SM, Key, P, Mvcc, KeyCodec>> =
        true;

}  // namespace kythira
