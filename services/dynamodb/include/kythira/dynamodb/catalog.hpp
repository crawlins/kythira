// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file catalog.hpp
/// @brief DynamoDB's table catalog: descriptors, status transitions, table
///        ids and background jobs (`.kiro/specs/dynamodb-compatible-api/`
///        task 6, Requirements 7.2-7.4).
///
/// The catalog is the reserved range `\0meta/dynamodb/` of the node's
/// `kv_engine`, small enough for one group, and changes only through Raft
/// entries (`catalog_command`), so it is linearizable (7.2). Each table has
/// a versioned descriptor under `table/<name>`.
///
/// - Table ids come from a counter under `next-table-id` that only grows,
///   written in the same batch as the descriptor, so an id is never reused
///   and a recreated table never sees an old table's data (7.4).
/// - Status transitions are entries. `CreateTable` writes `CREATING`, and
///   `UpdateTable` writes `UPDATING` when the change needs work. The catalog
///   leader then proposes `set_status` once the work is done. SDK waiters
///   poll that status.
/// - `DeleteTable` marks the descriptor `DELETING` and records a delete job
///   under `job/<id>`. The leader drives the job with bounded
///   `drop_table_step` entries on the item groups and records each step as
///   a `job_progress` entry, so a new leader resumes where the old one
///   stopped (7.3). The last step erases the job and the descriptor, and
///   only then may the name be reused.
///
/// A follow-on entry names the table id it acts on and does nothing when
/// the stored table has another id, so a stale entry from a former leader
/// cannot touch a recreated table.
///
/// Global secondary indexes (task 11) take their ids from the same counter
/// as tables, so an index key space is never reused either. Deleting a
/// table records one delete job per index beside the table's own; an
/// index's job drops its key space and never touches the descriptor.

#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

enum class table_status : std::uint8_t {
    creating = 0,
    active = 1,
    updating = 2,
    deleting = 3,
};

[[nodiscard]] inline auto status_name(table_status s) -> std::string_view {
    switch (s) {
        case table_status::creating:
            return "CREATING";
        case table_status::active:
            return "ACTIVE";
        case table_status::updating:
            return "UPDATING";
        case table_status::deleting:
            return "DELETING";
    }
    return "ACTIVE";
}

enum class billing_mode : std::uint8_t {
    provisioned = 0,
    pay_per_request = 1,
};

enum class table_class : std::uint8_t {
    standard = 0,
    standard_infrequent_access = 1,
};

/// A table's time to live (task 13): which attribute holds each item's
/// expiry, in seconds since the epoch.
struct time_to_live {
    bool enabled = false;
    /// The attribute while enabled; empty while disabled.
    std::string attribute;
    /// When `UpdateTimeToLive` last changed it, from the entry; 0 never.
    std::uint64_t changed_ms = 0;

    friend auto operator==(const time_to_live&, const time_to_live&) -> bool = default;
};

/// What the catalog knows about a table. Later tasks add streams and
/// replicas; each new field gets a new descriptor format version.
struct table_descriptor {
    std::string name;
    std::uint64_t table_id = 0;
    key_schema schema;
    table_status status = table_status::creating;
    /// Milliseconds since the epoch, from the entry (never the replica's
    /// clock).
    std::uint64_t created_ms = 0;
    billing_mode billing = billing_mode::provisioned;
    std::uint64_t read_capacity = 0;
    std::uint64_t write_capacity = 0;
    table_class storage_class = table_class::standard;
    bool deletion_protection = false;
    /// The warm throughput the table was asked for, in units per second;
    /// 0 when never set. `DescribeTable` shows at least AWS's defaults.
    std::uint64_t warm_read = 0;
    std::uint64_t warm_write = 0;
    /// Local secondary indexes, fixed at creation (task 9).
    std::vector<local_index> lsis;
    /// Global secondary indexes (task 11).
    std::vector<global_index> gsis;
    std::vector<std::pair<std::string, std::string>> tags;
    /// Bumped by every entry that changes the descriptor.
    std::uint64_t revision = 0;
    /// Time to live (task 13).
    time_to_live ttl;

    friend auto operator==(const table_descriptor&, const table_descriptor&) -> bool = default;
};

/// A background delete in progress.
struct delete_job {
    std::uint64_t table_id = 0;
    std::string name;
    key_schema schema;
    std::uint64_t deleted = 0;

    friend auto operator==(const delete_job&, const delete_job&) -> bool = default;
};

