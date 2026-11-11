// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table_api.hpp
/// @brief The DynamoDB control plane: `CreateTable`, `DescribeTable`,
///        `ListTables`, `UpdateTable`, `DeleteTable`, `DescribeLimits` and the
///        tagging operations (`.kiro/specs/dynamodb-compatible-api/` task 7,
///        Requirement 7.1).
///
/// A handler checks its request the way DynamoDB does, with AWS's wording,
/// then sends one catalog command through the backend and renders the
/// descriptor that comes back as a `TableDescription`. DynamoDB Local
/// words several of these checks differently from AWS, and Kythira keeps
/// AWS's wording.
///
/// Features that later tasks add are refused outright with a message that
/// names them, rather than silently ignored: global secondary indexes
/// (task 11), streams (16), encryption settings, resource policies,
/// on-demand throughput and replicas (22).
///
/// Local secondary indexes (task 9) are checked with DynamoDB Local's
/// wording, which is the only recording there is, and fixed at creation.
///
/// Warm throughput is recorded and reported but limits nothing: Kythira
/// has no per-partition throughput ceiling for it to raise. A table shows
/// at least AWS's defaults for a new table, 12,000 read and 4,000 write
/// units per second, or its provisioned capacity when that is higher. A
/// request may raise the values but never lower them, as on AWS; the
/// message for a lower value is Kythira's, since it has not been recorded
/// against AWS.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// DescribeLimits' answer: AWS's default quotas.
inline constexpr std::uint64_t k_account_max_capacity_units = 80000;
inline constexpr std::uint64_t k_table_max_capacity_units = 40000;

namespace table_api_detail {

[[nodiscard]] inline auto modeled(std::string_view type, std::string message) -> aws_json::error {
    aws_json::error e;
    e.type = dynamodb_error_style().service_prefix + std::string(type);
    e.message = std::move(message);
    return e;
}

/// `ResourceInUseException` about an index, in DynamoDB Local's words.
[[nodiscard]] inline auto in_use_message(std::string_view what) -> aws_json::error {
    return modeled("ResourceInUseException",
                   "Attempt to change a resource which is still in use: " + std::string(what));
}

[[nodiscard]] inline auto invalid(std::string what) -> aws_json::error {
    return validation_error(std::string(k_invalid_parameters) + std::move(what));
}

[[nodiscard]] inline auto unsupported(std::string_view what) -> aws_json::error {
    return validation_error("Kythira does not support " + std::string(what) + " yet");
}

[[nodiscard]] inline auto view_type_name(stream_view_type v) -> std::string_view {
    switch (v) {
        case stream_view_type::keys_only:
            return "KEYS_ONLY";
        case stream_view_type::new_image:
            return "NEW_IMAGE";
        case stream_view_type::old_image:
            return "OLD_IMAGE";
        case stream_view_type::new_and_old_images:
            return "NEW_AND_OLD_IMAGES";
    }
    return "KEYS_ONLY";
}

[[nodiscard]] inline auto view_type_of(std::string_view name) -> std::optional<stream_view_type> {
    for (const auto v : {stream_view_type::keys_only, stream_view_type::new_image,
                         stream_view_type::old_image, stream_view_type::new_and_old_images}) {
        if (view_type_name(v) == name) {
            return v;
        }
    }
    return std::nullopt;
}

/// A stream's `StreamLabel`: its enable time, UTC, to the millisecond.
[[nodiscard]] inline auto stream_label(std::uint64_t ms) -> std::string {
    const auto secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::array<char, 32> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03u",
                                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                                 tm.tm_min, tm.tm_sec, static_cast<unsigned>(ms % 1000));
    return {buf.data(), static_cast<std::size_t>(n)};
}

/// `arn:...:table/<name>/stream/<label>`.
[[nodiscard]] inline auto stream_arn(const call_context& ctx, std::string_view table,
                                     std::uint64_t label_ms) -> std::string {
    return ctx.table_arn(table) + "/stream/" + stream_label(label_ms);
}

/// A `StreamSpecification`, checked in DynamoDB's words: the stream to
/// enable, or one with `disabled_ms` set to disable the table's.
[[nodiscard]] inline auto stream_spec_from(const boost::json::object& spec)
    -> aws_json::result<table_stream> {
    const auto* enabled = spec.if_contains("StreamEnabled");
    if (enabled == nullptr || !enabled->is_bool()) {
        return validation_error("Stream StreamEnabled was null");
    }
    const auto* view = spec.if_contains("StreamViewType");
    const bool has_view = view != nullptr && view->is_string() && !view->as_string().empty();
    if (enabled->as_bool()) {
        if (!has_view) {
            return validation_error(
                "Invalid StreamViewType: Stream ViewType must not be null or empty when enabling "
                "streams.");
        }
        const auto v = view_type_of(view->as_string());
        if (!v) {
            return validation_error("Invalid StreamViewType: " + std::string(view->as_string()));
        }
        return table_stream{0, *v, 0};
    }
    if (has_view) {
        return validation_error(
            "Invalid StreamViewType: Stream StreamEnabled was false but a StreamViewType was "
            "present.");
    }
    return table_stream{0, stream_view_type::keys_only, 1};
}

/// The error a catalog result carries.
[[nodiscard]] inline auto error_of(const catalog_result& r) -> aws_json::error {
    aws_json::error e;
    e.type = r.error_type;
    e.message = r.error_message;
    return e;
}

