// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file routed_backend.hpp
/// @brief The backend a gateway serves from when its node also runs a
///        strong host (task 27, design.md "Multi-region strong
///        consistency").
///
/// A strong table's items live in the strong host, whose groups span three
/// regions; everything else lives in the cluster's own host. The strong
/// catalog hands out table and index ids from `k_strong_table_id_base`, so
/// a key, a table id or a change log stream key says which host owns it.
/// This backend sends each call to that host:
///
/// | Call | Goes to |
/// |---|---|
/// | `catalog`, `describe`, `list_tables`, `now_ms`, `throttler` | the cluster's host |
/// | `write`, `read`, `settle_item` | the host owning the table id |
/// | `read_range`, `read_stamped_range`, `change_shards` | the host owning the range |
/// | `read_changes` | the host owning the stream's range start |
/// | `transact` | the host owning every action's table |
/// | `serve`, `leads_catalog` | the cluster's host |
///
/// The cluster's catalog stays the one the API reads: a region's
/// descriptor of a strong table carries the strong ids. Without a strong
/// host every call goes to the cluster's.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::dynamodb {

template<typename Local, typename Strong = Local> class routed_backend {
public:
    /// `strong` is null on a node without a strong host.
    explicit routed_backend(Local& local, Strong* strong = nullptr)
        : _local(local), _strong(strong) {}

    [[nodiscard]] auto has_strong() const -> bool { return _strong != nullptr; }
    [[nodiscard]] auto local() -> Local& { return _local; }
    /// The strong host's backend; only when `has_strong()`.
    [[nodiscard]] auto strong() -> Strong& { return *_strong; }

    // -- dynamo_backend -------------------------------------------------------

    auto catalog(const catalog_command& c) -> catalog_result { return _local.catalog(c); }

    auto write(const item_command& c) -> item_result {
        return by_table(c.table_id, [&](auto& b) { return b.write(c); });
    }

    auto read(std::uint64_t table_id, const key_schema& schema, const item& key, bool consistent)
        -> std::optional<item> {
        return by_table(table_id,
                        [&](auto& b) { return b.read(table_id, schema, key, consistent); });
    }

    auto read_range(const kythira::key_range& range, kythira::scan_direction direction,
                    bool consistent, const range_visitor& visit) -> void {
        by_range(range, [&](auto& b) { b.read_range(range, direction, consistent, visit); });
    }

    auto describe(std::string_view name) -> std::optional<table_descriptor> {
        return _local.describe(name);
    }

    auto list_tables(std::string_view exclusive_start, std::size_t limit)
        -> std::vector<std::string> {
        return _local.list_tables(exclusive_start, limit);
    }

    auto now_ms() -> std::uint64_t { return _local.now_ms(); }

    /// One host's transaction: every action's table must live there.
    auto transact(const std::vector<item_command>& actions) -> txn_report {
        bool strong = false;
        bool local = false;
        for (const auto& a : actions) {
            (route_strong(a.table_id) ? strong : local) = true;
        }
        if (strong && local) {
            throw std::invalid_argument(
                "routed_backend: a transaction cannot span a strong table and another");
        }
        if (strong) {
            return _strong->transact(actions);
        }
        return _local.transact(actions);
    }

    auto settle_item(const table_descriptor& table, const item& key) -> void {
        by_table(table.table_id, [&](auto& b) { b.settle_item(table, key); });
    }

    auto throttler() -> capacity_throttler& { return _local.throttler(); }

    // -- change_feed_backend --------------------------------------------------

    auto change_shards(const kythira::key_range& range) -> std::vector<change_shard> {
        return by_range(range, [&](auto& b) { return b.change_shards(range); });
    }

    auto read_changes(const std::string& key, std::uint64_t from_seq, std::size_t max_records)
        -> change_page {
        const auto parsed = parse_change_stream_key(key);
        if (parsed && _strong != nullptr && is_strong_key(parsed->first)) {
            return _strong->read_changes(key, from_seq, max_records);
        }
        return _local.read_changes(key, from_seq, max_records);
    }

    // -- replica_backend ------------------------------------------------------

    auto read_stamped_range(const kythira::key_range& range, const stamped_visitor& visit) -> void {
        by_range(range, [&](auto& b) { b.read_stamped_range(range, visit); });
    }

    // -- the cluster host's own ops -------------------------------------------

    auto serve(std::string_view op) -> std::string
    requires requires(Local& b) {
        { b.serve(op) } -> std::same_as<std::string>;
    }
    {
        return _local.serve(op);
    }

    [[nodiscard]] auto leads_catalog() -> bool
    requires requires(Local& b) {
        { b.leads_catalog() } -> std::same_as<bool>;
    }
    {
        return _local.leads_catalog();
    }

private:
    [[nodiscard]] auto route_strong(std::uint64_t table_id) const -> bool {
        return _strong != nullptr && is_strong_table(table_id);
    }

    template<typename F> auto by_table(std::uint64_t table_id, F&& f) -> decltype(auto) {
        if (route_strong(table_id)) {
            return f(*_strong);
        }
        return f(_local);
    }

    /// A range lies wholly in one host: every range a handler reads is one
    /// table's, or one index's.
    template<typename F> auto by_range(const kythira::key_range& range, F&& f) -> decltype(auto) {
        if (_strong == nullptr) {
            return f(_local);
        }
        const bool start = is_strong_key(range.start);
        const bool end = !range.end || *range.end > strong_key_start();
        if (start != end) {
            throw std::invalid_argument("routed_backend: a range spans the strong host's keys");
        }
        if (start) {
            return f(*_strong);
        }
        return f(_local);
    }

    Local& _local;
    Strong* _strong;
};

}  // namespace kythira::dynamodb
