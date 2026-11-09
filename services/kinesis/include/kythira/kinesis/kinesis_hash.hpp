// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_hash.hpp
/// @brief The 128-bit hash-key space Kinesis shards divide
///        (.kiro/specs/kinesis-compatible-streams/ Requirements 6.2 and 7.1).
///
/// A record's hash key is the MD5 of its partition key read as an unsigned
/// big-endian 128-bit integer, or the caller's `ExplicitHashKey` (a decimal
/// string). A shard owns an inclusive range `[start, end]` of that space.
/// AWS prints hash keys as decimal strings without leading zeros, and so do
/// these helpers.

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::kinesis {

/// GCC and Clang both provide it on every target Kythira builds for.
__extension__ using hash_key = unsigned __int128;

inline constexpr hash_key max_hash_key = ~hash_key{0};

/// Decimal, no leading zeros ("0" for zero), as AWS prints hash keys.
[[nodiscard]] inline auto to_decimal(hash_key v) -> std::string {
    if (v == 0) {
        return "0";
    }
    std::string out;
    while (v != 0) {
        out.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    return {out.rbegin(), out.rend()};
}

/// Parse a decimal hash key. `nullopt` for an empty string, a non-digit, or
/// a value above 2^128 - 1 (AWS rejects those as InvalidArgumentException).
/// Leading zeros are accepted, as AWS accepts them.
[[nodiscard]] inline auto parse_hash_key(std::string_view s) -> std::optional<hash_key> {
    if (s.empty() || s.size() > 64) {
        return std::nullopt;
    }
    hash_key v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto d = static_cast<hash_key>(c - '0');
        if (v > (max_hash_key - d) / 10) {
            return std::nullopt;
        }
        v = (v * 10) + d;
    }
    return v;
}

/// MD5 of the partition key as a big-endian 128-bit integer.
[[nodiscard]] inline auto partition_key_hash(std::string_view partition_key) -> hash_key {
    std::array<unsigned char, 16> md{};
    unsigned int len = 0;
    if (::EVP_Digest(partition_key.data(), partition_key.size(), md.data(), &len, ::EVP_md5(),
                     nullptr) != 1 ||
        len != md.size()) {
        throw std::runtime_error("kinesis: EVP_Digest(md5) failed");
    }
    hash_key v = 0;
    for (const unsigned char b : md) {
        v = (v << 8U) | b;
    }
    return v;
}

/// An inclusive slice of the hash-key space.
struct hash_range {
    hash_key _start = 0;
    hash_key _end = 0;

    [[nodiscard]] auto contains(hash_key k) const noexcept -> bool {
        return k >= _start && k <= _end;
    }
    friend auto operator==(const hash_range&, const hash_range&) -> bool = default;
};

/// The ranges `CreateStream` gives `n` shards: equal slices of 2^128 in
/// order, the last one taking the remainder, exactly as AWS divides them
/// (shard i starts at i * floor(2^128 / n)).
[[nodiscard]] inline auto even_ranges(std::uint32_t n) -> std::vector<hash_range> {
    if (n == 0) {
        throw std::invalid_argument("kinesis: a stream needs at least one shard");
    }
    // floor(2^128 / n) without 2^128: (max / n) is it unless n divides
    // 2^128 exactly, which only powers of two do.
    hash_key step = max_hash_key / n;
    if ((n & (n - 1)) == 0) {
        step += 1;
    }
    std::vector<hash_range> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const hash_key start = step * i;
        const hash_key end = i + 1 == n ? max_hash_key : (step * (i + 1)) - 1;
        out.push_back({start, end});
    }
    return out;
}

}  // namespace kythira::kinesis