[[nodiscard]] inline auto valid_table_name(std::string_view name) -> bool {
    if (name.size() < 3 || name.size() > 255) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline auto bad_table_name() -> aws_json::error {
    return validation_error(
        "Invalid table/index name.  Table/index names must be between 3 and 255 characters long, "
        "and may contain only the characters a-z, A-Z, 0-9, '_', '-', and '.'");
}

/// A table name, or the name inside a table ARN (DynamoDB accepts either
/// in `TableName`).
[[nodiscard]] inline auto table_name_of(std::string_view given) -> std::string {
    if (given.starts_with("arn:")) {
        const auto slash = given.find(":table/");
        if (slash != std::string_view::npos) {
            auto rest = given.substr(slash + 7);
            return std::string(rest.substr(0, rest.find('/')));
        }
    }
    return std::string(given);
}

[[nodiscard]] inline auto type_letter(attribute_type t) -> std::string_view {
    switch (t) {
        case attribute_type::N:
            return "N";
        case attribute_type::B:
            return "B";
        default:
            return "S";
    }
}

[[nodiscard]] inline auto millis_to_seconds(std::uint64_t ms) -> double {
    // Exact while ms < 2^53, about 285,000 years.
    return static_cast<double>(ms) / 1000.0;
}

/// A UUID-shaped `TableId` derived from the table id, stable for a table's
/// life and never reused, as the id is.
[[nodiscard]] inline auto table_uuid(std::uint64_t id) -> std::string {
    std::array<char, 40> buf{};
    std::snprintf(buf.data(), buf.size(), "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(id & 0xFFFFFFFFFFFFULL));
    return std::string(buf.data());
}

[[nodiscard]] inline auto tags_json(const table_descriptor& d) -> boost::json::array {
    boost::json::array out;
    for (const auto& [k, v] : d.tags) {
        out.push_back(boost::json::object{{"Key", k}, {"Value", v}});
    }
    return out;
}

inline constexpr std::uint64_t k_default_warm_read = 12000;
inline constexpr std::uint64_t k_default_warm_write = 4000;

/// The warm throughput `DescribeTable` shows.
[[nodiscard]] inline auto warm_read_of(const table_descriptor& d) -> std::uint64_t {
    return std::max({k_default_warm_read, d.warm_read, d.read_capacity});
}
[[nodiscard]] inline auto warm_write_of(const table_descriptor& d) -> std::uint64_t {
    return std::max({k_default_warm_write, d.warm_write, d.write_capacity});
}

/// Reads `WarmThroughput` into `d`, refusing a value that is not positive
/// or is below what the table already shows.
[[nodiscard]] inline auto warm_from(const boost::json::object& in, table_descriptor& d)
    -> std::optional<aws_json::error> {
    const auto* w = in.if_contains("WarmThroughput");
    if (w == nullptr) {
        return std::nullopt;
    }
    const auto& o = w->as_object();
    if (!o.contains("ReadUnitsPerSecond") && !o.contains("WriteUnitsPerSecond")) {
        return invalid("WarmThroughput needs ReadUnitsPerSecond or WriteUnitsPerSecond");
    }
    for (const auto& [member, field, shown] :
         {std::tuple{"ReadUnitsPerSecond", &d.warm_read, warm_read_of(d)},
          std::tuple{"WriteUnitsPerSecond", &d.warm_write, warm_write_of(d)}}) {
        const auto* v = o.if_contains(member);
        if (v == nullptr) {
            continue;
        }
        const auto n = v->to_number<std::int64_t>();
        if (n <= 0) {
            return invalid(std::string("WarmThroughput ") + member + " must be positive");
        }
        if (static_cast<std::uint64_t>(n) < shown) {
            return invalid(std::string("WarmThroughput ") + member +
                           " cannot be lower than the table's current value of " +
                           std::to_string(shown));
        }
        *field = static_cast<std::uint64_t>(n);
    }
    return std::nullopt;
}

}  // namespace table_api_detail

/// A descriptor as a `TableDescription`.
/// `CreateTable`'s `LocalSecondaryIndexes`, checked in DynamoDB Local's
/// order and words. `type_of` gives an attribute's definition.
template<typename TypeOf>
[[nodiscard]] auto local_indexes_from(const boost::json::array& list, const std::string& table_hash,
                                      const std::string* table_range, const TypeOf& type_of)
    -> aws_json::result<std::vector<local_index>> {
    if (table_range == nullptr) {
        return validation_error(
            "Local Secondary indices are not allowed on hash tables, only hash and range tables");
    }
    if (list.size() > k_max_local_indexes) {
        return validation_error("Cannot have more than 5 local secondary indexes per table");
    }
    std::vector<local_index> out;
    std::size_t projected = 0;
    for (const auto& entry : list) {
        const auto& o = entry.as_object();
        local_index lsi;
        lsi.name = std::string(o.at("IndexName").as_string());
        lsi.number = static_cast<std::uint8_t>(out.size());
        if (!table_api_detail::valid_table_name(lsi.name)) {
            return table_api_detail::bad_table_name();
        }
        const auto& ks = o.at("KeySchema").as_array();
        std::vector<std::string> hash;
        std::vector<std::string> range;
        for (const auto& k : ks) {
            const auto& ko = k.as_object();
            (ko.at("KeyType").as_string() == "HASH" ? hash : range)
                .emplace_back(ko.at("AttributeName").as_string());
        }
        if (hash.empty()) {
            return validation_error(
                "No Hash Key specified in schema.  All Dynamo DB tables must have exactly one "
                "hash key");
        }
        if (hash.size() > 1) {
            return validation_error(
                "Too many hash keys specified.  All Dynamo DB tables must have exactly one hash "
                "key");
        }
        if (ks[0].as_object().at("KeyType").as_string() != "HASH") {
            return validation_error(
                "Invalid key order. Hash Key must be specified first in key schema, Range Key "
                "must be specifed second");
        }
        if (range.empty()) {
            return validation_error("Local secondary indices must have a range key");
        }
        if (hash[0] != table_hash) {
            return validation_error(
                "Local Secondary indices must have the same hash key as the main table");
        }
        if (range[0] == hash[0]) {
            return validation_error("Two keys can not have the same name");
        }
        const auto type = type_of(range[0]);
        if (!type) {
            return validation_error(
                "Local Secondary Index range key not specified in Attribute Definitions.Type "
                "unknown.");
        }
        lsi.sort_name = range[0];
        lsi.sort_type = *type;
        const auto& p = o.at("Projection").as_object();
        const auto* pt = p.if_contains("ProjectionType");
        const auto* nk = p.if_contains("NonKeyAttributes");
        if (pt == nullptr) {
            return validation_error("Indexes must have a projection specified");
        }
        const auto kind = pt->as_string();
        if (kind != "INCLUDE" && nk != nullptr) {
            return validation_error(
                "No attributes should be specified to be projected unless projection type is "
                "INCLUDE");
        }
        if (kind == "INCLUDE") {
            if (nk == nullptr || nk->as_array().empty()) {
                return validation_error(
                    "If Projection type INCLUDE is specified, some non-key attributes to include "
                    "in the projection must be specified as well");
            }
            for (const auto& a : nk->as_array()) {
                std::string name(a.as_string());
                if (std::find(lsi.non_key.begin(), lsi.non_key.end(), name) != lsi.non_key.end()) {
                    return validation_error("Cannot have two attributes with the same name");
                }
                lsi.non_key.push_back(std::move(name));
            }
            projected += lsi.non_key.size();
        }
        lsi.projection = kind == "ALL"       ? projection_kind::all
                         : kind == "INCLUDE" ? projection_kind::include
                                             : projection_kind::keys_only;
        out.push_back(std::move(lsi));
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (out[i].name == out[j].name) {
                return validation_error("Two local secondary indices have the same name");
            }
        }
    }
    if (projected > k_max_projected_attributes) {
        // DynamoDB Local's text; the limit it enforces is AWS's 100.
        return validation_error(
            "No more than 20 attributes per table can be projected into Local Secondary Indices");
    }
    return out;
}

