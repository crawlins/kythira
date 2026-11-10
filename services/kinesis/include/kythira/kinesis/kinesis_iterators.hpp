// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_iterators.hpp
/// @brief Shard iterators (`.kiro/specs/kinesis-compatible-streams/` task 7,
///        Requirement 9.2): stateless HMAC-SHA256 tokens any node can check.
///
/// A token names the stream (account, name and the catalog's stream id, so
/// a token for a deleted and recreated stream is refused), the shard, the
/// position and when it was issued. The lifetime is the stream's
/// `iterator-lifetime` switch at read time, so `GetRecords` checks expiry
/// once it has the stream (`iterator_expired`). Nothing about an iterator is replicated: a
/// `GetRecords` on any node checks the MAC and reads from there.
///
/// The position is a pair: read records whose counter is at least
/// `_counter` and whose arrival time is at least `_min_timestamp_ms`. Every
/// iterator type maps onto it: `AT_SEQUENCE_NUMBER` and
/// `AFTER_SEQUENCE_NUMBER` set the counter, `TRIM_HORIZON` the shard's
/// first counter, `LATEST` one past its last, and `AT_TIMESTAMP` the
/// timestamp with the shard's first counter. Resolving a timestamp needs
/// the records, so it happens in `GetRecords`, on whichever node serves it,
/// and the next iterator carries a plain counter.
///
/// Keys. Tokens are signed with the first key of an `iterator_key_ring`
/// and checked with whichever key the token's key id names, so an operator
/// rotates by putting a new key first and keeping the old one for at least
/// one iterator lifetime (design: "tokens signed by the previous key stay
/// valid for the iterator lifetime"). Every gateway of a cluster must hold
/// the same ring. The daemon reads it from `KINESIS_ITERATOR_KEYS_FILE`
/// (`parse_iterator_keys`) or, without one, derives a single key from the
/// forwarding secret all gateways already share (`derived_iterator_key`).
/// The design's bootstrap-generated key sealed like the CA key replaces
/// the derivation when the module gains a sealed-secret store.
///
/// Wire, before base64: version 1 (u8), key id (u32), stream id (u64),
/// shard ordinal (u32), counter (u64), minimum timestamp (i64), issue time
/// in epoch milliseconds (i64), account (12 ASCII digits), name length (u8),
/// name, then the 32-byte MAC over everything before it; integers are
/// big-endian. With a 128-character name that is 245 bytes, 328 in base64,
/// inside the model's 512-character limit.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

/// AWS's shard-iterator lifetime.
inline constexpr std::int64_t k_iterator_lifetime_ms = std::int64_t{5} * 60 * 1000;
/// The lifetime with the `iterator-lifetime` switch relaxed.
inline constexpr std::int64_t k_relaxed_iterator_lifetime_ms = std::int64_t{24} * 60 * 60 * 1000;

struct iterator_key {
    std::uint32_t _id = 0;
    std::string _secret;
};

/// The keys every gateway shares; the first signs.
struct iterator_key_ring {
    std::vector<iterator_key> _keys;

    [[nodiscard]] auto find(std::uint32_t id) const -> const iterator_key* {
        for (const auto& k : _keys) {
            if (k._id == id) {
                return &k;
            }
        }
        return nullptr;
    }
};

struct shard_iterator {
    std::string _account;
    std::string _stream;
    std::uint64_t _stream_id = 0;
    std::uint32_t _ordinal = 0;
    std::uint64_t _counter = 0;
    std::int64_t _min_timestamp_ms = 0;
    std::int64_t _issued_ms = 0;

    friend auto operator==(const shard_iterator&, const shard_iterator&) -> bool = default;
};

namespace iterator_detail {

inline constexpr std::uint8_t k_version = 1;
inline constexpr std::size_t k_mac_bytes = 32;
inline constexpr std::size_t k_account_digits = 12;

inline void put_u(std::string& out, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out += static_cast<char>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
}

[[nodiscard]] inline auto get_u(std::string_view in, std::size_t& at, int bytes)
    -> std::optional<std::uint64_t> {
    if (in.size() - at < static_cast<std::size_t>(bytes)) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = (v << 8U) | static_cast<unsigned char>(in[at++]);
    }
    return v;
}