/// One change to the catalog.
struct catalog_command {
    enum class kind : std::uint8_t {
        create_table = 1,
        set_status = 2,
        update_table = 3,
        delete_table = 4,
        job_progress = 5,
        /// `TagResource`: `table.tags` are added or overwrite same-keyed
        /// tags.
        tag_resource = 6,
        /// `UntagResource`: the keys of `table.tags` are removed.
        untag_resource = 7,
        /// `UpdateTimeToLive` (task 13): `table.ttl` holds the request's
        /// `enabled` and `attribute`. Refused as AWS refuses it: enabling
        /// what is enabled, disabling what is disabled, naming another
        /// attribute while enabled, or a change less than
        /// `ttl_min_interval_ms` after the last one.
        set_ttl = 8,
    };

    /// Which settings an `update_table` changes.
    enum update_field : std::uint8_t {
        update_billing = 1U << 0U,
        update_throughput = 1U << 1U,
        update_class = 1U << 2U,
        update_deletion_protection = 1U << 3U,
        update_warm_throughput = 1U << 4U,
        /// `GlobalSecondaryIndexUpdates`: `table.gsis` is the list the
        /// table should have. A name the table lacks is created (CREATING,
        /// with a new key space), a name the command lacks is deleted
        /// (DELETING, with a delete job for its key space), an index whose
        /// command copy is ACTIVE while it is CREATING becomes ACTIVE (the
        /// backfill finished), and a provisioned index takes the command's
        /// throughput. One index may be created or deleted at a time.
        update_gsis = 1U << 5U,
    };

    kind op = kind::create_table;
    /// `create_table`: the new table (its id, status, revision and creation
    /// time are the catalog's to set); `update_table`: the name and the new
    /// settings; `set_status` and `job_progress`: name and `table_id`.
    table_descriptor table;
    std::uint8_t update_fields = 0;
    /// `set_status`: the status to leave and the one to enter.
    table_status from = table_status::creating;
    table_status to = table_status::active;
    std::uint64_t now_ms = 0;
    /// `job_progress`: items deleted by the step, and whether the table is
    /// now empty on every group.
    std::uint64_t deleted = 0;
    bool done = false;
    /// `set_ttl`: the shortest time allowed between two changes (AWS: an
    /// hour); 0 allows any.
    std::uint64_t ttl_min_interval_ms = 0;

    friend auto operator==(const catalog_command&, const catalog_command&) -> bool = default;
};

/// What `apply()` returns for a `catalog_command`.
struct catalog_result {
    /// Empty on success, else the error type (a full Coral name) and
    /// message for the client.
    std::string error_type;
    std::string error_message;
    /// The descriptor after the entry, when the table still exists.
    std::optional<table_descriptor> table;

    [[nodiscard]] auto ok() const -> bool { return error_type.empty(); }

    friend auto operator==(const catalog_result&, const catalog_result&) -> bool = default;
};

namespace catalog_detail {

/// 2 added `gsis` (task 11) to the descriptor and to the commands and
/// results that carry one; 3 added `ttl` after the revision (task 13), and
/// the `set_ttl` command with its interval. Older versions still decode.
inline constexpr std::uint8_t k_descriptor_format_version = 3;
inline constexpr std::uint8_t k_job_format_version = 1;
inline constexpr std::uint8_t k_command_format_version = 3;
inline constexpr std::uint8_t k_result_format_version = 3;

[[nodiscard]] inline auto table_key(std::string_view name) -> std::string {
    return std::string(k_catalog_prefix) + "table/" + std::string(name);
}
[[nodiscard]] inline auto tables_range() -> kythira::key_range {
    return prefix_range(std::string(k_catalog_prefix) + "table/");
}
[[nodiscard]] inline auto job_key(std::uint64_t table_id) -> std::string {
    // Big-endian, so jobs run oldest first.
    std::string out = std::string(k_catalog_prefix) + "job/";
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((table_id >> shift) & 0xFFU));
    }
    return out;
}
[[nodiscard]] inline auto jobs_range() -> kythira::key_range {
    return prefix_range(std::string(k_catalog_prefix) + "job/");
}
[[nodiscard]] inline auto next_id_key() -> std::string {
    return std::string(k_catalog_prefix) + "next-table-id";
}

[[nodiscard]] inline auto u64_string(std::uint64_t v) -> std::string {
    std::string out;
    item_sm_detail::put_u64(out, v);
    return out;
}

[[nodiscard]] inline auto get_flag(detail::reader& r) -> std::optional<bool> {
    const auto b = r.byte();
    if (!b || *b > 1) {
        return std::nullopt;
    }
    return *b == 1;
}

