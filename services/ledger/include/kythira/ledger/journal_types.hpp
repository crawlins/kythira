// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What a ledger transaction, revision and block look like, in Ion, and how
// each is hashed (.kiro/specs/verifiable-ledger/ task 3, Requirements 3.1-3.4
// and 5.1-5.5; design "Hashing").
//
// A transaction reaches the ledger group as one Raft entry:
//
//   { type: ledger_txn, txId: "<22 base62>", blockTimestamp: <timestamp>,
//     seq: <block seq>, previousBlockHash: {{...}}, blockHash: {{...}},
//     statements: [ <statement>, ... ], readSet: <read set, optional> }
//
// The leader fills in seq, previousBlockHash and blockHash from its own
// applied state (ledger_state_machine::prepare). Every replica recomputes
// the block; the seq and previous hash tell a stale proposal (the leader's
// view moved on before the entry applied: rejected, nothing written) from a
// real divergence (same inputs, different hash: the group stops).
//
// A statement is an Ion struct with a symbol `op`:
//
//   {op: create_table, table: "t"}       {op: drop_table, table: "t"}
//   {op: undrop_table, table: "t"}
//   {op: create_index, table: "t", index: "by_vin", path: ["vin"]}
//   {op: insert,  table: "t", id: "<22 base62>", value: <document>}
//   {op: replace, table: "t", id: "...", value: <document>, ifVersion: 3}
//   {op: patch,   table: "t", id: "...", set: [{path: ["a", 0], value: v}],
//                 remove: [["b"]], ifVersion: 3}
//   {op: delete,  table: "t", id: "...", ifVersion: 3}
//
// ifVersion is optional (Requirement 5.4). A path step is a field name
// (string or symbol) or a list index (int).
//
// Hashes (design "Hashing"): a statement hashes as the Ion hash of its
// struct; a revision as dot(ionhash(data), ionhash(metadata)), metadata only
// for a delete; entriesHash is the Merkle root (dot) of the statement hashes
// followed by the revision hashes; blockHash is
// dot(dot(entriesHash, ionhash({hashVersion, seq, txId, blockTimestamp})),
// previousBlockHash). The block hash uses only `dot` above the Ion hashes so
// that a revision's proof (its entries-tree path, the header hash, the
// previous block hash, then the accumulator path) folds onto the revision
// hash all the way to the digest, as AWS's verification procedure expects.
// The first block's previousBlockHash is 32 zero bytes.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/merkle.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::ledger {

/// The block hash field list version (design "Hashing").
inline constexpr std::int64_t hash_version = 1;

/// Length of a document id and of a transaction id (Requirement 5.2).
inline constexpr std::size_t id_length = 22;

/// Default document size limit, as Ion binary (Requirement 5.3).
inline constexpr std::size_t default_max_document_bytes = 128 * 1024;

/// The first block's previous hash.
inline constexpr hash256 genesis_previous_hash{};

/// An entry, statement or stored record that does not have the shape this
/// header defines.
class ledger_format_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ── Names, ids and time ─────────────────────────────────────────────────────

/// QLDB's table name rule: a letter or underscore, then letters, digits and
/// underscores, at most 128 characters.
inline auto valid_table_name(std::string_view name) -> bool {
    const auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (name.empty() || name.size() > 128 || !alpha(name.front())) {
        return false;
    }
    return std::ranges::all_of(name, [&](char c) { return alpha(c) || digit(c); });
}

inline constexpr std::string_view base62_digits =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/// 22 base62 characters (document and transaction ids).
inline auto valid_id(std::string_view id) -> bool {
    return id.size() == id_length && std::ranges::all_of(id, [](char c) {
               return base62_digits.find(c) != std::string_view::npos;
           });
}

/// A random id: 22 base62 characters from the OpenSSL CSPRNG (about 131
/// bits), by rejection sampling so every digit is equally likely.
inline auto new_id() -> std::string {
    std::string out;
    out.reserve(id_length);
    std::array<unsigned char, 32> bytes{};
    while (out.size() < id_length) {
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw std::runtime_error("new_id: RAND_bytes failed");
        }
        for (const unsigned char b : bytes) {
            if (b < 248 && out.size() < id_length) {  // 248 = 4 * 62
                out.push_back(base62_digits[b % 62]);
            }
        }
    }
    return out;
}

