// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_kv_commands.hpp
/// @brief Binary log-entry codec for the Redis-compatible KV state machine
///        (.kiro/specs/redis-compatible-kv/ design Component 2).
///
/// Every command the gateway proposes to a shard is one of these records;
/// `redis_kv_state_machine::apply` decodes nothing else. The format is a
/// hand-rolled big-endian layout rather than JSON/CBOR because the value is
/// an opaque compiler artefact: base64-ing an 8 MiB object file to go through
/// a text serializer would cost more than the whole Raft round trip.
///
///     byte 0        format version, always 0x01
///     byte 1        opcode
///     u32 BE        key length
///     key bytes
///     payload       per opcode, below
///
///     set   (0x01)  u64 BE expire_at_ms (0 = never), u32 BE value len, value
///     del   (0x02)  (no payload)
///     sweep (0x03)  key is empty; u32 BE count, then count x
///                   { u32 BE keylen, key, u64 BE expire_at_ms }
///     evict (0x04)  key is empty; u32 BE count, then count x { u32 BE keylen, key }
///
/// Format version 0x02 (.kiro/specs/disk-backed-build-cache/ design Component
/// 2) adds three opcodes for the disk-backed store. Opcodes 0x01-0x04 keep
/// version 0x01, so a log written by the in-memory gateway decodes unchanged
/// and the in-memory gateway's entries stay byte-identical; 0x05-0x07 are
/// valid only under version 0x02.
///
///     put_chunk (0x05)  key is the key being uploaded (routing only);
///                       32-byte sha256; u32 BE len; bytes
///     commit    (0x06)  key; u64 BE expire_at_ms; u64 BE total_len;
///                       32-byte value sha256; u32 BE n; n x 32-byte chunk sha256
///     touch     (0x07)  key is empty; u32 BE count, then count x { u32 BE keylen, key }
///
/// `put_chunk` carries the uploading key, where design.md's layout left it
/// empty: `multi_raft` routes every proposal by `redis_kv_partitioner`, and an
/// empty routing key is refused by cross-shard admission, so a chunk must name
/// the shard its `commit` will land in. `touch` is never proposed through the
/// log (it travels on the forwarding link as an advisory batch); the opcode is
/// reserved here so the format stays one namespace.
///
/// Big-endian throughout matches `default_shard_key_codec` so the byte order
/// in a log dump is the byte order in a shard-map dump.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

inline constexpr std::uint8_t redis_kv_format_version = 0x01;
/// The version byte of `put_chunk`, `commit` and `touch` records.
inline constexpr std::uint8_t redis_kv_format_version_2 = 0x02;

/// A SHA-256, as the chunk store names chunks.
using redis_kv_sha256 = std::array<std::byte, 32>;

enum class redis_kv_opcode : std::uint8_t {
    set = 0x01,
    del = 0x02,
    sweep = 0x03,
    evict = 0x04,
    put_chunk = 0x05,
    commit = 0x06,
    touch = 0x07,
};

struct redis_kv_set_command {
    std::string _key;
    std::vector<std::byte> _value;
    /// Absolute wall-clock deadline in ms since the Unix epoch; 0 = never.
    /// The gateway resolves `SETEX seconds` into this on the leader before
    /// proposing, so `apply` never consults a clock.
    std::uint64_t _expire_at_ms = 0;
};

struct redis_kv_del_command {
    std::string _key;
};

/// One expired key named by the leader's sweep. `apply` deletes it only if the
/// stored deadline still equals `_expire_at_ms`: a concurrent SET that
/// replaced the value (and its deadline) between the sweep being proposed and
/// applied must survive.
struct redis_kv_sweep_entry {
    std::string _key;
    std::uint64_t _expire_at_ms = 0;
};

struct redis_kv_sweep_command {
    std::vector<redis_kv_sweep_entry> _entries;
};

struct redis_kv_evict_command {
    std::vector<std::string> _keys;
};

/// One chunk of a value being uploaded to the disk-backed store. `apply`
/// stores it and holds it as pending until a `commit` names it or the orphan
/// grace period passes.
struct redis_kv_put_chunk_command {
    /// The key whose upload this chunk belongs to. Routing only: the chunk is
    /// stored by hash, and `apply` does not look at the key.
    std::string _key;
    redis_kv_sha256 _sha{};
    std::vector<std::byte> _bytes;
};

