// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file txn_coordinator.hpp
/// @brief The stateless transaction coordinator (task 6 of
///        `.kiro/specs/multi-raft-transactions/`).
///
/// A coordinator buffers a transaction's writes and, at `commit()`, drives
/// them through the participant shards with the envelope ops of
/// `txn_envelope.hpp`. It keeps nothing durable: everything a crashed
/// coordinator leaves behind is in the shards' replicated state, and
/// `resolve_intent()` (or task 7's resolver) finishes it from there.
///
/// ### The protocol (Requirement 3)
///
/// - Every key on one shard: one `one_phase` entry, all writes or none.
/// - Otherwise: the primary is the smallest key of the largest shard group,
///   so the primary's prepare carries the most work. Every shard's prepare is
///   sent at once; sending the primary's concurrently rather than first is
///   safe because a resolver that finds no record leaves the rollback marker,
///   and a primary prepare that arrives after it fails (Requirement 4.4).
/// - Any conflict, or any prepare that fails: abort every shard, best effort.
/// - All prepared: `commit_primary`. The transaction is committed exactly
///   when that entry applies, and `commit()` reports from its result.
///   Secondaries are then sent without waiting.
///
/// A send that comes back with a routing refusal (a split or merge moved a
/// key) is handed to the client's `refresh()`, and that group's keys are
/// re-partitioned and sent again, until the transaction's timeout.
///
/// ### Clocks
///
/// TTL deadlines and the `now` of a resolve come from the client's
/// `now_ms()`, the participant shards' clock, so a deadline is stamped and
/// judged against the same clock (Requirement 4.1). `apply()` reads none.

#include <raft/shard_types.hpp>
#include <concepts/future.hpp>
#include <raft/raft_timestamp_oracle.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/txn_state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

/// @brief What one key looks like to a read (Requirement 5.1, 5.5): the
///        wrapped state machine's committed value, and any intent on it.
template<shard_key Key> struct txn_read_result {
    std::optional<std::vector<std::byte>> _value{};
    std::optional<intent_view<Key>> _intent{};
};

/// @brief How a coordinator reaches the participant shards.
///
/// - `resolve(key)`: the group that owns `key` now, or nothing.
/// - `send(keys, envelope, timeout)`: submit an encoded envelope to the group
///   owning every key in `keys`; the returned `kythira::future` holds the
///   encoded `txn_result`, or the exception the shard refused it with.
/// - `refresh(error)`: whether `error` was a stale-routing refusal the client
///   has now repaired, so the coordinator should re-partition and retry.
/// - `read(key, timeout)`: the committed value and any intent, from the
///   group's leader.
/// - `now_ms()`: the participant shards' clock, for TTL deadlines.
template<typename C, typename Key>
concept txn_participant_client =
    shard_key<Key> &&
    requires(C& c, const Key& k, const std::vector<Key>& keys, const std::vector<std::byte>& env,
             std::chrono::milliseconds t, std::exception_ptr e) {
        typename C::group_id_type;
        { c.resolve(k) } -> std::same_as<std::optional<typename C::group_id_type>>;
        { c.send(keys, env, t) } -> future<std::vector<std::byte>>;
        { c.refresh(e) } -> std::same_as<bool>;
        { c.read(k, t) } -> std::same_as<txn_read_result<Key>>;
        { c.now_ms() } -> std::same_as<std::uint64_t>;
    };

/// @brief A client of shards whose state machines keep versions
///        (`mvcc::on`, tier 2), which a coordinator needs for timestamps and
///        snapshot reads.
///
/// - `allocate_timestamps(n, timeout)`: `n` timestamps from the cluster's
///   oracle (Requirement 7).
/// - `read_at(key, ts, timeout)`: the newest version at or below `ts` and
///   any intent, from the group's leader after a read-index confirmation
///   (Requirement 8.5).
///
/// With such a client every write the coordinator sends carries a commit
/// timestamp, whatever the transaction's isolation level.
template<typename C, typename Key>
concept txn_snapshot_client =
    txn_participant_client<C, Key> &&
    requires(C& c, const Key& k, std::size_t n, std::uint64_t ts, std::chrono::milliseconds t) {
        { c.allocate_timestamps(n, t) } -> std::same_as<timestamp_range>;
        { c.read_at(k, ts, t) } -> std::same_as<txn_read_result<Key>>;
    };