/// Milliseconds since the Unix epoch as an Ion timestamp with millisecond
/// precision in UTC, e.g. "2026-11-09T12:00:00.123Z".
inline auto format_timestamp(std::int64_t millis) -> std::string {
    using namespace std::chrono;
    const sys_time<milliseconds> t{milliseconds{millis}};
    const auto day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> hms{t - day};
    const int y = static_cast<int>(ymd.year());
    if (y < 1 || y > 9999) {
        throw ledger_format_error("timestamp out of range");
    }
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", y,
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()),
                  static_cast<int>(hms.subseconds().count()));
    return buf.data();
}

/// The inverse of format_timestamp; nullopt for any other form.
inline auto parse_timestamp(std::string_view text) -> std::optional<std::int64_t> {
    if (text.size() != 24) {
        return std::nullopt;
    }
    const auto num = [&](std::size_t at, std::size_t len) -> std::optional<int> {
        int v = 0;
        for (std::size_t i = at; i < at + len; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return std::nullopt;
            }
            v = v * 10 + (text[i] - '0');
        }
        return v;
    };
    const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2),
               s = num(17, 2), ms = num(20, 3);
    if (!y || !mo || !d || !h || !mi || !s || !ms) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day ymd{year{*y}, month{static_cast<unsigned>(*mo)},
                             day{static_cast<unsigned>(*d)}};
    if (!ymd.ok() || *h > 23 || *mi > 59 || *s > 59) {
        return std::nullopt;
    }
    const auto t = sys_days{ymd} + hours{*h} + minutes{*mi} + seconds{*s} + milliseconds{*ms};
    const std::int64_t millis = t.time_since_epoch().count();
    if (format_timestamp(millis) != text) {
        return std::nullopt;  // separators, or anything else not in canonical form
    }
    return millis;
}

inline auto timestamp_value(std::int64_t millis) -> ion_value {
    return ion_value::timestamp(format_timestamp(millis));
}

inline auto timestamp_millis(const ion_value& v) -> std::optional<std::int64_t> {
    const auto text = v.as_timestamp_text();
    return text ? parse_timestamp(*text) : std::nullopt;
}

inline auto hash_value(const hash256& h) -> ion_value {
    return ion_value::blob(h);
}

inline auto hash_from(const ion_value* v) -> std::optional<hash256> {
    if (v == nullptr) {
        return std::nullopt;
    }
    const auto bytes = v->as_bytes();
    if (!bytes || bytes->size() != hash_size) {
        return std::nullopt;
    }
    hash256 out{};
    std::ranges::copy(*bytes, out.begin());
    return out;
}

// ── Field access helpers ────────────────────────────────────────────────────

namespace journal_detail {

inline auto required(const ion_value& s, std::string_view name) -> const ion_value& {
    const ion_value* v = s.find(name);
    if (v == nullptr) {
        throw ledger_format_error("missing field " + std::string(name));
    }
    return *v;
}

inline auto text_field(const ion_value& s, std::string_view name) -> std::string {
    auto t = required(s, name).as_text();
    if (!t) {
        throw ledger_format_error("field " + std::string(name) + " is not text");
    }
    return std::move(*t);
}

inline auto int_field(const ion_value& s, std::string_view name) -> std::int64_t {
    auto v = required(s, name).as_int64();
    if (!v) {
        throw ledger_format_error("field " + std::string(name) + " is not a 64-bit int");
    }
    return *v;
}

inline auto uint_field(const ion_value& s, std::string_view name) -> std::uint64_t {
    const auto v = int_field(s, name);
    if (v < 0) {
        throw ledger_format_error("field " + std::string(name) + " is negative");
    }
    return static_cast<std::uint64_t>(v);
}

inline auto hash_field(const ion_value& s, std::string_view name) -> hash256 {
    auto h = hash_from(&required(s, name));
    if (!h) {
        throw ledger_format_error("field " + std::string(name) + " is not a 32-byte blob");
    }
    return *h;
}

inline auto time_field(const ion_value& s, std::string_view name) -> std::int64_t {
    auto t = timestamp_millis(required(s, name));
    if (!t) {
        throw ledger_format_error("field " + std::string(name) +
                                  " is not a millisecond UTC timestamp");
    }
    return *t;
}

}  // namespace journal_detail

