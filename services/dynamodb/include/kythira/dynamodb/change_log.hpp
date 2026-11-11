// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file change_log.hpp
/// @brief The change log of a DynamoDB item group: its records, its streams
///        and the state the group keeps about them
///        (`.kiro/specs/dynamodb-compatible-api/` task 15, Requirement 14.1).
///
/// Every write to an item of a table whose changes are logged appends one
/// *change record* (`INSERT`, `MODIFY` or `REMOVE`, with the item before
/// and after, and whether TTL deleted it) in the same apply. A write that
/// changes nothing (a failed condition, a delete of a missing item, a put
/// of the same item) appends nothing, as DynamoDB Streams documents.
///
/// The records go to the node's `record_segment_store`, outside the
/// snapshot, one store stream per group *epoch*: a group starts epoch 0
/// with its first logged write, and every split or merge closes the
/// group's stream and opens a new one per resulting group, each naming the
/// streams it came from. That is the lineage DynamoDB Streams gives its
/// shards (task 16). A stream's key is `ddb/<hex of the group's range
/// start>/<epoch>`; the epoch grows along every lineage, so a key is never
/// reused, even when a later group starts where an older one did.
///
/// The group keeps a `change_log_state` in its own key range (meta column
/// family, beside the applied index), so it moves with the group's
/// snapshots, splits and merges: the epoch, the next sequence number, the
/// segments of the open stream and of every closed stream still kept. The
/// sequence numbers are the store's and start at 1 in each stream.
///
/// Record timestamps come from the command (the proposer's clock when it
/// built the write), clamped never to go back, so every replica appends
/// the same bytes and the store rolls segments at the same records.

#include <kythira/dynamodb/item_codec.hpp>

#include <raft/record_segment_store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// `eventName`.
enum class change_event : std::uint8_t {
    insert = 1,
    modify = 2,
    remove = 3,
};

/// One change to one item.
struct change_record {
    change_event event = change_event::insert;
    std::uint64_t table_id = 0;
    /// The item's key attributes.
    item keys;
    /// The item before the write (`MODIFY`, `REMOVE`).
    std::optional<item> old_image;
    /// The item after the write (`INSERT`, `MODIFY`).
    std::optional<item> new_image;
    /// Deleted by TTL: AWS gives such records the service's identity.
    bool ttl_delete = false;
    /// When the write was made (ms since the epoch), the store's timestamp.
    std::uint64_t written_ms = 0;
    /// The label (ms since the epoch) of the table's DynamoDB stream the
    /// writer saw enabled (task 16); 0 when the write was logged for
    /// something else. A record belongs to that stream alone, whatever the
    /// clocks of the writer and the catalog say.
    std::uint64_t stream_ms = 0;

    friend auto operator==(const change_record&, const change_record&) -> bool = default;
};

/// One store stream of a group, open or closed.
struct log_stream {
    std::string key;
    /// The streams this one continues (none for a group's first).
    std::vector<std::string> parents;
    /// Its sealed segments, oldest first.
    std::vector<kythira::record_segment_descriptor> sealed;
    /// The open stream's active segment as of the last snapshot (written by
    /// `checkpoint`, so a replica installing the snapshot can fetch and
    /// check that prefix). Empty on a closed stream.
    std::optional<kythira::record_segment_descriptor> active;
    /// The first sequence number of the stream and the last appended (0:
    /// none yet).
    std::uint64_t first_seq = 1;
    std::uint64_t last_seq = 0;
    /// The tables with a record in the stream, ascending (task 16): a
    /// closed stream is a shard of only those tables' DynamoDB streams.
    std::vector<std::uint64_t> tables;

    friend auto operator==(const log_stream&, const log_stream&) -> bool = default;
};

/// What a group records about its change log.
struct change_log_state {
    std::uint64_t epoch = 0;
    /// The latest record timestamp, so timestamps never go back.
    std::uint64_t last_ms = 0;
    log_stream current;
    /// Closed streams still kept, oldest first, until retention trims
    /// their last record (`trim_change_log`).
    std::vector<log_stream> closed;

    friend auto operator==(const change_log_state&, const change_log_state&) -> bool = default;
};

