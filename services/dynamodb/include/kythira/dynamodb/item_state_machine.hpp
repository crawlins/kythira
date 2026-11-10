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
/// they read at different times. Later tasks add LSI entries, the TTL
/// index and change records to the same batch.

#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    if (!schema || !payload_bytes || !expr_bytes || !returns || !old_flag || !max_keys ||
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
    if (!type || !message || !r.done()) {
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

    [[nodiscard]] auto load_version(const std::string& key) const -> std::uint64_t {
        const auto v = engine().get(kythira::column_family::meta, key);
        if (!v) {
            return 0;
        }
        std::string_view in(*v);
        const auto n = item_sm_detail::get_u64(in);
        if (!n || !in.empty()) {
            throw kythira::persistent_storage_error("dynamo_item_state_machine: corrupt version");
        }
        return *n;
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
        const std::size_t limit = c.max_keys == 0 ? 1 : c.max_keys;
        engine().scan(kythira::column_family::data, mine, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (r.deleted == limit) {
                              r.more = true;
                              return false;
                          }
                          batch.erase(kythira::column_family::data, std::string(k));
                          batch.erase(kythira::column_family::meta, std::string(k));
                          ++r.deleted;
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
    kythira::key_range _range;
    LogIndex _applied{};
    LogIndex _durable_floor{};
    std::deque<std::pair<LogIndex, kythira::engine_sequence>> _ring;
};

static_assert(kythira::persistent_splittable_state_machine<dynamo_item_state_machine<>>);

}  // namespace kythira::dynamodb
