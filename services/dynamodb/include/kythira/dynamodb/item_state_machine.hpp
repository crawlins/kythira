// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file item_state_machine.hpp
/// @brief The state machine of a group of DynamoDB items
///        (`.kiro/specs/dynamodb-compatible-api/` task 6, Requirements 8.2,
///        20.1).
///
/// `dynamo_item_state_machine` is a `persistent_splittable_state_machine`:
/// its items stay in the node's `kv_engine` under the group's key range
/// (`key_layout.hpp`), it snapshots by checkpoint manifest, and it splits
/// and merges by handing out ranges, never by copying items.
///
/// A single-item write is one Raft entry, an `item_command`. `apply()` reads
/// the committed item, evaluates the condition against it (never at the
/// gateway, Requirement 8.2), applies the update, bumps the item's version
/// and writes the applied index, all in one engine batch. A condition that
/// fails, or an update that cannot apply to the stored item, is a normal
/// result returned to the client: the entry still advances the applied
/// index and changes nothing else.
///
/// The command carries the table id and key schema, so a group needs no
/// catalog lookup to apply it, and replicas cannot disagree about a schema
/// they read at different times. The command also carries the table's
/// local secondary indexes, and the same batch updates the item's entries
/// in them and its collection's size (`local_index.hpp`, task 9). Later
/// tasks add the TTL index and change records to that batch.
///
/// A table with LSIs has a limit on each item collection (10 GB on AWS):
/// a write that would grow a collection past it fails with
/// `ItemCollectionSizeLimitExceededException`, while a write that shrinks
/// one always applies.

#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// `ReturnValues`.
enum class return_values : std::uint8_t {
    none = 0,
    all_old = 1,
    updated_old = 2,
    all_new = 3,
    updated_new = 4,
};

/// One write to a group of items.
struct item_command {
    enum class kind : std::uint8_t {
        put = 1,
        update = 2,
        delete_ = 3,
        /// Deletes up to `max_keys` items of a deleted table from this
        /// group (Requirement 7.3: background delete, bounded per entry).
        drop_table_step = 4,
    };

    kind op = kind::put;
    std::uint64_t table_id = 0;
    key_schema schema;
    /// The item for `put`; the key for `update` and `delete_`.
    item payload;
    /// `condition` for every write, `update` for `update`.
    expression_set expressions;
    return_values returns = return_values::none;
    /// `ReturnValuesOnConditionCheckFailure=ALL_OLD`.
    bool old_on_condition_failure = false;
    std::uint32_t max_keys = 0;
    /// The table's local secondary indexes, maintained by every write.
    std::vector<local_index> lsis;

    friend auto operator==(const item_command&, const item_command&) -> bool = default;
};

/// What `apply()` returns for an `item_command`.
struct item_result {
    enum class status : std::uint8_t {
        ok = 0,
        /// `ConditionalCheckFailedException`.
        condition_failed = 1,
        /// The update cannot apply to the stored item (a ValidationException
        /// the gateway could not see before the item was read).
        error = 2,
    };

    status outcome = status::ok;
    /// `ok`: the attributes `ReturnValues` asked for; `condition_failed`:
    /// the stored item when `ReturnValuesOnConditionCheckFailure=ALL_OLD`.
    std::optional<item> attributes;
    std::string error_type;
    std::string error_message;
    /// The item's version after the write (0 for a delete or no write).
    std::uint64_t version = 0;
    /// `drop_table_step`: items deleted, and whether any remain here.
    std::uint64_t deleted = 0;
    bool more = false;
    /// The item collection's size after a write that applied, for a table
    /// with LSIs (`ItemCollectionMetrics`).
    std::optional<std::uint64_t> collection_bytes;

    friend auto operator==(const item_result&, const item_result&) -> bool = default;
};

namespace item_sm_detail {

inline constexpr std::uint8_t k_command_format_version = 1;
inline constexpr std::uint8_t k_result_format_version = 1;

inline void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

[[nodiscard]] inline auto get_u64(std::string_view& in) -> std::optional<std::uint64_t> {
    if (in.size() < 8) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[i])) << (8 * i);
    }
    in.remove_prefix(8);
    return v;
}