// ── Statements ──────────────────────────────────────────────────────────────

enum class statement_op : std::uint8_t {
    create_table,
    drop_table,
    undrop_table,
    insert,
    replace,
    patch,
    remove,  // `delete` in Ion
    create_index,
};

inline auto op_name(statement_op op) -> std::string_view {
    switch (op) {
        case statement_op::create_table:
            return "create_table";
        case statement_op::drop_table:
            return "drop_table";
        case statement_op::undrop_table:
            return "undrop_table";
        case statement_op::insert:
            return "insert";
        case statement_op::replace:
            return "replace";
        case statement_op::patch:
            return "patch";
        case statement_op::remove:
            return "delete";
        case statement_op::create_index:
            return "create_index";
    }
    return "";
}

inline auto op_from_name(std::string_view name) -> std::optional<statement_op> {
    for (const auto op : {statement_op::create_table, statement_op::drop_table,
                          statement_op::undrop_table, statement_op::insert, statement_op::replace,
                          statement_op::patch, statement_op::remove, statement_op::create_index}) {
        if (op_name(op) == name) {
            return op;
        }
    }
    return std::nullopt;
}

/// One step of a patch path: a struct field or a list index.
using path_step = std::variant<std::string, std::size_t>;
using document_path = std::vector<path_step>;

struct patch_set {
    document_path path;
    ion_value value;
};

/// A parsed statement, with the Ion it came from (what gets hashed).
struct statement {
    statement_op op = statement_op::create_table;
    std::string table;
    std::string id;   // document statements
    ion_value value;  // insert, replace
    std::optional<std::int64_t> if_version;
    std::vector<patch_set> sets;         // patch
    std::vector<document_path> removes;  // patch
    std::string index;                   // create_index
    document_path index_path;            // create_index
    ion_value ion;

    [[nodiscard]] auto is_document_op() const -> bool {
        return op == statement_op::insert || op == statement_op::replace ||
               op == statement_op::patch || op == statement_op::remove;
    }
};

namespace journal_detail {

inline auto path_to_ion(const document_path& path) -> ion_value {
    ion_value out = ion_value::make_list();
    for (const auto& step : path) {
        if (const auto* name = std::get_if<std::string>(&step)) {
            out.push_back(ion_value::string(*name));
        } else {
            out.push_back(
                ion_value::integer(static_cast<std::int64_t>(std::get<std::size_t>(step))));
        }
    }
    return out;
}

inline auto path_from_ion(const ion_value& v) -> document_path {
    if (!v.is_sequence() || v.items().empty()) {
        throw ledger_format_error("a path is a non-empty list");
    }
    document_path out;
    for (const auto& step : v.items()) {
        if (auto text = step.as_text()) {
            out.emplace_back(std::move(*text));
        } else if (auto index = step.as_int64(); index && *index >= 0) {
            out.emplace_back(static_cast<std::size_t>(*index));
        } else {
            throw ledger_format_error("a path step is a field name or a list index");
        }
    }
    return out;
}

}  // namespace journal_detail

