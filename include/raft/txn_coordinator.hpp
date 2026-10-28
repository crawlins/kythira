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
    /// Another transaction held a key (no-wait locking).
    write_conflict,
    /// A resolver aborted the record before the commit point.
    expired,
    /// A prepare failed or timed out; the commit point was never attempted.
    prepare_failed,
    /// A key resolved to no shard.
    unroutable,
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

    /// @brief The isolation levels this coordinator can give: tier 1 only
    ///        until the multi-version state machine lands (tasks 11-12).
    [[nodiscard]] static auto offered_isolations() -> std::vector<txn_isolation> {
        return {txn_isolation::read_committed, txn_isolation::read_committed_wait};
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
        _options = options;
        _writes.clear();
        _txn = fresh_id();
        _open = true;
        return _txn;
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
    auto get(const Key& key) -> std::optional<std::vector<std::byte>> {
        require_open("get");
        const auto deadline = std::chrono::steady_clock::now() + _options.timeout;
        auto backoff = std::chrono::milliseconds{5};
        for (;;) {
            auto read = _client.read(key, remaining(deadline));
            if (_options.isolation != txn_isolation::read_committed_wait ||
                !read._intent.has_value() || read._intent->_txn == _txn) {
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
            return outcome_type{._txn = _txn};
        }
        for (std::size_t attempt = 0;; ++attempt) {
            auto out = attempt_commit();
            if (out._kind != txn_outcome_kind::aborted ||
                out._reason != txn_abort_reason::write_conflict ||
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

    struct group_writes {
        std::vector<txn_write<Key>> _writes;
        [[nodiscard]] auto keys() const -> std::vector<Key> {
            std::vector<Key> out;
            out.reserve(_writes.size());
            for (const auto& w : _writes) {
                out.push_back(w._key);
            }
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

    [[nodiscard]] auto encode(const txn_envelope<Key>& env) const -> std::vector<std::byte> {
        return encode_txn_envelope<Key, KeyCodec>(env, _codec);
    }

    [[nodiscard]] auto decode(const std::vector<std::byte>& bytes) const -> txn_result<Key> {
        return decode_txn_result<Key, KeyCodec>(bytes, _codec);
    }

    /// Group `writes` by the shard each key resolves to now.
    auto partition(const std::vector<txn_write<Key>>& writes)
        -> std::optional<std::map<group_id_type, group_writes>> {
        std::map<group_id_type, group_writes> out;
        for (const auto& w : writes) {
            auto g = _client.resolve(w._key);
            if (!g.has_value()) {
                return std::nullopt;
            }
            out[*g]._writes.push_back(w);
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
        auto groups = partition(all_writes());
        if (!groups.has_value()) {
            return aborted(txn_abort_reason::unroutable);
        }
        if (groups->size() == 1) {
            auto one = commit_one_phase(groups->begin()->second, deadline);
            if (one.has_value()) {
                return *one;
            }
            // A split moved a key off the shard: fall through to two phases
            // over the refreshed map.
            groups = partition(all_writes());
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

    [[nodiscard]] static auto primary_of(const std::map<group_id_type, group_writes>& groups)
        -> Key {
        const group_writes* largest = nullptr;
        for (const auto& [g, gw] : groups) {
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
        const auto bytes =
            encode(txn_prepare<Key>{_txn, primary, group._writes, {}, 0, ttl_deadline, is_primary});
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
                    auto regrouped = partition(p._group._writes);
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

        std::string why;
        const auto committed =
            send_and_wait({primary}, txn_commit_primary<Key>{_txn, primary, 0}, deadline, &why);
        if (!committed.has_value()) {
            return outcome_type{._kind = txn_outcome_kind::unknown,
                                ._txn = _txn,
                                ._primary = primary,
                                ._detail = why};
        }
        auto out = from_result(*committed, primary);
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
            auto env = encode(txn_commit_secondary<Key>{_txn, keys, 0});
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
    write_map _writes;
    std::function<void(txn_step)> _crash_hook;
    /// Sends nobody waits on, kept so `drain()` can wait for them.
    std::vector<send_future> _in_flight;
};

}  // namespace kythira
