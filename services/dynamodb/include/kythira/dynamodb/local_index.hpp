// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file local_index.hpp
/// @brief Local secondary indexes: their definition, where their entries
///        live, and what an entry holds (`.kiro/specs/dynamodb-compatible-api/`
///        task 9, Requirements 11.1, 11.5).
///
/// An LSI entry lives beside its item collection, in the same group:
///
/// | What | Key (data column family unless noted) |
/// |---|---|
/// | Base item | `t/<id>/<h16(pk)><enc(pk)>\0<enc(sk)>` |
/// | LSI entry | `t/<id>/<h16(pk)><enc(pk)>\1<lsi no><enc(lsi sk)><enc(sk)>` |
/// | Collection size | `t/<id>/<h16(pk)><enc(pk)>\2`, meta column family |
///
/// Every key of a collection shares its partition prefix, so a split at a
/// partition boundary (the only place `dynamo_item_state_machine` allows)
/// keeps a collection, its entries and its size counter in one group, and
/// the state machine updates all three in the entry's one batch. The base
/// sort key ends an entry key, so two items with the same index sort key
/// get two entries, ordered by the base sort key.
///
/// An item without the index's sort key attribute has no entry (indexes
/// are sparse). An entry holds the item's projection: the table and index
/// keys for `KEYS_ONLY`, those plus the named attributes for `INCLUDE`, and
/// the whole item for `ALL`.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// `Projection.ProjectionType`.
enum class projection_kind : std::uint8_t {
    keys_only = 0,
    include = 1,
    all = 2,
};

[[nodiscard]] inline auto projection_name(projection_kind p) -> std::string_view {
    switch (p) {
        case projection_kind::keys_only:
            return "KEYS_ONLY";
        case projection_kind::include:
            return "INCLUDE";
        case projection_kind::all:
            return "ALL";
    }
    return "ALL";
}

/// One local secondary index. Its partition key is the table's.
struct local_index {
    std::string name;
    /// The index's place in the table's list, which names its entries'
    /// key space. Fixed when the table is created (LSIs cannot be added or
    /// removed later).
    std::uint8_t number = 0;
    std::string sort_name;
    attribute_type sort_type = attribute_type::S;
    projection_kind projection = projection_kind::all;
    /// `INCLUDE`'s `NonKeyAttributes`, in the order given.
    std::vector<std::string> non_key;

    friend auto operator==(const local_index&, const local_index&) -> bool = default;
};

/// AWS's limits on LSIs.
inline constexpr std::size_t k_max_local_indexes = 5;
/// The sum of every index's `NonKeyAttributes` count on one table.
inline constexpr std::size_t k_max_projected_attributes = 100;
/// The largest item collection of a table with LSIs.
inline constexpr std::uint64_t k_max_collection_bytes = std::uint64_t{10} * 1024 * 1024 * 1024;
/// What AWS adds to an index entry's size for its own bookkeeping.
inline constexpr std::size_t k_index_entry_overhead = 100;

namespace lsi_detail {

inline constexpr char k_base_marker = '\0';
inline constexpr char k_index_marker = '\1';
inline constexpr char k_collection_marker = '\2';
/// `t/` + 8 id bytes + `/`.
inline constexpr std::size_t k_table_prefix_bytes = 11;

}  // namespace lsi_detail

/// The key schema an index is queried by.
[[nodiscard]] inline auto index_schema(const key_schema& table, const local_index& lsi)
    -> key_schema {
    key_schema s;
    s.partition_name = table.partition_name;
    s.partition_type = table.partition_type;
    s.sort_name = lsi.sort_name;
    s.sort_type = lsi.sort_type;
    return s;
}

/// `t/<id>/<h16(pk)><enc(pk)>\1<no>`: every entry of index `number` in one
/// collection.
[[nodiscard]] inline auto index_prefix(std::uint64_t table_id, const attribute_value& pk,
                                       std::uint8_t number) -> std::string {
    auto out = partition_prefix(table_id, pk);
    out.push_back(lsi_detail::k_index_marker);
    out.push_back(static_cast<char>(number));
    return out;
}

/// The key of `it`'s entry in `lsi`, or nothing when `it` lacks the
/// index's sort key (or a table key).
[[nodiscard]] inline auto index_entry_key(std::uint64_t table_id, const key_schema& table,
                                          const local_index& lsi, const item& it)
    -> std::optional<std::string> {
    const auto* pk = find_attribute(it, table.partition_name);
    const auto* ls = find_attribute(it, lsi.sort_name);
    const auto* sk = table.sort_name ? find_attribute(it, *table.sort_name) : nullptr;
    if (pk == nullptr || ls == nullptr || sk == nullptr) {
        return std::nullopt;
    }
    return index_prefix(table_id, *pk, lsi.number) + encode_key_component(*ls) +
           encode_key_component(*sk);
}

/// Where a collection's size is counted (meta column family).
[[nodiscard]] inline auto collection_size_key(std::uint64_t table_id, const attribute_value& pk)
    -> std::string {
    auto out = partition_prefix(table_id, pk);
    out.push_back(lsi_detail::k_collection_marker);
    return out;
}

/// Whether an attribute is in `lsi`'s projection (keys always are).
[[nodiscard]] inline auto projects(const key_schema& table, const local_index& lsi,
                                   std::string_view name) -> bool {
    if (lsi.projection == projection_kind::all || name == table.partition_name ||
        (table.sort_name && name == *table.sort_name) || name == lsi.sort_name) {
        return true;
    }
    return lsi.projection == projection_kind::include &&
           std::find(lsi.non_key.begin(), lsi.non_key.end(), name) != lsi.non_key.end();
}