inline void put_descriptor_body(std::string& out, const table_descriptor& d) {
    detail::put_bytes(out, d.name);
    detail::put_varint(out, d.table_id);
    item_sm_detail::put_schema(out, d.schema);
    out.push_back(static_cast<char>(d.status));
    detail::put_varint(out, d.created_ms);
    out.push_back(static_cast<char>(d.billing));
    detail::put_varint(out, d.read_capacity);
    detail::put_varint(out, d.write_capacity);
    out.push_back(static_cast<char>(d.storage_class));
    out.push_back(d.deletion_protection ? '\1' : '\0');
    detail::put_varint(out, d.warm_read);
    detail::put_varint(out, d.warm_write);
    put_local_indexes(out, d.lsis);
    put_global_indexes(out, d.gsis);
    detail::put_varint(out, d.tags.size());
    for (const auto& [k, v] : d.tags) {
        detail::put_bytes(out, k);
        detail::put_bytes(out, v);
    }
    detail::put_varint(out, d.revision);
    out.push_back(d.ttl.enabled ? '\1' : '\0');
    detail::put_bytes(out, d.ttl.attribute);
    detail::put_varint(out, d.ttl.changed_ms);
}

/// `version` is the format of the enclosing form (descriptor, command or
/// result), all of which gained `gsis` at 2 and `ttl` at 3.
[[nodiscard]] inline auto get_descriptor_body(detail::reader& r, std::uint8_t version)
    -> std::optional<table_descriptor> {
    table_descriptor d;
    auto name = r.bytes();
    const auto id = r.varint();
    auto schema = item_sm_detail::get_schema(r);
    const auto status = r.byte();
    const auto created = r.varint();
    const auto billing = r.byte();
    const auto rcu = r.varint();
    const auto wcu = r.varint();
    const auto cls = r.byte();
    const auto protection = get_flag(r);
    const auto warm_read = r.varint();
    const auto warm_write = r.varint();
    auto lsis = get_local_indexes(r);
    std::optional<std::vector<global_index>> gsis;
    if (lsis) {
        gsis = version >= 2 ? get_global_indexes(r) : std::vector<global_index>{};
    }
    const auto tag_count = gsis ? r.count() : std::nullopt;
    if (!name || !id || !schema || !status || *status > 3 || !created || !billing || *billing > 1 ||
        !rcu || !wcu || !cls || *cls > 1 || !protection || !warm_read || !warm_write || !lsis ||
        !tag_count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *tag_count; ++i) {
        auto k = r.bytes();
        auto v = r.bytes();
        if (!k || !v) {
            return std::nullopt;
        }
        d.tags.emplace_back(std::move(*k), std::move(*v));
    }
    const auto revision = r.varint();
    if (!revision) {
        return std::nullopt;
    }
    if (version >= 3) {
        const auto enabled = get_flag(r);
        auto attribute = r.bytes();
        const auto changed = r.varint();
        if (!enabled || !attribute || !changed) {
            return std::nullopt;
        }
        d.ttl.enabled = *enabled;
        d.ttl.attribute = std::move(*attribute);
        d.ttl.changed_ms = *changed;
    }
    d.name = std::move(*name);
    d.table_id = *id;
    d.schema = std::move(*schema);
    d.status = static_cast<table_status>(*status);
    d.created_ms = *created;
    d.billing = static_cast<billing_mode>(*billing);
    d.read_capacity = *rcu;
    d.write_capacity = *wcu;
    d.storage_class = static_cast<table_class>(*cls);
    d.deletion_protection = *protection;
    d.warm_read = *warm_read;
    d.warm_write = *warm_write;
    d.lsis = std::move(*lsis);
    d.gsis = std::move(*gsis);
    d.revision = *revision;
    return d;
}

}  // namespace catalog_detail

/// A descriptor's stored form: a format version, then its fields.
[[nodiscard]] inline auto encode_table_descriptor(const table_descriptor& d) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(catalog_detail::k_descriptor_format_version));
    catalog_detail::put_descriptor_body(out, d);
    return out;
}

[[nodiscard]] inline auto decode_table_descriptor(std::string_view bytes)
    -> std::optional<table_descriptor> {
    if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) == 0 ||
        static_cast<std::uint8_t>(bytes[0]) > catalog_detail::k_descriptor_format_version) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(1));
    auto d = catalog_detail::get_descriptor_body(r, static_cast<std::uint8_t>(bytes[0]));
    if (!d || !r.done()) {
        return std::nullopt;
    }
    return d;
}

