// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file key_layout.hpp
/// @brief Where DynamoDB's catalog and items live in the node's `kv_engine`
///        (`.kiro/specs/dynamodb-compatible-api/` design "Key layout", task 6).
///
/// Every key is an ordered byte string:
///
/// | Key space | Column family | Key |
/// |---|---|---|
/// | Catalog | data | `\0meta/dynamodb/table/<name>`, `.../job/<id>`, `.../next-table-id` |
/// | Base item | data | `t/<id>/<h16(pk)><enc(pk)>\0<enc(sk)>` |
/// | Item version | meta | the base item's key |
///
/// `<id>` is the table id as 8 big-endian bytes, so one table's keys are
/// contiguous and tables sort by id. `h16` is the first two bytes of the
/// SHA-256 of the encoded partition key: partitions spread evenly over the
/// hash space, so groups split by hash range and parallel `Scan` segments
/// are hash slices, while the encoded partition key after it keeps an item
/// collection contiguous for `Query`.
///
/// The design's table puts item versions in a `v/<id>/` key space. They live
/// instead in the meta column family under the base item's own key, so a
/// group's key range covers an item and its version together: a checkpoint,
/// split, merge or drop of `[start, end)` moves both, which a separate
/// `v/` prefix outside the range could not.

#include <kythira/dynamodb/item_codec.hpp>

#include <raft/persistent_state_machine.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kythira::dynamodb {

/// The catalog's reserved range (Requirement 7.2).
inline constexpr std::string_view k_catalog_prefix{"\0meta/dynamodb/", 15};

/// The successor of a prefix: the smallest key above every key that starts
/// with it. Empty when there is none (all 0xFF).
[[nodiscard]] inline auto prefix_end(std::string prefix) -> std::optional<std::string> {
    while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xFF) {
        prefix.pop_back();
    }
    if (prefix.empty()) {
        return std::nullopt;
    }
    prefix.back() = static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
    return prefix;
}

/// Every key starting with `prefix`.
[[nodiscard]] inline auto prefix_range(std::string prefix) -> kythira::key_range {
    auto end = prefix_end(prefix);
    return kythira::key_range{std::move(prefix), std::move(end)};
}

/// The catalog group's range.
[[nodiscard]] inline auto catalog_range() -> kythira::key_range {
    return prefix_range(std::string(k_catalog_prefix));
}

/// `t/<id>/`: the prefix of every item key of table (or, later, index) `id`.
[[nodiscard]] inline auto table_prefix(std::uint64_t table_id) -> std::string {
    std::string out = "t/";
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((table_id >> shift) & 0xFFU));
    }
    out.push_back('/');
    return out;
}

/// Every key of table `id`, in every column family.
[[nodiscard]] inline auto table_range(std::uint64_t table_id) -> kythira::key_range {
    return prefix_range(table_prefix(table_id));
}

/// The two-byte hash that leads a partition's keys.
[[nodiscard]] inline auto partition_hash(std::string_view encoded_partition_key) -> std::string {
    const auto digest = kythira::sha256_of(encoded_partition_key);
    return std::string{static_cast<char>(digest[0]), static_cast<char>(digest[1])};
}

/// `t/<id>/<h16(pk)><enc(pk)>`: the prefix of one item collection. A
/// group may only be cut here (or at a table boundary), never inside a
/// collection, so `Query` stays one range read on one group.
[[nodiscard]] inline auto partition_prefix(std::uint64_t table_id, const attribute_value& pk)
    -> std::string {
    const auto enc = encode_key_component(pk);
    return table_prefix(table_id) + partition_hash(enc) + enc;
}

/// The base item key of `key` (an item or a `Key` parameter carrying the
/// schema's attributes, already validated), or nothing when an attribute
/// is missing.
[[nodiscard]] inline auto base_item_key(std::uint64_t table_id, const key_schema& schema,
                                        const item& key) -> std::optional<std::string> {
    const auto* pk = find_attribute(key, schema.partition_name);
    if (pk == nullptr) {
        return std::nullopt;
    }
    auto out = partition_prefix(table_id, *pk);
    out.push_back('\0');
    if (schema.sort_name) {
        const auto* sk = find_attribute(key, *schema.sort_name);
        if (sk == nullptr) {
            return std::nullopt;
        }
        out += encode_key_component(*sk);
    }
    return out;
}

/// True when `key` is a place a group of item keys may be split: a table
/// prefix, or a partition prefix with nothing after it. Anything else would
/// cut an item collection in two.
[[nodiscard]] inline auto is_partition_boundary(std::string_view key) -> bool {
    constexpr std::size_t k_table_prefix_bytes = 2 + 8 + 1;
    if (key.size() < k_table_prefix_bytes || !key.starts_with("t/") ||
        key[k_table_prefix_bytes - 1] != '/') {
        return false;
    }
    auto rest = key.substr(k_table_prefix_bytes);
    if (rest.empty()) {
        return true;
    }
    if (rest.size() < 2) {
        return false;
    }
    const auto hash = rest.substr(0, 2);
    rest.remove_prefix(2);
    // The rest must be exactly one encoded key component whose hash leads
    // it. S and B components end with 0x00 0x01; N components are
    // self-delimiting, so try both readings.
    const auto whole = rest;
    if (auto copy = rest; decode_key_bytes(copy) && copy.empty()) {
        return partition_hash(whole) == hash;
    }
    if (auto copy = rest; dynamo_decimal::decode_key(copy) && copy.empty()) {
        return partition_hash(whole) == hash;
    }
    return false;
}

}  // namespace kythira::dynamodb