/// `CreateTable`'s `GlobalSecondaryIndexes`, checked in DynamoDB Local's
/// order and words (ids are the catalog's to give). `type_of` gives an
/// attribute's definition.
template<typename TypeOf>
[[nodiscard]] auto global_indexes_from(const boost::json::array& list, const TypeOf& type_of)
    -> aws_json::result<std::vector<global_index>> {
    if (list.empty()) {
        return validation_error("GSI list is empty/invalid");
    }
    if (list.size() > k_max_global_indexes) {
        return validation_error("GlobalSecondaryIndex count exceeds the per-table limit");
    }
    std::vector<global_index> out;
    for (const auto& entry : list) {
        const auto& o = entry.as_object();
        global_index g;
        g.name = std::string(o.at("IndexName").as_string());
        if (!table_api_detail::valid_table_name(g.name)) {
            return table_api_detail::bad_table_name();
        }
        const auto& ks = o.at("KeySchema").as_array();
        std::vector<std::string> hash;
        std::vector<std::string> range;
        for (const auto& k : ks) {
            const auto& ko = k.as_object();
            (ko.at("KeyType").as_string() == "HASH" ? hash : range)
                .emplace_back(ko.at("AttributeName").as_string());
        }
        if (hash.empty()) {
            return validation_error(
                "No Hash Key specified in schema.  All Dynamo DB tables must have exactly one "
                "hash key");
        }
        if (hash.size() > 1) {
            return validation_error(
                "Too many hash keys specified.  All Dynamo DB tables must have exactly one hash "
                "key");
        }
        if (ks[0].as_object().at("KeyType").as_string() != "HASH") {
            return validation_error(
                "Invalid key order. Hash Key must be specified first in key schema, Range Key "
                "must be specifed second");
        }
        if (!range.empty() && range[0] == hash[0]) {
            return validation_error("Two keys can not have the same name");
        }
        for (const auto* name : {&hash[0], range.empty() ? nullptr : &range[0]}) {
            if (name != nullptr && !type_of(*name)) {
                return validation_error("Attribute " + *name +
                                        " is not defined in AttributeDefinitions");
            }
        }
        g.schema.partition_name = hash[0];
        g.schema.partition_type = *type_of(hash[0]);
        if (!range.empty()) {
            g.schema.sort_name = range[0];
            g.schema.sort_type = *type_of(range[0]);
        }
        const auto* proj = o.if_contains("Projection");
        if (proj == nullptr) {
            return validation_error("Indexes must have a projection specified");
        }
        // A projection without a type projects everything, as DynamoDB
        // Local takes it (an LSI's is refused).
        const auto* pt = proj->as_object().if_contains("ProjectionType");
        const auto* nk = proj->as_object().if_contains("NonKeyAttributes");
        const std::string_view kind = pt == nullptr ? std::string_view("ALL") : pt->as_string();
        if (kind != "INCLUDE" && nk != nullptr) {
            return validation_error(
                "No attributes should be specified to be projected unless projection type is "
                "INCLUDE");
        }
        if (kind == "INCLUDE") {
            if (nk == nullptr || nk->as_array().empty()) {
                return validation_error(
                    "If Projection type INCLUDE is specified, some non-key attributes to include "
                    "in the projection must be specified as well");
            }
            for (const auto& a : nk->as_array()) {
                std::string name(a.as_string());
                if (std::find(g.non_key.begin(), g.non_key.end(), name) != g.non_key.end()) {
                    return validation_error("Cannot have two attributes with the same name");
                }
                g.non_key.push_back(std::move(name));
            }
        }
        g.projection = kind == "ALL"       ? projection_kind::all
                       : kind == "INCLUDE" ? projection_kind::include
                                           : projection_kind::keys_only;
        if (const auto* t = o.if_contains("ProvisionedThroughput")) {
            const auto& to = t->as_object();
            g.has_throughput = true;
            g.read_capacity =
                static_cast<std::uint64_t>(to.at("ReadCapacityUnits").to_number<std::int64_t>());
            g.write_capacity =
                static_cast<std::uint64_t>(to.at("WriteCapacityUnits").to_number<std::int64_t>());
        }
        out.push_back(std::move(g));
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (out[i].name == out[j].name) {
                return validation_error("Two global secondary indices have the same name");
            }
        }
    }
    return out;
}

/// `GlobalSecondaryIndexes` in a table description, in DynamoDB Local's
/// shape (`ProvisionedThroughput` only when one was given).
[[nodiscard]] inline auto global_index_descriptions(const table_descriptor& d,
                                                    const call_context& ctx) -> boost::json::array {
    boost::json::array out;
    for (const auto& g : d.gsis) {
        boost::json::object projection{{"ProjectionType", projection_name(g.projection)}};
        if (g.projection == projection_kind::include) {
            boost::json::array names;
            for (const auto& n : g.non_key) {
                names.emplace_back(n);
            }
            projection["NonKeyAttributes"] = std::move(names);
        }
        boost::json::array keys{
            boost::json::object{{"AttributeName", g.schema.partition_name}, {"KeyType", "HASH"}}};
        if (g.schema.sort_name) {
            keys.push_back(
                boost::json::object{{"AttributeName", *g.schema.sort_name}, {"KeyType", "RANGE"}});
        }
        boost::json::object o{
            {"IndexName", g.name},
            {"KeySchema", std::move(keys)},
            {"Projection", std::move(projection)},
            {"IndexStatus", index_status_name(g.status)},
        };
        if (g.has_throughput) {
            o["ProvisionedThroughput"] = boost::json::object{
                {"ReadCapacityUnits", g.read_capacity},
                {"WriteCapacityUnits", g.write_capacity},
            };
        }
        // `Backfilling` while CREATING (true, as AWS shows it; DynamoDB
        // Local shows false), the counts only once ACTIVE, as in Local.
        if (g.status == index_status::creating) {
            o["Backfilling"] = true;
        } else if (g.status == index_status::active) {
            o["IndexSizeBytes"] = 0;
            o["ItemCount"] = 0;
        }
        o["IndexArn"] = ctx.table_arn(d.name) + "/index/" + g.name;
        out.push_back(std::move(o));
    }
    return out;
}