[[nodiscard]] inline auto key_type_ok(std::uint8_t t) -> bool {
    return t == static_cast<std::uint8_t>(attribute_type::S) ||
           t == static_cast<std::uint8_t>(attribute_type::N) ||
           t == static_cast<std::uint8_t>(attribute_type::B);
}

inline void put_schema(std::string& out, const key_schema& s) {
    detail::put_bytes(out, s.partition_name);
    out.push_back(static_cast<char>(s.partition_type));
    out.push_back(s.sort_name ? '\1' : '\0');
    if (s.sort_name) {
        detail::put_bytes(out, *s.sort_name);
        out.push_back(static_cast<char>(s.sort_type));
    }
}

[[nodiscard]] inline auto get_schema(detail::reader& r) -> std::optional<key_schema> {
    key_schema s;
    auto pk = r.bytes();
    const auto pt = r.byte();
    const auto has_sk = r.byte();
    if (!pk || !pt || !has_sk || !key_type_ok(*pt) || *has_sk > 1) {
        return std::nullopt;
    }
    s.partition_name = std::move(*pk);
    s.partition_type = static_cast<attribute_type>(*pt);
    if (*has_sk == 1) {
        auto sk = r.bytes();
        const auto st = r.byte();
        if (!sk || !st || !key_type_ok(*st)) {
            return std::nullopt;
        }
        s.sort_name = std::move(*sk);
        s.sort_type = static_cast<attribute_type>(*st);
    }
    return s;
}

}  // namespace item_sm_detail

/// The Raft entry form of an item command: a format version, then the
/// fields in declaration order (items and expressions in their own
/// versioned binary forms).
[[nodiscard]] inline auto encode_item_command(const item_command& c) -> std::string {
    using namespace item_sm_detail;
    std::string out;
    out.push_back(static_cast<char>(k_command_format_version));
    out.push_back(static_cast<char>(c.op));
    put_u64(out, c.table_id);
    put_schema(out, c.schema);
    detail::put_bytes(out, encode_item(c.payload));
    detail::put_bytes(out, encode_expressions(c.expressions));
    out.push_back(static_cast<char>(c.returns));
    out.push_back(c.old_on_condition_failure ? '\1' : '\0');
    detail::put_varint(out, c.max_keys);
    put_local_indexes(out, c.lsis);
    return out;
}

/// Why an entry could not be read.
enum class item_command_status : std::uint8_t {
    ok,
    malformed,
    /// A newer format (the entry's or its expressions'): this replica must
    /// be upgraded before it can apply the entry.
    unknown_version,
};

[[nodiscard]] inline auto decode_item_command(std::string_view bytes, item_command& out)
    -> item_command_status {
    using namespace item_sm_detail;
    if (bytes.empty()) {
        return item_command_status::malformed;
    }
    const auto version = static_cast<std::uint8_t>(bytes[0]);
    if (version > k_command_format_version) {
        return item_command_status::unknown_version;
    }
    if (version == 0 || bytes.size() < 2) {
        return item_command_status::malformed;
    }
    const auto op = static_cast<std::uint8_t>(bytes[1]);
    if (op < 1 || op > 4) {
        return item_command_status::malformed;
    }
    bytes.remove_prefix(2);
    item_command c;
    c.op = static_cast<item_command::kind>(op);
    const auto id = get_u64(bytes);
    if (!id) {
        return item_command_status::malformed;
    }
    c.table_id = *id;
    detail::reader r(bytes);
    auto schema = get_schema(r);
    auto payload_bytes = r.bytes();
    auto expr_bytes = r.bytes();
    const auto returns = r.byte();
    const auto old_flag = r.byte();
    const auto max_keys = r.varint();
    auto lsis = get_local_indexes(r);
    if (!schema || !payload_bytes || !expr_bytes || !returns || !old_flag || !max_keys || !lsis ||
        !r.done() || *returns > static_cast<std::uint8_t>(return_values::updated_new) ||
        *old_flag > 1 || *max_keys > UINT32_MAX) {
        return item_command_status::malformed;
    }
    auto payload = decode_item(*payload_bytes);
    if (!payload) {
        return item_command_status::malformed;
    }
    switch (decode_expressions(*expr_bytes, c.expressions)) {
        case expr_decode_status::ok:
            break;
        case expr_decode_status::unknown_version:
            return item_command_status::unknown_version;
        default:
            return item_command_status::malformed;
    }
    c.schema = std::move(*schema);
    c.payload = std::move(*payload);
    c.returns = static_cast<return_values>(*returns);
    c.old_on_condition_failure = *old_flag == 1;
    c.max_keys = static_cast<std::uint32_t>(*max_keys);
    c.lsis = std::move(*lsis);
    out = std::move(c);
    return item_command_status::ok;
}