/// @brief How `commit()` ended (Requirement 3.8).
enum class txn_outcome_kind : std::uint8_t {
    committed,
    aborted,
    /// The primary commit was proposed but its result was not observed.
    /// Ask `transaction_status()`; never assume either way.
    unknown,
};

enum class txn_abort_reason : std::uint8_t {
    none,
    /// Another transaction held a key (no-wait locking), or, under
    /// `snapshot`, committed a newer version of one after this
    /// transaction's start timestamp (first committer wins; no holder).
    write_conflict,
    /// A resolver aborted the record before the commit point.
    expired,
    /// A prepare failed or timed out; the commit point was never attempted.
    prepare_failed,
    /// A key resolved to no shard.
    unroutable,
    /// The start timestamp fell below a shard's garbage-collection safe
    /// point (Requirement 8.6): begin again for a fresh one.
    snapshot_too_old,
    /// Under `serializable`, a key the transaction read was written after
    /// its start timestamp, or is held by another transaction
    /// (Requirement 9.2). Never retried: the reads are stale, so begin
    /// again and read afresh.
    read_conflict,
};

template<shard_key Key> struct txn_outcome {
    txn_outcome_kind _kind{txn_outcome_kind::committed};
    txn_abort_reason _reason{txn_abort_reason::none};
    txn_id _txn{};
    /// Empty for a one-phase commit, which has no primary.
    std::optional<Key> _primary{};
    /// `write_conflict`: the key and who held it.
    std::optional<Key> _conflict_key{};
    std::optional<txn_lock_holder<Key>> _holder{};
    std::uint64_t _commit_ts{0};
    /// Why, for `prepare_failed` and `unknown`.
    std::string _detail{};
};

/// @brief The protocol points a test can stop a coordinator at, as if its
///        process died there (`set_crash_hook`).
enum class txn_step : std::uint8_t {
    /// Every prepare has been sent; none has been waited for.
    prepares_sent,
    /// Every prepare succeeded; the commit point has not been proposed.
    prepared,
    /// The primary's record is committed.
    primary_committed,
    /// Every secondary commit has been sent.
    secondaries_sent,
};

/// @brief Buffers one transaction at a time and commits it across shards.
///
/// Not thread-safe: one coordinator per client thread, as with a database
/// session. It is cheap to make, and holds only the transaction in flight.
template<typename Client, shard_key Key,
         shard_key_codec<Key> KeyCodec = default_shard_key_codec<Key>>
requires txn_participant_client<Client, Key>
class transaction_coordinator {
public:
    using group_id_type = typename Client::group_id_type;
    using outcome_type = txn_outcome<Key>;

    explicit transaction_coordinator(Client& client, txn_options defaults = {},
                                     KeyCodec codec = KeyCodec{})
        : _client(client),
          _defaults(defaults),
          _codec(std::move(codec)),
          _rng(std::random_device{}()) {}

    transaction_coordinator(const transaction_coordinator&) = delete;
    auto operator=(const transaction_coordinator&) -> transaction_coordinator& = delete;

    /// @brief Waits for the secondary commits still in flight.
    ~transaction_coordinator() = default;

    /// Whether the shards keep versions, so every write needs a commit
    /// timestamp and `snapshot` can be offered.
    static constexpr bool k_mvcc = txn_snapshot_client<Client, Key>;

    /// @brief The isolation levels this coordinator can give: tier 1, plus
    ///        `snapshot` over shards that keep versions (task 12).
    [[nodiscard]] static auto offered_isolations() -> std::vector<txn_isolation> {
        if constexpr (k_mvcc) {
            return {txn_isolation::read_committed, txn_isolation::read_committed_wait,
                    txn_isolation::snapshot, txn_isolation::serializable};
        } else {
            return {txn_isolation::read_committed, txn_isolation::read_committed_wait};
        }
    }

    /// @brief Start a transaction with the client's defaults.
    auto begin() -> txn_id { return begin(_defaults); }