/// `LocalSecondaryIndexes` in a table description.
[[nodiscard]] inline auto local_index_descriptions(const table_descriptor& d,
                                                   const call_context& ctx) -> boost::json::array {
    boost::json::array out;
    for (const auto& lsi : d.lsis) {
        boost::json::object projection{{"ProjectionType", projection_name(lsi.projection)}};
        if (lsi.projection == projection_kind::include) {
            boost::json::array names;
            for (const auto& n : lsi.non_key) {
                names.emplace_back(n);
            }
            projection["NonKeyAttributes"] = std::move(names);
        }
        out.push_back(boost::json::object{
            {"IndexName", lsi.name},
            {"KeySchema",
             boost::json::array{
                 boost::json::object{{"AttributeName", d.schema.partition_name},
                                     {"KeyType", "HASH"}},
                 boost::json::object{{"AttributeName", lsi.sort_name}, {"KeyType", "RANGE"}}}},
            {"Projection", std::move(projection)},
            {"IndexSizeBytes", 0},
            {"ItemCount", 0},
            {"IndexArn", ctx.table_arn(d.name) + "/index/" + lsi.name},
        });
    }
    return out;
}

[[nodiscard]] inline auto table_description(const table_descriptor& d, const call_context& ctx)
    -> boost::json::object {
    using namespace table_api_detail;
    boost::json::array defs;
    boost::json::array keys;
    defs.push_back(boost::json::object{{"AttributeName", d.schema.partition_name},
                                       {"AttributeType", type_letter(d.schema.partition_type)}});
    keys.push_back(
        boost::json::object{{"AttributeName", d.schema.partition_name}, {"KeyType", "HASH"}});
    if (d.schema.sort_name) {
        defs.push_back(boost::json::object{{"AttributeName", *d.schema.sort_name},
                                           {"AttributeType", type_letter(d.schema.sort_type)}});
        keys.push_back(
            boost::json::object{{"AttributeName", *d.schema.sort_name}, {"KeyType", "RANGE"}});
    }
    for (std::size_t i = 0; i < d.lsis.size(); ++i) {
        const auto& lsi = d.lsis[i];
        bool seen = lsi.sort_name == d.schema.sort_name;
        for (std::size_t j = 0; j < i; ++j) {
            seen = seen || d.lsis[j].sort_name == lsi.sort_name;
        }
        if (!seen) {
            defs.push_back(boost::json::object{{"AttributeName", lsi.sort_name},
                                               {"AttributeType", type_letter(lsi.sort_type)}});
        }
    }
    const auto defined = [&](const std::string& name) {
        for (const auto& def : defs) {
            if (def.as_object().at("AttributeName").as_string() == name) {
                return true;
            }
        }
        return false;
    };
    // An index being deleted no longer defines its keys, as in DynamoDB
    // Local.
    for (const auto& g : maintained_indexes(d.gsis)) {
        if (!defined(g.schema.partition_name)) {
            defs.push_back(
                boost::json::object{{"AttributeName", g.schema.partition_name},
                                    {"AttributeType", type_letter(g.schema.partition_type)}});
        }
        if (g.schema.sort_name && !defined(*g.schema.sort_name)) {
            defs.push_back(boost::json::object{{"AttributeName", *g.schema.sort_name},
                                               {"AttributeType", type_letter(g.schema.sort_type)}});
        }
    }
    boost::json::object out;
    out["AttributeDefinitions"] = std::move(defs);
    out["TableName"] = d.name;
    out["KeySchema"] = std::move(keys);
    out["TableStatus"] = status_name(d.status);
    out["CreationDateTime"] = millis_to_seconds(d.created_ms);
    out["ProvisionedThroughput"] = boost::json::object{
        {"NumberOfDecreasesToday", 0},
        {"ReadCapacityUnits", d.read_capacity},
        {"WriteCapacityUnits", d.write_capacity},
    };
    out["TableSizeBytes"] = 0;
    out["ItemCount"] = 0;
    out["TableArn"] = ctx.table_arn(d.name);
    out["TableId"] = table_uuid(d.table_id);
    if (d.billing == billing_mode::pay_per_request) {
        out["BillingModeSummary"] = boost::json::object{{"BillingMode", "PAY_PER_REQUEST"}};
    }
    if (d.storage_class == table_class::standard_infrequent_access) {
        out["TableClassSummary"] =
            boost::json::object{{"TableClass", "STANDARD_INFREQUENT_ACCESS"}};
    }
    out["DeletionProtectionEnabled"] = d.deletion_protection;
    if (!d.lsis.empty()) {
        out["LocalSecondaryIndexes"] = local_index_descriptions(d, ctx);
    }
    if (!d.gsis.empty()) {
        out["GlobalSecondaryIndexes"] = global_index_descriptions(d, ctx);
    }
    out["WarmThroughput"] = boost::json::object{
        {"ReadUnitsPerSecond", warm_read_of(d)},
        {"WriteUnitsPerSecond", warm_write_of(d)},
        {"Status", status_name(d.status)},
    };
    // A disabled stream drops the specification but stays the latest.
    if (const auto* s = d.enabled_stream()) {
        out["StreamSpecification"] = boost::json::object{
            {"StreamEnabled", true}, {"StreamViewType", view_type_name(s->view)}};
    }
    if (!d.streams.empty()) {
        out["LatestStreamArn"] = stream_arn(ctx, d.name, d.streams.back().label_ms);
        out["LatestStreamLabel"] = stream_label(d.streams.back().label_ms);
    }
    if (!d.restore.source_backup_arn.empty()) {
        out["RestoreSummary"] = boost::json::object{
            {"SourceBackupArn", d.restore.source_backup_arn},
            {"SourceTableArn", d.restore.source_table_arn},
            {"RestoreDateTime", millis_to_seconds(d.restore.restore_ms)},
            {"RestoreInProgress", d.restore.in_progress},
        };
    }
    return out;
}

