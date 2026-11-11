// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file global_index.hpp
/// @brief Global secondary indexes: their definition, where their entries
///        live, what an entry holds, and the record of a change not yet
///        carried to them (`.kiro/specs/dynamodb-compatible-api/` task 11,
///        Requirements 11.2-11.6).
///
/// A GSI has its own id, from the same never-reused counter as tables, and
/// its entries form a key space of their own, laid out like a table's:
///
/// | What | Key (data column family unless noted) |
/// |---|---|
/// | GSI entry | `t/<index id>/<h16(gpk)><enc(gpk)>\0<enc(gsk)><enc(pk)><enc(sk)>` |
/// | Entry version | the entry's key, meta column family |
/// | Pending change | `t/<table id>/<h16(pk)><enc(pk)>\3<enc(sk)>`, meta column family |
///
/// The index keys come first, so a `Query` on the index is one range read
/// of one index collection, and splits at index partition boundaries keep
/// it in one group. The table's keys end the entry key, so items with equal
/// index keys get one entry each, ordered by the table key (as DynamoDB
/// Local orders them).
///
/// ## Asynchronous maintenance
///
/// The entries usually live in other groups than the item, so a write
/// cannot update them in its own batch. Instead the write leaves a *pending
/// change* beside the item, in the item's own group and batch: the item's
/// version after the write, its key, and the entry keys the indexes held
/// for it when the record was first written (the keys to remove). A later
/// write before propagation keeps those keys and only raises the version.
///
/// The base group's leader carries pending changes to the index groups
/// (`gsi_propagator.hpp`): it computes the entries from the item as it is
/// now, proposes `index_put` or `index_delete` with the item's version to
/// each entry's group, then proposes `index_ack` back to the item's group,
/// which drops the record when no newer write came meanwhile (otherwise it
/// keeps the record, now naming the entries just written as the ones to
/// remove).
///
/// Every write to an item of a table with GSIs bumps the item's version,
/// a delete included (its version stays behind in the meta family), and an
/// index group keeps an entry's version even after deleting the entry. An
/// index write applies only when its version is newer than what the entry
/// has, so a replayed, retried or late proposal, even one from a former
/// leader that lands after its successor's, never moves an entry back.
/// Those remaining versions are small and are never reclaimed yet; a later
/// task can drop them past a safe point.
///
/// Until the change log (task 15) exists, a group finds its pending changes
/// by scanning its meta range when it opens, and keeps their keys in memory
/// from then on.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// `IndexStatus`.
enum class index_status : std::uint8_t {
    creating = 0,
    active = 1,
    updating = 2,
    deleting = 3,
};

[[nodiscard]] inline auto index_status_name(index_status s) -> std::string_view {
    switch (s) {
        case index_status::creating:
            return "CREATING";
        case index_status::active:
            return "ACTIVE";
        case index_status::updating:
            return "UPDATING";
        case index_status::deleting:
            return "DELETING";
    }
    return "ACTIVE";
}

/// One global secondary index.
struct global_index {
    std::string name;
    /// The index's key space, from the catalog's table id counter.
    std::uint64_t index_id = 0;
    /// The index's partition and (optional) sort key.
    key_schema schema;
    projection_kind projection = projection_kind::all;
    /// `INCLUDE`'s `NonKeyAttributes`, in the order given.
    std::vector<std::string> non_key;
    /// Whether `ProvisionedThroughput` was given (DynamoDB Local shows it
    /// only then).
    bool has_throughput = false;
    std::uint64_t read_capacity = 0;
    std::uint64_t write_capacity = 0;
    index_status status = index_status::active;

    friend auto operator==(const global_index&, const global_index&) -> bool = default;
};

/// The indexes writes maintain: all but those being deleted (whose key
/// spaces their delete jobs are emptying).
[[nodiscard]] inline auto maintained_indexes(const std::vector<global_index>& all)
    -> std::vector<global_index> {
    std::vector<global_index> out;
    for (const auto& g : all) {
        if (g.status != index_status::deleting) {
            out.push_back(g);
        }
    }
    return out;
}

