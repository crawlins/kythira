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
/// A local secondary index (task 9) is read the same way. Its entries sit
/// in each item collection after the base items
/// (`<collection>\1<index no><enc(index sk)><enc(sk)>`), so a `Query` on
/// one is again one range read, and a `Scan` of one walks the table's
/// hash slice and keeps only that index's entries (a table `Scan` skips
/// them). Entries hold the index's projection. Asking for attributes it
/// does not project (`Select=ALL_ATTRIBUTES`, or a projection naming
/// them) reads each matching item from the table, as AWS does for LSIs. A
/// `Query` filter may only name projected attributes, while a `Scan` filter
/// may name any, as DynamoDB Local allows. An index's `LastEvaluatedKey`
/// and `ExclusiveStartKey` carry the table keys and the index sort key.
///
/// A global secondary index (task 11) has a key space of its own
/// (`global_index.hpp`): a `Query` reads one index collection, a `Scan` its
/// hash slices, and each entry is already the index's projection. As on
/// AWS, a GSI is read eventually (`ConsistentRead` is refused), and asking
/// for attributes it does not project is an error rather than a read of
/// the table. Its `LastEvaluatedKey` carries the table and index keys.

#include <kythira/dynamodb/api_common.hpp>
#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/persistent_state_machine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// The keys under `base` (a collection's base items, or one index's
/// entries in it) whose sort key component, which follows `base`, can
/// match the key condition. An index entry key goes on past its sort key
/// (with the table's sort key), so every bound past a value is the end of
/// that value's prefix, never the value's own key.
[[nodiscard]] inline auto range_under(const std::string& base, const key_condition& k)
    -> kythira::key_range {
    auto whole = prefix_range(base);
    if (!k.sort) {
        return whole;
    }
    const auto& v = k.sort->values;
    const auto at = [&](std::size_t i) { return base + encode_key_component(v[i]); };
    const auto past = [&](std::size_t i) { return prefix_end(at(i)).value_or(*whole.end); };
    switch (k.sort->op) {
        case expr_op::eq:
            return prefix_range(at(0));
        case expr_op::lt:
            return {whole.start, at(0)};
        case expr_op::le:
            return {whole.start, past(0)};
        case expr_op::gt:
            return {past(0), whole.end};
        case expr_op::ge:
            return {at(0), whole.end};
        case expr_op::between:
            return {at(0), past(1)};
        case expr_op::begins_with:
            return prefix_range(base + escaped_prefix(v[0]));
        default:
            return whole;
    }
}

/// The base-key range a key condition can match in table `id`.
[[nodiscard]] inline auto query_range(std::uint64_t table_id, const key_condition& k)
    -> kythira::key_range {
    auto base = partition_prefix(table_id, k.partition_value);
    base.push_back('\0');
    return range_under(base, k);
}

/// The entry range a key condition can match in index `lsi` of table `id`.
[[nodiscard]] inline auto index_query_range(std::uint64_t table_id, const local_index& lsi,
                                            const key_condition& k) -> kythira::key_range {
    return range_under(index_prefix(table_id, k.partition_value, lsi.number), k);
}

/// The index named `name`, or null.
[[nodiscard]] inline auto find_index(const table_descriptor& table, std::string_view name)
    -> const local_index* {
    for (const auto& lsi : table.lsis) {
        if (lsi.name == name) {
            return &lsi;
        }
    }
    return nullptr;
}

/// The GSI named `name`, or null (also while it is being deleted).
[[nodiscard]] inline auto find_global_index(const table_descriptor& table, std::string_view name)
    -> const global_index* {
    for (const auto& g : table.gsis) {
        if (g.name == name && g.status != index_status::deleting) {
            return &g;
        }
    }
    return nullptr;
}

/// The entry range a key condition can match in GSI `g`.
[[nodiscard]] inline auto global_query_range(const global_index& g, const key_condition& k)
    -> kythira::key_range {
    return range_under(gsi_collection_prefix(g, k.partition_value), k);
}

