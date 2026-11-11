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
///
/// A write to a table with GSIs also leaves a pending change beside the
/// item, and an index group applies versioned index writes
/// (`global_index.hpp`, task 11).

#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/local_index.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
        /// A transaction's `ConditionCheck` (Requirement 12.2): the
        /// condition is checked while the item is locked, and nothing is
        /// written.
        condition_check = 5,
        /// A `TransactGetItems` read (Requirement 12.5): the item is locked
        /// with every other item of the read, `validate` reports what it
        /// holds, and nothing is written.
        read_lock = 6,
        /// Writes one GSI entry (`target`, holding `payload`) at the item's
        /// `version`, unless the entry already has that version or newer
        /// (task 11).
        index_put = 7,
        /// Deletes one GSI entry at `version`, keeping the version.
        index_delete = 8,
        /// The propagator has written the GSI entries of the pending change
        /// at `target` up to `version`: the record goes, unless a newer
        /// write came, in which case `entries` (the entries now written)
        /// become the ones to remove.
        index_ack = 9,
        /// Backfill of a new GSI: each item of `entries` (base keys, all
        /// expected in the group of `target`, the first of them) that has
        /// no pending change gets one at its current version with no
        /// entries to remove, so the propagator writes its entries in
        /// every index of the table. Keys outside the group are skipped
        /// and reported with `more`.
        index_mark = 10,
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
    /// The table's global secondary indexes: every write records a pending
    /// change for them.
    std::vector<global_index> gsis;
    /// `index_*`: the engine key written (an entry, or a pending change).
    std::string target;
    /// `index_*`: the item version the write carries.
    std::uint64_t version = 0;
    /// `index_ack`: the entry keys the propagator wrote; `index_mark`: the
    /// base keys to mark.
    std::vector<std::string> entries;
    /// The table's changes are logged (task 15): a write that changes the
    /// item appends a change record (`change_log.hpp`).
    bool log_changes = false;
    /// A TTL delete: its record carries the service's identity.
    bool ttl_delete = false;
    /// When the proposer built the write (ms since the epoch), the record's
    /// timestamp.
    std::uint64_t written_ms = 0;

    friend auto operator==(const item_command&, const item_command&) -> bool = default;
};

/// Whether `op` is one of the index maintenance kinds.
[[nodiscard]] inline auto is_index_op(item_command::kind op) -> bool {
    return op == item_command::kind::index_put || op == item_command::kind::index_delete ||
           op == item_command::kind::index_ack || op == item_command::kind::index_mark;
}

/// The engine key a command is routed by: the item's base key, or an
/// index command's target. Nothing when a key attribute is missing.
[[nodiscard]] inline auto command_key(const item_command& c) -> std::optional<std::string> {
    if (is_index_op(c.op)) {
        return c.target;
    }
    return base_item_key(c.table_id, c.schema, c.payload);
}

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
    /// The size a write is charged for (task 12): the larger of the item
    /// before and after it, the stored item for a failed condition; 0 when
    /// not known (an error, or an answer from a node without the field).
    std::uint64_t charged_bytes = 0;

    friend auto operator==(const item_result&, const item_result&) -> bool = default;
};