/// The control-plane handlers over a backend.
template<dynamo_backend Backend> class table_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit table_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("CreateTable",
              [this](call_context& c, boost::json::object& in) { return create_table(c, in); });
        fe.on("DescribeTable",
              [this](call_context& c, boost::json::object& in) { return describe_table(c, in); });
        fe.on("ListTables",
              [this](call_context& c, boost::json::object& in) { return list_tables(c, in); });
        fe.on("UpdateTable",
              [this](call_context& c, boost::json::object& in) { return update_table(c, in); });
        fe.on("DeleteTable",
              [this](call_context& c, boost::json::object& in) { return delete_table(c, in); });
        fe.on("DescribeLimits",
              [this](call_context& c, boost::json::object& in) { return describe_limits(c, in); });
        fe.on("TagResource",
              [this](call_context& c, boost::json::object& in) { return tag_resource(c, in); });
        fe.on("UntagResource",
              [this](call_context& c, boost::json::object& in) { return untag_resource(c, in); });
        fe.on("ListTagsOfResource", [this](call_context& c, boost::json::object& in) {
            return list_tags_of_resource(c, in);
        });
    }

    auto create_table(call_context& ctx, boost::json::object& in) -> result {
        using namespace table_api_detail;
        const auto name = table_name_of(in.at("TableName").as_string());
        if (!valid_table_name(name)) {
            return bad_table_name();
        }
        // VectorIndexes come with task 26; refusing them beats creating a
        // table without the index the caller asked for.
        for (const char* member :
             {"SSESpecification", "ResourcePolicy", "OnDemandThroughput", "VectorIndexes"}) {
            if (in.contains(member)) {
                return unsupported(member);
            }
        }
        std::optional<table_stream> stream;
        if (const auto* s = in.if_contains("StreamSpecification")) {
            auto spec = stream_spec_from(s->as_object());
            if (!spec) {
                return spec.error();
            }
            if (spec->disabled_ms == 0) {
                stream = *spec;
            }
        }

        // Key schema, in DynamoDB's order of checks.
        const auto& ks = in.at("KeySchema").as_array();
        std::vector<std::string> hash;
        std::vector<std::string> range;
        for (const auto& k : ks) {
            const auto& o = k.as_object();
            (o.at("KeyType").as_string() == "HASH" ? hash : range)
                .emplace_back(o.at("AttributeName").as_string());
        }
        if (hash.empty()) {
            return validation_error(
                "No Hash Key specified in schema.  All Dynamo DB tables must "
                "have exactly one hash key");
        }
        if (hash.size() > 1) {
            return validation_error(
                "Too many hash keys specified.  All Dynamo DB tables must "
                "have exactly one hash key");
        }
        if (ks[0].as_object().at("KeyType").as_string() != "HASH") {
            return validation_error(
                "Invalid key order. Hash Key must be specified first in key "
                "schema, Range Key must be specifed second");
        }
        if (!range.empty() && range[0] == hash[0]) {
            return validation_error("Two keys can not have the same name");
        }

        // Attribute definitions: exactly the key attributes.
        std::vector<std::pair<std::string, attribute_type>> defs;
        for (const auto& a : in.at("AttributeDefinitions").as_array()) {
            const auto& o = a.as_object();
            const std::string attr(o.at("AttributeName").as_string());
            for (const auto& [seen, unused] : defs) {
                if (seen == attr) {
                    return validation_error("Cannot have two attributes with the same name");
                }
            }
            const auto t = o.at("AttributeType").as_string();
            defs.emplace_back(attr, t == "N"   ? attribute_type::N
                                    : t == "B" ? attribute_type::B
                                               : attribute_type::S);
        }
        const auto type_of = [&](const std::string& attr) -> std::optional<attribute_type> {
            for (const auto& [n, t] : defs) {
                if (n == attr) {
                    return t;
                }
            }
            return std::nullopt;
        };
        std::vector<std::string> key_names = hash;
        key_names.insert(key_names.end(), range.begin(), range.end());
        for (const auto& k : key_names) {
            if (!type_of(k)) {
                std::string keys;
                std::string attrs;
                for (const auto& n : key_names) {
                    keys += (keys.empty() ? "" : ", ") + n;
                }
                for (const auto& [n, t] : defs) {
                    attrs += (attrs.empty() ? "" : ", ") + n;
                }
                return invalid(
                    "Some index key attributes are not defined in "
                    "AttributeDefinitions. Keys: [" +
                    keys + "], AttributeDefinitions: [" + attrs + "]");
            }
        }
        std::vector<local_index> lsis;
        const auto* lsi_list = in.if_contains("LocalSecondaryIndexes");
        if (lsi_list != nullptr) {
            auto parsed = local_indexes_from(lsi_list->as_array(), hash[0],
                                             range.empty() ? nullptr : &range[0], type_of);
            if (!parsed) {
                return parsed.error();
            }
            lsis = std::move(*parsed);
            for (const auto& lsi : lsis) {
                if (std::find(key_names.begin(), key_names.end(), lsi.sort_name) ==
                    key_names.end()) {
                    key_names.push_back(lsi.sort_name);
                }
            }
        }
        std::vector<global_index> gsis;
        const auto* gsi_list = in.if_contains("GlobalSecondaryIndexes");
        if (gsi_list != nullptr) {
            auto parsed = global_indexes_from(gsi_list->as_array(), type_of);
            if (!parsed) {
                return parsed.error();
            }
            gsis = std::move(*parsed);
            for (const auto& g : gsis) {
                for (const auto& l : lsis) {
                    if (l.name == g.name) {
                        return validation_error(
                            "Local Secondary Indices AND Global Secondary Indices must have "
                            "different names");
                    }
                }
                for (const auto* n : {&g.schema.partition_name,
                                      g.schema.sort_name ? &*g.schema.sort_name : nullptr}) {
                    if (n != nullptr &&
                        std::find(key_names.begin(), key_names.end(), *n) == key_names.end()) {
                        key_names.push_back(*n);
                    }
                }
            }
        }
        if (defs.size() != key_names.size()) {
            if (lsi_list != nullptr || gsi_list != nullptr) {
                return validation_error(
                    "The number of attributes in key schema must match the number of attributes "
                    "defined in attribute definitions.");
            }
            return invalid(
                "Number of attributes in KeySchema does not exactly match number of "
                "attributes defined in AttributeDefinitions");
        }

        table_descriptor d;
        d.name = name;
        d.schema.partition_name = hash[0];
        d.schema.partition_type = *type_of(hash[0]);
        if (!range.empty()) {
            d.schema.sort_name = range[0];
            d.schema.sort_type = *type_of(range[0]);
        }
        d.lsis = std::move(lsis);
        d.gsis = std::move(gsis);
        if (auto e = billing_from(in, d, /*creating=*/true)) {
            return *e;
        }
        if (d.billing == billing_mode::provisioned) {
            for (const auto& g : d.gsis) {
                if (!g.has_throughput) {
                    return validation_error(
                        "No provisioned throughput specified for the global secondary index");
                }
            }
        }
        if (const auto* cls = in.if_contains("TableClass")) {
            d.storage_class = cls->as_string() == "STANDARD_INFREQUENT_ACCESS"
                                  ? table_class::standard_infrequent_access
                                  : table_class::standard;
        }
        if (const auto* dp = in.if_contains("DeletionProtectionEnabled")) {
            d.deletion_protection = dp->as_bool();
        }
        if (auto e = warm_from(in, d)) {
            return *e;
        }
        if (const auto* tags = in.if_contains("Tags")) {
            for (const auto& t : tags->as_array()) {
                const auto& o = t.as_object();
                d.tags.emplace_back(o.at("Key").as_string(), o.at("Value").as_string());
            }
        }

        catalog_command c;
        c.op = catalog_command::kind::create_table;
        if (stream) {
            d.streams = {*stream};
        }
        c.table = std::move(d);
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return error_of(r);
        }
        return boost::json::object{{"TableDescription", table_description(*r.table, ctx)}};
    }

    auto describe_table(call_context& ctx, boost::json::object& in) -> result {
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        const auto d = _backend.describe(name);
        if (!d) {
            return not_found(name);
        }
        return boost::json::object{{"Table", table_description(*d, ctx)}};
    }

    auto list_tables(call_context& /*ctx*/, boost::json::object& in) -> result {
        std::size_t limit = 100;
        if (const auto* l = in.if_contains("Limit")) {
            limit = static_cast<std::size_t>(l->to_number<std::int64_t>());
        }
        std::string start;
        if (const auto* s = in.if_contains("ExclusiveStartTableName")) {
            start = table_api_detail::table_name_of(s->as_string());
        }
        // One more than asked tells whether to hand back a cursor.
        auto names = _backend.list_tables(start, limit + 1);
        const bool more = names.size() > limit;
        if (more) {
            names.resize(limit);
        }
        boost::json::array out;
        for (const auto& n : names) {
            out.emplace_back(n);
        }
        boost::json::object r{{"TableNames", std::move(out)}};
        if (more) {
            r["LastEvaluatedTableName"] = names.back();
        }
        return r;
    }

    auto update_table(call_context& ctx, boost::json::object& in) -> result {
        using namespace table_api_detail;
        const auto name = table_name_of(in.at("TableName").as_string());
        for (const char* member :
             {"SSESpecification", "ReplicaUpdates", "OnDemandThroughput", "MultiRegionConsistency",
              "GlobalTableWitnessUpdates", "VectorIndexUpdates"}) {
            if (in.contains(member)) {
                return unsupported(member);
            }
        }
        if (!in.contains("BillingMode") && !in.contains("ProvisionedThroughput") &&
            !in.contains("TableClass") && !in.contains("DeletionProtectionEnabled") &&
            !in.contains("WarmThroughput") && !in.contains("GlobalSecondaryIndexUpdates") &&
            !in.contains("StreamSpecification")) {
            return validation_error(
                "At least one of ProvisionedThroughput, BillingMode, UpdateStreamEnabled, "
                "GlobalSecondaryIndexUpdates or SSESpecification or ReplicaUpdates is required");
        }
        std::optional<table_stream> stream;
        if (const auto* s = in.if_contains("StreamSpecification")) {
            auto spec = stream_spec_from(s->as_object());
            if (!spec) {
                return spec.error();
            }
            if (in.contains("BillingMode") || in.contains("ProvisionedThroughput")) {
                return validation_error("Cannot change IOPS and Streams at the same time.");
            }
            stream = *spec;
        }
        const auto current = _backend.describe(name);
        if (!current) {
            return not_found(name);
        }
        catalog_command c;
        c.op = catalog_command::kind::update_table;
        c.table = *current;
        if (stream) {
            c.table.streams = {*stream};
            c.update_fields |= catalog_command::update_stream;
        }
        if (in.contains("BillingMode") || in.contains("ProvisionedThroughput")) {
            if (auto e = billing_from(in, c.table, /*creating=*/false)) {
                return *e;
            }
            if (c.table.billing == billing_mode::provisioned &&
                current->billing == billing_mode::provisioned &&
                c.table.read_capacity == current->read_capacity &&
                c.table.write_capacity == current->write_capacity) {
                return validation_error(
                    "The provisioned throughput for the table will not change. The requested "
                    "value equals the current value. Current ReadCapacityUnits provisioned for "
                    "the table: " +
                    std::to_string(current->read_capacity) +
                    ". Requested ReadCapacityUnits: " + std::to_string(c.table.read_capacity) +
                    ". Current WriteCapacityUnits provisioned for the table: " +
                    std::to_string(current->write_capacity) +
                    ". Requested WriteCapacityUnits: " + std::to_string(c.table.write_capacity) +
                    ". Refer to the Amazon DynamoDB Developer Guide for current limits and how "
                    "to request higher limits.");
            }
            c.update_fields |= catalog_command::update_billing | catalog_command::update_throughput;
        }
        if (const auto* cls = in.if_contains("TableClass")) {
            c.table.storage_class = cls->as_string() == "STANDARD_INFREQUENT_ACCESS"
                                        ? table_class::standard_infrequent_access
                                        : table_class::standard;
            c.update_fields |= catalog_command::update_class;
        }
        if (const auto* dp = in.if_contains("DeletionProtectionEnabled")) {
            c.table.deletion_protection = dp->as_bool();
            c.update_fields |= catalog_command::update_deletion_protection;
        }
        if (in.contains("WarmThroughput")) {
            // Checked against the current values, before any billing
            // change in the same request raises them.
            table_descriptor before = *current;
            if (auto e = warm_from(in, before)) {
                return *e;
            }
            c.table.warm_read = before.warm_read;
            c.table.warm_write = before.warm_write;
            c.update_fields |= catalog_command::update_warm_throughput;
        }
        if (const auto* updates = in.if_contains("GlobalSecondaryIndexUpdates")) {
            auto list = global_index_updates(in, updates->as_array(), c.table);
            if (!list) {
                return list.error();
            }
            c.table.gsis = std::move(*list);
            c.update_fields |= catalog_command::update_gsis;
        }
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return error_of(r);
        }
        return boost::json::object{{"TableDescription", table_description(*r.table, ctx)}};
    }

    /// `UpdateTable`'s `GlobalSecondaryIndexUpdates` against `table` (with
    /// any billing change of the same request applied): the index list the
    /// table should have, checked in DynamoDB Local's order and words. One
    /// index can be created or deleted at a time, and none while another
    /// is still being created or deleted.
    static auto global_index_updates(const boost::json::object& in,
                                     const boost::json::array& updates,
                                     const table_descriptor& table)
        -> aws_json::result<std::vector<global_index>> {
        using table_api_detail::in_use_message;
        if (updates.empty()) {
            return validation_error("Nothing to update");
        }
        std::vector<std::string> named;
        std::size_t creates_or_deletes = 0;
        for (const auto& u : updates) {
            const auto& o = u.as_object();
            const char* action = o.contains("Create")   ? "Create"
                                 : o.contains("Delete") ? "Delete"
                                 : o.contains("Update") ? "Update"
                                                        : nullptr;
            if (action == nullptr) {
                return validation_error("Update global secondary index action is null");
            }
            std::string name(o.at(action).as_object().at("IndexName").as_string());
            if (std::find(named.begin(), named.end(), name) != named.end()) {
                return validation_error(
                    "Only one global secondary index update per index is allowed "
                    "simultaneously.");
            }
            named.push_back(std::move(name));
            creates_or_deletes += std::string_view(action) == "Update" ? 0 : 1;
        }
        const auto limit = [] {
            return table_api_detail::modeled(
                "LimitExceededException",
                "Subscriber limit exceeded: Only 1 online index can be created or deleted "
                "simultaneously per table");
        };
        if (creates_or_deletes > 1) {
            return limit();
        }
        const auto find = [&](const std::string& name) -> const global_index* {
            for (const auto& g : table.gsis) {
                if (g.name == name) {
                    return &g;
                }
            }
            return nullptr;
        };
        const global_index* busy = nullptr;
        for (const auto& g : table.gsis) {
            if (g.status != index_status::active) {
                busy = &g;
            }
        }
        // Every attribute the table defines now, for a new index's keys.
        const auto defined = [&](const std::string& attr) -> std::optional<attribute_type> {
            if (attr == table.schema.partition_name) {
                return table.schema.partition_type;
            }
            if (table.schema.sort_name && attr == *table.schema.sort_name) {
                return table.schema.sort_type;
            }
            for (const auto& l : table.lsis) {
                if (l.sort_name == attr) {
                    return l.sort_type;
                }
            }
            for (const auto& g : maintained_indexes(table.gsis)) {
                if (g.schema.partition_name == attr) {
                    return g.schema.partition_type;
                }
                if (g.schema.sort_name && *g.schema.sort_name == attr) {
                    return g.schema.sort_type;
                }
            }
            return std::nullopt;
        };
        std::vector<std::pair<std::string, attribute_type>> request_defs;
        if (const auto* defs = in.if_contains("AttributeDefinitions")) {
            for (const auto& a : defs->as_array()) {
                const auto& o = a.as_object();
                const auto t = o.at("AttributeType").as_string();
                request_defs.emplace_back(std::string(o.at("AttributeName").as_string()),
                                          t == "N"   ? attribute_type::N
                                          : t == "B" ? attribute_type::B
                                                     : attribute_type::S);
            }
        }
        std::vector<global_index> out = table.gsis;
        for (const auto& u : updates) {
            const auto& o = u.as_object();
            if (const auto* create = o.if_contains("Create")) {
                const auto& co = create->as_object();
                const std::string name(co.at("IndexName").as_string());
                if (busy != nullptr) {
                    if (busy->name == name) {
                        return in_use_message(busy->status == index_status::creating
                                                  ? "Index is being created."
                                                  : "Index is being deleted.");
                    }
                    return limit();
                }
                if (find(name) != nullptr) {
                    return validation_error("Attempting to create an index which already exists");
                }
                for (const auto& l : table.lsis) {
                    if (l.name == name) {
                        return validation_error(
                            "Local Secondary Indices AND Global Secondary Indices must have "
                            "different names");
                    }
                }
                // Each key of the new index: from the request's definitions,
                // which must agree with the table's own.
                std::optional<std::string> undefined;
                for (const auto& k : co.at("KeySchema").as_array()) {
                    const std::string attr(k.as_object().at("AttributeName").as_string());
                    const auto it = std::find_if(request_defs.begin(), request_defs.end(),
                                                 [&](const auto& d) { return d.first == attr; });
                    const auto known = defined(attr);
                    if (it != request_defs.end() && known && *known != it->second) {
                        return validation_error(
                            "invalid attribute, SchemaElement redefined should be of same type "
                            "as previously defined");
                    }
                    if (it == request_defs.end() && !known) {
                        undefined = attr;
                    }
                }
                if (undefined) {
                    return validation_error("No Attribute Schema Defined");
                }
                const auto type_of = [&](const std::string& attr) -> std::optional<attribute_type> {
                    for (const auto& [n, t] : request_defs) {
                        if (n == attr) {
                            return t;
                        }
                    }
                    return defined(attr);
                };
                auto parsed = global_indexes_from(boost::json::array{co}, type_of);
                if (!parsed) {
                    return parsed.error();
                }
                if (table.billing == billing_mode::provisioned && !parsed->front().has_throughput) {
                    return validation_error(
                        "No provisioned throughput specified for the global secondary index");
                }
                if (maintained_indexes(table.gsis).size() >= k_max_global_indexes) {
                    return validation_error(
                        "GlobalSecondaryIndex count exceeds the per-table limit");
                }
                parsed->front().status = index_status::creating;
                out.push_back(std::move(parsed->front()));
                continue;
            }
            const bool deleting = o.contains("Delete");
            const auto& body = o.at(deleting ? "Delete" : "Update").as_object();
            const std::string name(body.at("IndexName").as_string());
            if (busy != nullptr) {
                if (busy->name == name) {
                    if (busy->status == index_status::deleting) {
                        return in_use_message("Index is being deleted.");
                    }
                    return in_use_message(deleting ? "Index is being created."
                                                   : "Index is being created but not "
                                                     "backfilling.");
                }
                if (deleting) {
                    return limit();
                }
            }
            const auto* g = find(name);
            if (g == nullptr) {
                return table_api_detail::modeled("ResourceNotFoundException",
                                                 "Requested resource not found");
            }
            const auto at = std::find_if(out.begin(), out.end(),
                                         [&](const global_index& x) { return x.name == name; });
            if (deleting) {
                out.erase(at);
                continue;
            }
            // Update: the index's provisioned throughput.
            const auto* t = body.if_contains("ProvisionedThroughput");
            if (t == nullptr || table.billing != billing_mode::provisioned) {
                return validation_error(
                    "One or more parameter values were invalid: ProvisionedThroughput can only "
                    "be updated on an index of a table in PROVISIONED billing mode");
            }
            at->has_throughput = true;
            at->read_capacity = static_cast<std::uint64_t>(
                t->as_object().at("ReadCapacityUnits").to_number<std::int64_t>());
            at->write_capacity = static_cast<std::uint64_t>(
                t->as_object().at("WriteCapacityUnits").to_number<std::int64_t>());
        }
        return out;
    }

    auto delete_table(call_context& ctx, boost::json::object& in) -> result {
        catalog_command c;
        c.op = catalog_command::kind::delete_table;
        c.table.name = table_api_detail::table_name_of(in.at("TableName").as_string());
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return table_api_detail::error_of(r);
        }
        return boost::json::object{{"TableDescription", table_description(*r.table, ctx)}};
    }

    auto describe_limits(call_context& /*ctx*/, boost::json::object& /*in*/) -> result {
        return boost::json::object{
            {"AccountMaxReadCapacityUnits", k_account_max_capacity_units},
            {"AccountMaxWriteCapacityUnits", k_account_max_capacity_units},
            {"TableMaxReadCapacityUnits", k_table_max_capacity_units},
            {"TableMaxWriteCapacityUnits", k_table_max_capacity_units},
        };
    }

    auto tag_resource(call_context& ctx, boost::json::object& in) -> result {
        catalog_command c;
        c.op = catalog_command::kind::tag_resource;
        for (const auto& t : in.at("Tags").as_array()) {
            const auto& o = t.as_object();
            c.table.tags.emplace_back(o.at("Key").as_string(), o.at("Value").as_string());
        }
        return retag(ctx, in, std::move(c));
    }

    auto untag_resource(call_context& ctx, boost::json::object& in) -> result {
        catalog_command c;
        c.op = catalog_command::kind::untag_resource;
        for (const auto& k : in.at("TagKeys").as_array()) {
            c.table.tags.emplace_back(k.as_string(), "");
        }
        return retag(ctx, in, std::move(c));
    }

    auto list_tags_of_resource(call_context& ctx, boost::json::object& in) -> result {
        const std::string arn(in.at("ResourceArn").as_string());
        auto name = table_of_arn(ctx, arn);
        if (!name) {
            return name.error();
        }
        const auto d = _backend.describe(*name);
        if (!d || d->status == table_status::deleting) {
            return arn_not_found(arn);
        }
        return boost::json::object{{"Tags", table_api_detail::tags_json(*d)}};
    }

