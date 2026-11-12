// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file backend.hpp
/// @brief What the DynamoDB handlers need from the cluster, and an
///        in-process implementation (`.kiro/specs/dynamodb-compatible-api/`
///        task 7).
///
/// The handlers (`table_api.hpp`, `item_api.hpp`) turn requests into
/// catalog and item commands. A `dynamo_backend` gets those commands
/// committed and applied, and serves reads:
///
/// - `catalog(cmd)` commits a catalog entry and returns its result.
/// - `write(cmd)` commits an item entry on the group that owns the key.
/// - `read(...)` serves `GetItem`: a linearizable read when `consistent`,
///   otherwise any replica's committed state (Requirement 8.6).
/// - `read_range(...)` serves `Query` and `Scan`: the items whose base keys
///   fall in a range, in key order or reverse, across every group the range
///   covers, until the visitor returns false. Consistency as for `read`.
/// - `describe` and `list_tables` read the catalog.
/// - `now_ms()` is the clock that stamps entries. Replicas never read their
///   own clocks.
/// - `transact(actions)` runs one transaction (task 10): each action is an
///   item command on its own item (`put`, `update`, `delete_`,
///   `condition_check` or `read_lock`), every item is locked, every action
///   is judged against its committed item while locked, and either all
///   apply or none do. It returns each action's verdict; an outcome it
///   could not learn (a timeout after the commit point was proposed) is an
///   exception, so the client sees a 500 and retries, as against AWS.
///
/// `local_backend` applies the commands directly to the two state machines
/// on one engine, with no Raft. Handler tests use it, and so does a
/// single-process daemon. It does a table's follow-on work as soon as the
/// table entry is applied: CREATING and UPDATING become ACTIVE, and a
/// delete runs its bounded drop steps to the end. The Raft-hosted backend
/// does the same work from the catalog leader.

#include <kythira/dynamodb/capacity.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/gsi_propagator.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// Gets each item of a range read with its base key; returns false to stop.
using range_visitor = std::function<bool(std::string_view base_key, const item&)>;

/// A range read's visitor that also gets each item's last-writer-wins
/// stamp (task 22: a global table's backfill carries them).
using stamped_visitor = std::function<bool(std::string_view base_key, const item&,
                                           const std::optional<replica_stamp>&)>;

/// `c` with its change record timestamp set from the backend's clock, when
/// its table logs changes and nothing set one (task 15). Replicas take the
/// time from the entry, never from their own clocks.
[[nodiscard]] inline auto stamp_written(item_command c, std::uint64_t now_ms) -> item_command {
    if (c.log_changes && c.written_ms == 0) {
        c.written_ms = now_ms;
    }
    return c;
}

/// How a transaction a backend ran ended (`transact`).
struct txn_report {
    enum class status : std::uint8_t {
        committed,
        /// Nothing applied: an action was refused (`results`) or its item was
        /// held by another transaction (`conflicts`).
        cancelled,
    };
    status outcome = status::committed;
    /// One per action, in the request's order: the result the action's
    /// group reported for it, which is a refusal's result
    /// (`condition_failed` or `error`), or a `read_lock`'s item in
    /// `attributes`; empty for an action that passed with nothing to say.
    std::vector<std::optional<item_result>> results;
    /// One per action: another transaction held the action's item.
    std::vector<bool> conflicts;
};

template<typename B>
concept dynamo_backend =
    requires(B& b, const catalog_command& cc, const item_command& ic, std::string_view name,
             std::size_t limit, std::uint64_t table_id, const key_schema& schema, const item& key,
             bool consistent, const kythira::key_range& range, kythira::scan_direction direction,
             const range_visitor& visit, const std::vector<item_command>& actions,
             const table_descriptor& table) {
        { b.catalog(cc) } -> std::same_as<catalog_result>;
        { b.write(ic) } -> std::same_as<item_result>;
        { b.read(table_id, schema, key, consistent) } -> std::same_as<std::optional<item>>;
        { b.read_range(range, direction, consistent, visit) } -> std::same_as<void>;
        { b.describe(name) } -> std::same_as<std::optional<table_descriptor>>;
        { b.list_tables(name, limit) } -> std::same_as<std::vector<std::string>>;
        { b.now_ms() } -> std::same_as<std::uint64_t>;
        { b.transact(actions) } -> std::same_as<txn_report>;
        { b.settle_item(table, key) } -> std::same_as<void>;
        { b.throttler() } -> std::same_as<capacity_throttler&>;
    };

