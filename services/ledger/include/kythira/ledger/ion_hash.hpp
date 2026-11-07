// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Amazon Ion hash (amazon-ion-hash specification) over ion-c's reader
// (.kiro/specs/verifiable-ledger/ task 2, Requirement 12.1).
//
// Ion hash gives every Ion value one hash however it was encoded: text or
// binary, any field order, any symbol table, any equivalent timestamp or
// float encoding. The ledger hashes documents, revision metadata,
// statements and block headers with it (design.md, "Hashing"), so a block
// hash depends on the data, not on how a client happened to write it. No
// C or C++ implementation exists upstream; this one follows the spec and
// is checked against the amazon-ion/ion-hash-test vectors
// (tests/vectors/ion_hash_tests.ion).
//
// The algorithm, with H the hash function (SHA-256 here):
//
//   H(value)       = h(s(value))
//   s(scalar)      = B || TQ || escape(representation) || E
//   s(list, sexp)  = B || TQ || s(child_1) || ... || s(child_n) || E
//   s(struct)      = B || TQ || escape(sort(H(field_1) ... H(field_n))) || E
//   H(field)       = h(s(field name as a symbol) || s(field value))
//   s(annotated)   = B || 0xE0 || s(annotation_1) ... s(annotation_n) || s(value) || E
//
// B = 0x0B, E = 0x0E, and escape() puts 0x0C before every 0x0B, 0x0C and
// 0x0E. TQ is the Ion binary type descriptor with the length nibble
// replaced: 0x0F for a typed null, the value for a bool, 0 otherwise. The
// representation is the canonical Ion binary content: ints by magnitude
// with T=3 for negatives, floats always as 64-bit with NaN canonical and
// +0e0 empty, decimals and timestamps without a positive zero coefficient,
// timestamps in UTC with a zero fraction of non-negative exponent dropped,
// and symbols, strings and lobs as their bytes (a symbol with no text is
// TQ 0x71, empty). Field hashes are sorted as unsigned byte strings and only
// then escaped.

#include <kythira/ledger/ledger_hash.hpp>

#include <raft/exceptions.hpp>
#include <raft/ion_preflight.hpp>

#include <ionc/ion.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// Thrown for input the Ion hash cannot be computed over: malformed Ion,
/// nesting beyond the reader's limits, or not exactly one value.
class ion_hash_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The hash function h the algorithm is parameterised over. The ledger uses
/// SHA-256; the spec's test vectors also use the identity function, which
/// exposes the exact bytes s() produces.
using ion_hash_function = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)>;

/// SHA-256 as an `ion_hash_function`.
inline auto sha256_hash_function() -> ion_hash_function {
    return [](std::span<const std::uint8_t> bytes) {
        const hash256 h = sha256(bytes);
        return std::vector<std::uint8_t>(h.begin(), h.end());
    };
}