/// One store stream as DynamoDB Streams lists it (task 16): a shard of
/// every stream-enabled table the group's range overlaps.
struct change_shard {
    log_stream stream;
    /// Still taking records.
    bool open = false;

    friend auto operator==(const change_shard&, const change_shard&) -> bool = default;
};

/// A page of one store stream's records.
struct change_page {
    /// A group names the stream; when not, it was trimmed away (or never
    /// was).
    bool known = false;
    bool open = false;
    /// The first record still kept and the last appended.
    std::uint64_t first_seq = 1;
    std::uint64_t last_seq = 0;
    /// Records from the requested position on, with their sequence
    /// numbers.
    std::vector<std::pair<std::uint64_t, change_record>> records;

    friend auto operator==(const change_page&, const change_page&) -> bool = default;
};

namespace change_log_detail {

inline constexpr std::uint8_t k_record_format_version = 1;
inline constexpr std::uint8_t k_state_format_version = 1;

inline void put_flagged_item(std::string& out, const std::optional<item>& it) {
    out.push_back(it ? '\1' : '\0');
    if (it) {
        detail::put_bytes(out, encode_item(*it));
    }
}

[[nodiscard]] inline auto get_flagged_item(detail::reader& r)
    -> std::optional<std::optional<item>> {
    const auto flag = r.byte();
    if (!flag || *flag > 1) {
        return std::nullopt;
    }
    if (*flag == 0) {
        return std::optional<item>{};
    }
    const auto raw = r.bytes();
    if (!raw) {
        return std::nullopt;
    }
    auto it = decode_item(*raw);
    if (!it) {
        return std::nullopt;
    }
    return std::optional<std::optional<item>>(std::move(it));
}

inline void put_descriptor(std::string& out, const kythira::record_segment_descriptor& d) {
    detail::put_bytes(out, d._stream);
    detail::put_varint(out, d._first_seq);
    detail::put_varint(out, d._last_seq);
    detail::put_varint(out, d._records);
    detail::put_varint(out, static_cast<std::uint64_t>(d._first_timestamp_ms));
    detail::put_varint(out, static_cast<std::uint64_t>(d._last_timestamp_ms));
    detail::put_varint(out, d._bytes);
    out.append(reinterpret_cast<const char*>(d._sha256.data()), d._sha256.size());  // NOLINT
    out.push_back(d._sealed ? '\1' : '\0');
}

/// A reader like `detail::reader`, with the fixed-size read a digest needs.
class cursor {
public:
    explicit cursor(std::string_view in) : _in(in) {}

