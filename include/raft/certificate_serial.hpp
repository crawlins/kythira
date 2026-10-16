// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file certificate_serial.hpp
/// @brief `certificate_serial`: an X.509 certificate serial number of up to 20
///        octets (RFC 5280 §4.1.2.2), printed as lower-case hex.
///
/// The in-house `certificate_authority` issues 64-bit serials, but cloud CAs
/// (ACM Private CA, CAS, OCI Certificates) issue up to 20 octets, which a
/// `std::uint64_t` cannot hold. `.kiro/specs/cloud-composite-node-ids/`
/// Requirement 12.5.
///
/// The value is the serial's big-endian magnitude with leading zero octets
/// stripped, so `from_u64(5)`, `from_hex("05")` and `from_hex("0005")` are the
/// same serial and compare and hash equal. Ordering is numeric.
///
/// No OpenSSL types here; `certificate_serial_x509.hpp` converts to and from
/// `ASN1_INTEGER` and reads the serial out of a certificate.

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace raft::testing {

class certificate_serial {
public:
    /// RFC 5280 §4.1.2.2: conforming CAs SHALL NOT use serials longer than 20
    /// octets.
    static constexpr std::size_t max_octets = 20;

    /// The zero serial (no octets).
    constexpr certificate_serial() noexcept = default;

    /// A serial that fits 64 bits, as the in-house CA issues.
    [[nodiscard]] static constexpr auto from_u64(std::uint64_t value) noexcept
        -> certificate_serial {
        certificate_serial out;
        std::array<std::uint8_t, 8> be{};
        for (std::size_t i = 0; i < be.size(); ++i) {
            be[be.size() - 1 - i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
        out.assign_stripped(be);
        return out;
    }

    /// A big-endian magnitude. Leading zero octets are stripped first, so a
    /// DER content with its 0x00 sign octet is accepted. `std::nullopt` when
    /// more than `max_octets` significant octets remain.
    [[nodiscard]] static constexpr auto from_bytes(std::span<const std::uint8_t> big_endian)
        -> std::optional<certificate_serial> {
        const auto first = std::ranges::find_if(big_endian, [](auto b) { return b != 0; });
        const auto significant = static_cast<std::size_t>(big_endian.end() - first);
        if (significant > max_octets) {
            return std::nullopt;
        }
        certificate_serial out;
        out.assign_stripped(big_endian);
        return out;
    }

    /// Strict hex: an even, non-zero number of hex digits (either case), at
    /// most `2 * max_octets` of them, nothing else — no prefix, sign,
    /// whitespace or separators. `std::nullopt` otherwise.
    [[nodiscard]] static constexpr auto from_hex(std::string_view hex)
        -> std::optional<certificate_serial> {
        if (hex.empty() || hex.size() % 2 != 0 || hex.size() > 2 * max_octets) {
            return std::nullopt;
        }
        std::array<std::uint8_t, max_octets> be{};
        const std::size_t n = hex.size() / 2;
        for (std::size_t i = 0; i < n; ++i) {
            const int hi = hex_digit(hex[2 * i]);
            const int lo = hex_digit(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            be[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        return from_bytes(std::span<const std::uint8_t>(be.data(), n));
    }

    /// Hex as tools print it: either `from_hex`'s strict form, or octets of
    /// exactly two hex digits separated by single `:` (`openssl x509 -text`,
    /// ACM's `DescribeCertificate`) or single spaces (OCI's `serialNumber`).
    /// Mixed separators, empty groups and leading or trailing separators are
    /// rejected.
    [[nodiscard]] static constexpr auto parse(std::string_view text)
        -> std::optional<certificate_serial> {
        const bool colon = text.find(':') != std::string_view::npos;
        const bool space = text.find(' ') != std::string_view::npos;
        if (!colon && !space) {
            return from_hex(text);
        }
        if (colon && space) {
            return std::nullopt;
        }
        const char sep = colon ? ':' : ' ';
        // "aa:bb:cc" is 3n-1 characters for n octets.
        if ((text.size() + 1) % 3 != 0) {
            return std::nullopt;
        }
        const std::size_t n = (text.size() + 1) / 3;
        if (n > max_octets) {
            return std::nullopt;
        }
        std::array<char, 2 * max_octets> digits{};
        for (std::size_t i = 0; i < n; ++i) {
            if (i + 1 < n && text[3 * i + 2] != sep) {
                return std::nullopt;
            }
            digits[2 * i] = text[3 * i];
            digits[2 * i + 1] = text[3 * i + 1];
        }
        return from_hex(std::string_view(digits.data(), 2 * n));
    }

    /// Lower-case hex of the minimal big-endian magnitude, always an even
    /// number of digits; the zero serial is "00". `from_hex(to_hex())`
    /// round-trips.
    [[nodiscard]] auto to_hex() const -> std::string {
        if (_length == 0) {
            return "00";
        }
        constexpr std::string_view digits = "0123456789abcdef";
        std::string out;
        out.reserve(2 * _length);
        for (std::size_t i = 0; i < _length; ++i) {
            out += digits[_octets[i] >> 4];
            out += digits[_octets[i] & 0x0F];
        }
        return out;
    }

    /// `to_hex()` with `separator` between octets ("0a:1b:2c"), the form
    /// `openssl x509 -text` prints. `parse()` reads it back.
    [[nodiscard]] auto to_hex(char separator) const -> std::string {
        const auto plain = to_hex();
        std::string out;
        out.reserve(plain.size() + plain.size() / 2);
        for (std::size_t i = 0; i < plain.size(); i += 2) {
            if (i != 0) {
                out += separator;
            }
            out += plain[i];
            out += plain[i + 1];
        }
        return out;
    }

    /// The value when it fits 64 bits, as every in-house serial does.
    [[nodiscard]] constexpr auto to_u64() const noexcept -> std::optional<std::uint64_t> {
        if (_length > 8) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < _length; ++i) {
            value = (value << 8) | _octets[i];
        }
        return value;
    }

    /// The minimal big-endian magnitude (empty for zero).
    [[nodiscard]] constexpr auto bytes() const noexcept -> std::span<const std::uint8_t> {
        return {_octets.data(), _length};
    }

    /// Significant octets, 0 to `max_octets`.
    [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return _length; }

    [[nodiscard]] constexpr auto is_zero() const noexcept -> bool { return _length == 0; }

    /// Octets past `_length` are always zero, so member-wise equality is value
    /// equality.
    friend constexpr auto operator==(const certificate_serial&, const certificate_serial&) noexcept
        -> bool = default;

    /// Numeric order: canonical values have no leading zero octet, so a longer
    /// one is larger, and equal lengths compare octet by octet.
    friend constexpr auto operator<=>(const certificate_serial& a,
                                      const certificate_serial& b) noexcept
        -> std::strong_ordering {
        if (auto c = a._length <=> b._length; c != 0) {
            return c;
        }
        for (std::size_t i = 0; i < a._length; ++i) {
            if (auto c = a._octets[i] <=> b._octets[i]; c != 0) {
                return c;
            }
        }
        return std::strong_ordering::equal;
    }

    friend auto operator<<(std::ostream& os, const certificate_serial& s) -> std::ostream& {
        return os << s.to_hex();
    }

private:
    [[nodiscard]] static constexpr auto hex_digit(char c) noexcept -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    // Caller guarantees at most `max_octets` significant octets.
    constexpr void assign_stripped(std::span<const std::uint8_t> big_endian) noexcept {
        const auto first = std::ranges::find_if(big_endian, [](auto b) { return b != 0; });
        _length = static_cast<std::uint8_t>(big_endian.end() - first);
        std::ranges::copy(first, big_endian.end(), _octets.begin());
    }

    std::array<std::uint8_t, max_octets> _octets{};
    std::uint8_t _length{0};
};

}  // namespace raft::testing

template<> struct std::hash<raft::testing::certificate_serial> {
    auto operator()(const raft::testing::certificate_serial& s) const noexcept -> std::size_t {
        // FNV-1a over the canonical octets, so equal serials hash equal.
        std::uint64_t h = 14695981039346656037ULL;
        for (auto b : s.bytes()) {
            h = (h ^ b) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};
