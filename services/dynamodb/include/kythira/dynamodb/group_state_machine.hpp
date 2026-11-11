// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file group_state_machine.hpp
/// @brief The state machine every DynamoDB Raft group runs
///        (`.kiro/specs/dynamodb-compatible-api/` task 7, the Raft-hosted
///        backend).
///
/// A `multi_raft` host runs one state machine type for all its groups, but
/// DynamoDB has two kinds of group: the catalog group, whose range holds
/// `\0meta/dynamodb/`, and the item groups over `t/`. This wrapper is
/// whichever its range says. `open()` on a range that covers the catalog's
/// opens a `dynamo_catalog_state_machine` on exactly the catalog range (so
/// the bootstrap may give the catalog group everything below `t/`, which
/// keeps the shard map tiled without a filler group); any other range
/// opens a `dynamo_item_state_machine`. Every later call goes to that one.
///
/// Only item groups split or merge: the catalog is small and stays one
/// group, and `can_split_at` refuses every key outside `t/` (a catalog key
/// is never a partition boundary).
///
/// Every group runs inside core's `transactional_state_machine`
/// (`dynamo_txn_group_state_machine`), so `TransactWriteItems` and
/// `TransactGetItems` lock items across groups through the tier-1
/// coordinator (task 10, Requirement 12). Plain writes pass through it as
/// `txn_plain` entries and are refused while a transaction holds their item.

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kythira::dynamodb {

template<kythira::kv_engine Engine = kythira::memory_kv_engine, typename LogIndex = std::uint64_t>
class dynamo_group_state_machine {
public:
    using engine_type = Engine;
    using catalog_type = dynamo_catalog_state_machine<Engine, LogIndex>;
    using items_type = dynamo_item_state_machine<Engine, LogIndex>;

    /// Whether `range` covers the catalog's.
    [[nodiscard]] static auto is_catalog(const kythira::key_range& range) -> bool {
        return range.contains(catalog_range());
    }

    /// The node's change record store and segment fetcher, handed to the
    /// item group this opens (task 15, `change_log.hpp`).
    auto set_change_log(kythira::record_segment_store* store,
                        typename items_type::change_log_fetcher fetch = {}) -> void {
        _log = store;
        _fetch = std::move(fetch);
        if (_items) {
            _items->set_change_log(_log, _fetch);
        }
    }

    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        if (is_catalog(range)) {
            _catalog.emplace();
            _items.reset();
            _catalog->open(engine, catalog_range());
        } else {
            _items.emplace();
            _catalog.reset();
            _items->set_change_log(_log, _fetch);
            _items->open(engine, range);
        }
    }

    auto apply(const std::vector<std::byte>& entry, LogIndex index) -> std::vector<std::byte> {
        return _catalog ? _catalog->apply(entry, index) : items().apply(entry, index);
    }

    auto stage(const std::vector<std::byte>& entry, kythira::write_batch& batch)
        -> std::vector<std::byte> {
        return _catalog ? _catalog->stage(entry, batch) : items().stage(entry, batch);
    }

    auto commit_staged(kythira::write_batch& batch, LogIndex index) -> void {
        if (_catalog) {
            _catalog->commit_staged(batch, index);
        } else {
            items().commit_staged(batch, index);
        }
    }

    auto discard_staged() -> void {
        if (_catalog) {
            _catalog->discard_staged();
        } else {
            items().discard_staged();
        }
    }

    [[nodiscard]] auto key_prefix() const -> const std::string& {
        static const std::string none;
        return none;
    }

    /// Items validate their transaction writes; the catalog takes part in
    /// no transaction, so a write there is a gateway bug.
    [[nodiscard]] auto validate(const std::string& key, const std::vector<std::byte>& entry) const
        -> kythira::txn_validation {
        if (_catalog) {
            throw std::invalid_argument("dynamo_group_state_machine: a transaction in the catalog");
        }
        return items().validate(key, entry);
    }

    [[nodiscard]] auto applied_index() const -> LogIndex {
        return _catalog ? _catalog->applied_index() : items().applied_index();
    }

    [[nodiscard]] auto durable_applied_index() const -> LogIndex {
        return _catalog ? _catalog->durable_applied_index() : items().durable_applied_index();
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        return _catalog ? _catalog->checkpoint(dir) : items().checkpoint(dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                LogIndex index) -> void {
        if (_catalog) {
            _catalog->ingest(dir, manifest, index);
        } else {
            items().ingest(dir, manifest, index);
        }
    }

    auto drop() -> void {
        if (_catalog) {
            _catalog->drop();
        } else {
            items().drop();
        }
    }

    auto split_at(const std::vector<std::string>& keys) -> std::vector<kythira::key_range> {
        if (_catalog) {
            throw std::invalid_argument("dynamo_group_state_machine: the catalog does not split");
        }
        return items().split_at(keys);
    }

    auto adopt(const kythira::key_range& range) -> void {
        if (_catalog) {
            throw std::invalid_argument("dynamo_group_state_machine: the catalog does not merge");
        }
        items().adopt(range);
    }

    [[nodiscard]] auto range() const -> const kythira::key_range& {
        return _catalog ? catalog_range_ref() : items().range();
    }

    [[nodiscard]] static auto can_split_at(const std::string& key) -> bool {
        return items_type::can_split_at(key);
    }

    /// The catalog, when this is the catalog group (else null).
    [[nodiscard]] auto catalog() -> catalog_type* { return _catalog ? &*_catalog : nullptr; }
    [[nodiscard]] auto catalog() const -> const catalog_type* {
        return _catalog ? &*_catalog : nullptr;
    }

    /// The items, when this is an item group (else null).
    [[nodiscard]] auto item_group() -> items_type* { return _items ? &*_items : nullptr; }
    [[nodiscard]] auto item_group() const -> const items_type* {
        return _items ? &*_items : nullptr;
    }

private:
    [[nodiscard]] auto items() -> items_type& {
        if (!_items) {
            throw std::logic_error("dynamo_group_state_machine: not opened");
        }
        return *_items;
    }
    [[nodiscard]] auto items() const -> const items_type& {
        if (!_items) {
            throw std::logic_error("dynamo_group_state_machine: not opened");
        }
        return *_items;
    }

    [[nodiscard]] static auto catalog_range_ref() -> const kythira::key_range& {
        static const kythira::key_range r = catalog_range();
        return r;
    }

    std::optional<catalog_type> _catalog;
    std::optional<items_type> _items;
    kythira::record_segment_store* _log = nullptr;
    typename items_type::change_log_fetcher _fetch;
};

static_assert(kythira::persistent_staged_state_machine<dynamo_group_state_machine<>>);

/// The key a plain command writes, for the transaction wrapper's lock
/// check: an item command's base key (a drop step's table prefix), and for
/// anything else, which is a catalog command, the catalog prefix. No
/// transaction ever locks a catalog key, so that one never conflicts.
struct dynamo_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        item_command c;
        const std::string_view bytes(reinterpret_cast<const char*>(command.data()),  // NOLINT
                                     command.size());
        if (decode_item_command(bytes, c) != item_command_status::ok) {
            return std::string(k_catalog_prefix);
        }
        if (c.op == item_command::kind::drop_table_step) {
            return table_prefix(c.table_id);
        }
        auto key = command_key(c);
        return key ? std::move(*key) : table_prefix(c.table_id);
    }
};

/// What a DynamoDB `multi_raft` host runs in every group.
template<kythira::kv_engine Engine = kythira::memory_kv_engine>
using dynamo_txn_group_state_machine =
    kythira::transactional_state_machine<dynamo_group_state_machine<Engine>, std::string,
                                         dynamo_partitioner>;

static_assert(kythira::persistent_splittable_state_machine<dynamo_txn_group_state_machine<>>);

}  // namespace kythira::dynamodb
