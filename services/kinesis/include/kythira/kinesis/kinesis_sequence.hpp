// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_sequence.hpp
/// @brief Sequence numbers and shard ids as clients see them
///        (.kiro/specs/kinesis-compatible-streams/ Requirements 8.1 and 8.2).
///
/// A record's position is `(shard ordinal, per-shard counter, sub-record
/// index)`. The counter lives in the stream state machine and increments in
/// `apply()`, so it is independent of the Raft index and of which group
/// holds the shard (Requirement 8.1). Two printed forms:
///
/// - `aws` (the default): a 56-digit decimal, the length AWS prints, laid
///   out as `49` + ordinal (10 digits) + counter (20) + sub index (5) + 19
///   zeros. Fixed width, so comparing the strings compares the integers,
///   and within a shard that is record order (Requirement 8.2). The leading
///   `49` matches what AWS's numbers start with, which some clients log and
///   a few tests eyeball.
/// - `compact`: the counter alone, in decimal without leading zeros.
///
/// The trailing zeros are reserved; parsing an `aws` number that has
/// anything else there fails, so a number from real AWS is rejected as
/// `InvalidArgumentException` rather than misread as a position here.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kythira::kinesis {

enum class sequence_profile : std::uint8_t {
    aws,
    compact
};

struct sequence_number {
    std::uint32_t _ordinal = 0;
    std::uint64_t _counter = 0;
    std::uint32_t _sub = 0;

    friend auto operator==(const sequence_number&, const sequence_number&) -> bool = default;
    friend auto operator<=>(const sequence_number&, const sequence_number&) = default;
};

inline constexpr std::size_t aws_sequence_digits = 56;
inline constexpr std::uint32_t max_sub_index = 99'999;

namespace detail {

inline auto put_padded(std::string& out, std::uint64_t v, std::size_t width) -> void {
    std::array<char, 20> buf{};
    std::size_t n = 0;
    do {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    out.append(width - n, '0');
    while (n != 0) {
        out.push_back(buf[--n]);
    }
}

/// Digits only, at most 20, fitting in 64 bits.
[[nodiscard]] inline auto parse_u64(std::string_view s) -> std::optional<std::uint64_t> {
    if (s.empty() || s.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return std::nullopt;
        }
        v = (v * 10) + d;
    }
    return v;
}

}  // namespace detail

/// Print a position in `profile`'s form. The sub index must not exceed
/// `max_sub_index` (it is five digits in the `aws` form).
[[nodiscard]] inline auto format_sequence(const sequence_number& s,
                                          sequence_profile profile = sequence_profile::aws)
    -> std::string {
    if (profile == sequence_profile::compact) {
        return std::to_string(s._counter);
    }
    std::string out = "49";
    out.reserve(aws_sequence_digits);
    detail::put_padded(out, s._ordinal, 10);
    detail::put_padded(out, s._counter, 20);
    detail::put_padded(out, s._sub > max_sub_index ? max_sub_index : s._sub, 5);
    out.append(aws_sequence_digits - out.size(), '0');
    return out;
}

/// Parse a printed position. In the `aws` profile only the 56-digit form is
/// accepted; in `compact` only a bare counter (ordinal and sub index come
/// back as zero; the request names the shard). `nullopt` means the client
/// sent something that is not one of ours.
[[nodiscard]] inline auto parse_sequence(std::string_view s,
                                         sequence_profile profile = sequence_profile::aws)
    -> std::optional<sequence_number> {
    if (profile == sequence_profile::compact) {
        auto c = detail::parse_u64(s);
        if (!c) {
            return std::nullopt;
        }
        return sequence_number{._counter = *c};
    }
    if (s.size() != aws_sequence_digits || !s.starts_with("49")) {
        return std::nullopt;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    if (s.substr(37).find_first_not_of('0') != std::string_view::npos) {
        return std::nullopt;
    }
    auto ordinal = detail::parse_u64(s.substr(2, 10));
    auto counter = detail::parse_u64(s.substr(12, 20));
    auto sub = detail::parse_u64(s.substr(32, 5));
    if (!ordinal || !counter || !sub || *ordinal > UINT32_MAX) {
        return std::nullopt;
    }
    return sequence_number{._ordinal = static_cast<std::uint32_t>(*ordinal),
                           ._counter = *counter,
                           ._sub = static_cast<std::uint32_t>(*sub)};
}

/// `shardId-` plus the ordinal in 12 digits, as AWS names shards.
[[nodiscard]] inline auto shard_id_of(std::uint32_t ordinal) -> std::string {
    std::string out = "shardId-";
    detail::put_padded(out, ordinal, 12);
    return out;
}

/// The ordinal a shard id names: `shardId-` plus digits, or the bare
/// digits (a lenient reading; anything else is `nullopt`).
[[nodiscard]] inline auto parse_shard_id(std::string_view id) -> std::optional<std::uint32_t> {
    if (id.starts_with("shardId-")) {
        id.remove_prefix(8);
    }
    auto v = detail::parse_u64(id);
    if (!v || *v > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*v);
}

}  // namespace kythira::kinesis
