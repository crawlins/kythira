// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file api_common.hpp
/// @brief Helpers the item, query and batch handlers share
///        (`.kiro/specs/dynamodb-compatible-api/` tasks 7 and 8): reading
///        request members, finding a usable table, `ConsumedCapacity`
///        with AWS's formulas, and capacity admission (task 12).

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/capacity.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
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
#include <vector>

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
[[nodiscard]] auto usable_table(Backend& backend, const call_context& ctx,
                                std::string_view raw_name) -> aws_json::result<table_descriptor> {
    auto d = backend.describe(ctx.table_name(raw_name));
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
    boost::json::object out{{"TableName", call_context::bare_name(table)},
                            {"CapacityUnits", units}};
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

/// Whether capacity is enforced on `table`: the `throttling` switch, from
/// the cluster's profile and the table's `kythira:compat:throttling` tag.
[[nodiscard]] inline auto throttles(const dynamo_compat_profile& cluster,
                                    const table_descriptor& table) -> bool {
    return resolve_profile(cluster, table.tags).throttling;
}

/// The bytes a partition key's bucket is keyed by: the key attribute of
/// `schema` in `it`; empty when `it` lacks it.
[[nodiscard]] inline auto partition_of(const key_schema& schema, const item& it) -> std::string {
    const auto* pk = find_attribute(it, schema.partition_name);
    return pk == nullptr ? std::string{} : encode_item(item{{schema.partition_name, *pk}});
}

/// The bytes Contributor Insights counts a whole key by: the partition and
/// sort attributes of `schema` in `it`; empty when `schema` has no sort key
/// or `it` lacks either.
[[nodiscard]] inline auto key_of(const key_schema& schema, const item& it) -> std::string {
    if (!schema.sort_name) {
        return {};
    }
    const auto* pk = find_attribute(it, schema.partition_name);
    const auto* sk = find_attribute(it, *schema.sort_name);
    if (pk == nullptr || sk == nullptr) {
        return {};
    }
    return encode_item(item{{schema.partition_name, *pk}, {*schema.sort_name, *sk}});
}

/// A read target for one item of `table` (`GetItem`, a `BatchGetItem`
/// key, a transaction's `ConditionCheck`).
[[nodiscard]] inline auto read_target(const table_descriptor& table, const item& key)
    -> capacity_target {
    return {"", capacity_kind::read, partition_of(table.schema, key), key_of(table.schema, key)};
}

[[nodiscard]] inline auto insights_target_of(const capacity_target& t) -> insights_target {
    return {t.index, t.partition, t.key};
}

/// AWS's answer to a throttle: the exception with its `ThrottlingReasons`
/// (spelled `throttlingReasons` on `ThrottlingException`, as the model has
/// it), the resource being the table's or the index's ARN.
[[nodiscard]] inline auto throttle_error(const throttle& t, const std::string& table_arn)
    -> aws_json::error {
    auto e = table_api_detail::modeled(t.exception, t.message);
    const auto resource = t.index.empty() ? table_arn : table_arn + "/index/" + t.index;
    e.extra[t.exception == "ThrottlingException" ? "throttlingReasons" : "ThrottlingReasons"] =
        boost::json::array{boost::json::object{{"reason", t.reason}, {"resource", resource}}};
    return e;
}

/// Admits every target, or answers the first throttle; nothing when the
/// switch is off.
template<dynamo_backend Backend>
[[nodiscard]] auto admit(Backend& backend, const dynamo_compat_profile& cluster,
                         const call_context& ctx, const table_descriptor& table,
                         const std::vector<capacity_target>& targets)
    -> std::optional<aws_json::error> {
    if (!throttles(cluster, table)) {
        return std::nullopt;
    }
    const auto now = backend.now_ms();
    for (const auto& t : targets) {
        if (auto th = backend.throttler().admit(table, t, now)) {
            if (!table.insights.empty()) {
                backend.throttler().insights().throttled(table, insights_target_of(t), now);
            }
            return throttle_error(*th, ctx.table_arn(table.name));
        }
    }
    return std::nullopt;
}

/// Charges `units` to `target`, when the switch is on, and counts them for
/// Contributor Insights whether it is or not.
template<dynamo_backend Backend>
auto charge(Backend& backend, const dynamo_compat_profile& cluster, const table_descriptor& table,
            const capacity_target& target, double units) -> void {
    if (throttles(cluster, table)) {
        backend.throttler().charge(table, target, units, backend.now_ms());
    }
    if (!table.insights.empty()) {
        backend.throttler().insights().accessed(table, insights_target_of(target), units,
                                                backend.now_ms());
    }
}

/// The targets a write to `table` of `payload` draws on: the table, by the
/// item's partition key, and each GSI writes maintain that the write can
/// touch: for a put, the indexes whose partition key the item carries (by
/// that key); for an update or delete, every index, since only the item
/// group sees what the write changes. That over-counts an update that
/// leaves an index alone, and never under-counts.
[[nodiscard]] inline auto write_targets(const table_descriptor& table, const item& payload,
                                        item_command::kind op) -> std::vector<capacity_target> {
    std::vector<capacity_target> out{{"", capacity_kind::write, partition_of(table.schema, payload),
                                      key_of(table.schema, payload)}};
    for (const auto& g : maintained_indexes(table.gsis)) {
        auto partition = partition_of(g.schema, payload);
        if (op != item_command::kind::put || !partition.empty()) {
            out.push_back(
                {g.name, capacity_kind::write, std::move(partition), key_of(g.schema, payload)});
        }
    }
    return out;
}

/// Charges a write's `units` to every bucket it drew on.
template<dynamo_backend Backend>
auto charge_all(Backend& backend, const dynamo_compat_profile& cluster,
                const table_descriptor& table, const std::vector<capacity_target>& targets,
                double units) -> void {
    for (const auto& t : targets) {
        charge(backend, cluster, table, t, units);
    }
}

}  // namespace kythira::dynamodb::api_detail
