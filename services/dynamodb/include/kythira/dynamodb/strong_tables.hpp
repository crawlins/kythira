// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file strong_tables.hpp
/// @brief The worker that keeps each region's catalog in step with the
///        strong catalog (task 27, design.md "Multi-region strong
///        consistency").
///
/// `UpdateTable` with `MultiRegionConsistency=STRONG` (`table_api.hpp`)
/// writes the table into the strong catalog, whose group spans the three
/// regions, listing every replica region and the witness, and switches the
/// asking region's descriptor to the strong ids. It runs on each region's
/// catalog leader, with the gateway's other background work. One pass:
///
///   - A strong table that lists this region as a replica and that this
///     region's catalog lacks is adopted (`adopt_strong`): the descriptor
///     is the strong catalog's, with this region as its `region` and the
///     other replica regions as its `replicas`. The strong catalog then
///     marks this region's replica ACTIVE, or CREATION_FAILED when a table
///     of that name is already here.
///   - A strong table that lists this region as its witness gets its
///     witness marked ACTIVE: the region's strong voters already hold the
///     table, since every strong group spans the three regions.
///   - The other regions' replica and witness statuses are copied from the
///     strong catalog into this region's descriptor (`strong_members`).
///   - A table made strong here more than `k_salvage_delay_ms` ago has its
///     old key space salvaged: an item a request still in flight wrote
///     under the old table id is copied into the strong table (unless the
///     strong table already has that key), and the old key spaces get
///     delete jobs (`salvage_done`).

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

template<typename Backend> class strong_table_worker {
public:
    explicit strong_table_worker(Backend& backend) : _backend(backend) {}

    void set_region(std::string region) { _region = std::move(region); }
    void set_log(std::function<void(std::string_view)> log) { _log = std::move(log); }

    /// One pass; returns the changes it made.
    auto pass() -> std::size_t {
        if (!_backend.has_strong() || _region.empty()) {
            return 0;
        }
        std::size_t changed = 0;
        auto& strong = _backend.strong();
        for (const auto& name : strong.list_tables("", SIZE_MAX)) {
            const auto shared = strong.describe(name);
            if (!shared || !shared->strong || shared->status == table_status::deleting) {
                continue;
            }
            changed += witness(*shared);
            if (shared->replica_in(_region) != nullptr) {
                changed += replica(*shared);
            }
        }
        const auto now = _backend.now_ms();
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            if (d && d->strong && !d->retired_ids.empty() && now >= d->salvage_after_ms) {
                changed += salvage(*d);
            }
        }
        return changed;
    }