/// A backend DynamoDB Streams reads (task 16): the change log's store
/// streams over a key range, and pages of one stream's records.
template<typename B>
concept change_feed_backend =
    requires(B& b, const kythira::key_range& range, const std::string& key, std::uint64_t seq,
             std::size_t limit) {
        { b.change_shards(range) } -> std::same_as<std::vector<change_shard>>;
        { b.read_changes(key, seq, limit) } -> std::same_as<change_page>;
    };

/// A backend a global table replicates from (task 22): a range read that
/// carries each item's last-writer-wins stamp.
template<typename B>
concept replica_backend =
    requires(B& b, const kythira::key_range& range, const stamped_visitor& visit) {
        { b.read_stamped_range(range, visit) } -> std::same_as<void>;
    };

/// Both state machines on one engine, applied in-process.
template<kythira::kv_engine Engine = kythira::memory_kv_engine> class local_backend {
public:
    using clock_fn = std::function<std::uint64_t()>;

    /// Items of every table live in one group over `t/`. Its change
    /// records go to `change_log` (task 15); without one, DynamoDB Streams
    /// list shards that never hold a record.
    explicit local_backend(Engine& engine, clock_fn clock = system_clock_ms(),
                           kythira::record_segment_store* change_log = nullptr)
        : _clock(std::move(clock)) {
        _catalog.open(engine, catalog_range());
        _items.set_change_log(change_log, {});
        _items.open(engine, prefix_range("t/"));
        // The index can only be read once both machines are open.
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
        _index = std::max<std::uint64_t>(_catalog.applied_index(), _items.applied_index());
        // Finish any work a previous run left (a delete job, a CREATING
        // table).
        const std::lock_guard lock(_mu);
        settle_locked();
        propagate_locked();
    }

    [[nodiscard]] static auto system_clock_ms() -> clock_fn {
        return [] {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        };
    }

    auto catalog(const catalog_command& c) -> catalog_result {
        const std::lock_guard lock(_mu);
        auto r = apply_catalog_locked(c);
        settle_locked();
        propagate_locked();
        return r;
    }

    auto write(const item_command& c) -> item_result {
        const std::lock_guard lock(_mu);
        auto r = apply_item_locked(stamp_written(c, now_ms()));
        propagate_locked();
        trim_changes_locked();
        return r;
    }

    /// The change log's store streams overlapping `range`, as DynamoDB
    /// Streams shards (task 16). One group holds every table here.
    auto change_shards(const kythira::key_range& /*range*/) -> std::vector<change_shard> {
        const std::lock_guard lock(_mu);
        return _items.change_shards();
    }

    /// Up to `max_records` records of store stream `key` from `from_seq`.
    auto read_changes(const std::string& key, std::uint64_t from_seq, std::size_t max_records)
        -> change_page {
        const std::lock_guard lock(_mu);
        return _items.change_page_of(key, from_seq, max_records);
    }

    /// Drops change records past retention now (tests; writes do it at
    /// most once a minute).
    auto trim_changes() -> void {
        const std::lock_guard lock(_mu);
        _last_trim_ms = 0;
        trim_changes_locked();
    }

    /// Carries the item's pending GSI change before returning, for tables
    /// with synchronous GSIs (`gsi-consistency=sync`). Every write here
    /// already propagates before it returns.
    auto settle_item(const table_descriptor& /*table*/, const item& /*key*/) -> void {}

    auto read(std::uint64_t table_id, const key_schema& schema, const item& key,
              bool /*consistent*/) -> std::optional<item> {
        // One copy: every read is linearizable.
        const std::lock_guard lock(_mu);
        return _items.get_item(table_id, schema, key);
    }

    auto read_range(const kythira::key_range& range, kythira::scan_direction direction,
                    bool /*consistent*/, const range_visitor& visit) -> void {
        // The visitor runs under the lock, so a page is one snapshot; it
        // must not call back into the backend.
        const std::lock_guard lock(_mu);
        _items.scan_items(range, direction, visit);
    }

    /// `read_range` with each item's stamp.
    auto read_stamped_range(const kythira::key_range& range, const stamped_visitor& visit) -> void {
        const std::lock_guard lock(_mu);
        _items.scan_items(range, kythira::scan_direction::forward,
                          [&](std::string_view key, const item& it) {
                              return visit(key, it, _items.stamp_of(std::string(key)));
                          });
    }

    auto describe(std::string_view name) -> std::optional<table_descriptor> {
        const std::lock_guard lock(_mu);
        return _catalog.describe(name);
    }

    auto list_tables(std::string_view exclusive_start, std::size_t limit)
        -> std::vector<std::string> {
        const std::lock_guard lock(_mu);
        return _catalog.list_tables(exclusive_start, limit);
    }

    auto now_ms() -> std::uint64_t { return _clock(); }

    /// The capacity buckets (task 12): one process, so exact.
    auto throttler() -> capacity_throttler& { return _throttler; }

    /// Under the one lock: every action is judged (as each group's
    /// `validate` judges it under the transaction's locks), and if all pass,
    /// all apply.
    auto transact(const std::vector<item_command>& actions) -> txn_report {
        const std::lock_guard lock(_mu);
        txn_report r;
        r.results.resize(actions.size());
        r.conflicts.assign(actions.size(), false);
        bool refused = false;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const auto& a = actions[i];
            const auto key = base_item_key(a.table_id, a.schema, a.payload);
            if (!key) {
                throw std::invalid_argument("local_backend: a transaction action without a key");
            }
            const auto v = _items.validate(*key, to_bytes(encode_item_command(a)));
            if (!v._detail.empty()) {
                auto decoded = decode_item_result(view(v._detail));
                if (!decoded) {
                    throw std::logic_error("local_backend: unreadable validation detail");
                }
                r.results[i] = std::move(*decoded);
            }
            refused = refused || !v._ok;
        }
        if (refused) {
            r.outcome = txn_report::status::cancelled;
            return r;
        }
        for (const auto& a : actions) {
            (void)apply_item_locked(stamp_written(a, now_ms()));
        }
        propagate_locked();
        return r;
    }

    /// The item collection limit of tables with LSIs (tests lower it).
    auto set_collection_limit(std::uint64_t bytes) -> void {
        const std::lock_guard lock(_mu);
        _items.set_collection_limit(bytes);
    }

    /// The item state machine (tests inspect it).
    [[nodiscard]] auto items() const -> const dynamo_item_state_machine<Engine>& { return _items; }

    /// Items deleted per drop step.
    static constexpr std::uint32_t k_drop_step_keys = 1000;
    /// Items marked per backfill entry.
    static constexpr std::size_t k_backfill_batch = 256;