/// Parse and check the shape of one statement (not its effect on state).
inline auto parse_statement(const ion_value& ion) -> statement {
    namespace jd = journal_detail;
    if (!ion.is_struct()) {
        throw ledger_format_error("a statement is a struct");
    }
    statement out;
    const auto op = op_from_name(jd::text_field(ion, "op"));
    if (!op) {
        throw ledger_format_error("unknown statement op");
    }
    out.op = *op;
    out.table = jd::text_field(ion, "table");
    if (!valid_table_name(out.table)) {
        throw ledger_format_error("invalid table name");
    }
    if (out.is_document_op()) {
        out.id = jd::text_field(ion, "id");
        if (!valid_id(out.id)) {
            throw ledger_format_error("invalid document id");
        }
        if (const ion_value* v = ion.find("ifVersion"); v != nullptr) {
            out.if_version = v->as_int64();
            if (!out.if_version || *out.if_version < 0) {
                throw ledger_format_error("ifVersion is a non-negative int");
            }
        }
    }
    switch (out.op) {
        case statement_op::create_index:
            out.index = jd::text_field(ion, "index");
            if (!valid_table_name(out.index)) {
                throw ledger_format_error("invalid index name");
            }
            out.index_path = jd::path_from_ion(jd::required(ion, "path"));
            break;
        case statement_op::insert:
        case statement_op::replace:
            out.value = jd::required(ion, "value");
            break;
        case statement_op::patch: {
            if (const ion_value* sets = ion.find("set"); sets != nullptr) {
                if (!sets->is_sequence()) {
                    throw ledger_format_error("set is a list");
                }
                for (const auto& s : sets->items()) {
                    if (!s.is_struct()) {
                        throw ledger_format_error("a set is a struct");
                    }
                    out.sets.push_back(
                        {jd::path_from_ion(jd::required(s, "path")), jd::required(s, "value")});
                }
            }
            if (const ion_value* removes = ion.find("remove"); removes != nullptr) {
                if (!removes->is_sequence()) {
                    throw ledger_format_error("remove is a list");
                }
                for (const auto& r : removes->items()) {
                    out.removes.push_back(jd::path_from_ion(r));
                }
            }
            if (out.sets.empty() && out.removes.empty()) {
                throw ledger_format_error("a patch sets or removes something");
            }
            break;
        }
        default:
            break;
    }
    out.ion = ion;
    return out;
}

/// Builders for the statement forms in the file comment.
namespace statements {

inline auto table_op(statement_op op, std::string_view table) -> ion_value {
    ion_value s = ion_value::make_struct();
    s.set("op", ion_value::symbol(op_name(op))).set("table", ion_value::string(table));
    return s;
}

inline auto create_table(std::string_view table) -> ion_value {
    return table_op(statement_op::create_table, table);
}
inline auto drop_table(std::string_view table) -> ion_value {
    return table_op(statement_op::drop_table, table);
}
inline auto undrop_table(std::string_view table) -> ion_value {
    return table_op(statement_op::undrop_table, table);
}

/// An equality index named `index` on the value at `path` (Requirement
/// 6.2).
inline auto create_index(std::string_view table, std::string_view index, const document_path& path)
    -> ion_value {
    ion_value s = table_op(statement_op::create_index, table);
    s.set("index", ion_value::string(index)).set("path", journal_detail::path_to_ion(path));
    return s;
}

inline auto document_op(statement_op op, std::string_view table, std::string_view id,
                        std::optional<std::int64_t> if_version) -> ion_value {
    ion_value s = table_op(op, table);
    s.set("id", ion_value::string(id));
    if (if_version) {
        s.set("ifVersion", ion_value::integer(*if_version));
    }
    return s;
}

inline auto insert(std::string_view table, std::string_view id, ion_value value) -> ion_value {
    ion_value s = document_op(statement_op::insert, table, id, std::nullopt);
    s.set("value", std::move(value));
    return s;
}

inline auto replace(std::string_view table, std::string_view id, ion_value value,
                    std::optional<std::int64_t> if_version = std::nullopt) -> ion_value {
    ion_value s = document_op(statement_op::replace, table, id, if_version);
    s.set("value", std::move(value));
    return s;
}

inline auto patch(std::string_view table, std::string_view id, const std::vector<patch_set>& sets,
                  const std::vector<document_path>& removes,
                  std::optional<std::int64_t> if_version = std::nullopt) -> ion_value {
    ion_value s = document_op(statement_op::patch, table, id, if_version);
    if (!sets.empty()) {
        ion_value list = ion_value::make_list();
        for (const auto& set : sets) {
            ion_value entry = ion_value::make_struct();
            entry.set("path", journal_detail::path_to_ion(set.path)).set("value", set.value);
            list.push_back(std::move(entry));
        }
        s.set("set", std::move(list));
    }
    if (!removes.empty()) {
        ion_value list = ion_value::make_list();
        for (const auto& path : removes) {
            list.push_back(journal_detail::path_to_ion(path));
        }
        s.set("remove", std::move(list));
    }
    return s;
}

inline auto remove(std::string_view table, std::string_view id,
                   std::optional<std::int64_t> if_version = std::nullopt) -> ion_value {
    return document_op(statement_op::remove, table, id, if_version);
}

}  // namespace statements

// ── Patching ────────────────────────────────────────────────────────────────