[[nodiscard]] inline auto encode_delete_job(const delete_job& j) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(catalog_detail::k_job_format_version));
    detail::put_varint(out, j.table_id);
    detail::put_bytes(out, j.name);
    item_sm_detail::put_schema(out, j.schema);
    detail::put_varint(out, j.deleted);
    return out;
}

[[nodiscard]] inline auto decode_delete_job(std::string_view bytes) -> std::optional<delete_job> {
    if (bytes.empty() ||
        static_cast<std::uint8_t>(bytes[0]) != catalog_detail::k_job_format_version) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(1));
    delete_job j;
    const auto id = r.varint();
    auto name = r.bytes();
    auto schema = item_sm_detail::get_schema(r);
    const auto deleted = r.varint();
    if (!id || !name || !schema || !deleted || !r.done()) {
        return std::nullopt;
    }
    j.table_id = *id;
    j.name = std::move(*name);
    j.schema = std::move(*schema);
    j.deleted = *deleted;
    return j;
}

[[nodiscard]] inline auto encode_catalog_command(const catalog_command& c) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(catalog_detail::k_command_format_version));
    out.push_back(static_cast<char>(c.op));
    catalog_detail::put_descriptor_body(out, c.table);
    out.push_back(static_cast<char>(c.update_fields));
    out.push_back(static_cast<char>(c.from));
    out.push_back(static_cast<char>(c.to));
    detail::put_varint(out, c.now_ms);
    detail::put_varint(out, c.deleted);
    out.push_back(c.done ? '\1' : '\0');
    detail::put_varint(out, c.ttl_min_interval_ms);
    return out;
}

/// The command in `bytes`, or nothing when they are not one. A newer format
/// version is reported through `newer`, so the caller can stop the group
/// instead of guessing.
[[nodiscard]] inline auto decode_catalog_command(std::string_view bytes, bool& newer)
    -> std::optional<catalog_command> {
    newer = false;
    if (bytes.size() < 2) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(bytes[0]);
    if (version > catalog_detail::k_command_format_version) {
        newer = true;
        return std::nullopt;
    }
    const auto op = static_cast<std::uint8_t>(bytes[1]);
    // set_ttl came in version 3.
    if (version == 0 || op < 1 || op > (version >= 3 ? 8 : 7)) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(2));
    catalog_command c;
    c.op = static_cast<catalog_command::kind>(op);
    auto table = catalog_detail::get_descriptor_body(r, version);
    const auto fields = r.byte();
    const auto from = r.byte();
    const auto to = r.byte();
    const auto now = r.varint();
    const auto deleted = r.varint();
    const auto done = catalog_detail::get_flag(r);
    const auto interval = version >= 3 ? r.varint() : std::optional<std::uint64_t>{0};
    // update_gsis came with the descriptor's GSI list, in version 2.
    const std::uint8_t known_fields = version >= 2 ? 0x3F : 0x1F;
    if (!table || !fields || *fields > known_fields || !from || *from > 3 || !to || *to > 3 ||
        !now || !deleted || !done || !interval || !r.done()) {
        return std::nullopt;
    }
    c.table = std::move(*table);
    c.update_fields = *fields;
    c.from = static_cast<table_status>(*from);
    c.to = static_cast<table_status>(*to);
    c.now_ms = *now;
    c.deleted = *deleted;
    c.done = *done;
    c.ttl_min_interval_ms = *interval;
    return c;
}

[[nodiscard]] inline auto encode_catalog_result(const catalog_result& r) -> std::string {
    std::string out;
    out.push_back(static_cast<char>(catalog_detail::k_result_format_version));
    detail::put_bytes(out, r.error_type);
    detail::put_bytes(out, r.error_message);
    out.push_back(r.table ? '\1' : '\0');
    if (r.table) {
        catalog_detail::put_descriptor_body(out, *r.table);
    }
    return out;
}