/// The top-level attribute names `n` reads.
inline void names_in(const expr_node& n, std::set<std::string>& out) {
    if (n.op == expr_op::path) {
        out.insert(n.path.front().name);
    }
    for (const auto& a : n.args) {
        names_in(a, out);
    }
}

/// The names in `names` that `lsi` does not project.
[[nodiscard]] inline auto unprojected(const key_schema& table, const local_index& lsi,
                                      const std::set<std::string>& names)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& n : names) {
        if (!projects(table, lsi, n)) {
            out.push_back(n);
        }
    }
    return out;
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

/// The key attributes of `it`, as `LastEvaluatedKey`: the table's, and
/// the index's sort key when reading an index.
[[nodiscard]] inline auto key_of(const item& it, const key_schema& schema,
                                 const local_index* lsi = nullptr,
                                 const global_index* gsi = nullptr) -> item {
    item out;
    for (const auto& [name, value] : it) {
        if (name == schema.partition_name || (schema.sort_name && name == *schema.sort_name) ||
            (lsi != nullptr && name == lsi->sort_name) ||
            (gsi != nullptr && is_key_of(schema, *gsi, name))) {
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

/// Checks an index's `ExclusiveStartKey` (the table keys and the index
/// sort key), with DynamoDB Local's wording, and returns the entry key it
/// names.
[[nodiscard]] inline auto index_start_key(const boost::json::value& v,
                                          const table_descriptor& table, const local_index& lsi)
    -> aws_json::result<std::pair<item, std::string>> {
    auto key = item_from_json(v.as_object());
    if (!key) {
        return key.error();
    }
    const bool shared = table.schema.sort_name == lsi.sort_name;
    if (key->size() != (shared ? 2U : 3U)) {
        return validation_error("Exclusive Start Key must have same size as table's key schema");
    }
    const auto* ls = find_attribute(*key, lsi.sort_name);
    if (find_attribute(*key, table.schema.partition_name) == nullptr ||
        find_attribute(*key, *table.schema.sort_name) == nullptr || ls == nullptr) {
        return validation_error("The provided starting key is invalid");
    }
    if (ls->type != lsi.sort_type) {
        return validation_error("Type mismatch for attribute to update");
    }
    if (auto e = validate_key(key_of(*key, table.schema), table.schema)) {
        return validation_error("The provided starting key is invalid: " + e->message.value_or(""));
    }
    auto entry = index_entry_key(table.table_id, table.schema, lsi, *key);
    return std::pair{std::move(*key), std::move(*entry)};
}

/// Checks a GSI's `ExclusiveStartKey` (the table keys and the index
/// keys), with DynamoDB Local's wording, and returns the entry key it
/// names.
[[nodiscard]] inline auto global_start_key(const boost::json::value& v,
                                           const table_descriptor& table, const global_index& g)
    -> aws_json::result<std::pair<item, std::string>> {
    auto key = item_from_json(v.as_object());
    if (!key) {
        return key.error();
    }
    std::set<std::string> names{table.schema.partition_name, g.schema.partition_name};
    if (table.schema.sort_name) {
        names.insert(*table.schema.sort_name);
    }
    if (g.schema.sort_name) {
        names.insert(*g.schema.sort_name);
    }
    if (key->size() != names.size()) {
        return validation_error("The provided starting key is invalid");
    }
    for (const auto& n : names) {
        if (find_attribute(*key, n) == nullptr) {
            return validation_error("The provided starting key is invalid");
        }
    }
    if (auto e = validate_key(key_of(*key, table.schema), table.schema)) {
        return validation_error("The provided starting key is invalid: " + e->message.value_or(""));
    }
    auto entry = gsi_entry_key(table.schema, g, *key);
    if (!entry) {
        return validation_error("The provided starting key is invalid");
    }
    return std::pair{std::move(*key), std::move(*entry)};
}

/// The checks a read of GSI `g` must pass first, in DynamoDB Local's
/// words: no `ConsistentRead`, and nothing asked for that it does not
/// project.
[[nodiscard]] inline auto global_read_error(const boost::json::object& in,
                                            const table_descriptor& table, const global_index& g,
                                            const expression_set& compiled, bool query)
    -> std::optional<aws_json::error> {
    if (g.status == index_status::creating) {
        // AWS's answer; DynamoDB Local fails such a read with an internal
        // error.
        return validation_error("Cannot read from backfilling global secondary index: " + g.name);
    }
    if (api_detail::flag(in, "ConsistentRead")) {
        return validation_error("Consistent reads are not supported on global secondary indexes");
    }
    if (g.projection == projection_kind::all) {
        return std::nullopt;
    }
    if (api_detail::text(in, "Select") == "ALL_ATTRIBUTES") {
        return validation_error(std::string(k_invalid_parameters) + "Select type ALL_ATTRIBUTES " +
                                "is not supported for global secondary index " + g.name +
                                " because its projection type is not ALL");
    }
    const auto list_of = [&](const std::set<std::string>& names) {
        std::string list;
        for (const auto& n : names) {
            if (!projects(table.schema, g, n)) {
                list += (list.empty() ? "" : ", ") + n;
            }
        }
        return list;
    };
    if (compiled.projection) {
        std::set<std::string> names;
        for (const auto& p : compiled.projection->paths) {
            names.insert(p.front().name);
        }
        if (const auto list = list_of(names); !list.empty()) {
            return validation_error(std::string(k_invalid_parameters) + "Global secondary index " +
                                    g.name + " does not project [" + list + "]");
        }
    }
    if (query && compiled.filter) {
        std::set<std::string> names;
        names_in(*compiled.filter, names);
        if (const auto list = list_of(names); !list.empty()) {
            return validation_error(std::string(k_invalid_parameters) + "Secondary index " +
                                    g.name + " does not project one or more filter attributes: [" +
                                    list + "]");
        }
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

    /// The cluster's compatibility switches (Requirement 3); AWS's by
    /// default. A table's tags override them.
    void set_compat_profile(const dynamo_compat_profile& p) { _profile = p; }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("Query", [this](call_context& c, boost::json::object& in) { return query(c, in); });
        fe.on("Scan", [this](call_context& c, boost::json::object& in) { return scan(c, in); });
    }

    auto query(call_context& ctx, boost::json::object& in) -> result {
        using namespace query_detail;
        auto table = api_detail::usable_table(_backend, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        const local_index* lsi = nullptr;
        const global_index* gsi = nullptr;
        if (const auto index = api_detail::text(in, "IndexName"); !index.empty()) {
            lsi = find_index(*table, index);
            gsi = lsi == nullptr ? find_global_index(*table, index) : nullptr;
            if (lsi == nullptr && gsi == nullptr) {
                return no_such_index(index);
            }
        }
        request_keys keys;
        keys.table = table->schema;
        if (lsi != nullptr) {
            keys.index = index_schema(table->schema, *lsi);
        } else if (gsi != nullptr) {
            keys.index = index_schema(*gsi);
        }
        auto compiled = compile_request("Query", in, keys);
        if (!compiled) {
            return compiled.error();
        }
        if (auto e = select_error(in, compiled->projection.has_value(),
                                  lsi != nullptr || gsi != nullptr)) {
            return *e;
        }
        if (gsi != nullptr) {
            if (auto e = global_read_error(in, *table, *gsi, *compiled, true)) {
                return *e;
            }
        }
        if (lsi != nullptr && compiled->filter) {
            std::set<std::string> names;
            names_in(*compiled->filter, names);
            if (const auto missing = unprojected(table->schema, *lsi, names); !missing.empty()) {
                std::string list;
                for (const auto& n : missing) {
                    list += (list.empty() ? "" : ", ") + n;
                }
                return validation_error(
                    std::string(k_invalid_parameters) + "Secondary index " + lsi->name +
                    " does not project one or more filter attributes: [" + list + "]");
            }
        }
        const auto& k = *compiled->key;
        auto range = lsi != nullptr   ? index_query_range(table->table_id, *lsi, k)
                     : gsi != nullptr ? global_query_range(*gsi, k)
                                      : query_range(table->table_id, k);
        const bool forward =
            !in.contains("ScanIndexForward") || api_detail::flag(in, "ScanIndexForward");
        if (const auto* esk = in.if_contains("ExclusiveStartKey")) {
            auto start = resume_key(*esk, *table, lsi, gsi);
            if (!start) {
                return start.error();
            }
            if (!matches_key(k, start->first)) {
                return validation_error(
                    "The provided starting key does not match the range key predicate");
            }
            narrow(range, start->second, forward);
        }
        return read_page(
            ctx, in, *table, lsi, gsi, *compiled, range,
            forward ? kythira::scan_direction::forward : kythira::scan_direction::reverse, &k);
    }

    auto scan(call_context& ctx, boost::json::object& in) -> result {
        using namespace query_detail;
        auto table = api_detail::usable_table(_backend, in.at("TableName").as_string());
        if (!table) {
            return table.error();
        }
        const local_index* lsi = nullptr;
        const global_index* gsi = nullptr;
        if (const auto index = api_detail::text(in, "IndexName"); !index.empty()) {
            lsi = find_index(*table, index);
            gsi = lsi == nullptr ? find_global_index(*table, index) : nullptr;
            if (lsi == nullptr && gsi == nullptr) {
                return no_such_index(index);
            }
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
        if (lsi != nullptr) {
            keys.index = index_schema(table->schema, *lsi);
        } else if (gsi != nullptr) {
            keys.index = index_schema(*gsi);
        }
        auto compiled = compile_request("Scan", in, keys);
        if (!compiled) {
            return compiled.error();
        }
        if (auto e = select_error(in, compiled->projection.has_value(),
                                  lsi != nullptr || gsi != nullptr)) {
            return *e;
        }
        if (gsi != nullptr) {
            if (auto e = global_read_error(in, *table, *gsi, *compiled, false)) {
                return *e;
            }
        }
        auto range =
            segment_range(gsi != nullptr ? gsi->index_id : table->table_id, segment, segments);
        if (const auto* esk = in.if_contains("ExclusiveStartKey")) {
            auto start = resume_key(*esk, *table, lsi, gsi);
            if (!start) {
                return start.error();
            }
            const auto& key = start->second;
            if (key < range.start || (range.end && key >= *range.end)) {
                return validation_error("Exclusive start key must lie within the segment");
            }
            narrow(range, key, true);
        }
        return read_page(ctx, in, *table, lsi, gsi, *compiled, range,
                         kythira::scan_direction::forward, nullptr);
    }

private:
    /// `ExclusiveStartKey` checked for the table or the index, with the
    /// key it names in the engine.
    static auto resume_key(const boost::json::value& esk, const table_descriptor& table,
                           const local_index* lsi, const global_index* gsi)
        -> aws_json::result<std::pair<item, std::string>> {
        if (lsi != nullptr) {
            return query_detail::index_start_key(esk, table, *lsi);
        }
        if (gsi != nullptr) {
            return query_detail::global_start_key(esk, table, *gsi);
        }
        auto start = query_detail::start_key(esk, table.table_id, table.schema);
        if (!start) {
            return start.error();
        }
        auto key = *base_item_key(table.table_id, table.schema, *start);
        return std::pair{std::move(*start), std::move(key)};
    }

    /// Resumes a range after `key` in the read's direction.
    static void narrow(kythira::key_range& range, const std::string& key, bool forward) {
        if (forward) {
            range.start = std::max(range.start, query_detail::just_after(key));
        } else if (!range.end || key < *range.end) {
            range.end = key;
        }
    }

    /// Whether answering from `lsi` needs each item read from the table:
    /// the request wants attributes the index does not project.
    static auto needs_table(const boost::json::object& in, const table_descriptor& table,
                            const local_index* lsi, const expression_set& compiled) -> bool {
        if (lsi == nullptr || lsi->projection == projection_kind::all) {
            return false;
        }
        if (api_detail::text(in, "Select") == "ALL_ATTRIBUTES") {
            return true;
        }
        std::set<std::string> names;
        if (compiled.projection) {
            for (const auto& p : compiled.projection->paths) {
                names.insert(p.front().name);
            }
        }
        if (compiled.filter) {
            query_detail::names_in(*compiled.filter, names);
        }
        return !query_detail::unprojected(table.schema, *lsi, names).empty();
    }

    auto read_page(const call_context& ctx, const boost::json::object& in,
                   const table_descriptor& table, const local_index* lsi, const global_index* gsi,
                   const expression_set& compiled, const kythira::key_range& range,
                   kythira::scan_direction direction, const key_condition* key) -> result {
        const bool consistent = api_detail::flag(in, "ConsistentRead");
        // A GSI read draws on the index's capacity; a table or LSI read on
        // the table's. A Query also draws on its partition key's bucket.
        capacity_target target{gsi != nullptr ? gsi->name : std::string{}, capacity_kind::read, {}};
        if (key != nullptr) {
            target.partition = encode_item(item{{key->partition_name, key->partition_value}});
        }
        if (auto e = api_detail::admit(_backend, _profile, ctx, table, {target})) {
            return *e;
        }
        const bool count_only = api_detail::text(in, "Select") == "COUNT";
        // A Scan of a table (or an LSI) reads the whole slice: keep only
        // what it asked for. A GSI's key space holds only its entries.
        const bool sift = key == nullptr && !table.lsis.empty() && gsi == nullptr;
        std::size_t limit = SIZE_MAX;
        if (const auto* l = in.if_contains("Limit")) {
            limit = static_cast<std::size_t>(l->to_number<std::int64_t>());
        }
        std::vector<item> read;
        std::size_t bytes = 0;
        bool stopped = false;
        if (!range.empty()) {
            _backend.read_range(
                range, direction, consistent, [&](std::string_view at, const item& it) {
                    if (sift) {
                        const auto cls = classify_item_key(at, table.schema.partition_type);
                        const bool wanted =
                            cls && (lsi == nullptr ? cls->what == item_key_class::kind::base
                                                   : cls->what == item_key_class::kind::index &&
                                                         cls->number == lsi->number);
                        if (!wanted) {
                            return true;
                        }
                    }
                    if (key != nullptr && !matches_key(*key, it)) {
                        return true;
                    }
                    const auto size = item_size(it);
                    if (!read.empty() && bytes + size > query_detail::k_page_bytes) {
                        stopped = true;
                        return false;
                    }
                    bytes += size;
                    read.push_back(it);
                    if (read.size() == limit) {
                        stopped = true;
                        return false;
                    }
                    return true;
                });
        }
        // The table reads happen after the range read, which holds the
        // backend while it runs.
        const bool from_table = needs_table(in, table, lsi, compiled);
        const bool all_attributes = api_detail::text(in, "Select") == "ALL_ATTRIBUTES";
        boost::json::array items;
        std::size_t count = 0;
        for (const auto& entry : read) {
            std::optional<item> whole;
            if (from_table) {
                whole = _backend.read(table.table_id, table.schema,
                                      query_detail::key_of(entry, table.schema), consistent);
            }
            const item& source = whole ? *whole : entry;
            if (compiled.filter && !evaluate_condition(*compiled.filter, source)) {
                continue;
            }
            ++count;
            if (count_only) {
                continue;
            }
            if (compiled.projection) {
                items.push_back(item_to_json(project_item(*compiled.projection, source)));
            } else {
                items.push_back(item_to_json(
                    all_attributes || (lsi == nullptr && gsi == nullptr) ? source : entry));
            }
        }
        boost::json::object out;
        if (!count_only) {
            out["Items"] = std::move(items);
        }
        out["Count"] = count;
        out["ScannedCount"] = read.size();
        if (stopped && !read.empty()) {
            out["LastEvaluatedKey"] =
                item_to_json(query_detail::key_of(read.back(), table.schema, lsi, gsi));
        }
        const double units = api_detail::read_units(bytes, consistent);
        api_detail::charge(_backend, _profile, table, target, units);
        if (api_detail::wants_capacity(in)) {
            out["ConsumedCapacity"] = api_detail::capacity(in, table.name, units);
        }
        return out;
    }

    Backend& _backend;
    dynamo_compat_profile _profile{};
};

}  // namespace kythira::dynamodb
