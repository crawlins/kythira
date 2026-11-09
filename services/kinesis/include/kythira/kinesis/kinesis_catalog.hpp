// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_catalog.hpp
/// @brief The Kinesis catalog's keys and records
///        (.kiro/specs/kinesis-compatible-streams/ Requirements 6.2-6.4).
///
/// Everything here is a plain value with a byte codec; the stream state
/// machine (kinesis_stream_state_machine.hpp) keeps these values in its
/// key-ordered map and changes them only in `apply()`, so stream metadata
/// changes only through Raft entries (Requirement 6.2).
///
/// Key layout, in one multi_raft key space:
///
/// | Key | Holds |
/// |---|---|
/// | `\0meta/kinesis/next-id` | the next stream id (never decremented) |
/// | `\0meta/kinesis/streams/<account>/<name>` | a `stream_record` |
/// | `k/<16 hex stream id>/<8 hex ordinal>` | one Kinesis shard's `shard_state` |
///
/// The catalog is one contiguous range (the stream state machine vetoes
/// split points inside it), so the group holding `next-id` also holds every
/// stream record and allocates ids without a cross-group transaction. Ids
/// come from the counter, never from the name, so a recreated stream gets
/// fresh routing keys and never sees an old stream's records
/// (Requirement 6.3). Routing keys are fixed width, so their byte order is
/// (stream id, ordinal) order and one stream's shards are adjacent.

#include <kythira/kinesis/kinesis_hash.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::kinesis {

class kinesis_codec_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---- keys -----------------------------------------------------------------

/// `\0meta/kinesis/`, with its leading NUL.
[[nodiscard]] inline auto catalog_prefix() -> const std::string& {
    static const std::string p("\0meta/kinesis/", 14);
    return p;
}

[[nodiscard]] inline auto next_id_key() -> std::string {
    return catalog_prefix() + "next-id";
}

[[nodiscard]] inline auto streams_prefix() -> std::string {
    return catalog_prefix() + "streams/";
}

/// Stream names are per account, as on AWS: two accounts may each have a
/// stream called `orders`.
[[nodiscard]] inline auto account_streams_prefix(std::string_view account) -> std::string {
    return streams_prefix() + std::string(account) + "/";
}

[[nodiscard]] inline auto stream_key(std::string_view account, std::string_view name)
    -> std::string {
    return account_streams_prefix(account) + std::string(name);
}

/// An account id the catalog can key on: non-empty, at most 64 bytes, no
/// '/' (12 digits from IAM or a static key in practice).
[[nodiscard]] inline auto valid_account(std::string_view account) -> bool {
    return !account.empty() && account.size() <= 64 &&
           account.find('/') == std::string_view::npos &&
           account.find('\0') == std::string_view::npos;
}

inline constexpr std::size_t routing_key_size = 2 + 16 + 1 + 8;

namespace detail {

inline auto put_hex(std::string& out, std::uint64_t v, int digits) -> void {
    static constexpr std::string_view hex = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) {
        out.push_back(hex[(v >> (static_cast<unsigned>(i) * 4U)) & 0xFU]);
    }
}

[[nodiscard]] inline auto parse_hex(std::string_view s) -> std::optional<std::uint64_t> {
    std::uint64_t v = 0;
    for (const char c : s) {
        unsigned d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<unsigned>(c - 'a') + 10;
        } else {
            return std::nullopt;
        }
        v = (v << 4U) | d;
    }
    return v;
}

}  // namespace detail

/// `k/<16 hex id>/` — every routing key of one stream starts with it.
[[nodiscard]] inline auto routing_prefix(std::uint64_t stream_id) -> std::string {
    std::string out = "k/";
    detail::put_hex(out, stream_id, 16);
    out.push_back('/');
    return out;
}

[[nodiscard]] inline auto routing_key(std::uint64_t stream_id, std::uint32_t ordinal)
    -> std::string {
    auto out = routing_prefix(stream_id);
    detail::put_hex(out, ordinal, 8);
    return out;
}

struct routing_key_parts {
    std::uint64_t _stream_id = 0;
    std::uint32_t _ordinal = 0;
    friend auto operator==(const routing_key_parts&, const routing_key_parts&) -> bool = default;
};