private:
    static auto to_bytes(const std::string& s) -> std::vector<std::byte> {
        std::vector<std::byte> out(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }
    static auto view(const std::vector<std::byte>& b) -> std::string_view {
        return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT
    }

    auto apply_catalog_locked(const catalog_command& c) -> catalog_result {
        const auto out = _catalog.apply(to_bytes(encode_catalog_command(c)), ++_index);
        auto r = decode_catalog_result(view(out));
        if (!r) {
            throw std::logic_error("local_backend: unreadable catalog result");
        }
        return *r;
    }

    auto apply_item_locked(const item_command& c) -> item_result {
        const auto out = _items.apply(to_bytes(encode_item_command(c)), ++_index);
        auto r = decode_item_result(view(out));
        if (!r) {
            throw std::logic_error("local_backend: unreadable item result");
        }
        return *r;
    }

    /// Change log retention, at most once a minute of the clock.
    auto trim_changes_locked() -> void {
        const auto now = now_ms();
        if (_last_trim_ms != 0 && now < _last_trim_ms + 60'000) {
            return;
        }
        _last_trim_ms = std::max<std::uint64_t>(now, 1);
        if (now <= k_stream_retention_ms) {
            return;
        }
        const auto cutoff = now - k_stream_retention_ms;
        if (_items.change_log_trim_due(cutoff)) {
            item_command c;
            c.op = item_command::kind::trim_change_log;
            c.written_ms = cutoff;
            (void)apply_item_locked(c);
        }
    }

    /// Carries every pending GSI change to the indexes, at once
    /// (`gsi_propagator.hpp`).
    auto propagate_locked() -> void {
        while (_items.gsi_pending_count() > 0) {
            for (const auto& work : _items.gsi_pending_batch(64)) {
                const auto plan =
                    plan_propagation(_catalog.describe_id(work.pending.table_id), work);
                for (const auto& w : plan.index_writes) {
                    (void)apply_item_locked(w);
                }
                (void)apply_item_locked(plan.ack);
            }
        }
    }

    // The catalog leader's follow-on work, done at once.
    auto settle_locked() -> void {
        for (const auto& name : _catalog.list_tables("", SIZE_MAX)) {
            const auto d = _catalog.describe(name);
            // A restore's table stays CREATING until its items are loaded.
            if (d && (d->status == table_status::creating || d->status == table_status::updating) &&
                !d->restore.in_progress) {
                catalog_command c;
                c.op = catalog_command::kind::set_status;
                c.table.name = d->name;
                c.table.table_id = d->table_id;
                c.from = d->status;
                c.to = table_status::active;
                (void)apply_catalog_locked(c);
            }
        }
        // A new index: mark every item, carry the marks, then activate.
        for (const auto& name : _catalog.list_tables("", SIZE_MAX)) {
            const auto d = _catalog.describe(name);
            const auto* g =
                d && d->status == table_status::active ? backfilling_index(*d) : nullptr;
            if (g == nullptr) {
                continue;
            }
            std::vector<std::string> keys;
            _items.scan_items(table_range(d->table_id), kythira::scan_direction::forward,
                              [&](std::string_view key, const item&) {
                                  keys.emplace_back(key);
                                  return true;
                              });
            for (std::size_t at = 0; at < keys.size(); at += k_backfill_batch) {
                const auto end = std::min(keys.size(), at + k_backfill_batch);
                (void)apply_item_locked(backfill_marks(
                    *d, std::vector<std::string>(keys.begin() + static_cast<std::ptrdiff_t>(at),
                                                 keys.begin() + static_cast<std::ptrdiff_t>(end))));
            }
            propagate_locked();
            (void)apply_catalog_locked(activate_index(*d, g->name));
        }
        for (const auto& job : _catalog.delete_jobs()) {
            for (;;) {
                const auto step = apply_item_locked(drop_step_for(job, k_drop_step_keys));
                catalog_command c;
                c.op = catalog_command::kind::job_progress;
                c.table.table_id = job.table_id;
                c.deleted = step.deleted;
                c.done = !step.more;
                (void)apply_catalog_locked(c);
                if (!step.more) {
                    break;
                }
            }
        }
    }

    clock_fn _clock;
    capacity_throttler _throttler;
    std::mutex _mu;
    dynamo_catalog_state_machine<Engine> _catalog;
    dynamo_item_state_machine<Engine> _items;
    std::uint64_t _index = 0;
    std::uint64_t _last_trim_ms = 0;
};

static_assert(dynamo_backend<local_backend<>>);
static_assert(change_feed_backend<local_backend<>>);

}  // namespace kythira::dynamodb