/// The result form: a format version, the outcome, the fixed-width
/// counters, then the attributes and error text.
[[nodiscard]] inline auto encode_item_result(const item_result& r) -> std::string {
    using namespace item_sm_detail;
    std::string out;
    out.push_back(static_cast<char>(k_result_format_version));
    out.push_back(static_cast<char>(r.outcome));
    out.push_back(r.more ? '\1' : '\0');
    put_u64(out, r.version);
    put_u64(out, r.deleted);
    out.push_back(r.attributes ? '\1' : '\0');
    if (r.attributes) {
        detail::put_bytes(out, encode_item(*r.attributes));
    }
    detail::put_bytes(out, r.error_type);
    detail::put_bytes(out, r.error_message);
    out.push_back(r.collection_bytes ? '\1' : '\0');
    if (r.collection_bytes) {
        detail::put_varint(out, *r.collection_bytes);
    }
    return out;
}

[[nodiscard]] inline auto decode_item_result(std::string_view bytes) -> std::optional<item_result> {
    using namespace item_sm_detail;
    if (bytes.size() < 3 || static_cast<std::uint8_t>(bytes[0]) != k_result_format_version ||
        static_cast<std::uint8_t>(bytes[1]) > 2 || static_cast<std::uint8_t>(bytes[2]) > 1) {
        return std::nullopt;
    }
    item_result out;
    out.outcome = static_cast<item_result::status>(bytes[1]);
    out.more = bytes[2] == '\1';
    bytes.remove_prefix(3);
    const auto version = get_u64(bytes);
    const auto deleted = get_u64(bytes);
    if (!version || !deleted) {
        return std::nullopt;
    }
    out.version = *version;
    out.deleted = *deleted;
    detail::reader r(bytes);
    const auto has_attributes = r.byte();
    if (!has_attributes || *has_attributes > 1) {
        return std::nullopt;
    }
    if (*has_attributes == 1) {
        auto b = r.bytes();
        if (!b) {
            return std::nullopt;
        }
        out.attributes = decode_item(*b);
        if (!out.attributes) {
            return std::nullopt;
        }
    }
    auto type = r.bytes();
    auto message = r.bytes();
    const auto has_size = r.byte();
    if (!type || !message || !has_size || *has_size > 1) {
        return std::nullopt;
    }
    if (*has_size == 1) {
        out.collection_bytes = r.varint();
        if (!out.collection_bytes) {
            return std::nullopt;
        }
    }
    if (!r.done()) {
        return std::nullopt;
    }
    out.error_type = std::move(*type);
    out.error_message = std::move(*message);
    return out;
}

/// The state machine of one group of items. `Engine` is any `kv_engine`:
/// unit tests use `memory_kv_engine`, production RocksDB.
template<kythira::kv_engine Engine = kythira::memory_kv_engine, typename LogIndex = std::uint64_t>
class dynamo_item_state_machine {
public:
    using engine_type = Engine;