namespace item_sm_detail {

/// 2 added the GSI fields (task 11), 3 the change log fields (task 15).
/// A command without change log fields is still written as 2, so replicas
/// not yet upgraded keep applying writes to tables that log nothing.
inline constexpr std::uint8_t k_command_format_version = 3;
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
    const bool logged = c.log_changes || c.ttl_delete || c.written_ms != 0;
    out.push_back(static_cast<char>(logged ? k_command_format_version : 2));
    out.push_back(static_cast<char>(c.op));
    put_u64(out, c.table_id);
    put_schema(out, c.schema);
    detail::put_bytes(out, encode_item(c.payload));
    detail::put_bytes(out, encode_expressions(c.expressions));
    out.push_back(static_cast<char>(c.returns));
    out.push_back(c.old_on_condition_failure ? '\1' : '\0');
    detail::put_varint(out, c.max_keys);
    put_local_indexes(out, c.lsis);
    put_global_indexes(out, c.gsis);
    detail::put_bytes(out, c.target);
    detail::put_varint(out, c.version);
    detail::put_varint(out, c.entries.size());
    for (const auto& e : c.entries) {
        detail::put_bytes(out, e);
    }
    if (logged) {
        out.push_back(static_cast<char>((c.log_changes ? 1U : 0U) | (c.ttl_delete ? 2U : 0U)));
        detail::put_varint(out, c.written_ms);
    }
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
    if (op == 0) {
        return item_command_status::malformed;
    }
    if (op > static_cast<std::uint8_t>(item_command::kind::index_mark)) {
        // A kind a later release added: upgrade rather than guess.
        return item_command_status::unknown_version;
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
        *returns > static_cast<std::uint8_t>(return_values::updated_new) || *old_flag > 1 ||
        *max_keys > UINT32_MAX) {
        return item_command_status::malformed;
    }
    if (version >= 2) {
        auto gsis = get_global_indexes(r);
        auto target = r.bytes();
        const auto v = r.varint();
        const auto n = r.count();
        if (!gsis || !target || !v || !n) {
            return item_command_status::malformed;
        }
        for (std::size_t i = 0; i < *n; ++i) {
            auto e = r.bytes();
            if (!e) {
                return item_command_status::malformed;
            }
            c.entries.push_back(std::move(*e));
        }
        c.gsis = std::move(*gsis);
        c.target = std::move(*target);
        c.version = *v;
    } else if (is_index_op(c.op)) {
        return item_command_status::malformed;
    }
    if (version >= 3) {
        const auto flags = r.byte();
        const auto written = r.varint();
        if (!flags || *flags > 3 || !written) {
            return item_command_status::malformed;
        }
        c.log_changes = (*flags & 1U) != 0;
        c.ttl_delete = (*flags & 2U) != 0;
        c.written_ms = *written;
    }
    if (!r.done()) {
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
    // Trailing and optional, so a node without it still reads the rest.
    if (r.charged_bytes != 0) {
        detail::put_varint(out, r.charged_bytes);
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
        const auto charged = r.varint();
        if (!charged || !r.done()) {
            return std::nullopt;
        }
        out.charged_bytes = *charged;
    }
    out.error_type = std::move(*type);
    out.error_message = std::move(*message);
    return out;
}

/// A pending GSI change with the item as it is now (nothing when it was
/// deleted), for the propagator (`gsi_propagator.hpp`).
struct gsi_work {
    std::string pending_key;
    gsi_pending pending;
    std::optional<item> current;
};

/// The state machine of one group of items. `Engine` is any `kv_engine`:
/// unit tests use `memory_kv_engine`, production RocksDB.
template<kythira::kv_engine Engine = kythira::memory_kv_engine, typename LogIndex = std::uint64_t>
class dynamo_item_state_machine {
public:
    using engine_type = Engine;
    /// Fetches and installs a segment of the change log this node lacks
    /// (from the group's peers), or throws. `ingest` calls it for the open
    /// stream's active segment, without which appends cannot continue.
    using change_log_fetcher = std::function<void(const kythira::record_segment_descriptor&)>;

    /// The node's store for change records (task 15), shared by every group
    /// on the node, and how to fetch a segment from a peer. Without a store
    /// the group still keeps its change log state, so replicas agree, but
    /// keeps no records (unit tests that need none).
    auto set_change_log(kythira::record_segment_store* store, change_log_fetcher fetch = {})
        -> void {
        _log = store;
        _fetch = std::move(fetch);
    }

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
        load_pending();
        load_change_log(false);
    }

    auto apply(const std::vector<std::byte>& entry, LogIndex index) -> std::vector<std::byte> {
        if (index <= _applied) {
            return {};
        }
        kythira::write_batch batch;
        auto out = stage(entry, batch);
        commit_staged(batch, index);
        return out;
    }

    // -- persistent_staged_state_machine --------------------------------------
    //
    // How `transactional_state_machine` drives this machine: every write of
    // one entry (a one-phase commit's, or the intents a commit rolls
    // forward) is staged into one batch, then committed once with the
    // applied index. A later write of the batch reads what an earlier one
    // staged, so two writes to one item collection keep its size right.

    /// Stages one command's effects into `batch` and returns its result.
    auto stage(const std::vector<std::byte>& entry, kythira::write_batch& batch)
        -> std::vector<std::byte> {
        const auto c = decode_entry(entry);
        const auto first = batch.size();
        item_result result;
        if (c.op == item_command::kind::drop_table_step) {
            result = drop_table_step(c, batch);
        } else if (is_index_op(c.op)) {
            result = index_write(c, batch);
        } else {
            result = write_item(c, batch, &_staged_changes);
        }
        note_staged(batch, first);
        return to_bytes(encode_item_result(result));
    }