    /// @throws isolation_unsupported_exception for a level this build cannot
    ///         give (Requirement 10.2). Never a silent downgrade.
    /// @throws std::logic_error if a transaction is already open.
    auto begin(txn_options options) -> txn_id {
        if (_open) {
            throw std::logic_error("transaction_coordinator: a transaction is already open");
        }
        const auto offered = offered_isolations();
        if (std::find(offered.begin(), offered.end(), options.isolation) == offered.end()) {
            throw isolation_unsupported_exception(options.isolation, offered);
        }
        if (options.start_ts != 0 && !is_versioned(options.isolation)) {
            throw std::invalid_argument(
                "transaction_coordinator: a start timestamp needs snapshot or serializable");
        }
        _options = options;
        _writes.clear();
        _reads.clear();
        _start_ts = 0;
        if constexpr (k_mvcc) {
            if (is_versioned(options.isolation)) {
                // Every read of the transaction is at this timestamp, and
                // its writes conflict with anything committed after it.
                _start_ts = options.start_ts != 0
                                ? options.start_ts
                                : _client.allocate_timestamps(1, options.timeout)._first;
            }
        }
        _txn = fresh_id();
        _open = true;
        return _txn;
    }

    /// @brief The snapshot or serializable transaction's start timestamp;
    ///        zero otherwise.
    [[nodiscard]] auto start_ts() const -> std::uint64_t { return _start_ts; }

    /// @brief Under `serializable`, validate `key` at commit as if it had
    ///        been read, without reading it (a Redis `WATCH`): the commit
    ///        aborts with `read_conflict` if a version of it committed after
    ///        the start timestamp, or another transaction holds it.
    ///
    /// @throws std::logic_error below `serializable`, where nothing would
    ///         validate it.
    auto watch(const Key& key) -> void {
        require_open("watch");
        if (_options.isolation != txn_isolation::serializable) {
            throw std::logic_error("transaction_coordinator: watch needs serializable");
        }
        _reads.insert(key);
    }

    /// @brief Buffer a write. Nothing reaches a shard before `commit()`
    ///        (Requirement 3.1); a second write to a key replaces the first.
    auto put(const Key& key, std::vector<std::byte> command) -> void {
        require_open("put");
        _writes.insert_or_assign(key, std::move(command));
    }

    /// @brief The committed value of `key`, at the transaction's isolation.
    ///
    /// Intents are invisible (Requirement 5.1). Under `read_committed_wait` a
    /// key held by another transaction is polled with backoff and, once that
    /// transaction's TTL has passed, resolved, until the transaction's
    /// timeout (Requirement 5.3). The transaction's own buffered writes are
    /// not visible: they are opaque commands until they apply.
    ///
    /// @throws std::runtime_error under `read_committed_wait` if the key is
    ///         still held when the timeout runs out.
    /// @throws snapshot_too_old_exception under `snapshot` if the start
    ///         timestamp is below the shard's safe point (Requirement 8.6).
    ///
    /// Under `snapshot` the read is at the start timestamp, through the
    /// shard leader's linearizable path, and an intent whose holder started
    /// at or below it is waited for or resolved as `read_committed_wait`
    /// does: that holder may yet commit below the read point
    /// (Requirement 8.4). A holder that started later cannot, and is
    /// ignored.
    auto get(const Key& key) -> std::optional<std::vector<std::byte>> {
        require_open("get");
        const auto deadline = std::chrono::steady_clock::now() + _options.timeout;
        auto backoff = std::chrono::milliseconds{5};
        if (_options.isolation == txn_isolation::serializable) {
            _reads.insert(key);  // validated at commit (Requirement 9.1)
        }
        for (;;) {
            auto read = read_once(key, deadline);
            const bool waits = _options.isolation == txn_isolation::read_committed_wait ||
                               (is_versioned(_options.isolation) && read._intent.has_value() &&
                                read._intent->_start_ts <= _start_ts);
            if (!waits || !read._intent.has_value() || read._intent->_txn == _txn) {
                return std::move(read._value);
            }
            if (_client.now_ms() >= read._intent->_deadline_ms) {
                resolve_intent(key, *read._intent);
                continue;
            }
            if (std::chrono::steady_clock::now() + backoff > deadline) {
                throw std::runtime_error("transaction_coordinator: key still locked by " +
                                         read._intent->_txn.to_string() + " at the timeout");
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, std::chrono::milliseconds{100});
        }
    }

