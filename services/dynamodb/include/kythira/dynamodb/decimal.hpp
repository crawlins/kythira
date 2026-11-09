// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file decimal.hpp
/// @brief DynamoDB's `N` type: an exact decimal of up to 38 significant
///        digits (`.kiro/specs/dynamodb-compatible-api/` task 3,
///        Requirement 6.2 and 6.3).
///
/// A value is a sign, 1 to 38 decimal digits with no leading or trailing
/// zero, and a base-10 exponent: value = digits x 10^exponent. Zero has no
/// digits and no sign. Everything is integer arithmetic on digit arrays, so
/// parsing, formatting, `+`, `-` and comparison give the same answer on
/// every platform; there is no floating point anywhere.
///
/// The limits are AWS's: at most 38 significant digits (leading and trailing
/// zeros do not count), and a magnitude between 1E-130 and
/// 9.9999999999999999999999999999999999999E+125, which is an adjusted
/// exponent (the exponent of the leading digit) between -130 and 125. A
/// value outside them is refused, never rounded: arithmetic whose exact
/// result needs a 39th digit fails the way a 39-digit literal does. The
/// error text is AWS's (`message()`); task 8's DynamoDB Local differential
/// is what pins it.
///
/// `to_string()` writes plain notation, as DynamoDB returns numbers:
/// `1E+2` reads back as `100`, `1.50` as `1.5`, `-0` as `0`.
///
/// `encode_key()` is the order-preserving byte form used for key attributes
/// (Requirement 6.3): comparing two encodings as unsigned bytes gives the
/// numeric order, and an encoding is self-delimiting, so it can sit in the
/// middle of a composite key. Layout:
///
///   zero      0x80
///   positive  0x81, adjusted exponent + 130, digit pairs as 1..100, 0x00
///   negative  0x7F, then every following byte of the positive form of the
///             magnitude inverted (255 - b), so the terminator is 0xFF
///
/// Digit pairs are base-100 digits from the most significant end, an odd
/// count padded with one trailing zero digit; the terminator sorts below any
/// pair byte, so a shorter mantissa (a prefix, hence a smaller magnitude)
/// sorts first.

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::dynamodb {

/// The outcome of parsing or arithmetic.
enum class decimal_status : std::uint8_t {
    ok,
    not_a_number,     ///< not a decimal literal
    too_many_digits,  ///< more than 38 significant digits
    overflow,         ///< magnitude above 9.99...E+125
    underflow,        ///< non-zero magnitude below 1E-130
};

class dynamo_decimal {
public:
    static constexpr int k_max_digits = 38;
    static constexpr int k_max_adjusted_exponent = 125;
    static constexpr int k_min_adjusted_exponent = -130;

    /// Zero.
    constexpr dynamo_decimal() = default;

    /// Parses a DynamoDB number literal: an optional sign, digits with at
    /// most one decimal point (at least one digit, either side), and an
    /// optional exponent (`e` or `E`, optional sign, digits). No whitespace,
    /// no `Infinity` or `NaN`, no hexadecimal. On anything but `ok`, `out` is
    /// unchanged.
    [[nodiscard]] static auto parse(std::string_view text, dynamo_decimal& out) -> decimal_status {
        std::size_t i = 0;
        bool neg = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            neg = text[i] == '-';
            ++i;
        }
        std::vector<std::uint8_t> digits;
        digits.reserve(text.size());
        std::int64_t frac = 0;
        bool point = false;
        bool any = false;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c >= '0' && c <= '9') {
                any = true;
                // Leading zeros carry no information; dropping them here keeps
                // a long run of them from costing memory.
                if (!digits.empty() || c != '0') {
                    digits.push_back(static_cast<std::uint8_t>(c - '0'));
                }
                if (point) {
                    ++frac;
                }
            } else if (c == '.' && !point) {
                point = true;
            } else {
                break;
            }
        }
        if (!any) {
            return decimal_status::not_a_number;
        }
        std::int64_t exponent = 0;
        if (i < text.size()) {
            if (text[i] != 'e' && text[i] != 'E') {
                return decimal_status::not_a_number;
            }
            ++i;
            bool eneg = false;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                eneg = text[i] == '-';
                ++i;
            }
            if (i == text.size()) {
                return decimal_status::not_a_number;
            }
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (c < '0' || c > '9') {
                    return decimal_status::not_a_number;
                }
                // Saturate: anything this large is out of range either way,
                // and the digit count below cannot pull it back.
                if (exponent < k_exponent_saturation) {
                    exponent = exponent * 10 + (c - '0');
                }
            }
            if (eneg) {
                exponent = -exponent;
            }
        }
        return normalize(neg, digits, exponent - frac, out);
    }

    /// Parses or returns nothing; for literals known to be valid.
    [[nodiscard]] static auto from_string(std::string_view text) -> std::optional<dynamo_decimal> {
        dynamo_decimal d;
        if (parse(text, d) != decimal_status::ok) {
            return std::nullopt;
        }
        return d;
    }

    [[nodiscard]] constexpr auto is_zero() const noexcept -> bool { return _ndigits == 0; }
    [[nodiscard]] constexpr auto is_negative() const noexcept -> bool { return _negative; }
    /// Significant digits; zero has none.
    [[nodiscard]] constexpr auto digit_count() const noexcept -> int { return _ndigits; }
    /// The exponent of the leading digit. Meaningless for zero.
    [[nodiscard]] constexpr auto adjusted_exponent() const noexcept -> int {
        return _exponent + _ndigits - 1;
    }

    /// Plain notation, as DynamoDB writes numbers back.
    [[nodiscard]] auto to_string() const -> std::string {
        if (is_zero()) {
            return "0";
        }
        std::string out;
        if (_negative) {
            out.push_back('-');
        }
        const int n = _ndigits;
        const int int_digits = n + _exponent;  // digits left of the point
        if (_exponent >= 0) {
            append_digits(out, 0, n);
            out.append(static_cast<std::size_t>(_exponent), '0');
        } else if (int_digits > 0) {
            append_digits(out, 0, int_digits);
            out.push_back('.');
            append_digits(out, int_digits, n);
        } else {
            out += "0.";
            out.append(static_cast<std::size_t>(-int_digits), '0');
            append_digits(out, 0, n);
        }
        return out;
    }

    /// The approximate stored size AWS's item-size formula charges a number:
    /// one byte per two significant digits, rounded up, plus one.
    [[nodiscard]] constexpr auto size_bytes() const noexcept -> std::size_t {
        return static_cast<std::size_t>((std::max<int>(_ndigits, 1) + 1) / 2) + 1;
    }

    [[nodiscard]] auto negated() const -> dynamo_decimal {
        dynamo_decimal r = *this;
        if (!r.is_zero()) {
            r._negative = !r._negative;
        }
        return r;
    }

    /// Exact `a + b`. Fails rather than round when the result needs more
    /// than 38 digits or leaves the range.
    [[nodiscard]] static auto add(const dynamo_decimal& a, const dynamo_decimal& b,
                                  dynamo_decimal& out) -> decimal_status {
        if (a.is_zero()) {
            out = b;
            return decimal_status::ok;
        }
        if (b.is_zero()) {
            out = a;
            return decimal_status::ok;
        }
        // Both operands on one digit grid starting at the smaller exponent.
        // The span is at most 125 + 130 + 37 digits, plus one for a carry.
        const int lo = std::min(a._exponent, b._exponent);
        const int hi = std::max(a.adjusted_exponent(), b.adjusted_exponent());
        const auto width = static_cast<std::size_t>(hi - lo) + 2U;
        std::vector<int> x(width, 0);  // least significant first
        std::vector<int> y(width, 0);
        a.scatter(x, lo);
        b.scatter(y, lo);
        bool neg = a._negative;
        if (a._negative == b._negative) {
            for (std::size_t k = 0; k < width; ++k) {
                x[k] += y[k];
                if (x[k] >= 10) {
                    x[k] -= 10;
                    ++x[k + 1];
                }
            }
        } else {
            // Subtract the smaller magnitude from the larger; the sign is the
            // larger's.
            if (compare_magnitude(a, b) == std::strong_ordering::less) {
                std::swap(x, y);
                neg = b._negative;
            }
            for (std::size_t k = 0; k < width; ++k) {
                x[k] -= y[k];
                if (x[k] < 0) {
                    x[k] += 10;
                    --x[k + 1];
                }
            }
        }
        std::vector<std::uint8_t> digits;  // most significant first
        digits.reserve(width);
        bool started = false;
        for (std::size_t k = width; k-- > 0;) {
            if (started || x[k] != 0) {
                started = true;
                digits.push_back(static_cast<std::uint8_t>(x[k]));
            }
        }
        return normalize(neg, digits, lo, out);
    }

    /// Exact `a - b`.
    [[nodiscard]] static auto subtract(const dynamo_decimal& a, const dynamo_decimal& b,
                                       dynamo_decimal& out) -> decimal_status {
        return add(a, b.negated(), out);
    }

    friend auto operator<=>(const dynamo_decimal& a, const dynamo_decimal& b) noexcept
        -> std::strong_ordering {
        const int sa = a.signum();
        const int sb = b.signum();
        if (sa != sb) {
            return sa <=> sb;
        }
        if (sa == 0) {
            return std::strong_ordering::equal;
        }
        const auto m = compare_magnitude(a, b);
        return sa > 0 ? m : 0 <=> m;
    }
    friend auto operator==(const dynamo_decimal& a, const dynamo_decimal& b) noexcept -> bool {
        return (a <=> b) == std::strong_ordering::equal;
    }

    /// The order-preserving, self-delimiting key encoding (see the file
    /// comment).
    [[nodiscard]] auto encode_key() const -> std::string {
        std::string out;
        if (is_zero()) {
            out.push_back(static_cast<char>(k_zero_tag));
            return out;
        }
        out.reserve(3 + static_cast<std::size_t>((_ndigits + 1) / 2));
        out.push_back(static_cast<char>(_negative ? k_negative_tag : k_positive_tag));
        const auto put = [&](unsigned b) {
            out.push_back(static_cast<char>(_negative ? 255U - b : b));
        };
        put(static_cast<unsigned>(adjusted_exponent() - k_min_adjusted_exponent));
        for (int k = 0; k < _ndigits; k += 2) {
            const unsigned hi_digit = _digits[static_cast<std::size_t>(k)];
            const unsigned lo_digit =
                k + 1 < _ndigits ? _digits[static_cast<std::size_t>(k) + 1U] : 0U;
            put(hi_digit * 10U + lo_digit + 1U);
        }
        put(0U);
        return out;
    }

    /// Reads one key encoding from the front of `in`, advancing it past the
    /// bytes used. Nothing when the bytes are not an encoding.
    [[nodiscard]] static auto decode_key(std::string_view& in) -> std::optional<dynamo_decimal> {
        if (in.empty()) {
            return std::nullopt;
        }
        const auto tag = static_cast<unsigned char>(in[0]);
        if (tag == k_zero_tag) {
            in.remove_prefix(1);
            return dynamo_decimal{};
        }
        if (tag != k_positive_tag && tag != k_negative_tag) {
            return std::nullopt;
        }
        const bool neg = tag == k_negative_tag;
        const auto get = [&](std::size_t k) -> unsigned {
            const auto b = static_cast<unsigned char>(in[k]);
            return neg ? 255U - b : b;
        };
        if (in.size() < 3) {
            return std::nullopt;
        }
        const int adjusted = static_cast<int>(get(1)) + k_min_adjusted_exponent;
        if (adjusted > k_max_adjusted_exponent) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> digits;
        std::size_t k = 2;
        for (;; ++k) {
            if (k >= in.size()) {
                return std::nullopt;
            }
            const unsigned b = get(k);
            if (b == 0U) {
                break;
            }
            if (b > 100U) {
                return std::nullopt;
            }
            digits.push_back(static_cast<std::uint8_t>((b - 1U) / 10U));
            digits.push_back(static_cast<std::uint8_t>((b - 1U) % 10U));
        }
        // Canonical only: a leading zero digit, a trailing zero pair or a
        // padded odd count whose pad is not zero would decode to a value
        // that re-encodes differently.
        if (digits.empty() || digits.front() == 0 ||
            (digits.size() >= 2 && digits[digits.size() - 1] == 0 &&
             digits[digits.size() - 2] == 0)) {
            return std::nullopt;
        }
        if (digits.back() == 0) {
            digits.pop_back();
        }
        if (digits.size() > static_cast<std::size_t>(k_max_digits)) {
            return std::nullopt;
        }
        dynamo_decimal d;
        d._negative = neg;
        d._ndigits = static_cast<std::uint8_t>(digits.size());
        std::copy(digits.begin(), digits.end(), d._digits.begin());
        d._exponent = static_cast<std::int16_t>(adjusted - d._ndigits + 1);
        in.remove_prefix(k + 1);
        return d;
    }

    /// AWS's `ValidationException` text for a failed parse or computation.
    /// `text` is the offending literal, when there is one.
    [[nodiscard]] static auto message(decimal_status s, std::string_view text = {}) -> std::string {
        switch (s) {
            case decimal_status::ok:
                return {};
            case decimal_status::not_a_number:
                return "The parameter cannot be converted to a numeric value: " + std::string(text);
            case decimal_status::too_many_digits:
                return "Attempting to store more than 38 significant digits in a Number";
            case decimal_status::overflow:
                return "Number overflow. Attempting to store a number with magnitude larger than "
                       "supported range";
            case decimal_status::underflow:
                return "Number underflow. Attempting to store a number with magnitude smaller "
                       "than supported range";
        }
        return {};
    }

