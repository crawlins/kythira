// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file query_api.hpp
/// @brief `Query` and `Scan` (`.kiro/specs/dynamodb-compatible-api/`
///        task 8, Requirements 9.1-9.3).
///
/// Both are range reads through `dynamo_backend::read_range`:
///
/// - `Query` reads one item collection. The partition key picks the
///   collection's prefix (`partition_prefix`), and the sort-key condition
///   narrows it to a sub-range, since the sort-key encoding preserves
///   order: `sk < v` is `[collection, collection + enc(v))`, and
///   `begins_with` is the prefix range of the escaped bytes. Each item read
///   is checked against the key condition again before it counts.
/// - `Scan` reads the table, or one segment of it. Keys lead with a 2-byte
///   hash of the partition key, so segment `i` of `n` is the slice of
///   hashes `[i * 65536 / n, (i + 1) * 65536 / n)`. Segments are balanced
///   whatever the key distribution, and a cursor is a key, which stays
///   valid across splits and leader changes.
///
/// Paging follows AWS: a page ends after `Limit` items are read (before
/// the filter), or before the item that would take the page past 1 MB of
/// item data. Either way `LastEvaluatedKey` names the last item read, even
/// when no item follows it. `ExclusiveStartKey` resumes after that key,
/// whether or not an item still has it.
///
/// Secondary indexes arrive with tasks 9 and 11; until then every
/// `IndexName` names an index the table does not have.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/persistent_state_machine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::dynamodb {

namespace query_detail {

/// The page limit on item data AWS applies to one `Query` or `Scan`.
inline constexpr std::size_t k_page_bytes = std::size_t{1024} * 1024;

/// The smallest key after every key that starts with exactly `key`: `key`
/// followed by a 0 byte.
[[nodiscard]] inline auto just_after(std::string key) -> std::string {
    key.push_back('\0');
    return key;
}

/// The escaped bytes of an S or B value, without the end marker: a prefix
/// of the encoding of every value that begins with it.
[[nodiscard]] inline auto escaped_prefix(const attribute_value& v) -> std::string {
    auto enc = encode_key_component(v);
    enc.resize(enc.size() - 2);  // drop the 0x00 0x01 end marker
    return enc;
}

/// The base-key range a key condition can match in table `id`.
[[nodiscard]] inline auto query_range(std::uint64_t table_id, const key_condition& k)
    -> kythira::key_range {
    auto base = partition_prefix(table_id, k.partition_value);
    base.push_back('\0');
    auto whole = prefix_range(base);
    if (!k.sort) {
        return whole;
    }
    const auto& v = k.sort->values;
    switch (k.sort->op) {
        case expr_op::eq: {
            auto exact = base + encode_key_component(v[0]);
            return {exact, just_after(exact)};
        }
        case expr_op::lt:
            return {whole.start, base + encode_key_component(v[0])};
        case expr_op::le:
            return {whole.start, just_after(base + encode_key_component(v[0]))};
        case expr_op::gt:
            return {just_after(base + encode_key_component(v[0])), whole.end};
        case expr_op::ge:
            return {base + encode_key_component(v[0]), whole.end};
        case expr_op::between:
            return {base + encode_key_component(v[0]),
                    just_after(base + encode_key_component(v[1]))};
        case expr_op::begins_with:
            return prefix_range(base + escaped_prefix(v[0]));
        default:
            return whole;
    }
}

/// Two hash bytes, big-endian.
[[nodiscard]] inline auto hash_bytes(std::uint32_t h) -> std::string {
    return std::string{static_cast<char>((h >> 8U) & 0xFFU), static_cast<char>(h & 0xFFU)};
}

/// Segment `segment` of `total` of table `id`.
[[nodiscard]] inline auto segment_range(std::uint64_t table_id, std::uint32_t segment,
                                        std::uint32_t total) -> kythira::key_range {
    const auto prefix = table_prefix(table_id);
    if (total <= 1) {
        return prefix_range(prefix);
    }
    constexpr std::uint64_t k_hashes = 65536;
    const auto lo = static_cast<std::uint32_t>(segment * k_hashes / total);
    const auto hi = static_cast<std::uint32_t>((std::uint64_t{segment} + 1) * k_hashes / total);
    kythira::key_range r{prefix + hash_bytes(lo), std::nullopt};
    r.end = hi >= k_hashes ? prefix_end(prefix) : std::optional(prefix + hash_bytes(hi));
    return r;
}

/// The key attributes of `it`, as `LastEvaluatedKey`.
[[nodiscard]] inline auto key_of(const item& it, const key_schema& schema) -> item {
    item out;
    for (const auto& [name, value] : it) {
        if (name == schema.partition_name || (schema.sort_name && name == *schema.sort_name)) {
            out.emplace_back(name, value);
        }
    }
    return out;
}

[[nodiscard]] inline auto no_such_index(std::string_view name) -> aws_json::error {
    return validation_error("The table does not have the specified index: " + std::string(name));
}

/// Checks `Select` against the projection, as AWS does.
[[nodiscard]] inline auto select_error(const boost::json::object& in, bool has_projection,
                                       bool has_index) -> std::optional<aws_json::error> {
    const auto select = api_detail::text(in, "Select");
    const std::string_view what =
        in.contains("AttributesToGet") ? "AttributesToGet" : "ProjectionExpression";
    if (select == "SPECIFIC_ATTRIBUTES" && !has_projection) {
        return validation_error(
            "Must specify the AttributesToGet or ProjectionExpression when choosing to get "
            "SPECIFIC_ATTRIBUTES");
    }
    if (select == "ALL_PROJECTED_ATTRIBUTES" && !has_index) {
        return validation_error(
            "ALL_PROJECTED_ATTRIBUTES can be used only when Querying using an IndexName");
    }
    if (select == "ALL_ATTRIBUTES" && has_projection) {
        return validation_error("Cannot specify the " + std::string(what) +
                                " when choosing to get ALL_ATTRIBUTES");
    }
    if (select == "COUNT" && has_projection) {
        return validation_error("Cannot specify the " + std::string(what) +
                                " when choosing to get only the Count");
    }
    return std::nullopt;
}

/// Checks `ExclusiveStartKey`'s shape against the schema and returns its
/// base key.
[[nodiscard]] inline auto start_key(const boost::json::value& v, std::uint64_t table_id,
                                    const key_schema& schema) -> aws_json::result<item> {
    auto key = item_from_json(v.as_object());
    if (!key) {
        return key.error();
    }
    const std::size_t want = schema.sort_name ? 2 : 1;
    if (key->size() != want) {
        return validation_error("Exclusive Start Key must have same size as table's key schema");
    }
    if (auto e = validate_key(*key, schema)) {
        return validation_error("The provided starting key is invalid: " + e->message.value_or(""));
    }
    if (!base_item_key(table_id, schema, *key)) {
        return validation_error("The provided starting key is invalid");
    }
    return std::move(*key);
}

}  // namespace query_detail