namespace ion_hash_detail {

inline constexpr std::uint8_t begin_marker = 0x0B;
inline constexpr std::uint8_t end_marker = 0x0E;
inline constexpr std::uint8_t escape_byte = 0x0C;
inline constexpr std::uint8_t annotation_tq = 0xE0;
inline constexpr std::uint8_t symbol_no_text_tq = 0x71;

using bytes = std::vector<std::uint8_t>;

inline auto check(iERR err, const char* what) -> void {
    if (err != IERR_OK) {
        throw ion_hash_error(std::string("ion hash: ") + what + ": " + ion_error_to_str(err));
    }
}

inline auto append(bytes& out, std::span<const std::uint8_t> in) -> void {
    out.insert(out.end(), in.begin(), in.end());
}

inline auto append_escaped(bytes& out, std::span<const std::uint8_t> in) -> void {
    for (const std::uint8_t b : in) {
        if (b == begin_marker || b == end_marker || b == escape_byte) {
            out.push_back(escape_byte);
        }
        out.push_back(b);
    }
}

/// Ion binary VarUInt: 7 bits per byte, most significant first, the last
/// byte flagged with 0x80.
inline auto append_var_uint(bytes& out, std::uint64_t value) -> void {
    bytes groups;
    do {
        groups.push_back(static_cast<std::uint8_t>(value & 0x7FU));
        value >>= 7U;
    } while (value != 0);
    std::ranges::reverse(groups);
    groups.back() |= 0x80U;
    append(out, groups);
}

/// Ion binary VarInt: as VarUInt, but the first byte gives up its 0x40 bit to
/// the sign. `negative` with a zero magnitude is negative zero (0xC0).
inline auto append_var_int(bytes& out, std::uint64_t magnitude, bool negative) -> void {
    bytes groups;
    do {
        groups.push_back(static_cast<std::uint8_t>(magnitude & 0x7FU));
        magnitude >>= 7U;
    } while (magnitude != 0);
    std::ranges::reverse(groups);
    if ((groups.front() & 0x40U) != 0) {
        groups.insert(groups.begin(), 0);
    }
    if (negative) {
        groups.front() |= 0x40U;
    }
    groups.back() |= 0x80U;
    append(out, groups);
}

inline auto append_var_int(bytes& out, std::int64_t value) -> void {
    const bool negative = value < 0;
    const std::uint64_t magnitude =
        negative ? (~static_cast<std::uint64_t>(value) + 1U) : static_cast<std::uint64_t>(value);
    append_var_int(out, magnitude, negative);
}

/// Ion binary Int (signed magnitude, big-endian, minimal): `magnitude` has no
/// leading zero bytes; an empty magnitude is zero.
inline auto append_signed_magnitude(bytes& out, const bytes& magnitude, bool negative) -> void {
    const std::size_t at = out.size();
    if (magnitude.empty() || (magnitude.front() & 0x80U) != 0) {
        out.push_back(0);
    }
    append(out, magnitude);
    if (negative) {
        out[at] |= 0x80U;
    }
}

/// Decimal digits (most significant first, one digit per byte) to a minimal
/// big-endian unsigned magnitude.
inline auto bcd_to_magnitude(std::span<const std::uint8_t> digits) -> bytes {
    bytes le;  // little-endian base-256 accumulator
    for (const std::uint8_t d : digits) {
        unsigned carry = d;
        for (auto& b : le) {
            const unsigned v = (static_cast<unsigned>(b) * 10U) + carry;
            b = static_cast<std::uint8_t>(v & 0xFFU);
            carry = v >> 8U;
        }
        while (carry != 0) {
            le.push_back(static_cast<std::uint8_t>(carry & 0xFFU));
            carry >>= 8U;
        }
    }
    while (!le.empty() && le.back() == 0) {
        le.pop_back();
    }
    std::ranges::reverse(le);
    return le;
}

/// A decimal as the Ion binary model sees it: sign, coefficient magnitude
/// and exponent.
struct decimal_parts {
    bool negative = false;
    bytes coefficient;  // minimal big-endian; empty for zero
    std::int32_t exponent = 0;
};

inline auto parts_of(const decQuad& quad) -> decimal_parts {
    std::array<std::uint8_t, DECQUAD_Pmax> digits{};
    decimal_parts p;
    p.negative = decQuadGetCoefficient(&quad, digits.data()) != 0;
    p.exponent = decQuadGetExponent(&quad);
    p.coefficient = bcd_to_magnitude(digits);
    return p;
}

inline auto parts_of(const ION_DECIMAL& dec) -> decimal_parts {
    if (dec.type == ION_DECIMAL_TYPE_QUAD) {
        return parts_of(dec.value.quad_value);
    }
    const decNumber* dn = dec.value.num_value;
    if (dn == nullptr || (dn->bits & DECSPECIAL) != 0) {
        throw ion_hash_error("ion hash: decimal is not a finite number");
    }
    std::vector<std::uint8_t> digits(static_cast<std::size_t>(dn->digits));
    decNumberGetBCD(dn, digits.data());
    decimal_parts p;
    p.negative = decNumberIsNegative(dn);
    p.exponent = dn->exponent;
    p.coefficient = bcd_to_magnitude(digits);
    return p;
}

/// The canonical binary decimal content: nothing for 0d0, otherwise the
/// exponent and then the coefficient unless it is positive zero.
inline auto decimal_representation(const decimal_parts& p) -> bytes {
    bytes out;
    const bool positive_zero = p.coefficient.empty() && !p.negative;
    if (positive_zero && p.exponent == 0) {
        return out;
    }
    append_var_int(out, p.exponent);
    if (!positive_zero) {
        append_signed_magnitude(out, p.coefficient, p.negative);
    }
    return out;
}

/// The canonical binary timestamp content: offset, then the UTC fields the
/// precision has, then a fraction unless it is zero with a non-negative
/// exponent (those add no precision: 00.0 is kept, 00d0 and 00d1 are not).
inline auto timestamp_representation(ION_TIMESTAMP& ts) -> bytes {
    int precision = 0;
    check(ion_timestamp_get_precision(&ts, &precision), "reading timestamp precision");
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    decQuad fraction;
    std::memset(&fraction, 0, sizeof(fraction));
    check(ion_timestamp_get_thru_fraction(&ts, &year, &month, &day, &hour, &minute, &second,
                                          &fraction),
          "reading timestamp fields");
    BOOL has_offset = FALSE;
    int offset = 0;
    check(ion_timestamp_has_local_offset(&ts, &has_offset), "reading timestamp offset");
    if (has_offset == TRUE) {
        check(ion_timestamp_get_local_offset(&ts, &offset), "reading timestamp offset");
    }

    const bool has_month = (precision & ION_TT_BIT_MONTH) != 0;
    const bool has_day = (precision & ION_TT_BIT_DAY) != 0;
    const bool has_minute = (precision & ION_TT_BIT_MIN) != 0;
    const bool has_second = (precision & ION_TT_BIT_SEC) != 0;
    const bool has_fraction = (precision & ION_TT_BIT_FRAC) != 0;

    if (has_minute && offset != 0) {
        // ion-c keeps the local fields; the binary form, and so the hash,
        // carries UTC. Seconds and the fraction do not move.
        using namespace std::chrono;
        const sys_days date = std::chrono::year{year} /
                              std::chrono::month{static_cast<unsigned>(month)} /
                              std::chrono::day{static_cast<unsigned>(day)};
        const auto local = date + hours{hour} + minutes{minute};
        const auto utc = local - minutes{offset};
        const auto utc_day = floor<days>(utc);
        const year_month_day ymd{utc_day};
        const hh_mm_ss<minutes> hm{duration_cast<minutes>(utc - utc_day)};
        year = static_cast<int>(ymd.year());
        month = static_cast<int>(static_cast<unsigned>(ymd.month()));
        day = static_cast<int>(static_cast<unsigned>(ymd.day()));
        hour = static_cast<int>(hm.hours().count());
        minute = static_cast<int>(hm.minutes().count());
    }
    if (year < 1) {
        throw ion_hash_error("ion hash: timestamp year out of range");
    }

    bytes out;
    if (has_offset == TRUE) {
        append_var_int(out, static_cast<std::int64_t>(offset));
    } else {
        append_var_int(out, 0, true);  // -00:00, unknown offset
    }
    append_var_uint(out, static_cast<std::uint64_t>(year));
    if (has_month) {
        append_var_uint(out, static_cast<std::uint64_t>(month));
    }
    if (has_day) {
        append_var_uint(out, static_cast<std::uint64_t>(day));
    }
    if (has_minute) {
        append_var_uint(out, static_cast<std::uint64_t>(hour));
        append_var_uint(out, static_cast<std::uint64_t>(minute));
    }
    if (has_second) {
        append_var_uint(out, static_cast<std::uint64_t>(second));
    }
    if (has_fraction) {
        const decimal_parts f = parts_of(fraction);
        if (f.negative) {
            throw ion_hash_error("ion hash: negative timestamp fraction");
        }
        if (!f.coefficient.empty() || f.exponent < 0) {
            append_var_int(out, f.exponent);
            if (!f.coefficient.empty()) {
                append_signed_magnitude(out, f.coefficient, false);
            }
        }
    }
    return out;
}

/// A float's 64-bit big-endian encoding: NaN canonical, +0e0 empty.
inline auto float_representation(double value) -> bytes {
    if (value == 0.0 && !std::signbit(value)) {
        return {};
    }
    std::uint64_t bits =
        std::isnan(value) ? 0x7FF8000000000000ULL : std::bit_cast<std::uint64_t>(value);
    bytes out(8);
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits & 0xFFU);
        bits >>= 8U;
    }
    return out;
}