private:
    static constexpr unsigned k_negative_tag = 0x7FU;
    static constexpr unsigned k_zero_tag = 0x80U;
    static constexpr unsigned k_positive_tag = 0x81U;
    static constexpr std::int64_t k_exponent_saturation = 1'000'000'000;

    bool _negative = false;
    std::uint8_t _ndigits = 0;
    std::int16_t _exponent = 0;
    std::array<std::uint8_t, k_max_digits> _digits{};

    [[nodiscard]] constexpr auto signum() const noexcept -> int {
        if (is_zero()) {
            return 0;
        }
        return _negative ? -1 : 1;
    }

    void append_digits(std::string& out, int from, int to) const {
        for (int k = from; k < to; ++k) {
            out.push_back(static_cast<char>('0' + _digits[static_cast<std::size_t>(k)]));
        }
    }

    // Writes the digits into a least-significant-first grid whose index 0 is
    // the digit at 10^lo.
    void scatter(std::vector<int>& grid, int lo) const {
        for (int k = 0; k < _ndigits; ++k) {
            const int power = _exponent + (_ndigits - 1 - k);
            grid[static_cast<std::size_t>(power - lo)] = _digits[static_cast<std::size_t>(k)];
        }
    }

    [[nodiscard]] static auto compare_magnitude(const dynamo_decimal& a,
                                                const dynamo_decimal& b) noexcept
        -> std::strong_ordering {
        if (a.is_zero() || b.is_zero()) {
            return a._ndigits <=> b._ndigits;
        }
        if (const auto c = a.adjusted_exponent() <=> b.adjusted_exponent(); c != 0) {
            return c;
        }
        const int n = std::min(a._ndigits, b._ndigits);
        for (int k = 0; k < n; ++k) {
            const auto i = static_cast<std::size_t>(k);
            if (const auto c = a._digits[i] <=> b._digits[i]; c != 0) {
                return c;
            }
        }
        // No trailing zeros, so the longer one has more non-zero digits.
        return a._ndigits <=> b._ndigits;
    }

    // Strips leading and trailing zeros from `digits` (most significant
    // first, values 0-9) and checks the limits.
    [[nodiscard]] static auto normalize(bool neg, const std::vector<std::uint8_t>& digits,
                                        std::int64_t exponent, dynamo_decimal& out)
        -> decimal_status {
        std::size_t first = 0;
        while (first < digits.size() && digits[first] == 0) {
            ++first;
        }
        if (first == digits.size()) {
            out = dynamo_decimal{};
            return decimal_status::ok;
        }
        std::size_t last = digits.size();
        while (digits[last - 1] == 0) {
            --last;
        }
        exponent += static_cast<std::int64_t>(digits.size() - last);
        const std::size_t n = last - first;
        if (n > static_cast<std::size_t>(k_max_digits)) {
            return decimal_status::too_many_digits;
        }
        const std::int64_t adjusted = exponent + static_cast<std::int64_t>(n) - 1;
        if (adjusted > k_max_adjusted_exponent) {
            return decimal_status::overflow;
        }
        if (adjusted < k_min_adjusted_exponent) {
            return decimal_status::underflow;
        }
        dynamo_decimal d;
        d._negative = neg;
        d._ndigits = static_cast<std::uint8_t>(n);
        d._exponent = static_cast<std::int16_t>(exponent);
        std::copy(digits.begin() + static_cast<std::ptrdiff_t>(first),
                  digits.begin() + static_cast<std::ptrdiff_t>(last), d._digits.begin());
        out = d;
        return decimal_status::ok;
    }
};

}  // namespace kythira::dynamodb