/// AWS's default limit on GSIs per table.
inline constexpr std::size_t k_max_global_indexes = 20;

namespace gsi_detail {

inline constexpr char k_pending_marker = '\3';
/// The first byte of a pending change's stored form. Item and entry
/// versions, counters and applied indexes in the meta family are 8 bytes
/// exactly, so a value of another length that starts here is a pending
/// change.
inline constexpr char k_pending_format = '\xD7';

}  // namespace gsi_detail

/// Whether `name` is one of `index`'s keys or the table's.
[[nodiscard]] inline auto is_key_of(const key_schema& table, const global_index& index,
                                    std::string_view name) -> bool {
    return name == table.partition_name || (table.sort_name && name == *table.sort_name) ||
           name == index.schema.partition_name ||
           (index.schema.sort_name && name == *index.schema.sort_name);
}

/// Whether `index` projects attribute `name` (every key always is).
[[nodiscard]] inline auto projects(const key_schema& table, const global_index& index,
                                   std::string_view name) -> bool {
    if (index.projection == projection_kind::all || is_key_of(table, index, name)) {
        return true;
    }
    return index.projection == projection_kind::include &&
           std::find(index.non_key.begin(), index.non_key.end(), name) != index.non_key.end();
}

/// The key of `it`'s entry in `index`, or nothing when `it` lacks an index
/// key attribute (indexes are sparse) or a table key.
[[nodiscard]] inline auto gsi_entry_key(const key_schema& table, const global_index& index,
                                        const item& it) -> std::optional<std::string> {
    const auto* gpk = find_attribute(it, index.schema.partition_name);
    const auto* gsk =
        index.schema.sort_name ? find_attribute(it, *index.schema.sort_name) : nullptr;
    const auto* pk = find_attribute(it, table.partition_name);
    const auto* sk = table.sort_name ? find_attribute(it, *table.sort_name) : nullptr;
    if (gpk == nullptr || (index.schema.sort_name && gsk == nullptr) || pk == nullptr ||
        (table.sort_name && sk == nullptr)) {
        return std::nullopt;
    }
    // An index key of the wrong type, or empty, never reaches an entry:
    // writes refuse it (`validate_global_index_keys`).
    if (gpk->type != index.schema.partition_type ||
        (gsk != nullptr && gsk->type != index.schema.sort_type)) {
        return std::nullopt;
    }
    auto out = partition_prefix(index.index_id, *gpk);
    out.push_back('\0');
    if (gsk != nullptr) {
        out += encode_key_component(*gsk);
    }
    out += encode_key_component(*pk);
    if (sk != nullptr) {
        out += encode_key_component(*sk);
    }
    return out;
}

/// `t/<index id>/<h16(gpk)><enc(gpk)>\0`: every entry with index partition
/// key `gpk`.
[[nodiscard]] inline auto gsi_collection_prefix(const global_index& index,
                                                const attribute_value& gpk) -> std::string {
    auto out = partition_prefix(index.index_id, gpk);
    out.push_back('\0');
    return out;
}

/// `it`'s entry in `index`: its projection, or nothing when `it` has no
/// entry there.
[[nodiscard]] inline auto gsi_entry(const key_schema& table, const global_index& index,
                                    const item& it) -> std::optional<item> {
    if (!gsi_entry_key(table, index, it)) {
        return std::nullopt;
    }
    if (index.projection == projection_kind::all) {
        return it;
    }
    item out;
    for (const auto& [name, value] : it) {
        if (projects(table, index, name)) {
            out.emplace_back(name, value);
        }
    }
    return out;
}

/// The key schema an index is read by, as the request compiler wants it.
[[nodiscard]] inline auto index_schema(const global_index& index) -> key_schema {
    return index.schema;
}