/// `it`'s entry in `lsi`: its projection, or nothing when `it` has no
/// entry there.
[[nodiscard]] inline auto index_entry(const key_schema& table, const local_index& lsi,
                                      const item& it) -> std::optional<item> {
    if (find_attribute(it, lsi.sort_name) == nullptr) {
        return std::nullopt;
    }
    if (lsi.projection == projection_kind::all) {
        return it;
    }
    item out;
    for (const auto& [name, value] : it) {
        if (projects(table, lsi, name)) {
            out.emplace_back(name, value);
        }
    }
    return out;
}

/// What an entry adds to its collection's size.
[[nodiscard]] inline auto entry_size(const item& entry) -> std::uint64_t {
    return item_size(entry) + k_index_entry_overhead;
}

/// Checks the index key attributes `it` carries, with DynamoDB's wording:
/// an index sort key of the wrong type, or an empty string or binary.
[[nodiscard]] inline auto validate_index_keys(const item& it, const std::vector<local_index>& lsis)
    -> std::optional<aws_json::error> {
    for (const auto& lsi : lsis) {
        const auto* v = find_attribute(it, lsi.sort_name);
        if (v == nullptr) {
            continue;
        }
        if (v->type != lsi.sort_type) {
            return validation_error(std::string(k_invalid_parameters) +
                                    "Type mismatch for Index Key");
        }
        if ((v->type == attribute_type::S || v->type == attribute_type::B) && v->str.empty()) {
            return validation_error(
                "One or more parameter values are not valid. A value specified for a secondary "
                "index key is not supported. The AttributeValue for a key attribute cannot "
                "contain an empty " +
                std::string(v->type == attribute_type::S ? "string" : "binary") +
                " value. IndexName: " + lsi.name + ", IndexKey: " + lsi.sort_name);
        }
    }
    return std::nullopt;
}

/// What a key in a table's range holds.
struct item_key_class {
    enum class kind : std::uint8_t {
        base,
        index
    };
    kind what = kind::base;
    /// The index number, for `index`.
    std::uint8_t number = 0;
};

/// Reads `key` (a data key of a table whose partition key has type
/// `partition_type`): a base item or an index entry. Nothing for anything
/// else.
[[nodiscard]] inline auto classify_item_key(std::string_view key, attribute_type partition_type)
    -> std::optional<item_key_class> {
    constexpr std::size_t k_lead = lsi_detail::k_table_prefix_bytes + 2;
    if (key.size() <= k_lead) {
        return std::nullopt;
    }
    auto rest = key.substr(k_lead);
    const bool ok = partition_type == attribute_type::N
                        ? dynamo_decimal::decode_key(rest).has_value()
                        : decode_key_bytes(rest).has_value();
    if (!ok || rest.empty()) {
        return std::nullopt;
    }
    if (rest[0] == lsi_detail::k_base_marker) {
        return item_key_class{};
    }
    if (rest[0] == lsi_detail::k_index_marker && rest.size() >= 2) {
        return item_key_class{item_key_class::kind::index, static_cast<std::uint8_t>(rest[1])};
    }
    return std::nullopt;
}

namespace lsi_detail {

inline void put_index(std::string& out, const local_index& lsi) {
    detail::put_bytes(out, lsi.name);
    out.push_back(static_cast<char>(lsi.number));
    detail::put_bytes(out, lsi.sort_name);
    out.push_back(static_cast<char>(lsi.sort_type));
    out.push_back(static_cast<char>(lsi.projection));
    detail::put_varint(out, lsi.non_key.size());
    for (const auto& a : lsi.non_key) {
        detail::put_bytes(out, a);
    }
}

[[nodiscard]] inline auto get_index(detail::reader& r) -> std::optional<local_index> {
    local_index lsi;
    auto name = r.bytes();
    const auto number = r.byte();
    auto sort = r.bytes();
    const auto type = r.byte();
    const auto projection = r.byte();
    const auto count = r.count();
    if (!name || !number || !sort || !type || !projection ||
        *projection > static_cast<std::uint8_t>(projection_kind::all) || !count ||
        (*type != static_cast<std::uint8_t>(attribute_type::S) &&
         *type != static_cast<std::uint8_t>(attribute_type::N) &&
         *type != static_cast<std::uint8_t>(attribute_type::B))) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *count; ++i) {
        auto a = r.bytes();
        if (!a) {
            return std::nullopt;
        }
        lsi.non_key.push_back(std::move(*a));
    }
    lsi.name = std::move(*name);
    lsi.number = *number;
    lsi.sort_name = std::move(*sort);
    lsi.sort_type = static_cast<attribute_type>(*type);
    lsi.projection = static_cast<projection_kind>(*projection);
    return lsi;
}

}  // namespace lsi_detail

/// A list of indexes in its stored form: a count, then each index.
inline void put_local_indexes(std::string& out, const std::vector<local_index>& lsis) {
    detail::put_varint(out, lsis.size());
    for (const auto& lsi : lsis) {
        lsi_detail::put_index(out, lsi);
    }
}

[[nodiscard]] inline auto get_local_indexes(detail::reader& r)
    -> std::optional<std::vector<local_index>> {
    const auto count = r.count();
    if (!count || *count > k_max_local_indexes) {
        return std::nullopt;
    }
    std::vector<local_index> out;
    for (std::size_t i = 0; i < *count; ++i) {
        auto lsi = lsi_detail::get_index(r);
        if (!lsi) {
            return std::nullopt;
        }
        out.push_back(std::move(*lsi));
    }
    return out;
}

}  // namespace kythira::dynamodb