private:
    /// Marks this region's witness ACTIVE.
    auto witness(const table_descriptor& shared) -> std::size_t {
        const auto* w = shared.witness_in(_region);
        if (w == nullptr || w->status != witness_status::creating) {
            return 0;
        }
        catalog_command c;
        c.op = catalog_command::kind::strong_members;
        c.table.name = shared.name;
        c.table.table_id = shared.table_id;
        c.table.schema = shared.schema;
        c.table.witnesses = {table_witness{_region, witness_status::active}};
        c.now_ms = _backend.now_ms();
        return _backend.strong().catalog(c).ok() ? 1 : 0;
    }

    /// Adopts the table here, or copies the other regions' statuses.
    auto replica(const table_descriptor& shared) -> std::size_t {
        const auto mine = *shared.replica_in(_region);
        const auto here = _backend.describe(shared.name);
        if (!here) {
            catalog_command c;
            c.op = catalog_command::kind::adopt_strong;
            c.table = shared;
            c.table.region = _region;
            std::erase_if(c.table.replicas,
                          [&](const table_replica& r) { return r.region == _region; });
            c.now_ms = _backend.now_ms();
            const auto r = _backend.catalog(c);
            return report(shared, r.ok() ? replica_status::active : replica_status::creation_failed,
                          r.ok() ? std::string() : r.error_message);
        }
        if (!here->strong || here->table_id != shared.table_id) {
            if (mine.status == replica_status::creating) {
                return report(shared, replica_status::creation_failed,
                              "Table " + shared.name + " already exists in " + _region);
            }
            return 0;
        }
        std::size_t changed = 0;
        if (mine.status == replica_status::creating) {
            changed += report(shared, replica_status::active, {});
        }
        // The other regions' statuses, as the strong catalog has them.
        catalog_command c;
        c.op = catalog_command::kind::strong_members;
        c.table.name = here->name;
        c.table.table_id = here->table_id;
        c.table.schema = here->schema;
        bool differs = false;
        for (const auto& r : shared.replicas) {
            const auto* have = here->replica_in(r.region);
            if (have != nullptr && (have->status != r.status || have->failure != r.failure)) {
                differs = true;
            }
            if (r.region != _region) {
                c.table.replicas.push_back(r);
            }
        }
        for (const auto& w : shared.witnesses) {
            const auto* have = here->witness_in(w.region);
            if (have != nullptr && have->status != w.status) {
                differs = true;
            }
        }
        c.table.witnesses = shared.witnesses;
        if (differs) {
            c.now_ms = _backend.now_ms();
            if (_backend.catalog(c).ok()) {
                ++changed;
            }
        }
        return changed;
    }

    /// Sets this region's replica status in the strong catalog.
    auto report(const table_descriptor& shared, replica_status status, std::string failure)
        -> std::size_t {
        catalog_command c;
        c.op = catalog_command::kind::strong_members;
        c.table.name = shared.name;
        c.table.table_id = shared.table_id;
        c.table.schema = shared.schema;
        c.table.replicas = {
            table_replica{.region = _region, .status = status, .failure = std::move(failure)}};
        c.now_ms = _backend.now_ms();
        const auto r = _backend.strong().catalog(c);
        if (!r.ok() && _log) {
            _log("strong tables: cannot report " + shared.name + "'s replica in " + _region + ": " +
                 r.error_message);
        }
        return r.ok() ? 1 : 0;
    }

    /// Moves what a request in flight wrote under the table's old id into
    /// the strong table, then retires the old key spaces.
    auto salvage(const table_descriptor& d) -> std::size_t {
        // The first pair is the table's own; index entries follow from the
        // items through the strong table's propagator.
        const auto [from, to] = d.retired_ids.front();
        std::vector<item> stray;
        _backend.read_range(table_range(from), kythira::scan_direction::forward, true,
                            [&](std::string_view, const item& it) {
                                stray.push_back(it);
                                return true;
                            });
        for (const auto& it : stray) {
            item key;
            for (const auto* name :
                 {&d.schema.partition_name, d.schema.sort_name ? &*d.schema.sort_name : nullptr}) {
                if (name != nullptr) {
                    if (const auto* v = find_attribute(it, *name)) {
                        key.emplace_back(*name, *v);
                    }
                }
            }
            if (!_backend.read(to, d.schema, key, true)) {
                item_command put;
                put.op = item_command::kind::put;
                put.table_id = to;
                put.schema = d.schema;
                put.payload = it;
                put.gsis = d.gsis;
                put.log_changes = d.logs_changes();
                put.written_ms = _backend.now_ms();
                (void)_backend.write(put);
            }
            item_command del;
            del.op = item_command::kind::delete_;
            del.table_id = from;
            del.schema = d.schema;
            del.payload = std::move(key);
            (void)_backend.write(del);
        }
        if (!stray.empty() && _log) {
            _log("strong tables: moved " + std::to_string(stray.size()) + " item(s) written to " +
                 d.name + " while it was made strong");
        }
        catalog_command c;
        c.op = catalog_command::kind::salvage_done;
        c.table.name = d.name;
        c.table.table_id = d.table_id;
        c.table.schema = d.schema;
        c.now_ms = _backend.now_ms();
        return _backend.catalog(c).ok() ? 1 : 0;
    }

    Backend& _backend;
    std::string _region;
    std::function<void(std::string_view)> _log;
};

}  // namespace kythira::dynamodb