[[nodiscard]] inline auto mac(std::string_view secret, std::string_view text)
    -> std::array<unsigned char, k_mac_bytes> {
    std::array<unsigned char, k_mac_bytes> out{};
    unsigned len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(text.data()), text.size(), out.data(), &len);
    return out;
}

inline constexpr std::string_view k_b64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] inline auto b64(std::string_view in) -> std::string {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto byte = [&in](std::size_t j) {
        return static_cast<unsigned>(static_cast<unsigned char>(in[j]));
    };
    for (; i + 2 < in.size(); i += 3) {
        const auto n = (byte(i) << 16U) | (byte(i + 1) << 8U) | byte(i + 2);
        out += k_b64[(n >> 18U) & 63U];
        out += k_b64[(n >> 12U) & 63U];
        out += k_b64[(n >> 6U) & 63U];
        out += k_b64[n & 63U];
    }
    if (i < in.size()) {
        auto n = byte(i) << 16U;
        if (i + 1 < in.size()) {
            n |= byte(i + 1) << 8U;
        }
        out += k_b64[(n >> 18U) & 63U];
        out += k_b64[(n >> 12U) & 63U];
        out += i + 1 < in.size() ? k_b64[(n >> 6U) & 63U] : '=';
        out += '=';
    }
    return out;
}

[[nodiscard]] inline auto unb64(std::string_view in) -> std::optional<std::string> {
    if (in.empty() || in.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(in.size() / 4 * 3);
    unsigned acc = 0;
    int bits = 0;
    std::size_t pad = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '=') {
            if (i + 2 < in.size()) {
                return std::nullopt;
            }
            ++pad;
            continue;
        }
        if (pad > 0) {
            return std::nullopt;
        }
        const auto p = k_b64.find(c);
        if (p == std::string_view::npos) {
            return std::nullopt;
        }
        acc = (acc << 6U) | static_cast<unsigned>(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> static_cast<unsigned>(bits)) & 0xFFU);
        }
    }
    return out;
}

}  // namespace iterator_detail

/// The iterator key file: one key per line, `<id> <secret>`, the id a
/// decimal u32 and the secret at least 16 bytes with no blanks. The first
/// key signs; later ones only verify (the previous key during a rotation).
/// Blank lines and `#` lines are skipped. A bad line fails the file with
/// its line number and without echoing it.
[[nodiscard]] inline auto parse_iterator_keys(std::string_view text) -> iterator_key_ring {
    iterator_key_ring ring;
    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        std::vector<std::string_view> fields;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
                ++i;
            }
            const auto start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
                ++i;
            }
            if (i > start) {
                fields.push_back(line.substr(start, i - start));
            }
        }
        if (fields.empty() || fields.front().starts_with('#')) {
            continue;
        }
        const auto where = "iterator keys line " + std::to_string(line_no) + ": ";
        if (fields.size() != 2) {
            throw std::invalid_argument(where + "expected 2 fields (id, secret), found " +
                                        std::to_string(fields.size()));
        }
        std::uint64_t id = 0;
        if (fields[0].empty() || fields[0].size() > 10) {
            throw std::invalid_argument(where + "id must be a number up to 4294967295");
        }
        for (const char c : fields[0]) {
            if (c < '0' || c > '9') {
                throw std::invalid_argument(where + "id must be a number up to 4294967295");
            }
            id = id * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (id > UINT32_MAX) {
            throw std::invalid_argument(where + "id must be a number up to 4294967295");
        }
        if (fields[1].size() < 16) {
            throw std::invalid_argument(where + "secret must be at least 16 bytes");
        }
        if (ring.find(static_cast<std::uint32_t>(id)) != nullptr) {
            throw std::invalid_argument(where + "id " + std::to_string(id) + " repeats");
        }
        ring._keys.push_back(
            {._id = static_cast<std::uint32_t>(id), ._secret = std::string(fields[1])});
    }
    if (ring._keys.empty()) {
        throw std::invalid_argument("iterator keys: no key");
    }
    return ring;
}

/// The one-key ring derived from the forwarding secret every gateway of a
/// cluster shares: HMAC-SHA256(secret, "kythira-kinesis-iterator-key-v1"),
/// key id 0. Changing the forwarding secret invalidates live iterators,
/// which clients recover from with a new GetShardIterator.
[[nodiscard]] inline auto derived_iterator_key(std::string_view forward_secret)
    -> iterator_key_ring {
    const auto m = iterator_detail::mac(forward_secret, "kythira-kinesis-iterator-key-v1");
    iterator_key_ring ring;
    ring._keys.push_back({._id = 0, ._secret = std::string(m.begin(), m.end())});
    return ring;
}