/// Exactly a routing key (lower-case hex, fixed width), or `nullopt`.
[[nodiscard]] inline auto parse_routing_key(std::string_view key)
    -> std::optional<routing_key_parts> {
    if (key.size() != routing_key_size || !key.starts_with("k/") || key[18] != '/') {
        return std::nullopt;
    }
    auto id = detail::parse_hex(key.substr(2, 16));
    auto ord = detail::parse_hex(key.substr(19, 8));
    if (!id || !ord) {
        return std::nullopt;
    }
    return routing_key_parts{*id, static_cast<std::uint32_t>(*ord)};
}

/// AWS stream names: 1-128 of `[a-zA-Z0-9_.-]`.
[[nodiscard]] inline auto valid_stream_name(std::string_view name) -> bool {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// ---- records --------------------------------------------------------------

enum class stream_status : std::uint8_t {
    creating = 0,
    active = 1,
    updating = 2,
    deleting = 3
};
enum class stream_mode : std::uint8_t {
    provisioned = 0,
    on_demand = 1
};
enum class encryption_type : std::uint8_t {
    none = 0,
    kms = 1
};

[[nodiscard]] inline auto to_string(stream_status s) -> std::string_view {
    switch (s) {
        case stream_status::creating:
            return "CREATING";
        case stream_status::active:
            return "ACTIVE";
        case stream_status::updating:
            return "UPDATING";
        case stream_status::deleting:
            return "DELETING";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline auto to_string(stream_mode m) -> std::string_view {
    return m == stream_mode::on_demand ? "ON_DEMAND" : "PROVISIONED";
}

/// The transitions SDK waiters observe (Requirement 6.1): CREATING becomes
/// ACTIVE, ACTIVE and UPDATING swap for resharding and mode changes, and any
/// live state may start DELETING. DELETING ends by removing the record, not
/// by another status.
[[nodiscard]] inline auto status_transition_allowed(stream_status from, stream_status to) -> bool {
    using enum stream_status;
    switch (to) {
        case active:
            return from == creating || from == updating;
        case updating:
            return from == active;
        case deleting:
            return from != deleting;
        case creating:
            return false;
    }
    return false;
}

/// One Kinesis shard as the catalog describes it. Its records and counter
/// live under its routing key, possibly in another group.
struct shard_entry {
    std::uint32_t _ordinal = 0;
    std::optional<std::uint32_t> _parent = std::nullopt;
    std::optional<std::uint32_t> _adjacent_parent = std::nullopt;
    hash_range _range{};
    /// Counter at the shard's start; its first record gets the next one.
    std::uint64_t _starting_counter = 0;
    /// Set once the shard is closed by resharding: its last counter.
    std::optional<std::uint64_t> _ending_counter = std::nullopt;

    [[nodiscard]] auto open() const noexcept -> bool { return !_ending_counter.has_value(); }
    friend auto operator==(const shard_entry&, const shard_entry&) -> bool = default;
};

struct stream_record {
    std::uint64_t _id = 0;
    std::string _account{};
    std::string _name{};
    stream_status _status = stream_status::creating;
    stream_mode _mode = stream_mode::provisioned;
    std::uint32_t _retention_hours = 24;
    /// Leader-stamped when the create was proposed.
    std::int64_t _created_ms = 0;
    /// The ordinal the next new shard (split or merge child) gets.
    std::uint32_t _next_ordinal = 0;
    std::vector<shard_entry> _shards{};
    std::map<std::string, std::string> _tags{};
    encryption_type _encryption = encryption_type::none;
    std::string _key_id{};

    [[nodiscard]] auto open_shard_count() const -> std::size_t {
        std::size_t n = 0;
        for (const auto& s : _shards) {
            n += s.open() ? 1 : 0;
        }
        return n;
    }

    /// The open shard whose range holds `k` (Requirement 7.1's routing).
    [[nodiscard]] auto shard_for(hash_key k) const -> const shard_entry* {
        for (const auto& s : _shards) {
            if (s.open() && s._range.contains(k)) {
                return &s;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto find_shard(std::uint32_t ordinal) const -> const shard_entry* {
        for (const auto& s : _shards) {
            if (s._ordinal == ordinal) {
                return &s;
            }
        }
        return nullptr;
    }

    friend auto operator==(const stream_record&, const stream_record&) -> bool = default;
};

// ---- codec ----------------------------------------------------------------
//
// Big-endian fixed-width integers, `u32 length + bytes` strings. Decoders
// throw `kinesis_codec_error` on truncation or an out-of-range enum, and a
// caller checks `remaining()` where trailing bytes would be an error.

namespace codec {

class writer {
public:
    auto u8(std::uint8_t v) -> writer& {
        _out.push_back(static_cast<std::byte>(v));
        return *this;
    }
    auto u16(std::uint16_t v) -> writer& { return be(v, 2); }
    auto u32(std::uint32_t v) -> writer& { return be(v, 4); }
    auto u64(std::uint64_t v) -> writer& { return be(v, 8); }
    auto i64(std::int64_t v) -> writer& { return be(static_cast<std::uint64_t>(v), 8); }
    auto hash(hash_key v) -> writer& {
        u64(static_cast<std::uint64_t>(v >> 64U));
        return u64(static_cast<std::uint64_t>(v));
    }
    auto str(std::string_view s) -> writer& {
        u32(static_cast<std::uint32_t>(s.size()));
        for (const char c : s) {
            _out.push_back(static_cast<std::byte>(c));
        }
        return *this;
    }
    auto bytes(std::span<const std::byte> b) -> writer& {
        u32(static_cast<std::uint32_t>(b.size()));
        return raw(b);
    }
    auto raw(std::span<const std::byte> b) -> writer& {
        _out.insert(_out.end(), b.begin(), b.end());
        return *this;
    }
    template<typename T> auto opt_u32(const std::optional<T>& v) -> writer& {
        u8(v ? 1 : 0);
        return u32(v.value_or(0));
    }
    template<typename T> auto opt_u64(const std::optional<T>& v) -> writer& {
        u8(v ? 1 : 0);
        return u64(v.value_or(0));
    }

    [[nodiscard]] auto take() -> std::vector<std::byte> { return std::move(_out); }
    [[nodiscard]] auto out() -> std::vector<std::byte>& { return _out; }

private:
    auto be(std::uint64_t v, int n) -> writer& {
        for (int i = n - 1; i >= 0; --i) {
            _out.push_back(static_cast<std::byte>((v >> (static_cast<unsigned>(i) * 8U)) & 0xFFU));
        }
        return *this;
    }
    std::vector<std::byte> _out{};
};

class reader {
public:
    explicit reader(std::span<const std::byte> in) : _in(in) {}

    auto u8() -> std::uint8_t { return static_cast<std::uint8_t>(be(1)); }
    auto u16() -> std::uint16_t { return static_cast<std::uint16_t>(be(2)); }
    auto u32() -> std::uint32_t { return static_cast<std::uint32_t>(be(4)); }
    auto u64() -> std::uint64_t { return be(8); }
    auto i64() -> std::int64_t { return static_cast<std::int64_t>(be(8)); }
    auto boolean() -> bool {
        const auto v = u8();
        if (v > 1) {
            throw kinesis_codec_error("kinesis codec: bad boolean");
        }
        return v == 1;
    }
    auto hash() -> hash_key {
        const auto hi = u64();
        return (static_cast<hash_key>(hi) << 64U) | u64();
    }
    auto str() -> std::string {
        auto b = take(u32());
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    auto bytes() -> std::vector<std::byte> {
        auto b = take(u32());
        return {b.begin(), b.end()};
    }
    auto take(std::size_t n) -> std::span<const std::byte> {
        need(n);
        auto b = _in.subspan(0, n);
        _in = _in.subspan(n);
        return b;
    }
    auto opt_u32() -> std::optional<std::uint32_t> {
        const bool has = boolean();
        const auto v = u32();
        return has ? std::optional{v} : std::nullopt;
    }
    auto opt_u64() -> std::optional<std::uint64_t> {
        const bool has = boolean();
        const auto v = u64();
        return has ? std::optional{v} : std::nullopt;
    }
    template<typename E> auto enumeration(std::uint8_t max) -> E {
        const auto v = u8();
        if (v > max) {
            throw kinesis_codec_error("kinesis codec: enum value out of range");
        }
        return static_cast<E>(v);
    }

    [[nodiscard]] auto remaining() const noexcept -> std::size_t { return _in.size(); }
    /// What is left, for a nested decoder that advances a span itself.
    [[nodiscard]] auto rest() -> std::span<const std::byte>& { return _in; }
    auto expect_end(std::string_view what) const -> void {
        if (!_in.empty()) {
            throw kinesis_codec_error("kinesis codec: trailing bytes after " + std::string(what));
        }
    }

private:
    auto need(std::size_t n) const -> void {
        if (_in.size() < n) {
            throw kinesis_codec_error("kinesis codec: truncated");
        }
    }
    auto be(std::size_t n) -> std::uint64_t {
        need(n);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v = (v << 8U) | static_cast<std::uint8_t>(_in[i]);
        }
        _in = _in.subspan(n);
        return v;
    }
    std::span<const std::byte> _in;
};

}  // namespace codec

inline auto encode(codec::writer& w, const shard_entry& s) -> void {
    w.u32(s._ordinal).opt_u32(s._parent).opt_u32(s._adjacent_parent);
    w.hash(s._range._start).hash(s._range._end);
    w.u64(s._starting_counter).opt_u64(s._ending_counter);
}

inline auto decode_shard_entry(codec::reader& r) -> shard_entry {
    shard_entry s;
    s._ordinal = r.u32();
    s._parent = r.opt_u32();
    s._adjacent_parent = r.opt_u32();
    s._range._start = r.hash();
    s._range._end = r.hash();
    s._starting_counter = r.u64();
    s._ending_counter = r.opt_u64();
    if (s._range._start > s._range._end) {
        throw kinesis_codec_error("kinesis codec: shard range ends before it starts");
    }
    return s;
}

inline constexpr std::uint8_t stream_record_version = 1;

inline auto encode(codec::writer& w, const stream_record& s) -> void {
    w.u8(stream_record_version).u64(s._id).str(s._account).str(s._name);
    w.u8(static_cast<std::uint8_t>(s._status)).u8(static_cast<std::uint8_t>(s._mode));
    w.u32(s._retention_hours).i64(s._created_ms).u32(s._next_ordinal);
    w.u32(static_cast<std::uint32_t>(s._shards.size()));
    for (const auto& sh : s._shards) {
        encode(w, sh);
    }
    w.u32(static_cast<std::uint32_t>(s._tags.size()));
    for (const auto& [k, v] : s._tags) {
        w.str(k).str(v);
    }
    w.u8(static_cast<std::uint8_t>(s._encryption)).str(s._key_id);
}

inline auto decode_stream_record(codec::reader& r) -> stream_record {
    if (r.u8() != stream_record_version) {
        throw kinesis_codec_error("kinesis codec: unknown stream record version");
    }
    stream_record s;
    s._id = r.u64();
    s._account = r.str();
    s._name = r.str();
    s._status = r.enumeration<stream_status>(3);
    s._mode = r.enumeration<stream_mode>(1);
    s._retention_hours = r.u32();
    s._created_ms = r.i64();
    s._next_ordinal = r.u32();
    const auto shards = r.u32();
    for (std::uint32_t i = 0; i < shards; ++i) {
        s._shards.push_back(decode_shard_entry(r));
    }
    const auto tags = r.u32();
    for (std::uint32_t i = 0; i < tags; ++i) {
        auto k = r.str();
        s._tags[std::move(k)] = r.str();
    }
    s._encryption = r.enumeration<encryption_type>(1);
    s._key_id = r.str();
    return s;
}

[[nodiscard]] inline auto encode_stream_record(const stream_record& s) -> std::vector<std::byte> {
    codec::writer w;
    encode(w, s);
    return w.take();
}

[[nodiscard]] inline auto decode_stream_record(std::span<const std::byte> bytes) -> stream_record {
    codec::reader r(bytes);
    auto s = decode_stream_record(r);
    r.expect_end("stream record");
    return s;
}

/// The shard table `CreateStream` builds: ordinals 0..n-1 over even ranges.
[[nodiscard]] inline auto initial_shards(std::uint32_t n) -> std::vector<shard_entry> {
    std::vector<shard_entry> out;
    auto ranges = even_ranges(n);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back({._ordinal = i, ._range = ranges[i]});
    }
    return out;
}

}  // namespace kythira::kinesis