    [[nodiscard]] auto done() const -> bool { return _in.empty(); }
    [[nodiscard]] auto byte() -> std::optional<std::uint8_t> {
        if (_in.empty()) {
            return std::nullopt;
        }
        const auto b = static_cast<std::uint8_t>(_in.front());
        _in.remove_prefix(1);
        return b;
    }
    [[nodiscard]] auto varint() -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const auto b = byte();
            if (!b) {
                return std::nullopt;
            }
            v |= static_cast<std::uint64_t>(*b & 0x7FU) << shift;
            if ((*b & 0x80U) == 0) {
                return v;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] auto bytes() -> std::optional<std::string> {
        const auto n = varint();
        if (!n || *n > _in.size()) {
            return std::nullopt;
        }
        std::string s(_in.substr(0, *n));
        _in.remove_prefix(*n);
        return s;
    }
    [[nodiscard]] auto fixed(std::size_t n) -> std::optional<std::string_view> {
        if (n > _in.size()) {
            return std::nullopt;
        }
        const auto s = _in.substr(0, n);
        _in.remove_prefix(n);
        return s;
    }
    [[nodiscard]] auto count(std::size_t max) -> std::optional<std::size_t> {
        const auto n = varint();
        if (!n || *n > max) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(*n);
    }

private:
    std::string_view _in;
};

[[nodiscard]] inline auto read_descriptor(cursor& c)
    -> std::optional<kythira::record_segment_descriptor> {
    kythira::record_segment_descriptor d;
    auto stream = c.bytes();
    const auto first = c.varint();
    const auto last = c.varint();
    const auto records = c.varint();
    const auto first_ts = c.varint();
    const auto last_ts = c.varint();
    const auto size = c.varint();
    const auto sha = c.fixed(d._sha256.size());
    const auto sealed = c.byte();
    if (!stream || !first || !last || !records || !first_ts || !last_ts || !size || !sha ||
        !sealed || *sealed > 1) {
        return std::nullopt;
    }
    d._stream = std::move(*stream);
    d._first_seq = *first;
    d._last_seq = *last;
    d._records = *records;
    d._first_timestamp_ms = static_cast<std::int64_t>(*first_ts);
    d._last_timestamp_ms = static_cast<std::int64_t>(*last_ts);
    d._bytes = *size;
    std::memcpy(d._sha256.data(), sha->data(), d._sha256.size());
    d._sealed = *sealed == 1;
    return d;
}

inline void put_stream(std::string& out, const log_stream& s) {
    detail::put_bytes(out, s.key);
    detail::put_varint(out, s.parents.size());
    for (const auto& p : s.parents) {
        detail::put_bytes(out, p);
    }
    detail::put_varint(out, s.sealed.size());
    for (const auto& d : s.sealed) {
        put_descriptor(out, d);
    }
    out.push_back(s.active ? '\1' : '\0');
    if (s.active) {
        put_descriptor(out, *s.active);
    }
    detail::put_varint(out, s.first_seq);
    detail::put_varint(out, s.last_seq);
    detail::put_varint(out, s.tables.size());
    for (const auto t : s.tables) {
        detail::put_varint(out, t);
    }
}

/// Far above anything real, so a corrupt count cannot allocate the world.
inline constexpr std::size_t k_max_entries = std::size_t{1} << 20U;

[[nodiscard]] inline auto read_stream(cursor& c) -> std::optional<log_stream> {
    log_stream s;
    auto key = c.bytes();
    const auto parents = c.count(k_max_entries);
    if (!key || !parents) {
        return std::nullopt;
    }
    s.key = std::move(*key);
    for (std::size_t i = 0; i < *parents; ++i) {
        auto p = c.bytes();
        if (!p) {
            return std::nullopt;
        }
        s.parents.push_back(std::move(*p));
    }
    const auto sealed = c.count(k_max_entries);
    if (!sealed) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *sealed; ++i) {
        auto d = read_descriptor(c);
        if (!d) {
            return std::nullopt;
        }
        s.sealed.push_back(std::move(*d));
    }
    const auto has_active = c.byte();
    if (!has_active || *has_active > 1) {
        return std::nullopt;
    }
    if (*has_active == 1) {
        auto d = read_descriptor(c);
        if (!d) {
            return std::nullopt;
        }
        s.active = std::move(*d);
    }
    const auto first = c.varint();
    const auto last = c.varint();
    if (!first || !last) {
        return std::nullopt;
    }
    s.first_seq = *first;
    s.last_seq = *last;
    const auto tables = c.count(k_max_entries);
    if (!tables) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < *tables; ++i) {
        const auto t = c.varint();
        if (!t) {
            return std::nullopt;
        }
        s.tables.push_back(*t);
    }
    return s;
}

[[nodiscard]] inline auto hex(std::string_view bytes) -> std::string {
    static constexpr std::string_view k_digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        out.push_back(k_digits[b >> 4U]);
        out.push_back(k_digits[b & 0xFU]);
    }
    return out;
}

/// Whether two items hold the same attributes, in any order.
[[nodiscard]] inline auto same_item(const item& a, const item& b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    return std::ranges::all_of(a, [&](const auto& attr) {
        const auto* other = find_attribute(b, attr.first);
        return other != nullptr && *other == attr.second;
    });
}

}  // namespace change_log_detail

[[nodiscard]] inline auto encode_change_record(const change_record& r) -> std::string {
    using namespace change_log_detail;
    std::string out;
    out.push_back(static_cast<char>(k_record_format_version));
    out.push_back(static_cast<char>(r.event));
    detail::put_varint(out, r.table_id);
    detail::put_bytes(out, encode_item(r.keys));
    put_flagged_item(out, r.old_image);
    put_flagged_item(out, r.new_image);
    out.push_back(r.ttl_delete ? '\1' : '\0');
    detail::put_varint(out, r.written_ms);
    detail::put_varint(out, r.stream_ms);
    return out;
}