/// The type nibble of an ion-c type id (tid_INT -> 2, and so on).
inline auto type_nibble(ION_TYPE type) -> std::uint8_t {
    return static_cast<std::uint8_t>((ION_TYPE_INT(type) >> 8) & 0x0F);
}

/// s() of a symbol given as an ion-c symbol (field names, annotations and
/// symbol values alike).
inline auto serialize_symbol(const ION_SYMBOL& symbol) -> bytes {
    bytes out{begin_marker};
    if (symbol.value.value == nullptr) {
        out.push_back(symbol_no_text_tq);
    } else {
        out.push_back(0x70);
        append_escaped(out, std::span<const std::uint8_t>(
                                symbol.value.value, static_cast<std::size_t>(symbol.value.length)));
    }
    out.push_back(end_marker);
    return out;
}

/// The bytes of the current string or lob, read in pieces until ion-c has
/// no more.
///
/// Not by length first: ion-c's text reader reports a string's or lob's
/// length only when the whole value fits under its single-value threshold
/// (4 KiB) or lookahead, and a refused ion_reader_get_string_length has
/// already consumed part of the value, so the reads after it return a
/// truncated string (measured: 3616 of 20000 bytes). Partial reads alone
/// work in both encodings. They end differently: the text reader returns 0
/// bytes, the binary reader moves past the value after its last byte and
/// refuses the next call with IERR_INVALID_STATE, which here means "done"
/// once bytes have been read.
template<typename ReadPartial>
auto read_in_pieces(ReadPartial&& read_partial, const char* what) -> bytes {
    constexpr std::size_t chunk = 64 * 1024;
    bytes out;
    for (;;) {
        const std::size_t at = out.size();
        out.resize(at + chunk);
        SIZE got = 0;
        const iERR err = read_partial(out.data() + at, static_cast<SIZE>(chunk), &got);
        if (err == IERR_INVALID_STATE && at > 0) {
            out.resize(at);
            return out;
        }
        check(err, what);
        out.resize(at + static_cast<std::size_t>(got > 0 ? got : 0));
        if (got <= 0) {
            return out;
        }
    }
}