/// Why a patch did not apply.
class patch_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace journal_detail {

/// The child at `step`, or nullptr.
inline auto child(ion_value& v, const path_step& step) -> ion_value* {
    if (const auto* name = std::get_if<std::string>(&step)) {
        return v.find(*name);
    }
    const auto index = std::get<std::size_t>(step);
    if (!v.is_sequence() || index >= v.items().size()) {
        return nullptr;
    }
    return &v.items()[index];
}

/// The container that holds the path's last step; every earlier step must
/// exist.
inline auto parent_of(ion_value& root, const document_path& path) -> ion_value& {
    ion_value* at = &root;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        at = child(*at, path[i]);
        if (at == nullptr) {
            throw patch_error("path does not exist");
        }
    }
    return *at;
}

}  // namespace journal_detail

/// Set `path` to `value`: a field is added or replaced; a list index
/// replaces that element, or appends when it equals the list's length.
inline auto patch_set_path(ion_value& root, const document_path& path, ion_value value) -> void {
    ion_value& parent = journal_detail::parent_of(root, path);
    const auto& last = path.back();
    if (const auto* name = std::get_if<std::string>(&last)) {
        if (!parent.is_struct()) {
            throw patch_error("field step on a non-struct");
        }
        parent.set(*name, std::move(value));
        return;
    }
    const auto index = std::get<std::size_t>(last);
    if (!parent.is_sequence() || index > parent.items().size()) {
        throw patch_error("list index out of range");
    }
    if (index == parent.items().size()) {
        parent.push_back(std::move(value));
    } else {
        parent.items()[index] = std::move(value);
    }
}

/// Remove what `path` names; it must exist.
inline auto patch_remove_path(ion_value& root, const document_path& path) -> void {
    ion_value& parent = journal_detail::parent_of(root, path);
    const auto& last = path.back();
    if (const auto* name = std::get_if<std::string>(&last)) {
        if (!parent.is_struct() || !parent.erase(*name)) {
            throw patch_error("field does not exist");
        }
        return;
    }
    const auto index = std::get<std::size_t>(last);
    if (!parent.is_sequence() || index >= parent.items().size()) {
        throw patch_error("list index out of range");
    }
    parent.items().erase(parent.items().begin() + static_cast<std::ptrdiff_t>(index));
}

/// The value at `path` in `root`, or nullptr when the path is absent.
inline auto value_at(const ion_value& root, const document_path& path) -> const ion_value* {
    const ion_value* at = &root;
    for (const auto& step : path) {
        if (const auto* name = std::get_if<std::string>(&step)) {
            at = at->find(*name);
        } else {
            const auto index = std::get<std::size_t>(step);
            at = at->is_sequence() && index < at->items().size() ? &at->items()[index] : nullptr;
        }
        if (at == nullptr) {
            return nullptr;
        }
    }
    return at;
}

// ── Transaction entries ─────────────────────────────────────────────────────

/// What a transaction read, for optimistic validation in apply
/// (Requirement 7.2-7.3). The reads saw the first `blocks` blocks (seq <
/// blocks). Apply rejects the transaction with occ_conflict if any document
/// read, any index key looked up or any table scanned has changed in a
/// block at or after that.
struct read_set {
    struct document_read {
        std::string table;
        std::string id;
        std::int64_t version = -1;  // -1: the read found no document
    };
    struct index_read {
        std::string table;
        std::string index;
        hash256 value_hash{};  // ionhash of the looked-up value
    };

    std::uint64_t blocks = 0;
    std::vector<document_read> documents;
    std::vector<index_read> indexes;
    std::vector<std::string> tables;

