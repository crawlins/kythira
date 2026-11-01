// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file crc32.hpp
/// @brief CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), the checksum
///        the disk-backed build cache's pack records and binary log segments
///        carry (.kiro/specs/disk-backed-build-cache/ design Components 1a
///        and 4).
///
/// A CRC here detects torn writes and bit rot, not tampering: every chunk is
/// also named by its SHA-256, which is what reads are checked against. The
/// table is built at compile time so the header has no dependency beyond the
/// standard library; zlib's `crc32` computes the same function and would only
/// add a link dependency for a few hundred bytes of code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kythira {

namespace detail {

[[nodiscard]] constexpr auto make_crc32_table() noexcept -> std::array<std::uint32_t, 256> {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
        }
        table[i] = c;
    }
    return table;
}

inline constexpr auto crc32_table = make_crc32_table();

}  // namespace detail

/// Continue a CRC-32 over `bytes`. Start with `crc = 0`; feed the previous
/// result back in to checksum data that arrives in pieces.
[[nodiscard]] constexpr auto crc32_update(std::uint32_t crc,
                                          std::span<const std::byte> bytes) noexcept
    -> std::uint32_t {
    crc = ~crc;
    for (const std::byte b : bytes) {
        crc = detail::crc32_table[(crc ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

[[nodiscard]] constexpr auto crc32(std::span<const std::byte> bytes) noexcept -> std::uint32_t {
    return crc32_update(0, bytes);
}

}  // namespace kythira
