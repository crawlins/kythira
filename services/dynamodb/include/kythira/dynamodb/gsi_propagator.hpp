// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gsi_propagator.hpp
/// @brief What carrying one pending GSI change to the indexes takes
///        (`.kiro/specs/dynamodb-compatible-api/` task 11, Requirements
///        11.2-11.4).
///
/// `plan_propagation` turns a pending change (`global_index.hpp`), read with
/// the item it belongs to, into the index writes it needs and the
/// `index_ack` that ends it. Both backends run the same plan:
///
/// - `local_backend` runs it under its lock after every write, so its
///   indexes are never behind (one process, one copy).
/// - `raft_backend` runs it on the leader of each item group, from a worker
///   thread, through the ordinary routed write path, so the index writes
///   reach the index entries' groups wherever they are. A change is carried
///   only while this node leads its group; a new leader finds the change
///   still recorded and carries it again, which the entry versions make
///   harmless.
///
/// The index writes of one change all carry the item's version. The ack
/// goes only after every one of them was applied, so a change is never
/// dropped before the indexes hold it.

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kythira::dynamodb {

/// The writes that carry one pending change.
struct gsi_plan {
    /// `index_put` and `index_delete`, in any order.
    std::vector<item_command> index_writes;
    /// Sent after every index write applied.
    item_command ack;
};

/// The plan for `work`, a pending change of an item of `table` (nothing
/// when the table is gone: its delete job removes the change with the
/// rest of its key space, so only the ack is planned).
[[nodiscard]] inline auto plan_propagation(const std::optional<table_descriptor>& table,
                                           const gsi_work& work) -> gsi_plan {
    gsi_plan plan;
    plan.ack.op = item_command::kind::index_ack;
    plan.ack.target = work.pending_key;
    plan.ack.version = work.pending.version;
    if (!table || table->table_id != work.pending.table_id) {
        return plan;
    }
    std::vector<std::string> now;
    for (const auto& g : table->gsis) {
        if (!work.current) {
            continue;
        }
        auto key = gsi_entry_key(table->schema, g, *work.current);
        if (!key) {
            continue;
        }
        item_command put;
        put.op = item_command::kind::index_put;
        put.target = *key;
        put.version = work.pending.version;
        put.payload = *gsi_entry(table->schema, g, *work.current);
        plan.index_writes.push_back(std::move(put));
        now.push_back(std::move(*key));
    }
    for (const auto& old : work.pending.old_entries) {
        if (std::find(now.begin(), now.end(), old) != now.end()) {
            continue;
        }
        // An entry of an index that has since been removed goes with that
        // index's key space.
        const auto space = key_space_of(old);
        const bool live =
            space && std::any_of(table->gsis.begin(), table->gsis.end(),
                                 [&](const global_index& g) { return g.index_id == *space; });
        if (!live) {
            continue;
        }
        item_command del;
        del.op = item_command::kind::index_delete;
        del.target = old;
        del.version = work.pending.version;
        plan.index_writes.push_back(std::move(del));
    }
    plan.ack.entries = std::move(now);
    return plan;
}

}  // namespace kythira::dynamodb