inline auto read_string_bytes(hREADER reader) -> bytes {
    return read_in_pieces(
        [reader](BYTE* buf, SIZE max, SIZE* got) {
            return ion_reader_read_partial_string(reader, buf, max, got);
        },
        "reading string");
}

inline auto read_lob_bytes(hREADER reader) -> bytes {
    return read_in_pieces(
        [reader](BYTE* buf, SIZE max, SIZE* got) {
            return ion_reader_read_lob_partial_bytes(reader, buf, max, got);
        },
        "reading lob");
}

/// TQ and representation of the current non-null scalar.
inline auto scalar_parts(hREADER reader, ION_TYPE type) -> std::pair<std::uint8_t, bytes> {
    const auto t = static_cast<std::uint8_t>(type_nibble(type) << 4U);
    switch (ION_TYPE_INT(type)) {
        case tid_BOOL_INT: {
            BOOL value = FALSE;
            check(ion_reader_read_bool(reader, &value), "reading bool");
            return {static_cast<std::uint8_t>(t | (value == TRUE ? 1U : 0U)), {}};
        }
        case tid_INT_INT: {
            ION_INT* big = nullptr;
            check(ion_int_alloc(nullptr, &big), "allocating int");
            struct int_guard {
                ION_INT* p;
                ~int_guard() { ion_int_free(p); }
            } guard{big};
            check(ion_reader_read_ion_int(reader, big), "reading int");
            int32_t signum = 0;
            check(ion_int_signum(big, &signum), "reading int sign");
            SIZE length = 0;
            check(ion_int_abs_bytes_length(big, &length), "reading int length");
            bytes magnitude(static_cast<std::size_t>(length));
            if (length > 0) {
                SIZE written = 0;
                check(ion_int_to_abs_bytes(big, 0, magnitude.data(), length, &written),
                      "reading int magnitude");
                magnitude.resize(static_cast<std::size_t>(written));
            }
            const auto first =
                std::ranges::find_if(magnitude, [](std::uint8_t b) { return b != 0; });
            magnitude.erase(magnitude.begin(), first);
            return {static_cast<std::uint8_t>(signum < 0 ? 0x30 : 0x20), std::move(magnitude)};
        }
        case tid_FLOAT_INT: {
            double value = 0;
            check(ion_reader_read_double(reader, &value), "reading float");
            return {t, float_representation(value)};
        }
        case tid_DECIMAL_INT: {
            ION_DECIMAL dec;
            std::memset(&dec, 0, sizeof(dec));
            check(ion_reader_read_ion_decimal(reader, &dec), "reading decimal");
            struct decimal_guard {
                ION_DECIMAL* p;
                ~decimal_guard() { ion_decimal_free(p); }
            } guard{&dec};
            return {t, decimal_representation(parts_of(dec))};
        }
        case tid_TIMESTAMP_INT: {
            ION_TIMESTAMP ts;
            std::memset(&ts, 0, sizeof(ts));
            check(ion_reader_read_timestamp(reader, &ts), "reading timestamp");
            return {t, timestamp_representation(ts)};
        }
        case tid_STRING_INT:
            return {t, read_string_bytes(reader)};
        case tid_CLOB_INT:
        case tid_BLOB_INT:
            return {t, read_lob_bytes(reader)};
        default:
            throw ion_hash_error("ion hash: unexpected scalar type");
    }
}

}  // namespace ion_hash_detail