/// Checks the index key attributes `it` carries, with DynamoDB's wording:
/// a key of the wrong type, or an empty string or binary.
[[nodiscard]] inline auto validate_global_index_keys(const item& it,
                                                     const std::vector<global_index>& gsis)
    -> std::optional<aws_json::error> {
    for (const auto& g : gsis) {
        const std::pair<const std::string*, attribute_type> keys[] = {
            {&g.schema.partition_name, g.schema.partition_type},
            {g.schema.sort_name ? &*g.schema.sort_name : nullptr, g.schema.sort_type},
        };
        for (const auto& [name, type] : keys) {
            if (name == nullptr) {
                continue;
            }
            const auto* v = find_attribute(it, *name);
            if (v == nullptr) {
                continue;
            }
            if (v->type != type) {
                return validation_error(std::string(k_invalid_parameters) +
                                        "Type mismatch for Index Key");
            }
            if ((v->type == attribute_type::S || v->type == attribute_type::B) && v->str.empty()) {
                return validation_error(
                    "One or more parameter values are not valid. A value specified for a "
                    "secondary index key is not supported. The AttributeValue for a key "
                    "attribute cannot contain an empty " +
                    std::string(v->type == attribute_type::S ? "string" : "binary") +
                    " value. IndexName: " + g.name + ", IndexKey: " + *name);
            }
        }
    }
    return std::nullopt;
}

/// Where the pending change of the item with key `key` is kept (meta
/// column family): in its collection, after its LSI entries and size, so
/// it always shares the item's group.
[[nodiscard]] inline auto gsi_pending_key(std::uint64_t table_id, const key_schema& schema,
                                          const item& key) -> std::optional<std::string> {
    const auto* pk = find_attribute(key, schema.partition_name);
    if (pk == nullptr) {
        return std::nullopt;
    }
    auto out = partition_prefix(table_id, *pk);
    out.push_back(gsi_detail::k_pending_marker);
    if (schema.sort_name) {
        const auto* sk = find_attribute(key, *schema.sort_name);
        if (sk == nullptr) {
            return std::nullopt;
        }
        out += encode_key_component(*sk);
    }
    return out;
}

/// A change to an item that its table's GSIs have not seen yet.
struct gsi_pending {
    std::uint64_t table_id = 0;
    /// The item's base key.
    std::string base_key;
    /// The item's version after the newest write.
    std::uint64_t version = 0;
    /// The entries the indexes hold for the item (to remove unless the
    /// item still maps to them).
    std::vector<std::string> old_entries;

    friend auto operator==(const gsi_pending&, const gsi_pending&) -> bool = default;
};

[[nodiscard]] inline auto encode_gsi_pending(const gsi_pending& p) -> std::string {
    std::string out(1, gsi_detail::k_pending_format);
    detail::put_varint(out, p.table_id);
    detail::put_bytes(out, p.base_key);
    detail::put_varint(out, p.version);
    detail::put_varint(out, p.old_entries.size());
    for (const auto& k : p.old_entries) {
        detail::put_bytes(out, k);
    }
    return out;
}

/// Whether `value` (from the meta family) is a pending change.
[[nodiscard]] inline auto is_gsi_pending_value(std::string_view value) -> bool {
    return value.size() != 8 && !value.empty() && value[0] == gsi_detail::k_pending_format;
}

[[nodiscard]] inline auto decode_gsi_pending(std::string_view bytes) -> std::optional<gsi_pending> {
    if (!is_gsi_pending_value(bytes)) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(1));
    gsi_pending p;
    const auto id = r.varint();
    auto key = r.bytes();
    const auto version = r.varint();
    const auto n = r.count();
    if (!id || !key || !version || !n) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *n; ++i) {
        auto k = r.bytes();
        if (!k) {
            return std::nullopt;
        }
        p.old_entries.push_back(std::move(*k));
    }
    if (!r.done()) {
        return std::nullopt;
    }
    p.table_id = *id;
    p.base_key = std::move(*key);
    p.version = *version;
    return p;
}

/// The index id an entry key belongs to (`t/<id>/...`), or nothing.
[[nodiscard]] inline auto key_space_of(std::string_view key) -> std::optional<std::uint64_t> {
    constexpr std::size_t k_lead = 2 + 8 + 1;
    if (key.size() < k_lead || !key.starts_with("t/") || key[k_lead - 1] != '/') {
        return std::nullopt;
    }
    std::uint64_t id = 0;
    for (std::size_t i = 2; i < 10; ++i) {
        id = (id << 8U) | static_cast<unsigned char>(key[i]);
    }
    return id;
}