[[nodiscard]] inline auto decode_change_record(std::string_view bytes)
    -> std::optional<change_record> {
    using namespace change_log_detail;
    if (bytes.size() < 2 || static_cast<std::uint8_t>(bytes[0]) != k_record_format_version) {
        return std::nullopt;
    }
    const auto event = static_cast<std::uint8_t>(bytes[1]);
    if (event < 1 || event > 3) {
        return std::nullopt;
    }
    detail::reader r(bytes.substr(2));
    change_record out;
    out.event = static_cast<change_event>(event);
    const auto table = r.varint();
    const auto keys = r.bytes();
    auto old_image = get_flagged_item(r);
    auto new_image = get_flagged_item(r);
    const auto ttl = r.byte();
    const auto written = r.varint();
    const auto stream = r.varint();
    if (!table || !keys || !old_image || !new_image || !ttl || *ttl > 1 || !written || !stream ||
        !r.done()) {
        return std::nullopt;
    }
    auto k = decode_item(*keys);
    if (!k) {
        return std::nullopt;
    }
    out.table_id = *table;
    out.keys = std::move(*k);
    out.old_image = std::move(*old_image);
    out.new_image = std::move(*new_image);
    out.ttl_delete = *ttl == 1;
    out.written_ms = *written;
    out.stream_ms = *stream;
    return out;
}

[[nodiscard]] inline auto encode_change_log_state(const change_log_state& s) -> std::string {
    using namespace change_log_detail;
    std::string out;
    out.push_back(static_cast<char>(k_state_format_version));
    detail::put_varint(out, s.epoch);
    detail::put_varint(out, s.last_ms);
    put_stream(out, s.current);
    detail::put_varint(out, s.closed.size());
    for (const auto& c : s.closed) {
        put_stream(out, c);
    }
    return out;
}

[[nodiscard]] inline auto decode_change_log_state(std::string_view bytes)
    -> std::optional<change_log_state> {
    using namespace change_log_detail;
    if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) != k_state_format_version) {
        return std::nullopt;
    }
    cursor c(bytes.substr(1));
    change_log_state s;
    const auto epoch = c.varint();
    const auto last_ms = c.varint();
    auto current = read_stream(c);
    const auto closed = c.count(k_max_entries);
    if (!epoch || !last_ms || !current || !closed) {
        return std::nullopt;
    }
    s.epoch = *epoch;
    s.last_ms = *last_ms;
    s.current = std::move(*current);
    for (std::size_t i = 0; i < *closed; ++i) {
        auto st = read_stream(c);
        if (!st) {
            return std::nullopt;
        }
        s.closed.push_back(std::move(*st));
    }
    if (!c.done()) {
        return std::nullopt;
    }
    return s;
}

/// The wire form of a group's shards (the raft backend's `shards` op).
[[nodiscard]] inline auto encode_change_shards(const std::vector<change_shard>& shards)
    -> std::string {
    std::string out;
    detail::put_varint(out, shards.size());
    for (const auto& s : shards) {
        change_log_detail::put_stream(out, s.stream);
        out.push_back(s.open ? '\1' : '\0');
    }
    return out;
}

[[nodiscard]] inline auto decode_change_shards(std::string_view bytes)
    -> std::optional<std::vector<change_shard>> {
    using namespace change_log_detail;
    cursor c(bytes);
    const auto n = c.count(k_max_entries);
    if (!n) {
        return std::nullopt;
    }
    std::vector<change_shard> out;
    for (std::size_t i = 0; i < *n; ++i) {
        auto s = read_stream(c);
        const auto open = c.byte();
        if (!s || !open || *open > 1) {
            return std::nullopt;
        }
        out.push_back(change_shard{std::move(*s), *open == 1});
    }
    if (!c.done()) {
        return std::nullopt;
    }
    return out;
}