    [[nodiscard]] auto empty() const -> bool {
        return documents.empty() && indexes.empty() && tables.empty();
    }

    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value docs = ion_value::make_list();
        for (const auto& d : documents) {
            ion_value v = ion_value::make_struct();
            v.set("table", ion_value::string(d.table))
                .set("id", ion_value::string(d.id))
                .set("version", ion_value::integer(d.version));
            docs.push_back(std::move(v));
        }
        ion_value idx = ion_value::make_list();
        for (const auto& i : indexes) {
            ion_value v = ion_value::make_struct();
            v.set("table", ion_value::string(i.table))
                .set("index", ion_value::string(i.index))
                .set("valueHash", hash_value(i.value_hash));
            idx.push_back(std::move(v));
        }
        ion_value tbl = ion_value::make_list();
        for (const auto& t : tables) {
            tbl.push_back(ion_value::string(t));
        }
        ion_value r = ion_value::make_struct();
        r.set("blocks", ion_value::integer(static_cast<std::int64_t>(blocks)))
            .set("documents", std::move(docs))
            .set("indexes", std::move(idx))
            .set("tables", std::move(tbl));
        return r;
    }

    static auto from_ion(const ion_value& v) -> read_set {
        namespace jd = journal_detail;
        if (!v.is_struct()) {
            throw ledger_format_error("a read set is a struct");
        }
        read_set r;
        r.blocks = jd::uint_field(v, "blocks");
        const auto list = [&](std::string_view name) -> const std::vector<ion_value>& {
            const ion_value& l = jd::required(v, name);
            if (!l.is_sequence()) {
                throw ledger_format_error("read set " + std::string(name) + " is a list");
            }
            return l.items();
        };
        for (const auto& d : list("documents")) {
            document_read dr{jd::text_field(d, "table"), jd::text_field(d, "id"),
                             jd::int_field(d, "version")};
            if (!valid_table_name(dr.table) || !valid_id(dr.id) || dr.version < -1) {
                throw ledger_format_error("invalid document read");
            }
            r.documents.push_back(std::move(dr));
        }
        for (const auto& i : list("indexes")) {
            index_read ir{jd::text_field(i, "table"), jd::text_field(i, "index"),
                          jd::hash_field(i, "valueHash")};
            if (!valid_table_name(ir.table) || !valid_table_name(ir.index)) {
                throw ledger_format_error("invalid index read");
            }
            r.indexes.push_back(std::move(ir));
        }
        for (const auto& t : list("tables")) {
            auto name = t.as_text();
            if (!name || !valid_table_name(*name)) {
                throw ledger_format_error("invalid table read");
            }
            r.tables.push_back(std::move(*name));
        }
        return r;
    }
};

struct transaction_entry {
    std::string tx_id;
    std::int64_t block_timestamp = 0;  // ms since the epoch
    std::uint64_t seq = 0;
    hash256 previous_block_hash{};
    hash256 block_hash{};
    std::vector<statement> statements;
    std::optional<read_set> reads;  // absent: blind writes
};

inline constexpr std::string_view entry_type = "ledger_txn";

inline auto encode_entry(const transaction_entry& e) -> std::vector<std::uint8_t> {
    ion_value list = ion_value::make_list();
    for (const auto& s : e.statements) {
        list.push_back(s.ion);
    }
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(entry_type))
        .set("txId", ion_value::string(e.tx_id))
        .set("blockTimestamp", timestamp_value(e.block_timestamp))
        .set("seq", ion_value::integer(static_cast<std::int64_t>(e.seq)))
        .set("previousBlockHash", hash_value(e.previous_block_hash))
        .set("blockHash", hash_value(e.block_hash))
        .set("statements", std::move(list));
    if (e.reads) {
        v.set("readSet", e.reads->to_ion());
    }
    return v.to_binary();
}

/// Throws ledger_format_error (or ion_value_error) for anything malformed.
inline auto decode_entry(const ion_value& v) -> transaction_entry {
    namespace jd = journal_detail;
    if (!v.is_struct() || jd::text_field(v, "type") != entry_type) {
        throw ledger_format_error("not a ledger transaction entry");
    }
    transaction_entry e;
    e.tx_id = jd::text_field(v, "txId");
    if (!valid_id(e.tx_id)) {
        throw ledger_format_error("invalid transaction id");
    }
    e.block_timestamp = jd::time_field(v, "blockTimestamp");
    e.seq = jd::uint_field(v, "seq");
    e.previous_block_hash = jd::hash_field(v, "previousBlockHash");
    e.block_hash = jd::hash_field(v, "blockHash");
    const ion_value& list = jd::required(v, "statements");
    if (!list.is_sequence() || list.items().empty()) {
        throw ledger_format_error("statements is a non-empty list");
    }
    for (const auto& s : list.items()) {
        e.statements.push_back(parse_statement(s));
    }
    if (const ion_value* r = v.find("readSet"); r != nullptr) {
        e.reads = read_set::from_ion(*r);
    }
    return e;
}