    /// @brief Commit the buffered writes. Closes the transaction whatever the
    ///        outcome. Retries a conflict up to `max_retries` times, each
    ///        under a fresh transaction id.
    auto commit() -> outcome_type {
        require_open("commit");
        _open = false;
        if (_writes.empty()) {
            // Requirement 9.3: a read-only transaction commits without a
            // prepare, at every level. Its reads were one snapshot, which is
            // serializable at the start timestamp on its own.
            return outcome_type{._txn = _txn};
        }
        for (std::size_t attempt = 0;; ++attempt) {
            auto out = attempt_commit();
            // A newer committed version (no holder) is not retried: the
            // writes were made against a snapshot that is now stale, and
            // retrying them later would be a lost update. A read conflict is
            // not retried either (it is not a write_conflict).
            const bool version_conflict = out._reason == txn_abort_reason::write_conflict &&
                                          !out._holder.has_value() && _start_ts != 0;
            if (out._kind != txn_outcome_kind::aborted ||
                out._reason != txn_abort_reason::write_conflict || version_conflict ||
                attempt >= _options.max_retries) {
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10} * (attempt + 1));
            _txn = fresh_id();
        }
    }

    /// @brief Discard the buffered writes. Nothing was sent, so nothing needs
    ///        undoing.
    auto rollback() -> void {
        require_open("rollback");
        _writes.clear();
        _reads.clear();
        _open = false;
    }

    /// @brief The fate of `txn`, from its primary's shard. An absent record
    ///        reads as aborted, without writing the rollback marker.
    auto transaction_status(const txn_id& txn, const Key& primary) -> txn_record_status {
        const auto res = send_and_wait({primary}, txn_heartbeat<Key>{txn, primary, 0},
                                       std::chrono::steady_clock::now() + _defaults.timeout);
        if (!res.has_value() || res->_status != txn_result_status::record) {
            throw std::runtime_error("transaction_coordinator: no answer from the primary of " +
                                     txn.to_string());
        }
        return res->_record;
    }

    /// @brief Finish another transaction's intent on `key`, as any party that
    ///        meets one may (Requirement 4.3, 4.5).
    ///
    /// Asks the primary's shard to `check_and_resolve`, then commits or
    /// removes the intent on `key`'s shard to match. A pending record whose
    /// TTL has not passed is left alone.
    ///
    /// @return The record's status as the primary reported it.
    auto resolve_intent(const Key& key, const intent_view<Key>& intent) -> txn_record_status {
        const auto deadline = std::chrono::steady_clock::now() + _defaults.timeout;
        const auto record = send_and_wait(
            {intent._primary},
            txn_check_and_resolve<Key>{intent._txn, intent._primary, _client.now_ms()}, deadline);
        if (!record.has_value() || record->_status != txn_result_status::record) {
            throw std::runtime_error("transaction_coordinator: could not resolve " +
                                     intent._txn.to_string());
        }
        if (record->_record == txn_record_status::committed) {
            std::ignore = send_and_wait(
                {key}, txn_commit_secondary<Key>{intent._txn, {key}, record->_commit_ts}, deadline);
        } else if (record->_record == txn_record_status::aborted) {
            std::ignore =
                send_and_wait({key}, txn_abort<Key>{intent._txn, intent._primary, {key}}, deadline);
        }
        return record->_record;
    }

    /// @brief Test hook: called at each protocol step; throwing from it
    ///        abandons the transaction there, as a process death would.
    auto set_crash_hook(std::function<void(txn_step)> hook) -> void {
        _crash_hook = std::move(hook);
    }

    /// @brief Wait for the secondary commits sent by earlier `commit()`s.
    auto drain() -> void {
        for (auto& f : _in_flight) {
            while (!f.wait(std::chrono::seconds{1})) {
            }
        }
        _in_flight.clear();
    }

    [[nodiscard]] auto current_txn() const -> const txn_id& { return _txn; }