    /// Writes `batch` with the applied index `index`.
    auto commit_staged(kythira::write_batch& batch, LogIndex index) -> void {
        if (!_staged_changes.empty()) {
            append_changes(batch);
        }
        put_applied(batch, index);
        const auto seq = engine().write(batch);
        note_pending(batch);
        _staged.clear();
        _applied = index;
        _ring.emplace_back(index, seq);
        trim_ring();
    }

    /// Forgets what was staged; nothing reached the engine.
    auto discard_staged() -> void {
        _staged.clear();
        _staged_changes.clear();
    }

    /// Transaction state is kept at the item's own engine key, in the
    /// engine's `txn` column family: the keys are engine keys already.
    [[nodiscard]] auto key_prefix() const -> const std::string& {
        static const std::string none;
        return none;
    }

    // -- txn_validating_state_machine (Requirement 12.1) ----------------------

    /// Whether a transaction's write of `key` may proceed, judged against
    /// the committed item: its condition holds, an update can apply, and
    /// the result keeps every limit. A refusal's detail is the encoded
    /// `item_result` the write would have returned (with the stored item
    /// when `ReturnValuesOnConditionCheckFailure=ALL_OLD`). A `read_lock`
    /// always passes, and its detail is the encoded result carrying the
    /// stored item, which is how `TransactGetItems` reads under its locks.
    ///
    /// Each write is judged alone. Two writes of one transaction to one item
    /// collection that each fit under its size limit, but not together, both
    /// pass, and the second then fails when it applies; the limit is 10 GB,
    /// so this is noted rather than handled.
    [[nodiscard]] auto validate(const std::string& key, const std::vector<std::byte>& entry) const
        -> kythira::txn_validation {
        const auto c = decode_entry(entry);
        if (c.op == item_command::kind::drop_table_step || is_index_op(c.op)) {
            throw std::invalid_argument(
                "dynamo_item_state_machine: a drop step or index write in a transaction");
        }
        if (base_item_key(c.table_id, c.schema, c.payload) != key) {
            throw std::invalid_argument(
                "dynamo_item_state_machine: a transaction write filed under another key");
        }
        kythira::write_batch scratch;
        const auto r = write_item(c, scratch);
        if (c.op == item_command::kind::read_lock) {
            return {true, to_bytes(encode_item_result(r))};
        }
        if (r.outcome == item_result::status::ok) {
            return {};
        }
        return {false, to_bytes(encode_item_result(r))};
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

    /// The engine's checkpoint of the range, plus (when the group logs
    /// changes) a file describing the open stream's active segment as it
    /// stands, so a replica installing the snapshot can fetch exactly that
    /// prefix from a peer and check it.
    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        auto m = engine().checkpoint(_range, dir);
        if (const auto active = active_descriptor()) {
            std::string body;
            change_log_detail::put_descriptor(body, *active);
            const auto path = dir / std::string(k_active_segment_file);
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                out.flush();
                if (!out) {
                    throw kythira::persistent_storage_error(
                        "dynamo_item_state_machine: cannot write " + path.string());
                }
            }
            const auto [digest, size] = kythira::sha256_of_file(path);
            m.files.push_back(
                kythira::checkpoint_file{std::string(k_active_segment_file), size, digest});
        }
        return m;
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                LogIndex index) -> void {
        auto engine_manifest = manifest;
        std::optional<kythira::record_segment_descriptor> active;
        for (auto it = engine_manifest.files.begin(); it != engine_manifest.files.end(); ++it) {
            if (it->name == k_active_segment_file) {
                active = read_active_file(dir / it->name, *it);
                engine_manifest.files.erase(it);
                break;
            }
        }
        engine().ingest(_range, dir, engine_manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
        load_pending();
        load_change_log(true, active);
    }

    /// Leaves the change log's segments in the store: closed streams may
    /// still be read through the groups that continue them, and task 16's
    /// retention removes what no group names.
    auto drop() -> void {
        engine().drop(_range);
        _pending.clear();
        _log_state.reset();
    }

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
        if (_log_state) {
            // Every resulting group, this one included, continues the
            // closed stream in a new one (the lineage Streams shows).
            auto next = successor_state({close_current()}, _log_state->epoch + 1,
                                        _log_state->last_ms, _log_state->closed);
            for (const auto& cut : cuts) {
                next.current.key = change_stream_key(cut, next.epoch);
                batch.put(kythira::column_family::meta, change_log_state_key(cut),
                          encode_change_log_state(next));
            }
            next.current.key = change_stream_key(_range.start, next.epoch);
            batch.put(kythira::column_family::meta, change_log_state_key(_range.start),
                      encode_change_log_state(next));
            _log_state = std::move(next);
        }
        engine().write(batch);
        _range.end = cuts.front();
        std::erase_if(_pending, [&](const std::string& k) { return !_range.contains(k); });
        return children;
    }

    /// Widens this group over an adjacent range of the same engine.
    auto adopt(const kythira::key_range& range) -> void {
        if (_range.contains(range)) {
            return;
        }
        kythira::write_batch batch;
        const auto old_start = _range.start;
        std::optional<change_log_state> other;
        if (const auto v =
                engine().get(kythira::column_family::meta, change_log_state_key(range.start))) {
            other = decode_change_log_state(*v);
            if (!other) {
                throw kythira::persistent_storage_error(
                    "dynamo_item_state_machine: corrupt change log state");
            }
        }
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
        if (_log_state || other) {
            // Both streams close; the merged group continues them in one.
            std::vector<log_stream> parents;
            std::vector<log_stream> closed;
            std::uint64_t epoch = 0;
            std::uint64_t last_ms = 0;
            for (auto* st : {&_log_state, &other}) {
                if (!*st) {
                    continue;
                }
                parents.push_back(close_stream((*st)->current));
                closed.insert(closed.end(), (*st)->closed.begin(), (*st)->closed.end());
                epoch = std::max(epoch, (*st)->epoch);
                last_ms = std::max(last_ms, (*st)->last_ms);
            }
            std::ranges::sort(closed, {}, &log_stream::key);
            closed.erase(std::ranges::unique(closed, {}, &log_stream::key).begin(), closed.end());
            auto next = successor_state(parents, epoch + 1, last_ms, closed);
            next.current.key = change_stream_key(_range.start, next.epoch);
            batch.erase(kythira::column_family::meta, change_log_state_key(old_start));
            batch.erase(kythira::column_family::meta, change_log_state_key(range.start));
            batch.put(kythira::column_family::meta, change_log_state_key(_range.start),
                      encode_change_log_state(next));
            _log_state = std::move(next);
        }
        put_applied(batch, _applied);
        engine().write(batch);
        load_pending();
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

    // -- Change log (task 15) -------------------------------------------------

    /// The group's change log state: nothing until its first logged write.
    [[nodiscard]] auto change_log() const -> const std::optional<change_log_state>& {
        return _log_state;
    }

    /// Up to `limits` records of the open stream from `from_seq` on, from
    /// the local store (none without one), with their sequence numbers.
    [[nodiscard]] auto read_changes(std::uint64_t from_seq,
                                    const kythira::segment_read_limits& limits = {}) const
        -> std::vector<std::pair<std::uint64_t, change_record>> {
        std::vector<std::pair<std::uint64_t, change_record>> out;
        if (_log == nullptr || !_log_state) {
            return out;
        }
        const auto got = _log->read(_log_state->current.key, from_seq, limits);
        for (const auto& r : got._records) {
            auto rec = decode_change_record(
                {reinterpret_cast<const char*>(r._data.data()), r._data.size()});  // NOLINT
            if (!rec) {
                throw kythira::persistent_storage_error(
                    "dynamo_item_state_machine: corrupt change record");
            }
            out.emplace_back(r._seq, std::move(*rec));
        }
        return out;
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

    /// The item's version: 0 when it has never been written, or when it was
    /// deleted from a table without GSIs (a table with GSIs keeps a deleted
    /// item's version, so its next write is newer than its index entries).
    [[nodiscard]] auto item_version(std::uint64_t table_id, const key_schema& schema,
                                    const item& key) const -> std::uint64_t {
        const auto k = base_item_key(table_id, schema, key);
        if (!k || !_range.contains(*k)) {
            return 0;
        }
        return load_version(*k);
    }

    /// Up to `limit` pending GSI changes, in key order, from the first one
    /// at or after `from` (wrapping round to the start), read together with
    /// their items so each item matches its record's version.
    [[nodiscard]] auto gsi_pending_batch(std::size_t limit, const std::string& from = {}) const
        -> std::vector<gsi_work> {
        std::vector<gsi_work> out;
        auto it = _pending.lower_bound(from);
        for (std::size_t seen = 0; seen < _pending.size() && out.size() < limit; ++seen) {
            if (it == _pending.end()) {
                it = _pending.begin();
            }
            const auto v = engine().get(kythira::column_family::meta, *it);
            auto p = v ? decode_gsi_pending(*v) : std::nullopt;
            if (!p) {
                throw kythira::persistent_storage_error(
                    "dynamo_item_state_machine: corrupt pending index change");
            }
            gsi_work w;
            w.pending_key = *it;
            w.current = load(p->base_key);
            w.pending = std::move(*p);
            out.push_back(std::move(w));
            ++it;
        }
        return out;
    }

    /// The pending GSI change of the item with key `key`, read with the
    /// item, or nothing when its indexes are up to date (or the key is not
    /// in this group).
    [[nodiscard]] auto gsi_pending_for(std::uint64_t table_id, const key_schema& schema,
                                       const item& key) const -> std::optional<gsi_work> {
        const auto pkey = gsi_pending_key(table_id, schema, key);
        if (!pkey || !_range.contains(*pkey)) {
            return std::nullopt;
        }
        auto record = load_pending_record(*pkey);
        if (!record) {
            return std::nullopt;
        }
        gsi_work w;
        w.pending_key = *pkey;
        w.current = load(record->base_key);
        w.pending = std::move(*record);
        return w;
    }

    /// How many items have GSI changes not yet propagated (the lag).
    [[nodiscard]] auto gsi_pending_count() const -> std::size_t { return _pending.size(); }

    /// The version an index entry holds (kept after a delete), 0 for none.
    [[nodiscard]] auto entry_version(const std::string& entry_key) const -> std::uint64_t {
        if (!_range.contains(entry_key)) {
            return 0;
        }
        return load_version(entry_key);
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

    /// The committed value at `key`, or what this entry already staged there.
    [[nodiscard]] auto read_value(kythira::column_family cf, const std::string& key) const
        -> std::optional<std::string> {
        if (const auto it = _staged.find({cf, key}); it != _staged.end()) {
            return it->second;
        }
        return engine().get(cf, key);
    }

    /// Remembers the data and meta writes `batch` gained from operation
    /// `first` on, so later writes of the same entry read them.
    auto note_staged(const kythira::write_batch& batch, std::size_t first) -> void {
        const auto& ops = batch.operations();
        for (std::size_t i = first; i < ops.size(); ++i) {
            if (const auto* put = std::get_if<kythira::write_batch::put_op>(&ops[i])) {
                _staged[{put->cf, put->key}] = put->value;
            } else if (const auto* erase = std::get_if<kythira::write_batch::erase_op>(&ops[i])) {
                _staged[{erase->cf, erase->key}] = std::nullopt;
            }
        }
    }

    [[nodiscard]] auto decode_entry(const std::vector<std::byte>& entry) const -> item_command {
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
                    "dynamo_item_state_machine: an entry uses a newer item or expression "
                    "format; upgrade this node");
            case item_command_status::malformed:
                throw std::invalid_argument("dynamo_item_state_machine: malformed entry");
        }
        return c;
    }

    static auto to_bytes(const std::string& s) -> std::vector<std::byte> {
        std::vector<std::byte> out(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }

    [[nodiscard]] auto load(const std::string& key) const -> std::optional<item> {
        const auto v = read_value(kythira::column_family::data, key);
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
        const auto v = read_value(kythira::column_family::meta, key);
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

    /// Stages one item write. With `changes`, a write of a logged table that
    /// changed the item adds its change record there.
    auto write_item(const item_command& c, kythira::write_batch& batch,
                    std::vector<change_record>* changes = nullptr) const -> item_result {
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

        const std::uint64_t old_bytes = old ? item_size(*old) : 0;
        if (c.expressions.condition && !evaluate_condition(*c.expressions.condition, current)) {
            item_result r;
            r.outcome = item_result::status::condition_failed;
            r.charged_bytes = old_bytes;
            if (c.old_on_condition_failure && old) {
                r.attributes = *old;
            }
            return r;
        }

        if (c.op == item_command::kind::condition_check || c.op == item_command::kind::read_lock) {
            // Nothing to write. A read reports the item it found.
            item_result r;
            r.charged_bytes = old_bytes;
            if (c.op == item_command::kind::read_lock) {
                r.attributes = old;
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
            case item_command::kind::condition_check:
            case item_command::kind::read_lock:
            // GSI commands never reach write_item; apply routes them first.
            case item_command::kind::index_put:
            case item_command::kind::index_delete:
            case item_command::kind::index_ack:
            case item_command::kind::index_mark:
                break;
        }

        item_result r;
        r.charged_bytes = std::max<std::uint64_t>(old_bytes, next ? item_size(*next) : 0);
        if (next) {
            if (auto e = validate_index_keys(*next, c.lsis)) {
                return error_result(*e);
            }
            if (auto e = validate_global_index_keys(*next, c.gsis)) {
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
        // A table with GSIs keeps a deleted item's version, so the item's
        // versions only grow and an index never takes an older write for a
        // newer one (`global_index.hpp`).
        const bool keep_versions = !c.gsis.empty();
        if (next) {
            r.version = load_version(*key) + 1;
            std::string v;
            item_sm_detail::put_u64(v, r.version);
            batch.put(kythira::column_family::data, *key, encode_item(*next));
            batch.put(kythira::column_family::meta, *key, std::move(v));
        } else if (old) {
            batch.erase(kythira::column_family::data, *key);
            if (keep_versions) {
                std::string v;
                item_sm_detail::put_u64(v, load_version(*key) + 1);
                batch.put(kythira::column_family::meta, *key, std::move(v));
            } else {
                batch.erase(kythira::column_family::meta, *key);
            }
        }
        if (keep_versions && (next || old)) {
            record_pending(c, *key, old, next, load_version_staged(batch, *key), batch);
        }
        if (changes != nullptr && c.log_changes) {
            if (auto rec = make_change_record(c.table_id, c.schema, old, next)) {
                rec->ttl_delete = c.ttl_delete;
                rec->written_ms = c.written_ms;
                changes->push_back(std::move(*rec));
            }
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

    /// The version `batch` gives `key` (it has just been written there).
    [[nodiscard]] static auto load_version_staged(const kythira::write_batch& batch,
                                                  const std::string& key) -> std::uint64_t {
        for (const auto& op : std::views::reverse(batch.operations())) {
            if (const auto* put = std::get_if<kythira::write_batch::put_op>(&op);
                put != nullptr && put->cf == kythira::column_family::meta && put->key == key) {
                std::string_view in(put->value);
                return item_sm_detail::get_u64(in).value_or(0);
            }
        }
        return 0;
    }

    [[nodiscard]] auto load_pending_record(const std::string& key) const
        -> std::optional<gsi_pending> {
        const auto v = read_value(kythira::column_family::meta, key);
        if (!v) {
            return std::nullopt;
        }
        auto p = decode_gsi_pending(*v);
        if (!p) {
            throw kythira::persistent_storage_error(
                "dynamo_item_state_machine: corrupt pending index change");
        }
        return p;
    }

    /// Records that `c`'s write (from `old` to `next`, now at `version`)
    /// has not reached the table's GSIs. A first record names the entries
    /// the indexes hold now, to be removed; a later write before
    /// propagation keeps them and raises the version. A write that changes
    /// no entry and finds no record leaves none.
    auto record_pending(const item_command& c, const std::string& key,
                        const std::optional<item>& old, const std::optional<item>& next,
                        std::uint64_t version, kythira::write_batch& batch) const -> void {
        const auto pkey = gsi_pending_key(c.table_id, c.schema, c.payload);
        if (!pkey) {
            throw std::invalid_argument("dynamo_item_state_machine: command without its key");
        }
        auto record = load_pending_record(*pkey);
        if (!record) {
            bool changed = false;
            std::vector<std::string> old_entries;
            for (const auto& g : c.gsis) {
                const auto before = old ? gsi_entry_key(c.schema, g, *old) : std::nullopt;
                const auto after = next ? gsi_entry_key(c.schema, g, *next) : std::nullopt;
                if (before) {
                    old_entries.push_back(*before);
                }
                // A moved entry, or one whose projection changed.
                if (before != after ||
                    (before && gsi_entry(c.schema, g, *old) != gsi_entry(c.schema, g, *next))) {
                    changed = true;
                }
            }
            if (!changed) {
                return;
            }
            record = gsi_pending{c.table_id, key, version, std::move(old_entries)};
        } else {
            record->version = version;
        }
        batch.put(kythira::column_family::meta, *pkey, encode_gsi_pending(*record));
    }

    /// `index_put`, `index_delete` and `index_ack` (see `item_command`).
    auto index_write(const item_command& c, kythira::write_batch& batch) const -> item_result {
        if (!_range.contains(c.target)) {
            throw std::logic_error("dynamo_item_state_machine: key outside the group's range");
        }
        item_result r;
        if (c.op == item_command::kind::index_mark) {
            for (const auto& key : c.entries) {
                if (!_range.contains(key)) {
                    r.more = true;
                    continue;
                }
                const auto current = load(key);
                if (!current) {
                    continue;
                }
                const auto pkey = gsi_pending_key(c.table_id, c.schema, *current);
                if (!pkey || load_pending_record(*pkey)) {
                    continue;
                }
                batch.put(kythira::column_family::meta, *pkey,
                          encode_gsi_pending(gsi_pending{c.table_id, key, load_version(key), {}}));
                ++r.deleted;
            }
            return r;
        }
        if (c.op == item_command::kind::index_ack) {
            auto record = load_pending_record(c.target);
            if (!record) {
                return r;
            }
            if (record->version <= c.version) {
                batch.erase(kythira::column_family::meta, c.target);
            } else {
                record->old_entries = c.entries;
                batch.put(kythira::column_family::meta, c.target, encode_gsi_pending(*record));
            }
            return r;
        }
        if (c.version <= load_version(c.target)) {
            // Replayed, retried, or overtaken by a newer write.
            return r;
        }
        std::string v;
        item_sm_detail::put_u64(v, c.version);
        batch.put(kythira::column_family::meta, c.target, std::move(v));
        if (c.op == item_command::kind::index_put) {
            batch.put(kythira::column_family::data, c.target, encode_item(c.payload));
        } else {
            batch.erase(kythira::column_family::data, c.target);
        }
        r.version = c.version;
        return r;
    }

    /// Reads the keys of the pending GSI changes in this group's range.
    auto load_pending() -> void {
        _pending.clear();
        if (_engine == nullptr) {
            return;
        }
        engine().scan(kythira::column_family::meta, _range, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view v) {
                          if (is_gsi_pending_value(v)) {
                              _pending.emplace(k);
                          }
                          return true;
                      });
    }

    /// Follows the pending changes a written batch added or removed.
    auto note_pending(const kythira::write_batch& batch) -> void {
        for (const auto& op : batch.operations()) {
            if (const auto* put = std::get_if<kythira::write_batch::put_op>(&op)) {
                if (put->cf == kythira::column_family::meta && is_gsi_pending_value(put->value)) {
                    _pending.insert(put->key);
                }
            } else if (const auto* erase = std::get_if<kythira::write_batch::erase_op>(&op)) {
                if (erase->cf == kythira::column_family::meta) {
                    _pending.erase(erase->key);
                }
            }
        }
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
        // Then the keys with no item beside them: collection sizes, pending
        // GSI changes, and the versions deleted items and index entries
        // keep. Versions of items that were there went with them; the
        // group's applied index (at the range start) stays.
        engine().scan(kythira::column_family::meta, mine, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (k == _range.start || k == change_log_state_key(_range.start) ||
                              (classify_item_key(k, c.schema.partition_type) &&
                               engine().get(kythira::column_family::data, std::string(k)))) {
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

    /// The checkpoint file naming the open stream's active segment.
    static constexpr std::string_view k_active_segment_file = "ddb-change-log-active";

    /// Reads the group's change log state; after an install, also drops a
    /// local tail past it and fetches the active segment the snapshot
    /// named when this node lacks it. On open, a tail past the state is a
    /// write whose entry the engine never made durable: the Raft log
    /// replays it.
    auto load_change_log(bool installed,
                         const std::optional<kythira::record_segment_descriptor>& active = {})
        -> void {
        _log_state.reset();
        _staged_changes.clear();
        if (const auto v =
                engine().get(kythira::column_family::meta, change_log_state_key(_range.start))) {
            _log_state = decode_change_log_state(*v);
            if (!_log_state) {
                throw kythira::persistent_storage_error(
                    "dynamo_item_state_machine: corrupt change log state");
            }
        }
        if (_log == nullptr || !_log_state) {
            return;
        }
        const auto& cur = _log_state->current;
        _log->truncate_after(cur.key, cur.last_seq >= cur.first_seq
                                          ? std::optional<std::uint64_t>(cur.last_seq)
                                          : std::nullopt);
        if (installed && active && active->_stream == cur.key &&
            active->_last_seq == cur.last_seq && !_log->has_segment(*active)) {
            if (!_fetch) {
                throw kythira::record_segment_error(
                    "dynamo_item_state_machine: the snapshot names an active segment of " +
                    cur.key + " this node lacks, and no fetcher is set");
            }
            _fetch(*active);
        }
    }

    [[nodiscard]] static auto read_active_file(const std::filesystem::path& path,
                                               const kythira::checkpoint_file& file)
        -> kythira::record_segment_descriptor {
        const auto [digest, size] = kythira::sha256_of_file(path);
        if (size != file.size || digest != file.sha256) {
            throw kythira::persistent_storage_error(
                "dynamo_item_state_machine: the active segment file does not match its manifest");
        }
        std::ifstream in(path, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        change_log_detail::cursor c(body);
        auto d = change_log_detail::read_descriptor(c);
        if (!d || !c.done()) {
            throw kythira::persistent_storage_error(
                "dynamo_item_state_machine: corrupt active segment file");
        }
        return *d;
    }

    /// The open stream's active segment as the local store has it.
    [[nodiscard]] auto active_descriptor() const
        -> std::optional<kythira::record_segment_descriptor> {
        if (_log == nullptr || !_log_state) {
            return std::nullopt;
        }
        const auto& cur = _log_state->current;
        const auto first = cur.sealed.empty() ? cur.first_seq : cur.sealed.back()._last_seq + 1;
        if (cur.last_seq < first) {
            return std::nullopt;
        }
        return _log->describe(cur.key, first);
    }

    /// `s` closed: its active segment sealed in the local store (every
    /// replica seals at the same entry, so at the same record).
    [[nodiscard]] auto close_stream(log_stream s) const -> log_stream {
        s.active.reset();
        const auto first = s.sealed.empty() ? s.first_seq : s.sealed.back()._last_seq + 1;
        if (_log != nullptr && s.last_seq >= first) {
            if (auto d = _log->seal(s.key)) {
                s.sealed.push_back(std::move(*d));
            }
        }
        return s;
    }

    [[nodiscard]] auto close_current() const -> log_stream {
        return close_stream(_log_state->current);
    }

    /// The state of a group that continues `parents` (now closed) in a new
    /// stream; the caller names the stream.
    [[nodiscard]] static auto successor_state(const std::vector<log_stream>& parents,
                                              std::uint64_t epoch, std::uint64_t last_ms,
                                              std::vector<log_stream> closed) -> change_log_state {
        change_log_state next;
        next.epoch = epoch;
        next.last_ms = last_ms;
        next.closed = std::move(closed);
        for (const auto& p : parents) {
            next.current.parents.push_back(p.key);
            next.closed.push_back(p);
        }
        return next;
    }

    /// Appends the records the staged entry made to the open stream (the
    /// group's first logged write opens epoch 0), fsyncs them, and stages
    /// the new state. The records reach disk before the engine batch, so
    /// the engine never holds a state whose records were lost; a record
    /// the engine missed is truncated on open and replayed.
    auto append_changes(kythira::write_batch& batch) -> void {
        if (!_log_state) {
            _log_state.emplace();
            _log_state->current.key = change_stream_key(_range.start, 0);
        }
        auto& st = *_log_state;
        std::vector<std::string> bodies;
        std::vector<kythira::segment_record_view> views;
        bodies.reserve(_staged_changes.size());
        views.reserve(_staged_changes.size());
        for (auto& rec : _staged_changes) {
            rec.written_ms = std::max(rec.written_ms, st.last_ms);
            st.last_ms = rec.written_ms;
            bodies.push_back(encode_change_record(rec));
        }
        const auto first = st.current.last_seq + 1;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            views.push_back(kythira::segment_record_view{
                first + i, static_cast<std::int64_t>(_staged_changes[i].written_ms),
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(bodies[i].data()),
                                           bodies[i].size())});  // NOLINT
        }
        st.current.last_seq += bodies.size();
        st.current.active.reset();
        if (_log != nullptr) {
            const auto r = _log->append(st.current.key, views);
            st.current.sealed.insert(st.current.sealed.end(), r._sealed.begin(), r._sealed.end());
            _log->barrier();
        }
        batch.put(kythira::column_family::meta, change_log_state_key(_range.start),
                  encode_change_log_state(st));
        _staged_changes.clear();
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
    /// The meta keys of this group's pending GSI changes.
    std::set<std::string> _pending;
    /// Data and meta writes staged into the open batch (see `stage`).
    std::map<std::pair<kythira::column_family, std::string>, std::optional<std::string>> _staged;
    /// The node's change record store (task 15), or none.
    kythira::record_segment_store* _log = nullptr;
    change_log_fetcher _fetch;
    std::optional<change_log_state> _log_state;
    /// The change records of the open batch, appended at commit.
    std::vector<change_record> _staged_changes;
};

static_assert(kythira::persistent_staged_state_machine<dynamo_item_state_machine<>>);
static_assert(kythira::txn_validating_state_machine<dynamo_item_state_machine<>, std::string>);

}  // namespace kythira::dynamodb