inline auto decode_entry(std::span<const std::uint8_t> bytes) -> transaction_entry {
    return decode_entry(ion_value::parse(bytes));
}

// ── Index build entries ─────────────────────────────────────────────────────

/// Backfill of an index created on a non-empty table (Requirement 6.2):
/// {type: ledger_index_build, table, index, limit}. Each one indexes up to
/// `limit` more documents past the index's stored cursor; the one that
/// reaches the end of the table activates the index. The leader proposes
/// them in the background; they write no block, since no document changes.
struct index_build_entry {
    std::string table;
    std::string index;
    std::uint64_t limit = 0;
};

inline constexpr std::string_view index_build_type = "ledger_index_build";

inline auto encode_index_build(const index_build_entry& e) -> std::vector<std::uint8_t> {
    ion_value v = ion_value::make_struct();
    v.set("type", ion_value::symbol(index_build_type))
        .set("table", ion_value::string(e.table))
        .set("index", ion_value::string(e.index))
        .set("limit", ion_value::integer(static_cast<std::int64_t>(e.limit)));
    return v.to_binary();
}

inline auto decode_index_build(const ion_value& v) -> index_build_entry {
    namespace jd = journal_detail;
    index_build_entry e{jd::text_field(v, "table"), jd::text_field(v, "index"),
                        jd::uint_field(v, "limit")};
    if (!valid_table_name(e.table) || !valid_table_name(e.index) || e.limit == 0) {
        throw ledger_format_error("invalid index build entry");
    }
    return e;
}

// ── Revisions ───────────────────────────────────────────────────────────────

struct revision {
    std::string table;
    std::string id;
    std::int64_t version = 0;
    std::uint64_t seq = 0;  // block that wrote it
    std::string tx_id;
    std::int64_t tx_time = 0;       // ms since the epoch
    std::optional<ion_value> data;  // nullopt: the document was deleted
    hash256 hash{};

    [[nodiscard]] auto metadata() const -> ion_value {
        ion_value m = ion_value::make_struct();
        m.set("id", ion_value::string(id))
            .set("version", ion_value::integer(version))
            .set("txTime", timestamp_value(tx_time))
            .set("txId", ion_value::string(tx_id));
        return m;
    }

    /// dot(ionhash(data), ionhash(metadata)), or ionhash(metadata) for a
    /// delete.
    [[nodiscard]] auto compute_hash() const -> hash256 {
        const hash256 meta = metadata().hash();
        return data ? dot(data->hash(), meta) : meta;
    }

    /// The committed view: {tableName, blockAddress: {sequenceNo}, hash,
    /// data (absent for a delete), metadata}.
    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value address = ion_value::make_struct();
        address.set("sequenceNo", ion_value::integer(static_cast<std::int64_t>(seq)));
        ion_value r = ion_value::make_struct();
        r.set("tableName", ion_value::string(table))
            .set("blockAddress", std::move(address))
            .set("hash", hash_value(hash));
        if (data) {
            r.set("data", *data);
        }
        r.set("metadata", metadata());
        return r;
    }

    static auto from_ion(const ion_value& v) -> revision {
        namespace jd = journal_detail;
        if (!v.is_struct()) {
            throw ledger_format_error("a revision is a struct");
        }
        revision r;
        r.table = jd::text_field(v, "tableName");
        r.seq = jd::uint_field(jd::required(v, "blockAddress"), "sequenceNo");
        r.hash = jd::hash_field(v, "hash");
        if (const ion_value* d = v.find("data"); d != nullptr) {
            r.data = *d;
        }
        const ion_value& m = jd::required(v, "metadata");
        r.id = jd::text_field(m, "id");
        r.version = jd::int_field(m, "version");
        r.tx_time = jd::time_field(m, "txTime");
        r.tx_id = jd::text_field(m, "txId");
        return r;
    }
};

// ── Blocks ──────────────────────────────────────────────────────────────────

struct block {
    std::uint64_t seq = 0;
    std::string tx_id;
    std::int64_t timestamp = 0;  // ms since the epoch
    hash256 entries_hash{};
    hash256 previous_block_hash{};
    hash256 block_hash{};
    std::vector<ion_value> statements;
    std::vector<revision> revisions;