private:
    using clock = std::chrono::steady_clock;
    using send_future = decltype(std::declval<Client&>().send(
        std::declval<const std::vector<Key>&>(), std::declval<const std::vector<std::byte>&>(),
        std::chrono::milliseconds{}));
    using write_map = std::map<Key, std::vector<std::byte>>;

    /// One shard's share: its writes and, under `serializable`, the keys
    /// it validates and read-locks without writing.
    struct group_writes {
        std::vector<txn_write<Key>> _writes;
        std::vector<Key> _read_keys;
        /// Every key the shard holds for the transaction: what the send is
        /// routed and admitted by, and what a commit or abort releases.
        [[nodiscard]] auto keys() const -> std::vector<Key> {
            std::vector<Key> out;
            out.reserve(_writes.size() + _read_keys.size());
            for (const auto& w : _writes) {
                out.push_back(w._key);
            }
            out.insert(out.end(), _read_keys.begin(), _read_keys.end());
            return out;
        }
    };

    struct pending_prepare {
        group_writes _group;
        bool _is_primary{false};
        send_future _future;
    };

    auto require_open(const char* what) const -> void {
        if (!_open) {
            throw std::logic_error(std::string("transaction_coordinator: ") + what +
                                   " outside a transaction");
        }
    }

    auto fresh_id() -> txn_id { return txn_id{_rng(), _rng()}; }

    auto hook(txn_step step) -> void {
        if (_crash_hook) {
            _crash_hook(step);
        }
    }

    [[nodiscard]] static auto remaining(clock::time_point deadline) -> std::chrono::milliseconds {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
        return std::max(left, std::chrono::milliseconds{1});
    }

    /// Whether `f` is ready by `until`.
    [[nodiscard]] static auto ready_by(send_future& f, clock::time_point until) -> bool {
        return f.wait(remaining(until));
    }

    [[nodiscard]] static auto is_versioned(txn_isolation level) -> bool {
        return level == txn_isolation::snapshot || level == txn_isolation::serializable;
    }

    /// One read at the transaction's isolation.
    auto read_once(const Key& key, clock::time_point deadline) -> txn_read_result<Key> {
        if constexpr (k_mvcc) {
            if (is_versioned(_options.isolation)) {
                return _client.read_at(key, _start_ts, remaining(deadline));
            }
        }
        return _client.read(key, remaining(deadline));
    }

    /// A commit timestamp from the oracle over versioned shards; zero
    /// otherwise.
    auto commit_timestamp(clock::time_point deadline) -> std::uint64_t {
        if constexpr (k_mvcc) {
            return _client.allocate_timestamps(1, remaining(deadline))._first;
        } else {
            return 0;
        }
    }

    [[nodiscard]] auto encode(const txn_envelope<Key>& env) const -> std::vector<std::byte> {
        return encode_txn_envelope<Key, KeyCodec>(env, _codec);
    }

    [[nodiscard]] auto decode(const std::vector<std::byte>& bytes) const -> txn_result<Key> {
        return decode_txn_result<Key, KeyCodec>(bytes, _codec);
    }

    /// Group `writes` and `reads` by the shard each key resolves to now.
    auto partition(const std::vector<txn_write<Key>>& writes, const std::vector<Key>& reads = {})
        -> std::optional<std::map<group_id_type, group_writes>> {
        std::map<group_id_type, group_writes> out;
        for (const auto& w : writes) {
            auto g = _client.resolve(w._key);
            if (!g.has_value()) {
                return std::nullopt;
            }
            out[*g]._writes.push_back(w);
        }
        for (const auto& k : reads) {
            auto g = _client.resolve(k);
            if (!g.has_value()) {
                return std::nullopt;
            }
            out[*g]._read_keys.push_back(k);
        }
        return out;
    }

    /// The keys read (or watched) and not written: the ones whose
    /// validation needs a read intent of their own.
    [[nodiscard]] auto read_only_keys() const -> std::vector<Key> {
        std::vector<Key> out;
        for (const auto& k : _reads) {
            if (!_writes.contains(k)) {
                out.push_back(k);
            }
        }
        return out;
    }

    /// Send one envelope and wait for its result, re-sending after a routing
    /// repair. Empty if it failed, or the deadline passed, without a result.
    auto send_and_wait(const std::vector<Key>& keys, const txn_envelope<Key>& env,
                       clock::time_point deadline, std::string* why = nullptr)
        -> std::optional<txn_result<Key>> {
        const auto bytes = encode(env);
        while (clock::now() < deadline) {
            auto f = _client.send(keys, bytes, remaining(deadline));
            if (!ready_by(f, deadline)) {
                // The send owns the future; leave it to finish on its own.
                _in_flight.push_back(std::move(f));
                break;
            }
            try {
                return decode(std::move(f).get());
            } catch (...) {
                const auto error = std::current_exception();
                if (!_client.refresh(error)) {
                    if (why != nullptr) {
                        *why = describe(error);
                    }
                    return std::nullopt;
                }
            }
        }
        if (why != nullptr && why->empty()) {
            *why = "timed out";
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto describe(const std::exception_ptr& e) -> std::string {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            return ex.what();
        } catch (...) {
            return "unknown error";
        }
    }

    [[nodiscard]] auto all_writes() const -> std::vector<txn_write<Key>> {
        std::vector<txn_write<Key>> out;
        out.reserve(_writes.size());
        for (const auto& [k, c] : _writes) {
            out.push_back(txn_write<Key>{k, c});
        }
        return out;
    }

    [[nodiscard]] auto aborted(txn_abort_reason reason,
                               std::optional<Key> primary = std::nullopt) const -> outcome_type {
        return outcome_type{._kind = txn_outcome_kind::aborted,
                            ._reason = reason,
                            ._txn = _txn,
                            ._primary = std::move(primary)};
    }

    auto attempt_commit() -> outcome_type {
        const auto deadline = clock::now() + _options.timeout;
        auto groups = partition(all_writes(), read_only_keys());
        if (!groups.has_value()) {
            return aborted(txn_abort_reason::unroutable);
        }
        // Over versioned shards even one shard takes two phases: the commit
        // timestamp must be taken after the writes are visible as intents,
        // or a reader that starts after it could read the shard before a
        // one-phase entry applies and miss a write below its read point.
        if (!k_mvcc && groups->size() == 1) {
            auto one = commit_one_phase(groups->begin()->second, deadline);
            if (one.has_value()) {
                return *one;
            }
            // A split moved a key off the shard: fall through to two phases
            // over the refreshed map.
            groups = partition(all_writes(), read_only_keys());
            if (!groups.has_value()) {
                return aborted(txn_abort_reason::unroutable);
            }
        }
        return commit_two_phase(*groups, deadline);
    }

    /// Empty if a routing refusal means the keys no longer share a shard.
    auto commit_one_phase(const group_writes& group, clock::time_point deadline)
        -> std::optional<outcome_type> {
        const auto bytes = encode(txn_one_phase<Key>{_txn, group._writes, 0, 0});
        auto f = _client.send(group.keys(), bytes, remaining(deadline));
        if (!ready_by(f, deadline)) {
            _in_flight.push_back(std::move(f));
            return outcome_type{._kind = txn_outcome_kind::unknown,
                                ._txn = _txn,
                                ._detail = "one-phase commit timed out"};
        }
        try {
            const auto res = decode(std::move(f).get());
            return from_result(res, std::nullopt);
        } catch (...) {
            const auto error = std::current_exception();
            if (_client.refresh(error)) {
                return std::nullopt;
            }
            // Refused before it was proposed, or its outcome is unobserved:
            // the 1PC entry is its own commit point, so either may be true.
            return outcome_type{
                ._kind = txn_outcome_kind::unknown, ._txn = _txn, ._detail = describe(error)};
        }
    }

    [[nodiscard]] auto from_result(const txn_result<Key>& res, std::optional<Key> primary) const
        -> outcome_type {
        switch (res._status) {
            case txn_result_status::ok:
                return outcome_type{._txn = _txn, ._primary = std::move(primary)};
            case txn_result_status::write_conflict: {
                auto out = aborted(txn_abort_reason::write_conflict, std::move(primary));
                out._conflict_key = res._key;
                out._holder = res._holder;
                return out;
            }
            case txn_result_status::already_aborted:
                return aborted(txn_abort_reason::expired, std::move(primary));
            case txn_result_status::read_conflict: {
                auto out = aborted(txn_abort_reason::read_conflict, std::move(primary));
                out._conflict_key = res._key;
                out._holder = res._holder;
                return out;
            }
            case txn_result_status::snapshot_too_old: {
                auto out = aborted(txn_abort_reason::snapshot_too_old, std::move(primary));
                out._detail = "start timestamp " + std::to_string(_start_ts) +
                              " is below the safe point " + std::to_string(res._safe_point);
                return out;
            }
            case txn_result_status::record:
                if (res._record == txn_record_status::committed) {
                    return outcome_type{
                        ._txn = _txn, ._primary = std::move(primary), ._commit_ts = res._commit_ts};
                }
                return aborted(txn_abort_reason::expired, std::move(primary));
            default:
                return outcome_type{._kind = txn_outcome_kind::unknown,
                                    ._txn = _txn,
                                    ._primary = std::move(primary),
                                    ._detail = "unexpected status " +
                                               std::to_string(static_cast<int>(res._status))};
        }
    }

    /// The smallest written key of the shard with the most writes. Always a
    /// written key: a shard that only validates reads holds no record.
    [[nodiscard]] static auto primary_of(const std::map<group_id_type, group_writes>& groups)
        -> Key {
        const group_writes* largest = nullptr;
        for (const auto& [g, gw] : groups) {
            if (gw._writes.empty()) {
                continue;
            }
            if (largest == nullptr || gw._writes.size() > largest->_writes.size()) {
                largest = &gw;
            }
        }
        Key smallest = largest->_writes.front()._key;
        for (const auto& w : largest->_writes) {
            smallest = std::min(smallest, w._key);
        }
        return smallest;
    }

    auto send_prepare(const group_writes& group, const Key& primary, bool is_primary,
                      std::uint64_t ttl_deadline, clock::time_point deadline) -> pending_prepare {
        const auto bytes = encode(txn_prepare<Key>{_txn, primary, group._writes, group._read_keys,
                                                   _start_ts, ttl_deadline, is_primary});
        return pending_prepare{group, is_primary,
                               _client.send(group.keys(), bytes, remaining(deadline))};
    }

    auto commit_two_phase(const std::map<group_id_type, group_writes>& groups,
                          clock::time_point deadline) -> outcome_type {
        const Key primary = primary_of(groups);
        const auto ttl = static_cast<std::uint64_t>(_options.lock_ttl.count());
        const auto ttl_deadline = _client.now_ms() + ttl;
        const auto heartbeat_every = std::max(_options.lock_ttl / 3, std::chrono::milliseconds{1});

        std::vector<pending_prepare> pending;
        for (const auto& [g, gw] : groups) {
            const bool has_primary = std::any_of(gw._writes.begin(), gw._writes.end(),
                                                 [&](const auto& w) { return w._key == primary; });
            pending.push_back(send_prepare(gw, primary, has_primary, ttl_deadline, deadline));
        }
        try {
            hook(txn_step::prepares_sent);
        } catch (...) {
            // A crash does not recall what is already on the wire: keep the
            // prepares where drain() can wait for them to land.
            for (auto& p : pending) {
                _in_flight.push_back(std::move(p._future));
            }
            throw;
        }

        // Wait for every prepare, heartbeating the primary meanwhile
        // (Requirement 4.2) and re-sending any group a split moved.
        std::vector<group_writes> sent;
        std::optional<outcome_type> failure;
        auto next_heartbeat = clock::now() + heartbeat_every;
        while (!pending.empty()) {
            auto p = std::move(pending.back());
            pending.pop_back();
            const auto until = std::min(deadline, next_heartbeat);
            if (!ready_by(p._future, until)) {
                if (clock::now() >= deadline) {
                    _in_flight.push_back(std::move(p._future));
                    sent.push_back(std::move(p._group));
                    failure = aborted(txn_abort_reason::prepare_failed, primary);
                    failure->_detail = "prepare timed out";
                    continue;
                }
                auto hb = encode(txn_heartbeat<Key>{_txn, primary, _client.now_ms() + ttl});
                _in_flight.push_back(_client.send({primary}, hb, remaining(deadline)));
                next_heartbeat = clock::now() + heartbeat_every;
                pending.push_back(std::move(p));
                continue;
            }
            sent.push_back(p._group);
            try {
                const auto res = decode(std::move(p._future).get());
                if (res._status != txn_result_status::ok && !failure.has_value()) {
                    failure = from_result(res, primary);
                }
            } catch (...) {
                const auto error = std::current_exception();
                sent.pop_back();
                if (_client.refresh(error) && clock::now() < deadline) {
                    auto regrouped = partition(p._group._writes, p._group._read_keys);
                    if (regrouped.has_value()) {
                        for (const auto& [g, gw] : *regrouped) {
                            const bool has_primary =
                                std::any_of(gw._writes.begin(), gw._writes.end(),
                                            [&](const auto& w) { return w._key == primary; });
                            pending.push_back(
                                send_prepare(gw, primary, has_primary, ttl_deadline, deadline));
                        }
                        continue;
                    }
                }
                // Its fate is unknown, so it is aborted with the rest.
                sent.push_back(p._group);
                if (!failure.has_value()) {
                    failure = aborted(txn_abort_reason::prepare_failed, primary);
                    failure->_detail = describe(error);
                }
            }
        }

        if (failure.has_value()) {
            abort_groups(sent, primary, deadline);
            return *failure;
        }
        hook(txn_step::prepared);

        // Taken after every prepare succeeded, so it is above the start
        // timestamp of any snapshot that could have missed these intents.
        std::uint64_t commit_ts = 0;
        try {
            commit_ts = commit_timestamp(deadline);
        } catch (const std::exception& e) {
            abort_groups(sent, primary, deadline);
            auto out = aborted(txn_abort_reason::prepare_failed, primary);
            out._detail = std::string("no commit timestamp: ") + e.what();
            return out;
        }
        std::string why;
        const auto committed = send_and_wait(
            {primary}, txn_commit_primary<Key>{_txn, primary, commit_ts}, deadline, &why);
        if (!committed.has_value()) {
            return outcome_type{._kind = txn_outcome_kind::unknown,
                                ._txn = _txn,
                                ._primary = primary,
                                ._detail = why};
        }
        auto out = from_result(*committed, primary);
        if (out._kind == txn_outcome_kind::committed && out._commit_ts == 0) {
            out._commit_ts = commit_ts;
        }
        if (out._kind != txn_outcome_kind::committed) {
            // The resolver aborted the record first; clear what is left.
            abort_groups(sent, primary, deadline);
            return out;
        }
        hook(txn_step::primary_committed);

        // The primary's shard rolled its own intents forward at the commit.
        for (const auto& gw : sent) {
            auto keys = gw.keys();
            if (std::find(keys.begin(), keys.end(), primary) != keys.end()) {
                continue;
            }
            auto env = encode(txn_commit_secondary<Key>{_txn, keys, out._commit_ts});
            _in_flight.push_back(_client.send(keys, env, _options.timeout));
        }
        hook(txn_step::secondaries_sent);
        return out;
    }

    /// Best effort: an abort that is lost leaves intents a resolver clears
    /// after their TTL.
    auto abort_groups(const std::vector<group_writes>& groups, const Key& primary,
                      clock::time_point deadline) -> void {
        // The primary's shard first, so the rollback marker lands before any
        // late primary prepare.
        bool primary_done = false;
        for (const auto& gw : groups) {
            auto keys = gw.keys();
            if (std::find(keys.begin(), keys.end(), primary) == keys.end()) {
                continue;
            }
            std::ignore = send_and_wait(keys, txn_abort<Key>{_txn, primary, keys}, deadline);
            primary_done = true;
        }
        if (!primary_done) {
            std::ignore =
                send_and_wait({primary}, txn_abort<Key>{_txn, primary, {primary}}, deadline);
        }
        for (const auto& gw : groups) {
            auto keys = gw.keys();
            if (std::find(keys.begin(), keys.end(), primary) != keys.end()) {
                continue;
            }
            auto env = encode(txn_abort<Key>{_txn, primary, keys});
            _in_flight.push_back(_client.send(keys, env, _options.timeout));
        }
    }

    Client& _client;
    txn_options _defaults;
    KeyCodec _codec;
    std::mt19937_64 _rng;

    bool _open{false};
    txn_options _options{};
    txn_id _txn{};
    /// `snapshot` and `serializable` only; zero otherwise.
    std::uint64_t _start_ts{0};
    write_map _writes;
    /// `serializable` only: the keys read or watched, validated at commit.
    std::set<Key> _reads;
    std::function<void(txn_step)> _crash_hook;
    /// Sends nobody waits on, kept so `drain()` can wait for them.
    std::vector<send_future> _in_flight;
};

}  // namespace kythira