private:
    static auto not_found(std::string_view name) -> aws_json::error {
        return table_api_detail::modeled(
            "ResourceNotFoundException",
            "Requested resource not found: Table: " + std::string(name) + " not found");
    }

    // AWS spells it "ResourcArn" in this message.
    static auto arn_not_found(std::string_view arn) -> aws_json::error {
        return table_api_detail::modeled(
            "ResourceNotFoundException",
            "Requested resource not found: ResourcArn: " + std::string(arn) + " not found");
    }

    /// The table a `ResourceArn` names in this account and region.
    static auto table_of_arn(const call_context& ctx, const std::string& arn)
        -> aws_json::result<std::string> {
        const auto name = table_api_detail::table_name_of(arn);
        if (!arn.starts_with("arn:") || name == arn || !table_api_detail::valid_table_name(name)) {
            return validation_error("Invalid TableArn: Invalid ResourceArn provided as input " +
                                    arn);
        }
        if (ctx.table_arn(name) != arn) {
            return arn_not_found(arn);
        }
        return name;
    }

    auto retag(call_context& ctx, boost::json::object& in, catalog_command c) -> result {
        const std::string arn(in.at("ResourceArn").as_string());
        auto name = table_of_arn(ctx, arn);
        if (!name) {
            return name.error();
        }
        c.table.name = *name;
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            if (r.error_type.ends_with("ResourceNotFoundException")) {
                return arn_not_found(arn);
            }
            return table_api_detail::error_of(r);
        }
        return boost::json::object{};
    }

    /// Billing mode and throughput from a request onto `d`.
    static auto billing_from(const boost::json::object& in, table_descriptor& d, bool creating)
        -> std::optional<aws_json::error> {
        using namespace table_api_detail;
        if (const auto* m = in.if_contains("BillingMode")) {
            d.billing = m->as_string() == "PAY_PER_REQUEST" ? billing_mode::pay_per_request
                                                            : billing_mode::provisioned;
        } else if (creating) {
            d.billing = billing_mode::provisioned;
        }
        const auto* pt = in.if_contains("ProvisionedThroughput");
        if (d.billing == billing_mode::pay_per_request) {
            if (pt != nullptr) {
                return invalid(
                    "Neither ReadCapacityUnits nor WriteCapacityUnits can be "
                    "specified when BillingMode is PAY_PER_REQUEST");
            }
            d.read_capacity = 0;
            d.write_capacity = 0;
            return std::nullopt;
        }
        if (pt == nullptr) {
            if (creating || d.read_capacity == 0) {
                return invalid(
                    "ReadCapacityUnits and WriteCapacityUnits must both be specified "
                    "when BillingMode is PROVISIONED");
            }
            return std::nullopt;
        }
        const auto& o = pt->as_object();
        d.read_capacity =
            static_cast<std::uint64_t>(o.at("ReadCapacityUnits").to_number<std::int64_t>());
        d.write_capacity =
            static_cast<std::uint64_t>(o.at("WriteCapacityUnits").to_number<std::int64_t>());
        return std::nullopt;
    }

    Backend& _backend;
};

}  // namespace kythira::dynamodb