/// The wire form of a page (the raft backend's `changes` op).
[[nodiscard]] inline auto encode_change_page(const change_page& p) -> std::string {
    std::string out;
    out.push_back(p.known ? '\1' : '\0');
    out.push_back(p.open ? '\1' : '\0');
    detail::put_varint(out, p.first_seq);
    detail::put_varint(out, p.last_seq);
    detail::put_varint(out, p.records.size());
    for (const auto& [seq, rec] : p.records) {
        detail::put_varint(out, seq);
        detail::put_bytes(out, encode_change_record(rec));
    }
    return out;
}

[[nodiscard]] inline auto decode_change_page(std::string_view bytes) -> std::optional<change_page> {
    using namespace change_log_detail;
    cursor c(bytes);
    change_page p;
    const auto known = c.byte();
    const auto open = c.byte();
    const auto first = c.varint();
    const auto last = c.varint();
    const auto n = c.count(k_max_entries);
    if (!known || *known > 1 || !open || *open > 1 || !first || !last || !n) {
        return std::nullopt;
    }
    p.known = *known == 1;
    p.open = *open == 1;
    p.first_seq = *first;
    p.last_seq = *last;
    for (std::size_t i = 0; i < *n; ++i) {
        const auto seq = c.varint();
        const auto body = c.bytes();
        auto rec = body ? decode_change_record(*body) : std::nullopt;
        if (!seq || !rec) {
            return std::nullopt;
        }
        p.records.emplace_back(*seq, std::move(*rec));
    }
    if (!c.done()) {
        return std::nullopt;
    }
    return p;
}

/// The store stream of the group starting at `range_start` in `epoch`.
[[nodiscard]] inline auto change_stream_key(std::string_view range_start, std::uint64_t epoch)
    -> std::string {
    return "ddb/" + change_log_detail::hex(range_start) + "/" + std::to_string(epoch);
}

/// The range start and epoch a `change_stream_key` names, or nothing.
[[nodiscard]] inline auto parse_change_stream_key(std::string_view key)
    -> std::optional<std::pair<std::string, std::uint64_t>> {
    if (!key.starts_with("ddb/")) {
        return std::nullopt;
    }
    key.remove_prefix(4);
    const auto slash = key.find('/');
    if (slash == std::string_view::npos || slash % 2 != 0) {
        return std::nullopt;
    }
    std::string start;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < slash; i += 2) {
        const int hi = nibble(key[i]);
        const int lo = nibble(key[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        start.push_back(static_cast<char>((hi << 4) | lo));
    }
    const auto digits = key.substr(slash + 1);
    if (digits.empty() || digits.size() > 19 || (digits.size() > 1 && digits[0] == '0')) {
        return std::nullopt;
    }
    std::uint64_t epoch = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        epoch = epoch * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return std::pair{std::move(start), epoch};
}

/// Where a group keeps its `change_log_state` (meta column family): just
/// after its range start. An item group's range starts at a table or
/// partition prefix, and every key of an item collection continues its
/// prefix with a byte from 0 to 3 (`key_layout.hpp`), so byte 5 is free.
[[nodiscard]] inline auto change_log_state_key(std::string_view range_start) -> std::string {
    std::string k(range_start);
    k.push_back('\5');
    return k;
}

/// The record a write makes, or nothing when it changed nothing: `old` and
/// `next` are the item before and after.
[[nodiscard]] inline auto make_change_record(std::uint64_t table_id, const key_schema& schema,
                                             const std::optional<item>& old,
                                             const std::optional<item>& next)
    -> std::optional<change_record> {
    if (!old && !next) {
        return std::nullopt;
    }
    if (old && next && change_log_detail::same_item(*old, *next)) {
        return std::nullopt;
    }
    change_record r;
    r.table_id = table_id;
    r.event = !old ? change_event::insert : !next ? change_event::remove : change_event::modify;
    const auto& source = next ? *next : *old;
    if (const auto* v = find_attribute(source, schema.partition_name)) {
        r.keys.emplace_back(schema.partition_name, *v);
    }
    if (schema.sort_name) {
        if (const auto* v = find_attribute(source, *schema.sort_name)) {
            r.keys.emplace_back(*schema.sort_name, *v);
        }
    }
    r.old_image = old;
    r.new_image = next;
    return r;
}

}  // namespace kythira::dynamodb