/// Bind `_key` to the value made of `_chunks`, in order. A key becomes
/// visible only when this is applied (Requirement 3.2).
struct redis_kv_commit_command {
    std::string _key;
    std::uint64_t _expire_at_ms = 0;
    std::uint64_t _total_len = 0;
    /// SHA-256 of the whole value: what a reader checks the reassembled
    /// bytes against, and the HTTP `ETag` (Requirement 3.7).
    redis_kv_sha256 _value_sha{};
    std::vector<redis_kv_sha256> _chunks;
};

/// Keys a follower served, for the leader's LRU (Requirement 6.2). Advisory;
/// never proposed through the log.
struct redis_kv_touch_command {
    std::vector<std::string> _keys;
};

using redis_kv_command =
    std::variant<redis_kv_set_command, redis_kv_del_command, redis_kv_sweep_command,
                 redis_kv_evict_command, redis_kv_put_chunk_command, redis_kv_commit_command,
                 redis_kv_touch_command>;

class redis_kv_codec_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace redis_kv_detail {

inline auto put_u32(std::vector<std::byte>& out, std::uint32_t v) -> void {
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
}

inline auto put_u64(std::vector<std::byte>& out, std::uint64_t v) -> void {
    put_u32(out, static_cast<std::uint32_t>(v >> 32));
    put_u32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

inline auto put_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) -> void {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline auto put_string(std::vector<std::byte>& out, std::string_view s) -> void {
    put_bytes(out, std::as_bytes(std::span<const char>(s.data(), s.size())));
}

/// Cursor over an input buffer; every read is bounds-checked and a short
/// buffer is a `redis_kv_codec_error`, never undefined behaviour.
class reader {
public:
    explicit reader(std::span<const std::byte> data) : _data(data) {}

    auto u8() -> std::uint8_t {
        need(1);
        return static_cast<std::uint8_t>(_data[_pos++]);
    }

    auto u32() -> std::uint32_t {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v = (v << 8) | static_cast<std::uint32_t>(_data[_pos++]);
        }
        return v;
    }

    auto u64() -> std::uint64_t {
        auto hi = static_cast<std::uint64_t>(u32());
        auto lo = static_cast<std::uint64_t>(u32());
        return (hi << 32) | lo;
    }

    auto bytes(std::size_t n) -> std::span<const std::byte> {
        need(n);
        auto out = _data.subspan(_pos, n);
        _pos += n;
        return out;
    }

    auto string(std::size_t n) -> std::string {
        auto b = bytes(n);
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }

    [[nodiscard]] auto remaining() const noexcept -> std::size_t { return _data.size() - _pos; }

private:
    auto need(std::size_t n) const -> void {
        if (_data.size() - _pos < n) {
            throw redis_kv_codec_error("truncated redis kv command");
        }
    }

    std::span<const std::byte> _data;
    std::size_t _pos = 0;
};

inline auto put_header(std::vector<std::byte>& out, redis_kv_opcode op, std::string_view key)
    -> void {
    if (key.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("key too long to encode");
    }
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(op) >= 0x05
                                             ? redis_kv_format_version_2
                                             : redis_kv_format_version));
    out.push_back(static_cast<std::byte>(op));
    put_u32(out, static_cast<std::uint32_t>(key.size()));
    put_string(out, key);
}

}  // namespace redis_kv_detail

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_set_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._value.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("value too long to encode");
    }
    std::vector<std::byte> out;
    out.reserve(2 + 4 + c._key.size() + 8 + 4 + c._value.size());
    put_header(out, redis_kv_opcode::set, c._key);
    put_u64(out, c._expire_at_ms);
    put_u32(out, static_cast<std::uint32_t>(c._value.size()));
    put_bytes(out, c._value);
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_del_command& c)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(2 + 4 + c._key.size());
    redis_kv_detail::put_header(out, redis_kv_opcode::del, c._key);
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_sweep_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._entries.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("too many sweep entries to encode");
    }
    std::vector<std::byte> out;
    put_header(out, redis_kv_opcode::sweep, {});
    put_u32(out, static_cast<std::uint32_t>(c._entries.size()));
    for (const auto& e : c._entries) {
        if (e._key.size() > 0xFFFFFFFFu) {
            throw redis_kv_codec_error("key too long to encode");
        }
        put_u32(out, static_cast<std::uint32_t>(e._key.size()));
        put_string(out, e._key);
        put_u64(out, e._expire_at_ms);
    }
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_evict_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._keys.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("too many evict entries to encode");
    }
    std::vector<std::byte> out;
    put_header(out, redis_kv_opcode::evict, {});
    put_u32(out, static_cast<std::uint32_t>(c._keys.size()));
    for (const auto& k : c._keys) {
        if (k.size() > 0xFFFFFFFFu) {
            throw redis_kv_codec_error("key too long to encode");
        }
        put_u32(out, static_cast<std::uint32_t>(k.size()));
        put_string(out, k);
    }
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_put_chunk_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._bytes.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("chunk too long to encode");
    }
    std::vector<std::byte> out;
    out.reserve(2 + 4 + c._key.size() + 32 + 4 + c._bytes.size());
    put_header(out, redis_kv_opcode::put_chunk, c._key);
    put_bytes(out, c._sha);
    put_u32(out, static_cast<std::uint32_t>(c._bytes.size()));
    put_bytes(out, c._bytes);
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_commit_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._chunks.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("too many chunks to encode");
    }
    std::vector<std::byte> out;
    out.reserve(2 + 4 + c._key.size() + 8 + 8 + 32 + 4 + (32 * c._chunks.size()));
    put_header(out, redis_kv_opcode::commit, c._key);
    put_u64(out, c._expire_at_ms);
    put_u64(out, c._total_len);
    put_bytes(out, c._value_sha);
    put_u32(out, static_cast<std::uint32_t>(c._chunks.size()));
    for (const auto& sha : c._chunks) {
        put_bytes(out, sha);
    }
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_touch_command& c)
    -> std::vector<std::byte> {
    using namespace redis_kv_detail;
    if (c._keys.size() > 0xFFFFFFFFu) {
        throw redis_kv_codec_error("too many touch entries to encode");
    }
    std::vector<std::byte> out;
    put_header(out, redis_kv_opcode::touch, {});
    put_u32(out, static_cast<std::uint32_t>(c._keys.size()));
    for (const auto& k : c._keys) {
        if (k.size() > 0xFFFFFFFFu) {
            throw redis_kv_codec_error("key too long to encode");
        }
        put_u32(out, static_cast<std::uint32_t>(k.size()));
        put_string(out, k);
    }
    return out;
}