/// The Ion hash with a chosen hash function h.
class ion_hasher {
public:
    explicit ion_hasher(ion_hash_function h) : h_(std::move(h)) {}

    /// H(value) of the value `reader` is positioned on (after
    /// `ion_reader_next` returned its type), ignoring any field name. On
    /// return the reader is still on that value, so the caller's
    /// `ion_reader_next` moves past it.
    [[nodiscard]] auto hash_current(hREADER reader) const -> std::vector<std::uint8_t> {
        return h_(serialize_current(reader));
    }

    /// s(value) of the current value: the bytes H hashes.
    [[nodiscard]] auto serialize_current(hREADER reader) const -> std::vector<std::uint8_t> {
        return serialize_at(reader, 0);
    }

    /// Containers nested deeper than this are rejected. ion-c's text reader
    /// does not apply its own max_container_depth, and the walk recurses.
    static constexpr std::size_t max_depth = kythira::detail::ion_preflight_max_depth;

private:
    [[nodiscard]] auto serialize_at(hREADER reader, std::size_t depth) const
        -> std::vector<std::uint8_t> {
        namespace d = ion_hash_detail;
        ION_TYPE type = nullptr;
        d::check(ion_reader_get_type(reader, &type), "reading type");

        // Annotations first: stepping into a container drops them.
        SIZE annotation_count = 0;
        d::check(ion_reader_get_annotation_count(reader, &annotation_count), "reading annotations");
        d::bytes out;
        if (annotation_count > 0) {
            std::vector<ION_SYMBOL> annotations(static_cast<std::size_t>(annotation_count));
            SIZE got = 0;
            d::check(ion_reader_get_annotation_symbols(reader, annotations.data(), annotation_count,
                                                       &got),
                     "reading annotations");
            out = {d::begin_marker, d::annotation_tq};
            for (SIZE i = 0; i < got; ++i) {
                d::append(out, d::serialize_symbol(annotations[static_cast<std::size_t>(i)]));
            }
        }
        d::bytes body = serialize_unannotated(reader, type, depth);
        if (annotation_count == 0) {
            return body;
        }
        d::append(out, body);
        out.push_back(d::end_marker);
        return out;
    }

    [[nodiscard]] auto serialize_unannotated(hREADER reader, ION_TYPE type, std::size_t depth) const
        -> std::vector<std::uint8_t> {
        namespace d = ion_hash_detail;
        BOOL is_null = FALSE;
        d::check(ion_reader_is_null(reader, &is_null), "reading null flag");
        if (is_null == TRUE) {
            return {d::begin_marker,
                    static_cast<std::uint8_t>((d::type_nibble(type) << 4U) | 0x0FU), d::end_marker};
        }
        switch (ION_TYPE_INT(type)) {
            case tid_SYMBOL_INT: {
                ION_SYMBOL symbol;
                std::memset(&symbol, 0, sizeof(symbol));
                d::check(ion_reader_read_ion_symbol(reader, &symbol), "reading symbol");
                return d::serialize_symbol(symbol);
            }
            case tid_LIST_INT:
            case tid_SEXP_INT: {
                check_depth(depth);
                d::bytes out{d::begin_marker,
                             static_cast<std::uint8_t>(d::type_nibble(type) << 4U)};
                d::check(ion_reader_step_in(reader), "stepping into sequence");
                ION_TYPE child = nullptr;
                for (;;) {
                    d::check(ion_reader_next(reader, &child), "reading sequence");
                    if (child == tid_EOF) {
                        break;
                    }
                    d::append(out, serialize_at(reader, depth + 1));
                }
                d::check(ion_reader_step_out(reader), "stepping out of sequence");
                out.push_back(d::end_marker);
                return out;
            }
            case tid_STRUCT_INT: {
                check_depth(depth);
                std::vector<d::bytes> fields;
                d::check(ion_reader_step_in(reader), "stepping into struct");
                ION_TYPE child = nullptr;
                for (;;) {
                    d::check(ion_reader_next(reader, &child), "reading struct");
                    if (child == tid_EOF) {
                        break;
                    }
                    ION_SYMBOL* name = nullptr;
                    d::check(ion_reader_get_field_name_symbol(reader, &name), "reading field name");
                    if (name == nullptr) {
                        throw ion_hash_error("ion hash: struct field without a name");
                    }
                    d::bytes field = d::serialize_symbol(*name);
                    d::append(field, serialize_at(reader, depth + 1));
                    fields.push_back(h_(field));
                }
                d::check(ion_reader_step_out(reader), "stepping out of struct");
                std::ranges::sort(fields);  // unsigned lexicographic, before escaping
                d::bytes out{d::begin_marker, 0xD0};
                for (const auto& f : fields) {
                    d::append_escaped(out, f);
                }
                out.push_back(d::end_marker);
                return out;
            }
            default: {
                auto [tq, representation] = d::scalar_parts(reader, type);
                d::bytes out{d::begin_marker, tq};
                d::append_escaped(out, representation);
                out.push_back(d::end_marker);
                return out;
            }
        }
    }