    /// The Merkle root of the statement hashes then the revision hashes.
    [[nodiscard]] auto compute_entries_hash() const -> hash256 {
        std::vector<hash256> leaves;
        leaves.reserve(statements.size() + revisions.size());
        for (const auto& s : statements) {
            leaves.push_back(s.hash());
        }
        for (const auto& r : revisions) {
            leaves.push_back(r.hash);
        }
        return merkle_root(leaves);
    }

    /// The block's own fields {hashVersion, seq, txId, blockTimestamp},
    /// hashed as one leaf of the block hash.
    [[nodiscard]] auto header() const -> ion_value {
        ion_value h = ion_value::make_struct();
        h.set("hashVersion", ion_value::integer(hash_version))
            .set("seq", ion_value::integer(static_cast<std::int64_t>(seq)))
            .set("txId", ion_value::string(tx_id))
            .set("blockTimestamp", timestamp_value(timestamp));
        return h;
    }

    /// dot(dot(entriesHash, ionhash(header)), previousBlockHash): built
    /// from `dot` alone, so a revision's proof folds through its block to
    /// the digest (design "Hashing").
    [[nodiscard]] auto compute_block_hash() const -> hash256 {
        return dot(dot(entries_hash, header().hash()), previous_block_hash);
    }

    /// The proof that revision `i` is under this block's hash: its path in
    /// the entries tree, then the header hash and the previous block hash.
    /// Fold it onto the revision hash to get block_hash.
    [[nodiscard]] auto revision_proof(std::size_t i) const -> std::vector<hash256> {
        if (i >= revisions.size()) {
            throw std::out_of_range("block: revision index out of range");
        }
        std::vector<hash256> leaves;
        leaves.reserve(statements.size() + revisions.size());
        for (const auto& s : statements) {
            leaves.push_back(s.hash());
        }
        for (const auto& r : revisions) {
            leaves.push_back(r.hash);
        }
        auto proof = merkle_proof(leaves, statements.size() + i);
        proof.push_back(header().hash());
        proof.push_back(previous_block_hash);
        return proof;
    }

    /// True if every stored hash matches what the contents give.
    [[nodiscard]] auto verify() const -> bool {
        return std::ranges::all_of(revisions,
                                   [](const revision& r) { return r.compute_hash() == r.hash; }) &&
               compute_entries_hash() == entries_hash && compute_block_hash() == block_hash;
    }

    /// The header plus entriesHash, previousBlockHash, blockHash,
    /// statements and revisions.
    [[nodiscard]] auto to_ion() const -> ion_value {
        ion_value b = header();
        b.set("entriesHash", hash_value(entries_hash))
            .set("previousBlockHash", hash_value(previous_block_hash));
        ion_value list = ion_value::make_list();
        for (const auto& s : statements) {
            list.push_back(s);
        }
        ion_value revs = ion_value::make_list();
        for (const auto& r : revisions) {
            revs.push_back(r.to_ion());
        }
        b.set("blockHash", hash_value(block_hash))
            .set("statements", std::move(list))
            .set("revisions", std::move(revs));
        return b;
    }

    static auto from_ion(const ion_value& v) -> block {
        namespace jd = journal_detail;
        if (!v.is_struct()) {
            throw ledger_format_error("a block is a struct");
        }
        if (jd::int_field(v, "hashVersion") != hash_version) {
            throw ledger_format_error("unknown block hashVersion");
        }
        block b;
        b.seq = jd::uint_field(v, "seq");
        b.tx_id = jd::text_field(v, "txId");
        b.timestamp = jd::time_field(v, "blockTimestamp");
        b.entries_hash = jd::hash_field(v, "entriesHash");
        b.previous_block_hash = jd::hash_field(v, "previousBlockHash");
        b.block_hash = jd::hash_field(v, "blockHash");
        const ion_value& list = jd::required(v, "statements");
        const ion_value& revs = jd::required(v, "revisions");
        if (!list.is_sequence() || !revs.is_sequence()) {
            throw ledger_format_error("statements and revisions are lists");
        }
        b.statements = list.items();
        for (const auto& r : revs.items()) {
            b.revisions.push_back(revision::from_ion(r));
        }
        return b;
    }
};

}  // namespace kythira::ledger