[[nodiscard]] inline auto decode_catalog_result(std::string_view bytes)
    -> std::optional<catalog_result> {
    if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) == 0 ||
        static_cast<std::uint8_t>(bytes[0]) > catalog_detail::k_result_format_version) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(bytes[0]);
    detail::reader r(bytes.substr(1));
    catalog_result out;
    auto type = r.bytes();
    auto message = r.bytes();
    const auto has_table = catalog_detail::get_flag(r);
    if (!type || !message || !has_table) {
        return std::nullopt;
    }
    if (*has_table) {
        out.table = catalog_detail::get_descriptor_body(r, version);
        if (!out.table) {
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

/// The catalog group's state machine.
template<kythira::kv_engine Engine = kythira::memory_kv_engine, typename LogIndex = std::uint64_t>
class dynamo_catalog_state_machine {
public:
    using engine_type = Engine;

    /// Opens on the catalog range (`catalog_range()`); any other range is a
    /// host bug.
    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        if (!(range == catalog_range())) {
            throw std::invalid_argument("dynamo_catalog_state_machine: not the catalog range");
        }
        _engine = &engine;
        _ring.clear();
        _applied = 0;
        if (const auto v = _engine->get(kythira::column_family::meta, applied_key())) {
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
        kythira::write_batch batch;
        auto out = stage(entry, batch);
        commit_staged(batch, index);
        return out;
    }

    // -- persistent_staged_state_machine --------------------------------------
    //
    // The catalog takes part in no transaction, but its group runs the same
    // `transactional_state_machine` wrapper as the item groups (a host runs
    // one state machine type), which drives it through these.

    /// Stages one command's effects into `batch` and returns its result.
    auto stage(const std::vector<std::byte>& entry, kythira::write_batch& batch)
        -> std::vector<std::byte> {
        bool newer = false;
        const auto c = decode_catalog_command(
            std::string_view(reinterpret_cast<const char*>(entry.data()), entry.size()),  // NOLINT
            newer);
        if (!c) {
            if (newer) {
                throw std::runtime_error(
                    "dynamo_catalog_state_machine: an entry uses a newer catalog format; upgrade "
                    "this node");
            }
            throw std::invalid_argument("dynamo_catalog_state_machine: malformed entry");
        }
        catalog_result result;
        switch (c->op) {
            case catalog_command::kind::create_table:
                result = create_table(*c, batch);
                break;
            case catalog_command::kind::set_status:
                result = set_status(*c, batch);
                break;
            case catalog_command::kind::update_table:
                result = update_table(*c, batch);
                break;
            case catalog_command::kind::delete_table:
                result = delete_table(*c, batch);
                break;
            case catalog_command::kind::job_progress:
                result = job_progress(*c, batch);
                break;
            case catalog_command::kind::tag_resource:
            case catalog_command::kind::untag_resource:
                result = retag(*c, batch);
                break;
            case catalog_command::kind::set_ttl:
                result = set_ttl(*c, batch);
                break;
        }
        const auto out = encode_catalog_result(result);
        std::vector<std::byte> bytes(out.size());
        std::memcpy(bytes.data(), out.data(), out.size());
        return bytes;
    }

    /// Writes `batch` with the applied index `index`.
    auto commit_staged(kythira::write_batch& batch, LogIndex index) -> void {
        batch.put(kythira::column_family::meta, applied_key(),
                  catalog_detail::u64_string(static_cast<std::uint64_t>(index)));
        const auto seq = engine().write(batch);
        _applied = index;
        _ring.emplace_back(index, seq);
        trim_ring();
    }

    /// Nothing is kept between `stage` and `commit_staged`.
    auto discard_staged() -> void {}

    [[nodiscard]] auto key_prefix() const -> const std::string& {
        static const std::string none;
        return none;
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
        return engine().checkpoint(catalog_range(), dir);
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                LogIndex index) -> void {
        engine().ingest(catalog_range(), dir, manifest);
        _applied = index;
        _durable_floor = index;
        _ring.clear();
    }

    auto drop() -> void { engine().drop(catalog_range()); }

    // -- Reads ----------------------------------------------------------------

    [[nodiscard]] auto describe(std::string_view name) const -> std::optional<table_descriptor> {
        const auto v = engine().get(kythira::column_family::data, catalog_detail::table_key(name));
        if (!v) {
            return std::nullopt;
        }
        auto d = decode_table_descriptor(*v);
        if (!d) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt table");
        }
        return d;
    }

    /// The table with id `table_id`, or nothing. Reads every descriptor;
    /// callers cache the answer.
    [[nodiscard]] auto describe_id(std::uint64_t table_id) const
        -> std::optional<table_descriptor> {
        std::optional<table_descriptor> out;
        engine().scan(kythira::column_family::data, catalog_detail::tables_range(),
                      kythira::scan_direction::forward, [&](std::string_view, std::string_view v) {
                          auto d = decode_table_descriptor(v);
                          if (!d) {
                              throw kythira::persistent_storage_error(
                                  "dynamo_catalog_state_machine: corrupt table");
                          }
                          if (d->table_id == table_id) {
                              out = std::move(d);
                              return false;
                          }
                          return true;
                      });
        return out;
    }

    /// Table names in order, after `exclusive_start` (`ListTables`),
    /// at most `limit` of them.
    [[nodiscard]] auto list_tables(std::string_view exclusive_start, std::size_t limit) const
        -> std::vector<std::string> {
        std::vector<std::string> out;
        auto range = catalog_detail::tables_range();
        const auto prefix_size = range.start.size();
        if (!exclusive_start.empty()) {
            // The smallest key after `table/<start>`.
            range.start = catalog_detail::table_key(exclusive_start) + std::string(1, '\0');
        }
        engine().scan(kythira::column_family::data, range, kythira::scan_direction::forward,
                      [&](std::string_view k, std::string_view) {
                          if (out.size() == limit) {
                              return false;
                          }
                          out.emplace_back(k.substr(prefix_size));
                          return true;
                      });
        return out;
    }

    /// Delete jobs in progress, oldest first: what the catalog leader drives.
    [[nodiscard]] auto delete_jobs() const -> std::vector<delete_job> {
        std::vector<delete_job> out;
        engine().scan(kythira::column_family::data, catalog_detail::jobs_range(),
                      kythira::scan_direction::forward, [&](std::string_view, std::string_view v) {
                          auto j = decode_delete_job(v);
                          if (!j) {
                              throw kythira::persistent_storage_error(
                                  "dynamo_catalog_state_machine: corrupt job");
                          }
                          out.push_back(std::move(*j));
                          return true;
                      });
        return out;
    }

    /// The id the next table will get.
    [[nodiscard]] auto next_table_id() const -> std::uint64_t {
        const auto v = engine().get(kythira::column_family::data, catalog_detail::next_id_key());
        if (!v) {
            return 1;
        }
        std::string_view in(*v);
        const auto id = item_sm_detail::get_u64(in);
        if (!id || !in.empty()) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt id");
        }
        return *id;
    }

private:
    [[nodiscard]] auto engine() const -> engine_type& {
        if (_engine == nullptr) {
            throw std::logic_error("dynamo_catalog_state_machine: not opened");
        }
        return *_engine;
    }

    [[nodiscard]] static auto applied_key() -> const std::string& {
        static const std::string key = catalog_range().start;
        return key;
    }

    static auto failure(std::string_view type, std::string message) -> catalog_result {
        catalog_result r;
        r.error_type = dynamodb_error_style().service_prefix + std::string(type);
        r.error_message = std::move(message);
        return r;
    }

    static auto not_found(std::string_view name) -> catalog_result {
        return failure("ResourceNotFoundException",
                       "Requested resource not found: Table: " + std::string(name) + " not found");
    }

    static auto in_use(const table_descriptor& d) -> catalog_result {
        std::string_view doing = "updated";
        if (d.status == table_status::creating) {
            doing = "created";
        } else if (d.status == table_status::deleting) {
            doing = "deleted";
        }
        return failure("ResourceInUseException",
                       "Attempt to change a resource which is still in use: Table is being " +
                           std::string(doing) + ": " + d.name);
    }

    static auto store(kythira::write_batch& batch, const table_descriptor& d) -> void {
        batch.put(kythira::column_family::data, catalog_detail::table_key(d.name),
                  encode_table_descriptor(d));
    }

    auto create_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        if (describe(c.table.name)) {
            return failure("ResourceInUseException", "Table already exists: " + c.table.name);
        }
        table_descriptor d = c.table;
        // One value per tag key, in key order, as TagResource keeps them.
        const std::map<std::string, std::string> tags(d.tags.begin(), d.tags.end());
        d.tags.assign(tags.begin(), tags.end());
        d.table_id = next_table_id();
        std::uint64_t next = d.table_id + 1;
        for (auto& g : d.gsis) {
            g.index_id = next++;
            g.status = index_status::active;
        }
        d.status = table_status::creating;
        d.created_ms = c.now_ms;
        d.revision = 1;
        batch.put(kythira::column_family::data, catalog_detail::next_id_key(),
                  catalog_detail::u64_string(next));
        store(batch, d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto set_status(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || d->table_id != c.table.table_id) {
            return not_found(c.table.name);
        }
        // A transition from a status the table has left is a stale or
        // repeated entry: report the table as it is, change nothing.
        if (d->status == c.from && c.from != c.to) {
            d->status = c.to;
            ++d->revision;
            store(batch, *d);
        }
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto update_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        bool needs_work = false;
        if ((c.update_fields & catalog_command::update_billing) != 0 &&
            d->billing != c.table.billing) {
            d->billing = c.table.billing;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_throughput) != 0 &&
            (d->read_capacity != c.table.read_capacity ||
             d->write_capacity != c.table.write_capacity)) {
            d->read_capacity = c.table.read_capacity;
            d->write_capacity = c.table.write_capacity;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_class) != 0 &&
            d->storage_class != c.table.storage_class) {
            d->storage_class = c.table.storage_class;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_warm_throughput) != 0 &&
            (d->warm_read != c.table.warm_read || d->warm_write != c.table.warm_write)) {
            d->warm_read = c.table.warm_read;
            d->warm_write = c.table.warm_write;
            needs_work = true;
        }
        if ((c.update_fields & catalog_command::update_gsis) != 0) {
            if (auto refused = update_gsis(c, *d, batch)) {
                return *refused;
            }
        }
        if ((c.update_fields & catalog_command::update_deletion_protection) != 0) {
            // Takes effect at once and needs no background work, so the
            // table stays ACTIVE.
            d->deletion_protection = c.table.deletion_protection;
        }
        if (d->billing == billing_mode::pay_per_request) {
            d->read_capacity = 0;
            d->write_capacity = 0;
        }
        if (needs_work) {
            d->status = table_status::updating;
        }
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `update_gsis` (see `catalog_command::update_field`) applied to `d`.
    /// Returns the refusal, if any; on success `d` holds the new list.
    auto update_gsis(const catalog_command& c, table_descriptor& d,
                     kythira::write_batch& batch) const -> std::optional<catalog_result> {
        const auto named = [](const std::vector<global_index>& list,
                              const std::string& name) -> const global_index* {
            const auto it = std::find_if(list.begin(), list.end(),
                                         [&](const global_index& g) { return g.name == name; });
            return it == list.end() ? nullptr : &*it;
        };
        std::vector<global_index> added;
        std::vector<std::string> removed;
        for (const auto& g : c.table.gsis) {
            if (named(d.gsis, g.name) == nullptr) {
                added.push_back(g);
            }
        }
        for (const auto& g : d.gsis) {
            if (named(c.table.gsis, g.name) == nullptr) {
                removed.push_back(g.name);
            }
        }
        const bool busy = std::any_of(d.gsis.begin(), d.gsis.end(), [](const global_index& g) {
            return g.status != index_status::active;
        });
        if (added.size() + removed.size() > 1 || (busy && added.size() + removed.size() == 1)) {
            return failure("LimitExceededException",
                           "Subscriber limit exceeded: Only 1 online index can be created or "
                           "deleted simultaneously per table");
        }
        if (!added.empty() && d.gsis.size() >= k_max_global_indexes) {
            return failure("LimitExceededException",
                           "GlobalSecondaryIndex count exceeds the per-table limit");
        }
        for (auto& g : d.gsis) {
            const auto* want = named(c.table.gsis, g.name);
            if (want == nullptr) {
                if (g.status == index_status::active) {
                    g.status = index_status::deleting;
                    batch.put(kythira::column_family::data, catalog_detail::job_key(g.index_id),
                              encode_delete_job(delete_job{g.index_id, d.name, g.schema, 0}));
                }
                continue;
            }
            if (g.status == index_status::creating && want->status == index_status::active) {
                g.status = index_status::active;
            }
            if (g.has_throughput && want->has_throughput) {
                g.read_capacity = want->read_capacity;
                g.write_capacity = want->write_capacity;
            }
        }
        if (!added.empty()) {
            auto g = added.front();
            const auto id = next_table_id();
            g.index_id = id;
            g.status = index_status::creating;
            batch.put(kythira::column_family::data, catalog_detail::next_id_key(),
                      catalog_detail::u64_string(id + 1));
            d.gsis.push_back(std::move(g));
        }
        return std::nullopt;
    }

    auto delete_table(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status != table_status::active) {
            return in_use(*d);
        }
        if (d->deletion_protection) {
            catalog_result r;
            r.error_type = dynamodb_error_style().validation_type;
            r.error_message =
                "Resource cannot be deleted as it is currently protected against deletion. "
                "Disable deletion protection first.";
            return r;
        }
        d->status = table_status::deleting;
        ++d->revision;
        store(batch, *d);
        batch.put(kythira::column_family::data, catalog_detail::job_key(d->table_id),
                  encode_delete_job(delete_job{d->table_id, d->name, d->schema, 0}));
        for (const auto& g : d->gsis) {
            batch.put(kythira::column_family::data, catalog_detail::job_key(g.index_id),
                      encode_delete_job(delete_job{g.index_id, d->name, g.schema, 0}));
        }
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    auto job_progress(const catalog_command& c, kythira::write_batch& batch) const
        -> catalog_result {
        const auto key = catalog_detail::job_key(c.table.table_id);
        const auto v = engine().get(kythira::column_family::data, key);
        if (!v) {
            // Already finished (a repeated last step).
            return {};
        }
        auto job = decode_delete_job(*v);
        if (!job) {
            throw kythira::persistent_storage_error("dynamo_catalog_state_machine: corrupt job");
        }
        job->deleted += c.deleted;
        catalog_result r;
        if (c.done) {
            batch.erase(kythira::column_family::data, key);
            auto d = describe(job->name);
            if (d && d->table_id == job->table_id) {
                batch.erase(kythira::column_family::data, catalog_detail::table_key(job->name));
            } else if (d) {
                // A deleted index's key space is empty: the index goes.
                const auto n = std::erase_if(d->gsis, [&](const global_index& g) {
                    return g.index_id == job->table_id && g.status == index_status::deleting;
                });
                if (n > 0) {
                    ++d->revision;
                    store(batch, *d);
                }
            }
            return r;
        }
        batch.put(kythira::column_family::data, key, encode_delete_job(*job));
        r.table = describe(job->name);
        return r;
    }

    /// Tags in key order. AWS keeps one value per key; a deleting table
    /// cannot be tagged.
    auto retag(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d || (c.table.table_id != 0 && d->table_id != c.table.table_id)) {
            return not_found(c.table.name);
        }
        if (d->status == table_status::deleting) {
            return in_use(*d);
        }
        std::map<std::string, std::string> tags(d->tags.begin(), d->tags.end());
        for (const auto& [k, v] : c.table.tags) {
            if (c.op == catalog_command::kind::tag_resource) {
                tags[k] = v;
            } else {
                tags.erase(k);
            }
        }
        d->tags.assign(tags.begin(), tags.end());
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    /// `UpdateTimeToLive`, with DynamoDB's refusals in its words. The
    /// checks run in DynamoDB Local's order: the state first, then the
    /// interval.
    auto set_ttl(const catalog_command& c, kythira::write_batch& batch) const -> catalog_result {
        auto d = describe(c.table.name);
        if (!d) {
            return not_found(c.table.name);
        }
        if (d->status == table_status::deleting) {
            return in_use(*d);
        }
        const auto& want = c.table.ttl;
        if (d->ttl.enabled && want.attribute != d->ttl.attribute) {
            return invalid("TimeToLive is active on a different AttributeName");
        }
        if (d->ttl.enabled == want.enabled) {
            return invalid(want.enabled ? "TimeToLive is already enabled"
                                        : "TimeToLive is already disabled");
        }
        if (c.ttl_min_interval_ms != 0 && d->ttl.changed_ms != 0 &&
            c.now_ms < d->ttl.changed_ms + c.ttl_min_interval_ms) {
            return invalid("Time to live has been modified multiple times within a fixed interval");
        }
        d->ttl.enabled = want.enabled;
        d->ttl.attribute = want.enabled ? want.attribute : std::string{};
        d->ttl.changed_ms = c.now_ms;
        ++d->revision;
        store(batch, *d);
        catalog_result r;
        r.table = std::move(d);
        return r;
    }

    static auto invalid(std::string message) -> catalog_result {
        catalog_result r;
        r.error_type = dynamodb_error_style().validation_type;
        r.error_message = std::move(message);
        return r;
    }

    auto trim_ring() -> void {
        const auto durable = engine().durable_sequence();
        while (_ring.size() > 1 && _ring[1].second <= durable) {
            _durable_floor = _ring.front().first;
            _ring.pop_front();
        }
    }

    engine_type* _engine = nullptr;
    LogIndex _applied{};
    LogIndex _durable_floor{};
    std::deque<std::pair<LogIndex, kythira::engine_sequence>> _ring;
};

static_assert(kythira::persistent_state_machine<dynamo_catalog_state_machine<>>);

/// The `drop_table_step` entry the catalog leader proposes to an item group
/// for a delete job, deleting at most `max_keys` items.
[[nodiscard]] inline auto drop_step_for(const delete_job& job, std::uint32_t max_keys)
    -> item_command {
    item_command c;
    c.op = item_command::kind::drop_table_step;
    c.table_id = job.table_id;
    c.schema = job.schema;
    c.max_keys = max_keys;
    return c;
}

}  // namespace kythira::dynamodb