    // -- persistent_state_machine ------------------------------------------

    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        _engine = &engine;
        _range = range;
        _ring.clear();
        _applied = 0;
        if (const auto v = _engine->get(kythira::column_family::meta, _range.start)) {
            std::string_view in(*v);
            if (const auto index = item_sm_detail::get_u64(in); index && in.empty()) {
                _applied = static_cast<LogIndex>(*index);
            }
        }
        _durable_floor = _applied;
    }

    auto apply(const std::vector<std::byte>& entry, LogIndex index) -> std::vector<std::byte> {
        if (index <= _applied) {
            return {};
        }
        item_command c;
        const std::string_view bytes(reinterpret_cast<const char*>(entry.data()),  // NOLINT
                                     entry.size());
        switch (decode_item_command(bytes, c)) {
            case item_command_status::ok:
                break;
            case item_command_status::unknown_version:
                // Design "Error handling": stop the group rather than guess,
                // so a rolling upgrade finishes before new syntax is used.
                throw std::runtime_error(
                    "dynamo_item_state_machine: entry " + std::to_string(index) +
                    " uses a newer item or expression format; upgrade this node");
            case item_command_status::malformed:
                throw std::invalid_argument("dynamo_item_state_machine: malformed entry " +
                                            std::to_string(index));
        }
        kythira::write_batch batch;
        const auto result = c.op == item_command::kind::drop_table_step ? drop_table_step(c, batch)
                                                                        : write_item(c, batch);
        put_applied(batch, index);
        const auto seq = engine().write(batch);
        _applied = index;
        _ring.emplace_back(index, seq);
        trim_ring();
        const auto out = encode_item_result(result);
        std::vector<std::byte> bytes_out(out.size());
        std::memcpy(bytes_out.data(), out.data(), out.size());
        return bytes_out;
    }

    [[nodiscard]] auto applied_index() const -> LogIndex { return _applied; }

    [[nodiscard]] auto durable_applied_index() const -> LogIndex {
        const auto durable = engine().durable_sequence();
        LogIndex best = _durable_floor;
        for (const auto& [index, seq] : _ring) {
            if (seq > durable) {
                break;
            }
            best = index;
        }
        return best;
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        return engine().checkpoint(_range, dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                LogIndex index) -> void {
        engine().ingest(_range, dir, manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
    }

    auto drop() -> void { engine().drop(_range); }

    // -- persistent_splittable_state_machine --------------------------------

    /// Narrows this group to the part below the first cut and returns the
    /// child ranges. Every cut must be a partition boundary
    /// (`can_split_at`); each child's applied index is written at its start
    /// in the same batch, so a child opened on its range starts where this
    /// group was.
    auto split_at(const std::vector<std::string>& keys) -> std::vector<kythira::key_range> {
        std::vector<std::string> cuts = keys;
        std::ranges::sort(cuts);
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::erase_if(cuts, [&](const std::string& k) { return !_range.contains(k); });
        if (cuts.empty()) {
            return {};
        }
        if (cuts.front() == _range.start) {
            throw std::invalid_argument("dynamo_item_state_machine: split at the range start");
        }
        for (const auto& k : cuts) {
            if (!can_split_at(k)) {
                throw std::invalid_argument(
                    "dynamo_item_state_machine: split inside an item collection");
            }
        }
        std::vector<kythira::key_range> children;
        kythira::write_batch batch;
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            children.push_back(kythira::key_range{
                cuts[i],
                i + 1 < cuts.size() ? std::optional<std::string>(cuts[i + 1]) : _range.end});
            put_applied_at(batch, cuts[i], _applied);
        }
        engine().write(batch);
        _range.end = cuts.front();
        return children;
    }

    /// Widens this group over an adjacent range of the same engine.
    auto adopt(const kythira::key_range& range) -> void {
        if (_range.contains(range)) {
            return;
        }
        kythira::write_batch batch;
        if (_range.end.has_value() && range.start == *_range.end) {
            batch.erase(kythira::column_family::meta, range.start);
            _range.end = range.end;
        } else if (range.end.has_value() && *range.end == _range.start) {
            batch.erase(kythira::column_family::meta, _range.start);
            _range.start = range.start;
        } else {
            throw std::invalid_argument(
                "dynamo_item_state_machine: adopt of a range that is not "
                "adjacent");
        }
        put_applied(batch, _applied);
        engine().write(batch);
    }

    [[nodiscard]] auto range() const -> const kythira::key_range& { return _range; }

    /// The split veto: a group is cut only between item collections, so a
    /// `Query` is one range read on one group.
    [[nodiscard]] static auto can_split_at(const std::string& key) -> bool {
        return is_partition_boundary(key);
    }

    /// The item collection limit of tables with LSIs. Every replica of a
    /// group must use the same value; tests lower it.
    auto set_collection_limit(std::uint64_t bytes) -> void { _collection_limit = bytes; }

    /// The size of an item collection, as `ItemCollectionMetrics` counts it
    /// (0 for a table without LSIs, which keeps no count).
    [[nodiscard]] auto collection_size(std::uint64_t table_id, const attribute_value& pk) const
        -> std::uint64_t {
        return load_counter(collection_size_key(table_id, pk));
    }

    // -- Reads ----------------------------------------------------------------

    /// The stored item under `key`, read from the committed state. Corrupt
    /// bytes are a storage fault, not a missing item.
    [[nodiscard]] auto get_item(std::uint64_t table_id, const key_schema& schema,
                                const item& key) const -> std::optional<item> {
        const auto k = base_item_key(table_id, schema, key);
        if (!k || !_range.contains(*k)) {
            return std::nullopt;
        }
        return load(*k);
    }

    /// The item's version (0 when it has never been written or is deleted).
    [[nodiscard]] auto item_version(std::uint64_t table_id, const key_schema& schema,
                                    const item& key) const -> std::uint64_t {
        const auto k = base_item_key(table_id, schema, key);
        if (!k || !_range.contains(*k)) {
            return 0;
        }
        return load_version(*k);
    }

    /// Visits the committed items whose keys fall in `r` and in this
    /// group's range, in key order (or reverse key order), until `visit`
    /// returns false. Each call gets the item's base key and the item.
    /// Corrupt bytes are a storage fault.
    auto scan_items(const kythira::key_range& r, kythira::scan_direction direction,
                    const std::function<bool(std::string_view, const item&)>& visit) const -> void {
        kythira::key_range mine{std::max(r.start, _range.start), r.end};
        if (_range.end && (!mine.end || *_range.end < *mine.end)) {
            mine.end = _range.end;
        }
        if (mine.empty()) {
            return;
        }
        engine().scan(kythira::column_family::data, mine, direction,
                      [&](std::string_view k, std::string_view v) {
                          auto it = decode_item(v);
                          if (!it) {
                              throw kythira::persistent_storage_error(
                                  "dynamo_item_state_machine: corrupt item");
                          }
                          return visit(k, *it);
                      });
    }

private:
    [[nodiscard]] auto engine() const -> engine_type& {
        if (_engine == nullptr) {
            throw std::logic_error("dynamo_item_state_machine: not opened");
        }
        return *_engine;
    }

    [[nodiscard]] auto load(const std::string& key) const -> std::optional<item> {
        const auto v = engine().get(kythira::column_family::data, key);
        if (!v) {
            return std::nullopt;
        }
        auto it = decode_item(*v);
        if (!it) {
            throw kythira::persistent_storage_error("dynamo_item_state_machine: corrupt item");
        }
        return it;
    }

    [[nodiscard]] auto load_counter(const std::string& key) const -> std::uint64_t {
        const auto v = engine().get(kythira::column_family::meta, key);
        if (!v) {
            return 0;
        }
        std::string_view in(*v);
        const auto n = item_sm_detail::get_u64(in);
        if (!n || !in.empty()) {
            throw kythira::persistent_storage_error("dynamo_item_state_machine: corrupt counter");
        }
        return *n;
    }

    [[nodiscard]] auto load_version(const std::string& key) const -> std::uint64_t {
        return load_counter(key);
    }

    static auto put_applied_at(kythira::write_batch& batch, const std::string& at, LogIndex index)
        -> void {
        std::string v;
        item_sm_detail::put_u64(v, static_cast<std::uint64_t>(index));
        batch.put(kythira::column_family::meta, at, std::move(v));
    }

    auto put_applied(kythira::write_batch& batch, LogIndex index) const -> void {
        put_applied_at(batch, _range.start, index);
    }

    static auto updated_paths(const item_command& c) -> projection_expr {
        projection_expr p;
        if (c.expressions.update) {
            for (const auto& a : c.expressions.update->actions) {
                p.paths.push_back(a.path);
            }
        }
        return p;
    }

    static auto error_result(const aws_json::error& e) -> item_result {
        item_result r;
        r.outcome = item_result::status::error;
        r.error_type = e.type;
        r.error_message = e.message.value_or("");
        return r;
    }

    auto write_item(const item_command& c, kythira::write_batch& batch) const -> item_result {
        const auto key = base_item_key(c.table_id, c.schema, c.payload);
        if (!key) {
            throw std::invalid_argument("dynamo_item_state_machine: command without its key");
        }
        if (!_range.contains(*key)) {
            // The host routes by range; an entry for a key this group does
            // not own is a routing bug, never a client error.
            throw std::logic_error("dynamo_item_state_machine: key outside the group's range");
        }
        const auto old = load(*key);
        const item empty;
        const item& current = old ? *old : empty;

        if (c.expressions.condition && !evaluate_condition(*c.expressions.condition, current)) {
            item_result r;
            r.outcome = item_result::status::condition_failed;
            if (c.old_on_condition_failure && old) {
                r.attributes = *old;
            }
            return r;
        }

        std::optional<item> next;
        switch (c.op) {
            case item_command::kind::put:
                next = c.payload;
                break;
            case item_command::kind::update: {
                // An update of an item that does not exist creates it from
                // its key.
                item updated = old ? *old : c.payload;
                if (c.expressions.update) {
                    if (auto e = apply_update(*c.expressions.update, updated)) {
                        return error_result(*e);
                    }
                }
                if (auto e = validate_key_attributes(updated, c.schema)) {
                    return error_result(*e);
                }
                if (base_item_key(c.table_id, c.schema, updated) != key) {
                    // The compiler refuses updates of key attributes, so
                    // this guards a gateway bug, with AWS's wording.
                    const auto* before = find_attribute(c.payload, c.schema.partition_name);
                    const auto* after = find_attribute(updated, c.schema.partition_name);
                    const bool partition_changed =
                        before == nullptr || after == nullptr || !(*before == *after);
                    const auto& name = partition_changed || !c.schema.sort_name
                                           ? c.schema.partition_name
                                           : *c.schema.sort_name;
                    return error_result(validation_error(std::string(k_invalid_parameters) +
                                                         "Cannot update attribute " + name +
                                                         ". This attribute is part of the key"));
                }
                if (auto e = check_item_size(updated)) {
                    return error_result(*e);
                }
                next = std::move(updated);
                break;
            }
            case item_command::kind::delete_:
            case item_command::kind::drop_table_step:
                break;
        }

        item_result r;
        if (next) {
            if (auto e = validate_index_keys(*next, c.lsis)) {
                return error_result(*e);
            }
        }
        if (!c.lsis.empty()) {
            std::uint64_t size = 0;
            if (auto e = update_indexes(c, old, next, batch, size)) {
                return error_result(*e);
            }
            r.collection_bytes = size;
        }
        if (next) {
            r.version = load_version(*key) + 1;
            std::string v;
            item_sm_detail::put_u64(v, r.version);
            batch.put(kythira::column_family::data, *key, encode_item(*next));
            batch.put(kythira::column_family::meta, *key, std::move(v));
        } else if (old) {
            batch.erase(kythira::column_family::data, *key);
            batch.erase(kythira::column_family::meta, *key);
        }

        switch (c.returns) {
            case return_values::none:
                break;
            case return_values::all_old:
                r.attributes = old;
                break;
            case return_values::all_new:
                r.attributes = next;
                break;
            case return_values::updated_old:
                if (old) {
                    r.attributes = project_item(updated_paths(c), *old);
                }
                break;
            case return_values::updated_new:
                if (next) {
                    r.attributes = project_item(updated_paths(c), *next);
                }
                break;
        }
        // AWS omits Attributes rather than returning an empty map.
        if (r.attributes && r.attributes->empty()) {
            r.attributes.reset();
        }
        return r;
    }

    /// Moves `c`'s table's LSI entries from `old` to `next` and updates
    /// the collection's size, in `batch`. Returns the error instead when
    /// the write would grow the collection past the limit.
    auto update_indexes(const item_command& c, const std::optional<item>& old,
                        const std::optional<item>& next, kythira::write_batch& batch,
                        std::uint64_t& size_out) const -> std::optional<aws_json::error> {
        const auto* pk = find_attribute(c.payload, c.schema.partition_name);
        if (pk == nullptr) {
            throw std::invalid_argument("dynamo_item_state_machine: command without its key");
        }
        const auto size_key = collection_size_key(c.table_id, *pk);
        const auto size = load_counter(size_key);
        std::uint64_t removed = old ? item_size(*old) : 0;
        std::uint64_t added = next ? item_size(*next) : 0;
        std::vector<std::pair<std::string, item>> before;
        std::vector<std::pair<std::string, item>> after;
        for (const auto& lsi : c.lsis) {
            if (old) {
                if (auto e = index_entry(c.schema, lsi, *old)) {
                    removed += entry_size(*e);
                    before.emplace_back(*index_entry_key(c.table_id, c.schema, lsi, *old),
                                        std::move(*e));
                }
            }
            if (next) {
                if (auto e = index_entry(c.schema, lsi, *next)) {
                    added += entry_size(*e);
                    after.emplace_back(*index_entry_key(c.table_id, c.schema, lsi, *next),
                                       std::move(*e));
                }
            }
        }
        const auto total = size - std::min(size, removed) + added;
        if (added > removed && total > _collection_limit) {
            aws_json::error e;
            e.type =
                dynamodb_error_style().service_prefix + "ItemCollectionSizeLimitExceededException";
            e.message = "Item collection size limit exceeded";
            return e;
        }
        for (const auto& [k, unused] : before) {
            const bool kept = std::any_of(after.begin(), after.end(),
                                          [&](const auto& a) { return a.first == k; });
            if (!kept) {
                batch.erase(kythira::column_family::data, k);
            }
        }
        for (auto& [k, entry] : after) {
            batch.put(kythira::column_family::data, k, encode_item(entry));
        }
        if (total == 0) {
            batch.erase(kythira::column_family::meta, size_key);
        } else {
            std::string v;
            item_sm_detail::put_u64(v, total);
            batch.put(kythira::column_family::meta, size_key, std::move(v));
        }
        size_out = total;
        return std::nullopt;
    }

    auto drop_table_step(const item_command& c, kythira::write_batch& batch) const -> item_result {
        const auto table = table_range(c.table_id);
        // The part of the table this group owns.
        kythira::key_range mine{std::max(table.start, _range.start), table.end};
        if (_range.end && (!mine.end || *_range.end < *mine.end)) {
            mine.end = _range.end;
        }
        item_result r;
        if (mine.empty()) {
            return r;
        }
        // `limit` bounds the keys one entry erases; `deleted` counts only
        // base items, for the job's progress.
        const std::size_t limit = c.max_keys == 0 ? 1 : c.max_keys;
        std::size_t erased = 0;
        engine().scan(kythira::column_family::data, mine, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (erased == limit) {
                              r.more = true;
                              return false;
                          }
                          batch.erase(kythira::column_family::data, std::string(k));
                          batch.erase(kythira::column_family::meta, std::string(k));
                          ++erased;
                          const auto cls = classify_item_key(k, c.schema.partition_type);
                          if (cls && cls->what == item_key_class::kind::base) {
                              ++r.deleted;
                          }
                          return true;
                      });
        if (r.more) {
            return r;
        }
        // Then the keys with no item beside them: collection sizes. Item
        // versions went with their items; the group's applied index (at
        // the range start) stays.
        engine().scan(kythira::column_family::meta, mine, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (k == _range.start || classify_item_key(k, c.schema.partition_type)) {
                              return true;
                          }
                          if (erased == limit) {
                              r.more = true;
                              return false;
                          }
                          batch.erase(kythira::column_family::meta, std::string(k));
                          ++erased;
                          return true;
                      });
        return r;
    }

    // Keeps the newest entry at or below the durable sequence as the floor
    // and forgets older ones, so the ring holds only what is not yet
    // durable.
    auto trim_ring() -> void {
        const auto durable = engine().durable_sequence();
        while (_ring.size() > 1 && _ring[1].second <= durable) {
            _durable_floor = _ring.front().first;
            _ring.pop_front();
        }
    }

    engine_type* _engine = nullptr;
    std::uint64_t _collection_limit = k_max_collection_bytes;
    kythira::key_range _range;
    LogIndex _applied{};
    LogIndex _durable_floor{};
    std::deque<std::pair<LogIndex, kythira::engine_sequence>> _ring;
};

static_assert(kythira::persistent_splittable_state_machine<dynamo_item_state_machine<>>);

}  // namespace kythira::dynamodb