[[nodiscard]] inline auto encode_redis_kv_command(const redis_kv_command& c)
    -> std::vector<std::byte> {
    return std::visit([](const auto& v) { return encode_redis_kv_command(v); }, c);
}

/// Decode one record. Rejects truncated buffers, trailing bytes, unknown
/// versions and unknown opcodes — a state machine that tolerated any of
/// those would diverge silently across replicas.
[[nodiscard]] inline auto decode_redis_kv_command(std::span<const std::byte> data)
    -> redis_kv_command {
    using namespace redis_kv_detail;
    reader in(data);
    auto version = in.u8();
    if (version != redis_kv_format_version && version != redis_kv_format_version_2) {
        throw redis_kv_codec_error("unknown redis kv command format version " +
                                   std::to_string(version));
    }
    auto op = in.u8();
    // Each opcode has exactly one version: 0x01-0x04 under 0x01, 0x05-0x07
    // under 0x02. A record that pairs them otherwise was not written by this
    // codec.
    if ((op >= 0x05) != (version == redis_kv_format_version_2)) {
        throw redis_kv_codec_error("redis kv opcode " + std::to_string(op) +
                                   " under format version " + std::to_string(version));
    }
    auto key_len = in.u32();
    auto key = in.string(key_len);

    redis_kv_command result;
    switch (static_cast<redis_kv_opcode>(op)) {
        case redis_kv_opcode::set: {
            redis_kv_set_command c;
            c._key = std::move(key);
            c._expire_at_ms = in.u64();
            auto value_len = in.u32();
            auto v = in.bytes(value_len);
            c._value.assign(v.begin(), v.end());
            result = std::move(c);
            break;
        }
        case redis_kv_opcode::del: {
            result = redis_kv_del_command{std::move(key)};
            break;
        }
        case redis_kv_opcode::sweep: {
            if (!key.empty()) {
                throw redis_kv_codec_error("sweep record carries a key");
            }
            redis_kv_sweep_command c;
            auto count = in.u32();
            // Each entry is at least 12 bytes, so a count that cannot fit in
            // the buffer is rejected before any allocation.
            if (static_cast<std::size_t>(count) > in.remaining() / 12) {
                throw redis_kv_codec_error("sweep count exceeds buffer");
            }
            c._entries.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                redis_kv_sweep_entry e;
                auto n = in.u32();
                e._key = in.string(n);
                e._expire_at_ms = in.u64();
                c._entries.push_back(std::move(e));
            }
            result = std::move(c);
            break;
        }
        case redis_kv_opcode::evict: {
            if (!key.empty()) {
                throw redis_kv_codec_error("evict record carries a key");
            }
            redis_kv_evict_command c;
            auto count = in.u32();
            if (static_cast<std::size_t>(count) > in.remaining() / 4) {
                throw redis_kv_codec_error("evict count exceeds buffer");
            }
            c._keys.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto n = in.u32();
                c._keys.push_back(in.string(n));
            }
            result = std::move(c);
            break;
        }
        case redis_kv_opcode::put_chunk: {
            redis_kv_put_chunk_command c;
            c._key = std::move(key);
            auto sha = in.bytes(32);
            std::copy(sha.begin(), sha.end(), c._sha.begin());
            auto n = in.u32();
            auto b = in.bytes(n);
            c._bytes.assign(b.begin(), b.end());
            result = std::move(c);
            break;
        }
        case redis_kv_opcode::commit: {
            redis_kv_commit_command c;
            c._key = std::move(key);
            c._expire_at_ms = in.u64();
            c._total_len = in.u64();
            auto sha = in.bytes(32);
            std::copy(sha.begin(), sha.end(), c._value_sha.begin());
            auto n = in.u32();
            if (static_cast<std::size_t>(n) > in.remaining() / 32) {
                throw redis_kv_codec_error("commit chunk count exceeds buffer");
            }
            c._chunks.resize(n);
            for (auto& chunk : c._chunks) {
                auto b = in.bytes(32);
                std::copy(b.begin(), b.end(), chunk.begin());
            }
            result = std::move(c);
            break;
        }
        case redis_kv_opcode::touch: {
            if (!key.empty()) {
                throw redis_kv_codec_error("touch record carries a key");
            }
            redis_kv_touch_command c;
            auto count = in.u32();
            if (static_cast<std::size_t>(count) > in.remaining() / 4) {
                throw redis_kv_codec_error("touch count exceeds buffer");
            }
            c._keys.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto n = in.u32();
                c._keys.push_back(in.string(n));
            }
            result = std::move(c);
            break;
        }
        default:
            throw redis_kv_codec_error("unknown redis kv opcode " + std::to_string(op));
    }
    if (in.remaining() != 0) {
        throw redis_kv_codec_error("trailing bytes after redis kv command");
    }
    return result;
}