/// `Query` and `Scan` over a backend.
template<dynamo_backend Backend> class query_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit query_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("Query", [this](call_context& c, boost::json::object& in) { return query(c, in); });
        fe.on("Scan", [this](call_context& c, boost::json::object& in) { return scan(c, in); });
    }

    auto query(call_context& /*ctx*/, boost::json::object& in) -> result {
        using namespace query_detail;
        auto table = api_detail::usable_table(_backend, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        if (const auto index = api_detail::text(in, "IndexName"); !index.empty()) {
            return no_such_index(index);
        }
        request_keys keys;
        keys.table = table->schema;
        auto compiled = compile_request("Query", in, keys);
        if (!compiled) {
            return compiled.error();
        }
        if (auto e = select_error(in, compiled->projection.has_value(), false)) {
            return *e;
        }
        const auto& k = *compiled->key;
        auto range = query_range(table->table_id, k);
        const bool forward =
            !in.contains("ScanIndexForward") || api_detail::flag(in, "ScanIndexForward");
        if (const auto* esk = in.if_contains("ExclusiveStartKey")) {
            auto start = start_key(*esk, table->table_id, table->schema);
            if (!start) {
                return start.error();
            }
            if (!matches_key(k, *start)) {
                return validation_error(
                    "The provided starting key does not match the range key predicate");
            }
            narrow(range, *base_item_key(table->table_id, table->schema, *start), forward);
        }
        return read_page(
            in, *table, *compiled, range,
            forward ? kythira::scan_direction::forward : kythira::scan_direction::reverse, &k);
    }

    auto scan(call_context& /*ctx*/, boost::json::object& in) -> result {
        using namespace query_detail;
        auto table = api_detail::usable_table(_backend, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        if (const auto index = api_detail::text(in, "IndexName"); !index.empty()) {
            return no_such_index(index);
        }
        const auto* seg = in.if_contains("Segment");
        const auto* total = in.if_contains("TotalSegments");
        if (seg != nullptr && total == nullptr) {
            return validation_error(
                "If segment is specified, total segment must also be specified");
        }
        if (total != nullptr && seg == nullptr) {
            return validation_error(
                "If total segments is specified, a particular segment must also be specified for "
                "this request");
        }
        std::uint32_t segment = 0;
        std::uint32_t segments = 1;
        if (seg != nullptr) {
            const auto s = seg->to_number<std::int64_t>();
            const auto t = total->to_number<std::int64_t>();
            if (s >= t) {
                return validation_error("1 validation error detected: Value '" + std::to_string(s) +
                                        "' at 'segment' failed to satisfy constraint: Member "
                                        "must have value less than or equal to " +
                                        std::to_string(t - 1));
            }
            segment = static_cast<std::uint32_t>(s);
            segments = static_cast<std::uint32_t>(t);
        }
        request_keys keys;
        keys.table = table->schema;
        auto compiled = compile_request("Scan", in, keys);
        if (!compiled) {
            return compiled.error();
        }
        if (auto e = select_error(in, compiled->projection.has_value(), false)) {
            return *e;
        }
        auto range = segment_range(table->table_id, segment, segments);
        if (const auto* esk = in.if_contains("ExclusiveStartKey")) {
            auto start = start_key(*esk, table->table_id, table->schema);
            if (!start) {
                return start.error();
            }
            const auto key = *base_item_key(table->table_id, table->schema, *start);
            if (key < range.start || (range.end && key >= *range.end)) {
                return validation_error("Exclusive start key must lie within the segment");
            }
            narrow(range, key, true);
        }
        return read_page(in, *table, *compiled, range, kythira::scan_direction::forward, nullptr);
    }

private:
    /// Resumes a range after `key` in the read's direction.
    static void narrow(kythira::key_range& range, const std::string& key, bool forward) {
        if (forward) {
            range.start = std::max(range.start, query_detail::just_after(key));
        } else if (!range.end || key < *range.end) {
            range.end = key;
        }
    }

    auto read_page(const boost::json::object& in, const table_descriptor& table,
                   const expression_set& compiled, const kythira::key_range& range,
                   kythira::scan_direction direction, const key_condition* key) -> result {
        const bool consistent = api_detail::flag(in, "ConsistentRead");
        const bool count_only = api_detail::text(in, "Select") == "COUNT";
        std::size_t limit = SIZE_MAX;
        if (const auto* l = in.if_contains("Limit")) {
            limit = static_cast<std::size_t>(l->to_number<std::int64_t>());
        }
        boost::json::array items;
        std::size_t scanned = 0;
        std::size_t count = 0;
        std::size_t bytes = 0;
        std::optional<item> last;
        bool stopped = false;
        if (!range.empty()) {
            _backend.read_range(
                range, direction, consistent, [&](std::string_view /*base_key*/, const item& it) {
                    if (key != nullptr && !matches_key(*key, it)) {
                        return true;
                    }
                    const auto size = item_size(it);
                    if (scanned > 0 && bytes + size > query_detail::k_page_bytes) {
                        stopped = true;
                        return false;
                    }
                    bytes += size;
                    ++scanned;
                    last = it;
                    if (!compiled.filter || evaluate_condition(*compiled.filter, it)) {
                        ++count;
                        if (!count_only) {
                            items.push_back(item_to_json(
                                compiled.projection ? project_item(*compiled.projection, it) : it));
                        }
                    }
                    if (scanned == limit) {
                        stopped = true;
                        return false;
                    }
                    return true;
                });
        }
        boost::json::object out;
        if (!count_only) {
            out["Items"] = std::move(items);
        }
        out["Count"] = count;
        out["ScannedCount"] = scanned;
        if (stopped && last) {
            out["LastEvaluatedKey"] = item_to_json(query_detail::key_of(*last, table.schema));
        }
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] =
                api_detail::capacity(in, table.name, api_detail::read_units(bytes, consistent));
        }
        return out;
    }

    Backend& _backend;
};

}  // namespace kythira::dynamodb
