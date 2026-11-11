// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file api_common.hpp
/// @brief Helpers the item, query and batch handlers share
///        (`.kiro/specs/dynamodb-compatible-api/` tasks 7 and 8): reading
///        request members, finding a usable table, and the simple
///        `ConsumedCapacity` counts task 12 replaces with AWS's formula.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::dynamodb::api_detail {

/// Whether a write to `table` carries its GSI changes before it answers:
/// the table has indexes and `gsi-consistency` is `sync` for it (the
/// cluster's setting, or the table's `kythira:compat:gsi-consistency` tag).
[[nodiscard]] inline auto sync_gsis(const dynamo_compat_profile& cluster,
                                    const table_descriptor& table) -> bool {
    return !maintained_indexes(table.gsis).empty() &&
           !resolve_profile(cluster, table.tags).async_gsi;
}

/// What a missing, CREATING or DELETING table answers on the data plane.
[[nodiscard]] inline auto not_found() -> aws_json::error {
    return table_api_detail::modeled("ResourceNotFoundException", "Requested resource not found");
}

[[nodiscard]] inline auto flag(const boost::json::object& in, std::string_view member) -> bool {
    const auto* v = in.if_contains(member);
    return v != nullptr && v->is_bool() && v->as_bool();
}

[[nodiscard]] inline auto text(const boost::json::object& in, std::string_view member)
    -> std::string_view {
    const auto* v = in.if_contains(member);
    return v != nullptr && v->is_string() ? std::string_view(v->get_string()) : std::string_view();
}

/// The named table, if it can serve data-plane calls.
template<dynamo_backend Backend>
[[nodiscard]] auto usable_table(Backend& backend, std::string_view raw_name)
    -> aws_json::result<table_descriptor> {
    auto d = backend.describe(table_api_detail::table_name_of(raw_name));
    if (!d || d->status == table_status::creating || d->status == table_status::deleting) {
        return not_found();
    }
    return std::move(*d);
}

[[nodiscard]] inline auto wants_capacity(const boost::json::object& in) -> bool {
    const auto rc = text(in, "ReturnConsumedCapacity");
    return rc == "TOTAL" || rc == "INDEXES";
}

/// One table's `ConsumedCapacity` entry.
[[nodiscard]] inline auto capacity(const boost::json::object& in, const std::string& table,
                                   double units) -> boost::json::object {
    boost::json::object out{{"TableName", table}, {"CapacityUnits", units}};
    if (text(in, "ReturnConsumedCapacity") == "INDEXES") {
        out["Table"] = boost::json::object{{"CapacityUnits", units}};
    }
    return out;
}

/// One unit per 4 KB read (at least one), halved when eventually
/// consistent.
[[nodiscard]] inline auto read_units(std::size_t bytes, bool consistent) -> double {
    constexpr std::size_t k_read_unit_bytes = 4096;
    const auto units = static_cast<double>(
        std::max<std::size_t>(1, (bytes + k_read_unit_bytes - 1) / k_read_unit_bytes));
    return consistent ? units : units / 2.0;
}

/// `ItemCollectionMetrics` for a write to `table` whose key (or item) is
/// `key`, when the request asked for them and the table has local
/// secondary indexes. `SizeEstimateRangeGB` brackets the size between
/// whole gigabytes, as AWS's estimate does.
[[nodiscard]] inline auto collection_metrics(const boost::json::object& in,
                                             const table_descriptor& table, const item& key,
                                             const item_result& r)
    -> std::optional<boost::json::object> {
    if (table.lsis.empty() || !r.collection_bytes ||
        text(in, "ReturnItemCollectionMetrics") != "SIZE") {
        return std::nullopt;
    }
    const auto* pk = find_attribute(key, table.schema.partition_name);
    if (pk == nullptr) {
        return std::nullopt;
    }
    constexpr double k_gigabyte = 1024.0 * 1024.0 * 1024.0;
    const double low = std::floor(static_cast<double>(*r.collection_bytes) / k_gigabyte);
    return boost::json::object{
        {"ItemCollectionKey", item_to_json(item{{table.schema.partition_name, *pk}})},
        {"SizeEstimateRangeGB", boost::json::array{low, low + 1.0}},
    };
}

/// One unit per 1 KB written (at least one).
[[nodiscard]] inline auto write_units(std::size_t bytes) -> double {
    constexpr std::size_t k_write_unit_bytes = 1024;
    return static_cast<double>(
        std::max<std::size_t>(1, (bytes + k_write_unit_bytes - 1) / k_write_unit_bytes));
}

}  // namespace kythira::dynamodb::api_detail