/// The routing key of a record without decoding its payload: the header key
/// for `set`/`del`, the first entry's key for `sweep`/`evict` (which the
/// leader builds from one shard, so any entry routes the same). Returns an
/// empty string for a record it cannot read; `multi_raft`'s cross-shard
/// admission then rejects the proposal instead of misrouting it.
[[nodiscard]] inline auto redis_kv_command_key(std::span<const std::byte> data) -> std::string {
    using namespace redis_kv_detail;
    try {
        reader in(data);
        const auto version = in.u8();
        if (version != redis_kv_format_version && version != redis_kv_format_version_2) {
            return {};
        }
        auto op = static_cast<redis_kv_opcode>(in.u8());
        auto key = in.string(in.u32());
        if (op == redis_kv_opcode::set || op == redis_kv_opcode::del ||
            op == redis_kv_opcode::put_chunk || op == redis_kv_opcode::commit) {
            return key;
        }
        if (op == redis_kv_opcode::sweep || op == redis_kv_opcode::evict ||
            op == redis_kv_opcode::touch) {
            if (in.u32() == 0) {
                return {};
            }
            return in.string(in.u32());
        }
    } catch (const redis_kv_codec_error&) {
    }
    return {};
}

/// Satisfies `kythira::partitioner<std::string>` for `multi_raft_config::partitioner`.
struct redis_kv_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        return redis_kv_command_key(command);
    }
};

/// Peek at the opcode without decoding the payload; used by the gateway's
/// audit/trace logging so that an 8 MiB value is never copied for a log line.
[[nodiscard]] inline auto peek_redis_kv_opcode(std::span<const std::byte> data)
    -> std::optional<redis_kv_opcode> {
    if (data.size() < 2) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(data[0]);
    auto op = static_cast<std::uint8_t>(data[1]);
    if (version == redis_kv_format_version) {
        if (op < 0x01 || op > 0x04) {
            return std::nullopt;
        }
    } else if (version == redis_kv_format_version_2) {
        if (op < 0x05 || op > 0x07) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return static_cast<redis_kv_opcode>(op);
}

}  // namespace kythira
