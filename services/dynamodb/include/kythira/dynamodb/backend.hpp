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
///
/// `local_backend` applies the commands directly to the two state machines
/// on one engine, with no Raft. Handler tests use it, and so does a
/// single-process daemon. It does a table's follow-on work as soon as the
/// table entry is applied: CREATING and UPDATING become ACTIVE, and a
/// delete runs its bounded drop steps to the end. The Raft-hosted backend
/// does the same work from the catalog leader.

#include <kythira/dynamodb/catalog.hpp>
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

template<typename B>
concept dynamo_backend =
    requires(B& b, const catalog_command& cc, const item_command& ic, std::string_view name,
             std::size_t limit, std::uint64_t table_id, const key_schema& schema, const item& key,
             bool consistent, const kythira::key_range& range, kythira::scan_direction direction,
             const range_visitor& visit) {
        { b.catalog(cc) } -> std::same_as<catalog_result>;
        { b.write(ic) } -> std::same_as<item_result>;
        { b.read(table_id, schema, key, consistent) } -> std::same_as<std::optional<item>>;
        { b.read_range(range, direction, consistent, visit) } -> std::same_as<void>;
        { b.describe(name) } -> std::same_as<std::optional<table_descriptor>>;
        { b.list_tables(name, limit) } -> std::same_as<std::vector<std::string>>;
        { b.now_ms() } -> std::same_as<std::uint64_t>;
    };

/// Both state machines on one engine, applied in-process.
template<kythira::kv_engine Engine = kythira::memory_kv_engine> class local_backend {
public:
    using clock_fn = std::function<std::uint64_t()>;

    /// Items of every table live in one group over `t/`.
    explicit local_backend(Engine& engine, clock_fn clock = system_clock_ms())
        : _clock(std::move(clock)) {
        _catalog.open(engine, catalog_range());
        _items.open(engine, prefix_range("t/"));
        // The index can only be read once both machines are open.
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
        _index = std::max<std::uint64_t>(_catalog.applied_index(), _items.applied_index());
        // Finish any work a previous run left (a delete job, a CREATING
        // table).
        const std::lock_guard lock(_mu);
        settle_locked();
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
        return r;
    }

    auto write(const item_command& c) -> item_result {
        const std::lock_guard lock(_mu);
        return apply_item_locked(c);
    }

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

    /// Items deleted per drop step.
    static constexpr std::uint32_t k_drop_step_keys = 1000;

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

    // The catalog leader's follow-on work, done at once.
    auto settle_locked() -> void {
        for (const auto& name : _catalog.list_tables("", SIZE_MAX)) {
            const auto d = _catalog.describe(name);
            if (d && (d->status == table_status::creating || d->status == table_status::updating)) {
                catalog_command c;
                c.op = catalog_command::kind::set_status;
                c.table.name = d->name;
                c.table.table_id = d->table_id;
                c.from = d->status;
                c.to = table_status::active;
                (void)apply_catalog_locked(c);
            }
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
    std::mutex _mu;
    dynamo_catalog_state_machine<Engine> _catalog;
    dynamo_item_state_machine<Engine> _items;
    std::uint64_t _index = 0;
};

static_assert(dynamo_backend<local_backend<>>);

}  // namespace kythira::dynamodb