/// The base64 token for `it`, signed with the ring's first key. Throws
/// `std::invalid_argument` on an empty ring or a field the wire cannot
/// carry (an account that is not 12 digits, a name over 255 bytes).
[[nodiscard]] inline auto encode_iterator(const shard_iterator& it, const iterator_key_ring& ring)
    -> std::string {
    using namespace iterator_detail;
    if (ring._keys.empty()) {
        throw std::invalid_argument("kinesis: no iterator key");
    }
    if (it._account.size() != k_account_digits || it._stream.empty() || it._stream.size() > 255) {
        throw std::invalid_argument("kinesis: iterator fields out of range");
    }
    const auto& key = ring._keys.front();
    std::string text;
    text += static_cast<char>(k_version);
    put_u(text, key._id, 4);
    put_u(text, it._stream_id, 8);
    put_u(text, it._ordinal, 4);
    put_u(text, it._counter, 8);
    put_u(text, static_cast<std::uint64_t>(it._min_timestamp_ms), 8);
    put_u(text, static_cast<std::uint64_t>(it._issued_ms), 8);
    text += it._account;
    text += static_cast<char>(it._stream.size());
    text += it._stream;
    const auto m = mac(key._secret, text);
    text.append(reinterpret_cast<const char*>(m.data()), m.size());
    return b64(text);
}

/// The iterator `token` names, if a key of `ring` signed it; nullopt for
/// anything this cluster could not have issued. The MAC is checked before
/// anything in the token is trusted, and in constant time.
[[nodiscard]] inline auto decode_iterator(std::string_view token, const iterator_key_ring& ring)
    -> std::optional<shard_iterator> {
    using namespace iterator_detail;
    const auto raw = unb64(token);
    if (!raw || raw->size() < 1 + 4 + 8 + 4 + 8 + 8 + 8 + k_account_digits + 1 + k_mac_bytes) {
        return std::nullopt;
    }
    const std::string_view text(raw->data(), raw->size() - k_mac_bytes);
    std::size_t at = 0;
    if (static_cast<std::uint8_t>(text[at++]) != k_version) {
        return std::nullopt;
    }
    const auto key_id = get_u(text, at, 4);
    const auto* key = key_id ? ring.find(static_cast<std::uint32_t>(*key_id)) : nullptr;
    if (key == nullptr) {
        return std::nullopt;
    }
    const auto want = mac(key->_secret, text);
    if (CRYPTO_memcmp(want.data(), raw->data() + text.size(), k_mac_bytes) != 0) {
        return std::nullopt;
    }
    shard_iterator it;
    it._stream_id = *get_u(text, at, 8);
    it._ordinal = static_cast<std::uint32_t>(*get_u(text, at, 4));
    it._counter = *get_u(text, at, 8);
    it._min_timestamp_ms = static_cast<std::int64_t>(*get_u(text, at, 8));
    it._issued_ms = static_cast<std::int64_t>(*get_u(text, at, 8));
    it._account = std::string(text.substr(at, k_account_digits));
    at += k_account_digits;
    const auto len = static_cast<std::size_t>(static_cast<unsigned char>(text[at++]));
    if (len == 0 || text.size() - at != len) {
        return std::nullopt;
    }
    it._stream = std::string(text.substr(at, len));
    return it;
}

/// Whether `it` has outlived `lifetime_ms` at `now_ms`.
[[nodiscard]] inline auto iterator_expired(const shard_iterator& it, std::int64_t now_ms,
                                           std::int64_t lifetime_ms) -> bool {
    return now_ms - it._issued_ms > lifetime_ms;
}

/// A time as Java's `Date.toString` prints it in UTC, the form AWS's
/// ExpiredIteratorException message uses: `Mon Nov 10 01:00:00 UTC 2026`.
[[nodiscard]] inline auto java_date_text(std::int64_t epoch_ms) -> std::string {
    const auto t = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) {
        return std::to_string(epoch_ms);
    }
    std::array<char, 64> buf{};
    const auto n = std::strftime(buf.data(), buf.size(), "%a %b %d %H:%M:%S UTC %Y", &tm);
    return std::string(buf.data(), n);
}

}  // namespace kythira::kinesis