namespace gsi_detail {

inline void put_index(std::string& out, const global_index& g) {
    detail::put_bytes(out, g.name);
    detail::put_varint(out, g.index_id);
    detail::put_bytes(out, g.schema.partition_name);
    out.push_back(static_cast<char>(g.schema.partition_type));
    out.push_back(g.schema.sort_name ? '\1' : '\0');
    if (g.schema.sort_name) {
        detail::put_bytes(out, *g.schema.sort_name);
        out.push_back(static_cast<char>(g.schema.sort_type));
    }
    out.push_back(static_cast<char>(g.projection));
    detail::put_varint(out, g.non_key.size());
    for (const auto& a : g.non_key) {
        detail::put_bytes(out, a);
    }
    out.push_back(g.has_throughput ? '\1' : '\0');
    detail::put_varint(out, g.read_capacity);
    detail::put_varint(out, g.write_capacity);
    out.push_back(static_cast<char>(g.status));
}

[[nodiscard]] inline auto key_type(std::optional<std::uint8_t> t) -> bool {
    return t && (*t == static_cast<std::uint8_t>(attribute_type::S) ||
                 *t == static_cast<std::uint8_t>(attribute_type::N) ||
                 *t == static_cast<std::uint8_t>(attribute_type::B));
}

[[nodiscard]] inline auto get_index(detail::reader& r) -> std::optional<global_index> {
    global_index g;
    auto name = r.bytes();
    const auto id = r.varint();
    auto pk = r.bytes();
    const auto pt = r.byte();
    const auto has_sk = r.byte();
    if (!name || !id || !pk || !key_type(pt) || !has_sk || *has_sk > 1) {
        return std::nullopt;
    }
    if (*has_sk == 1) {
        auto sk = r.bytes();
        const auto st = r.byte();
        if (!sk || !key_type(st)) {
            return std::nullopt;
        }
        g.schema.sort_name = std::move(*sk);
        g.schema.sort_type = static_cast<attribute_type>(*st);
    }
    const auto projection = r.byte();
    const auto count = r.count();
    if (!projection || *projection > static_cast<std::uint8_t>(projection_kind::all) || !count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *count; ++i) {
        auto a = r.bytes();
        if (!a) {
            return std::nullopt;
        }
        g.non_key.push_back(std::move(*a));
    }
    const auto has_throughput = r.byte();
    const auto rcu = r.varint();
    const auto wcu = r.varint();
    const auto status = r.byte();
    if (!has_throughput || *has_throughput > 1 || !rcu || !wcu || !status ||
        *status > static_cast<std::uint8_t>(index_status::deleting)) {
        return std::nullopt;
    }
    g.name = std::move(*name);
    g.index_id = *id;
    g.schema.partition_name = std::move(*pk);
    g.schema.partition_type = static_cast<attribute_type>(*pt);
    g.projection = static_cast<projection_kind>(*projection);
    g.has_throughput = *has_throughput == 1;
    g.read_capacity = *rcu;
    g.write_capacity = *wcu;
    g.status = static_cast<index_status>(*status);
    return g;
}

}  // namespace gsi_detail

/// A list of GSIs in its stored form: a count, then each index.
inline void put_global_indexes(std::string& out, const std::vector<global_index>& gsis) {
    detail::put_varint(out, gsis.size());
    for (const auto& g : gsis) {
        gsi_detail::put_index(out, g);
    }
}

[[nodiscard]] inline auto get_global_indexes(detail::reader& r)
    -> std::optional<std::vector<global_index>> {
    const auto count = r.count();
    if (!count || *count > k_max_global_indexes) {
        return std::nullopt;
    }
    std::vector<global_index> out;
    for (std::size_t i = 0; i < *count; ++i) {
        auto g = gsi_detail::get_index(r);
        if (!g) {
            return std::nullopt;
        }
        out.push_back(std::move(*g));
    }
    return out;
}

}  // namespace kythira::dynamodb