    static auto check_depth(std::size_t depth) -> void {
        if (depth >= max_depth) {
            throw ion_hash_error("ion hash: nesting too deep");
        }
    }

    ion_hash_function h_;
};

/// Reader options the ledger opens Ion with: room for documents nested
/// deeper, and annotated more, than ion-c's defaults (10 each) allow, within
/// the bound the binary preflight enforces.
inline auto ledger_ion_reader_options() -> ION_READER_OPTIONS {
    ION_READER_OPTIONS options;
    std::memset(&options, 0, sizeof(options));
    options.max_container_depth = static_cast<SIZE>(kythira::detail::ion_preflight_max_depth);
    options.max_annotation_count = 32;
    return options;
}

/// H(value) with `h` of the single value encoded in `ion` (text or binary).
/// Throws `ion_hash_error` unless `ion` holds exactly one top-level value.
inline auto ion_hash_with(const ion_hasher& hasher, std::span<const std::uint8_t> ion)
    -> std::vector<std::uint8_t> {
    namespace d = ion_hash_detail;
    std::vector<BYTE> buffer(ion.begin(), ion.end());
    if (kythira::detail::ion_is_binary(buffer.data(), buffer.data() + buffer.size())) {
        try {
            kythira::detail::ion_binary_length_preflight(buffer.data(),
                                                         buffer.data() + buffer.size(), false, 0);
        } catch (const kythira::serialization_exception& e) {
            throw ion_hash_error(std::string("ion hash: ") + e.what());
        }
    }
    BYTE dummy = 0;
    ION_READER_OPTIONS options = ledger_ion_reader_options();
    hREADER reader = nullptr;
    d::check(ion_reader_open_buffer(&reader, buffer.empty() ? &dummy : buffer.data(),
                                    static_cast<SIZE>(buffer.size()), &options),
             "opening reader");
    kythira::detail::ion_reader_handle guard{reader};
    ION_TYPE type = nullptr;
    d::check(ion_reader_next(reader, &type), "reading value");
    if (type == tid_EOF) {
        throw ion_hash_error("ion hash: no value");
    }
    std::vector<std::uint8_t> result = hasher.hash_current(reader);
    d::check(ion_reader_next(reader, &type), "reading past value");
    if (type != tid_EOF) {
        throw ion_hash_error("ion hash: more than one top-level value");
    }
    return result;
}

/// The SHA-256 Ion hash of the value the reader is on.
inline auto ion_hash_current(hREADER reader) -> hash256 {
    static const ion_hasher hasher{sha256_hash_function()};
    const auto h = hasher.hash_current(reader);
    hash256 out{};
    std::ranges::copy(h, out.begin());
    return out;
}

/// The SHA-256 Ion hash of the single value encoded in `ion`.
inline auto ion_hash(std::span<const std::uint8_t> ion) -> hash256 {
    static const ion_hasher hasher{sha256_hash_function()};
    const auto h = ion_hash_with(hasher, ion);
    hash256 out{};
    std::ranges::copy(h, out.begin());
    return out;
}

inline auto ion_hash(std::string_view ion_text) -> hash256 {
    return ion_hash(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(ion_text.data()), ion_text.size()));
}

}  // namespace kythira::ledger
