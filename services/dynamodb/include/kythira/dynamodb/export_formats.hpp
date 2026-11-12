// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file export_formats.hpp
/// @brief The file formats of `ExportTableToPointInTime`, `ImportTable` and
///        `StartBulkPutItems` (`.kiro/specs/dynamodb-compatible-api/` task 20,
///        Requirements 17.3 and 22.1): Amazon Ion text written and read,
///        CSV read, gzip written and read, zstd read, and the MD5 digests
///        the export manifests carry.
///
/// Items cross these formats as DynamoDB JSON (`item_to_json`,
/// `item_from_json`), so every value a reader produces is checked by the
/// same code that checks a `PutItem`.
///
/// Ion follows AWS's export mapping: S is a string, N a decimal, B a blob,
/// BOOL a bool, NULL a null, L a list, M a struct, and SS, NS and BS lists
/// annotated `$dynamodb_SS`, `$dynamodb_NS` and `$dynamodb_BS`. The reader
/// takes anything an Ion text writer may produce for those: ints (decimal,
/// hex, binary, with underscores), decimals and floats become N; strings,
/// long strings and symbols become S; blobs and clobs become B. Timestamps,
/// s-expressions, typed nulls other than `null.null`, and nan or infinities
/// are refused, as they have no DynamoDB type.
///
/// gzip and zstd need zlib and libzstd at build time
/// (`KYTHIRA_DYNAMODB_HAS_ZLIB`, `KYTHIRA_DYNAMODB_HAS_ZSTD`, set by the
/// module's CMake when it finds them). Without zlib, exports are written
/// uncompressed (`.json`, `.ion`) and gzip input is refused; without
/// libzstd, zstd input is refused.

#include <kythira/dynamodb/item_codec.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(KYTHIRA_DYNAMODB_HAS_ZLIB)
#include <zlib.h>
#endif
#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
#include <zstd.h>
#endif

namespace kythira::dynamodb::export_formats {

/// A malformed input file, or one in a format this build cannot read.
class format_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// -- Digests ------------------------------------------------------------------

[[nodiscard]] inline auto md5_raw(std::string_view data) -> std::array<unsigned char, 16> {
    std::array<unsigned char, 16> out{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_md5(), nullptr) != 1 ||
        len != out.size()) {
        throw std::runtime_error("MD5 is not available");
    }
    return out;
}

/// Base64 of the raw digest: the manifest's `md5`, S3's `Content-MD5`.
[[nodiscard]] inline auto md5_base64(std::string_view data) -> std::string {
    const auto raw = md5_raw(data);
    return detail::base64_of(
        std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));  // NOLINT
}

/// Lowercase hex of the digest: a single-part S3 object's ETag.
[[nodiscard]] inline auto md5_hex(std::string_view data) -> std::string {
    static constexpr std::string_view k_hex = "0123456789abcdef";
    std::string out;
    for (const auto b : md5_raw(data)) {
        out.push_back(k_hex[b >> 4U]);
        out.push_back(k_hex[b & 0xFU]);
    }
    return out;
}

// -- Compression --------------------------------------------------------------

[[nodiscard]] constexpr auto has_gzip() -> bool {
#if defined(KYTHIRA_DYNAMODB_HAS_ZLIB)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] constexpr auto has_zstd() -> bool {
#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
    return true;
#else
    return false;
#endif
}

/// `data` as one gzip member. Throws without zlib.
[[nodiscard]] inline auto gzip(std::string_view data) -> std::string {
#if defined(KYTHIRA_DYNAMODB_HAS_ZLIB)
    z_stream z{};
    // 15 window bits, plus 16 for a gzip header and trailer.
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        throw std::runtime_error("gzip: deflateInit2 failed");
    }
    std::string out;
    out.resize(deflateBound(&z, static_cast<uLong>(data.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));  // NOLINT
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());  // NOLINT
    z.avail_out = static_cast<uInt>(out.size());
    const auto rc = deflate(&z, Z_FINISH);
    const auto written = z.total_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("gzip: deflate failed");
    }
    out.resize(written);
    return out;
#else
    (void)data;
    throw std::runtime_error("gzip: this build has no zlib");
#endif
}

/// Every gzip member of `data`, inflated and joined.
[[nodiscard]] inline auto gunzip(std::string_view data) -> std::string {
#if defined(KYTHIRA_DYNAMODB_HAS_ZLIB)
    std::string out;
    std::array<char, 65536> buf{};
    z_stream z{};
    // 15 window bits, plus 32 to take a gzip or zlib header.
    if (inflateInit2(&z, 15 + 32) != Z_OK) {
        throw format_error("gzip: inflateInit2 failed");
    }
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));  // NOLINT
    z.avail_in = static_cast<uInt>(data.size());
    for (;;) {
        z.next_out = reinterpret_cast<Bytef*>(buf.data());  // NOLINT
        z.avail_out = static_cast<uInt>(buf.size());
        const auto rc = inflate(&z, Z_NO_FLUSH);
        out.append(buf.data(), buf.size() - z.avail_out);
        if (rc == Z_STREAM_END) {
            if (z.avail_in == 0) {
                break;
            }
            // Another member follows (concatenated gzip files are one file).
            inflateReset(&z);
            continue;
        }
        if (rc != Z_OK) {
            inflateEnd(&z);
            throw format_error("gzip: the input is not valid gzip data");
        }
        if (z.avail_in == 0 && z.avail_out != 0) {
            inflateEnd(&z);
            throw format_error("gzip: the input ends inside a gzip member");
        }
    }
    inflateEnd(&z);
    return out;
#else
    (void)data;
    throw format_error("GZIP input is not supported: this build has no zlib");
#endif
}

/// Every zstd frame of `data`, decompressed and joined.
[[nodiscard]] inline auto unzstd(std::string_view data) -> std::string {
#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
    std::string out;
    std::array<char, 65536> buf{};
    ZSTD_DStream* z = ZSTD_createDStream();
    if (z == nullptr) {
        throw format_error("zstd: ZSTD_createDStream failed");
    }
    ZSTD_inBuffer in{data.data(), data.size(), 0};
    std::size_t last = 0;
    while (in.pos < in.size) {
        ZSTD_outBuffer o{buf.data(), buf.size(), 0};
        last = ZSTD_decompressStream(z, &o, &in);
        if (ZSTD_isError(last) != 0) {
            ZSTD_freeDStream(z);
            throw format_error(std::string("zstd: ") + ZSTD_getErrorName(last));
        }
        out.append(buf.data(), o.pos);
    }
    // Drain what the last frame still holds.
    while (last != 0) {
        ZSTD_outBuffer o{buf.data(), buf.size(), 0};
        last = ZSTD_decompressStream(z, &o, &in);
        if (ZSTD_isError(last) != 0 || o.pos == 0) {
            ZSTD_freeDStream(z);
            throw format_error("zstd: the input ends inside a frame");
        }
        out.append(buf.data(), o.pos);
    }
    ZSTD_freeDStream(z);
    return out;
#else
    (void)data;
    throw format_error("ZSTD input is not supported: this build has no libzstd");
#endif
}

/// `data` decompressed as `InputCompressionType` names it (`NONE`, `GZIP`,
/// `ZSTD`).
[[nodiscard]] inline auto decompress(std::string_view data, std::string_view compression)
    -> std::string {
    if (compression == "GZIP") {
        return gunzip(data);
    }
    if (compression == "ZSTD") {
        return unzstd(data);
    }
    return std::string(data);
}

// -- Ion text: writing --------------------------------------------------------

namespace ion_detail {

[[nodiscard]] inline auto is_keyword(std::string_view s) -> bool {
    return s == "null" || s == "true" || s == "false" || s == "nan";
}

/// True when `s` can be written as a bare symbol. `$` is left out so that
/// no name reads as a symbol id (`$10`) or a system symbol.
[[nodiscard]] inline auto bare_symbol(std::string_view s) -> bool {
    if (s.empty() || is_keyword(s)) {
        return false;
    }
    const auto letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return letter(s.front()) &&
           std::ranges::all_of(s, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

/// Appends `s` quoted with `quote` (`"` for a string, `'` for a symbol).
inline void quoted(std::string& out, std::string_view s, char quote) {
    static constexpr std::string_view k_hex = "0123456789abcdef";
    out.push_back(quote);
    for (const auto ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == quote || ch == '\\') {
            out.push_back('\\');
            out.push_back(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (c < 0x20U || c == 0x7FU) {
            out += "\\x";
            out.push_back(k_hex[c >> 4U]);
            out.push_back(k_hex[c & 0xFU]);
        } else {
            // UTF-8 is written as is: Ion text is UTF-8.
            out.push_back(ch);
        }
    }
    out.push_back(quote);
}

inline void field_name(std::string& out, std::string_view name) {
    if (bare_symbol(name)) {
        out += name;
    } else {
        quoted(out, name, '\'');
    }
}

/// A DynamoDB number as an Ion decimal: `123` is `123.`, `1.5E+3` is
/// `1.5d3`.
inline void decimal(std::string& out, std::string_view n) {
    const auto e = n.find_first_of("eE");
    const auto mantissa = n.substr(0, e);
    out += mantissa;
    if (e == std::string_view::npos) {
        if (mantissa.find('.') == std::string_view::npos) {
            out.push_back('.');
        }
        return;
    }
    out.push_back('d');
    auto exponent = n.substr(e + 1);
    if (!exponent.empty() && exponent.front() == '+') {
        exponent.remove_prefix(1);
    }
    out += exponent;
}

inline void blob(std::string& out, std::string_view base64) {
    out += "{{";
    out += base64;
    out += "}}";
}

}  // namespace ion_detail

/// Appends one DynamoDB JSON attribute value (`{"S":"x"}`) as Ion text.
inline void write_ion_value(std::string& out, const boost::json::value& attr) {
    namespace id = ion_detail;
    const auto& o = attr.as_object();
    if (o.size() != 1) {
        throw std::invalid_argument("write_ion_value: not one typed value");
    }
    const auto& member = *o.begin();
    const std::string_view type = member.key();
    const boost::json::value& v = member.value();
    const auto set = [&](std::string_view annotation, auto each) {
        out += annotation;
        out += "::[";
        bool first = true;
        for (const auto& e : v.as_array()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            each(e.as_string());
        }
        out.push_back(']');
    };
    if (type == "S") {
        id::quoted(out, v.as_string(), '"');
    } else if (type == "N") {
        id::decimal(out, v.as_string());
    } else if (type == "B") {
        id::blob(out, v.as_string());
    } else if (type == "BOOL") {
        out += v.as_bool() ? "true" : "false";
    } else if (type == "NULL") {
        out += "null";
    } else if (type == "SS") {
        set("$dynamodb_SS", [&](std::string_view s) { id::quoted(out, s, '"'); });
    } else if (type == "NS") {
        set("$dynamodb_NS", [&](std::string_view s) { id::decimal(out, s); });
    } else if (type == "BS") {
        set("$dynamodb_BS", [&](std::string_view s) { id::blob(out, s); });
    } else if (type == "L") {
        out.push_back('[');
        bool first = true;
        for (const auto& e : v.as_array()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            write_ion_value(out, e);
        }
        out.push_back(']');
    } else if (type == "M") {
        out.push_back('{');
        bool first = true;
        for (const auto& kv : v.as_object()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            id::field_name(out, kv.key());
            out.push_back(':');
            write_ion_value(out, kv.value());
        }
        out.push_back('}');
    } else {
        throw std::invalid_argument("write_ion_value: unknown type " + std::string(type));
    }
}

/// Appends an item (a DynamoDB JSON `Item` map) as an Ion struct.
inline void write_ion_item(std::string& out, const boost::json::object& item) {
    out.push_back('{');
    bool first = true;
    for (const auto& kv : item) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        ion_detail::field_name(out, kv.key());
        out.push_back(':');
        write_ion_value(out, kv.value());
    }
    out.push_back('}');
}

/// The version marker an Ion text file starts with.
inline constexpr std::string_view k_ion_version_marker = "$ion_1_0";

// -- Ion text: reading --------------------------------------------------------

/// Reads Ion text as a sequence of top-level values, each turned into a
/// DynamoDB JSON attribute value. `next()` returns nothing at the end and
/// throws `format_error` on text it cannot read.
class ion_reader {
public:
    explicit ion_reader(std::string_view text) : _s(text) {}

    [[nodiscard]] auto next() -> std::optional<boost::json::value> {
        for (;;) {
            skip_space();
            if (_at >= _s.size()) {
                return std::nullopt;
            }
            auto v = value(0, true);
            if (v) {
                return v;
            }
            // A version marker, skipped.
        }
    }

private:
    static constexpr int k_max_depth = 64;

    [[noreturn]] void fail(const std::string& what) const {
        throw format_error("Ion text at byte " + std::to_string(_at) + ": " + what);
    }

    [[nodiscard]] auto peek(std::size_t ahead = 0) const -> char {
        return _at + ahead < _s.size() ? _s[_at + ahead] : '\0';
    }

    void skip_space() {
        while (_at < _s.size()) {
            const auto c = _s[_at];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
                ++_at;
            } else if (c == '/' && peek(1) == '/') {
                while (_at < _s.size() && _s[_at] != '\n') {
                    ++_at;
                }
            } else if (c == '/' && peek(1) == '*') {
                const auto end = _s.find("*/", _at + 2);
                if (end == std::string_view::npos) {
                    fail("unterminated comment");
                }
                _at = end + 2;
            } else {
                return;
            }
        }
    }

    static auto ident_start(char c) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    }
    static auto ident_char(char c) -> bool { return ident_start(c) || (c >= '0' && c <= '9'); }

    auto identifier() -> std::string_view {
        const auto from = _at;
        while (_at < _s.size() && ident_char(_s[_at])) {
            ++_at;
        }
        return _s.substr(from, _at - from);
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    auto hex_digits(int n) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const auto c = peek();
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("bad hex escape");
            }
            v = v * 16U + d;
            ++_at;
        }
        return v;
    }

    /// The body of a quoted run up to `quote` (one char, or `'''`), with
    /// escapes resolved.
    void quoted_body(std::string& out, std::string_view quote) {
        for (;;) {
            if (_at >= _s.size()) {
                fail("unterminated string");
            }
            if (_s.substr(_at, quote.size()) == quote) {
                _at += quote.size();
                return;
            }
            const auto c = _s[_at++];
            if (c != '\\') {
                if (quote.size() == 1 && c == '\n') {
                    fail("newline in a quoted string");
                }
                out.push_back(c);
                continue;
            }
            const auto e = peek();
            ++_at;
            switch (e) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case '0':
                    out.push_back('\0');
                    break;
                case 'a':
                    out.push_back('\a');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'v':
                    out.push_back('\v');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                case '\'':
                    out.push_back('\'');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case '?':
                    out.push_back('?');
                    break;
                case 'x':
                    append_utf8(out, hex_digits(2));
                    break;
                case 'u':
                    append_utf8(out, hex_digits(4));
                    break;
                case 'U':
                    append_utf8(out, hex_digits(8));
                    break;
                case '\n':
                    break;  // a line continuation
                case '\r':
                    if (peek() == '\n') {
                        ++_at;
                    }
                    break;
                default:
                    fail("bad escape");
            }
        }
    }

    /// One or more adjacent long strings, joined.
    auto long_strings() -> std::string {
        std::string out;
        while (_s.substr(_at, 3) == "'''") {
            _at += 3;
            quoted_body(out, "'''");
            const auto save = _at;
            skip_space();
            if (_s.substr(_at, 3) != "'''") {
                _at = save;
                break;
            }
        }
        return out;
    }

    /// A number token turned into a DynamoDB N string.
    auto number() -> std::string {
        const auto from = _at;
        while (_at < _s.size()) {
            const auto c = _s[_at];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '.' || c == '_' || c == '+' || c == '-') {
                // A sign is part of the number only at its start or after
                // an exponent letter.
                if ((c == '+' || c == '-') && _at != from) {
                    const auto prev = _s[_at - 1];
                    if (prev != 'e' && prev != 'E' && prev != 'd' && prev != 'D') {
                        break;
                    }
                }
                ++_at;
            } else {
                break;
            }
        }
        std::string t;
        for (const auto c : _s.substr(from, _at - from)) {
            if (c != '_') {
                t.push_back(c);
            }
        }
        bool negative = false;
        std::string_view body(t);
        if (!body.empty() && body.front() == '-') {
            negative = true;
            body.remove_prefix(1);
        }
        if (body.size() > 2 && body[0] == '0' &&
            (body[1] == 'x' || body[1] == 'X' || body[1] == 'b' || body[1] == 'B')) {
            const int base = body[1] == 'x' || body[1] == 'X' ? 16 : 2;
            std::size_t used = 0;
            unsigned long long v = 0;
            try {
                v = std::stoull(std::string(body.substr(2)), &used, base);
            } catch (const std::exception&) {
                fail("integer out of range");
            }
            if (used != body.size() - 2) {
                fail("bad integer " + t);
            }
            return (negative ? "-" : "") + std::to_string(v);
        }
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        bool digits = false;
        bool point = false;
        bool exponent = false;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const auto c = body[i];
            if (c >= '0' && c <= '9') {
                digits = true;
                out.push_back(c);
            } else if (c == '.' && !point && !exponent) {
                point = true;
                // `123.` is the decimal 123; DynamoDB wants no bare point.
                if (i + 1 < body.size() && body[i + 1] >= '0' && body[i + 1] <= '9') {
                    out.push_back('.');
                }
            } else if ((c == 'e' || c == 'E' || c == 'd' || c == 'D') && digits && !exponent) {
                exponent = true;
                out.push_back('E');
                if (i + 1 < body.size() && (body[i + 1] == '+' || body[i + 1] == '-')) {
                    out.push_back(body[++i]);
                }
                if (i + 1 >= body.size()) {
                    fail("bad number " + t);
                }
            } else {
                fail("bad number " + t);
            }
        }
        if (!digits) {
            fail("bad number " + t);
        }
        return out;
    }

    /// `{{ base64 }}` or `{{ "clob" }}` / `{{ '''clob''' }}`, as base64.
    auto lob() -> std::string {
        _at += 2;
        skip_space();
        std::string raw;
        bool clob = false;
        if (peek() == '"') {
            ++_at;
            quoted_body(raw, "\"");
            clob = true;
        } else if (_s.substr(_at, 3) == "'''") {
            raw = long_strings();
            clob = true;
        }
        std::string b64;
        if (!clob) {
            while (_at < _s.size() && _s[_at] != '}') {
                const auto c = _s[_at++];
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    b64.push_back(c);
                }
            }
        }
        skip_space();
        if (_s.substr(_at, 2) != "}}") {
            fail("unterminated blob");
        }
        _at += 2;
        if (clob) {
            return detail::base64_of(raw);
        }
        if (!aws_json::base64_decode(b64)) {
            fail("blob is not base64");
        }
        return b64;
    }

    static auto attr(std::string_view type, boost::json::value v) -> boost::json::value {
        boost::json::object o;
        o[type] = std::move(v);
        return o;
    }

    /// One value, with any annotations. Returns nothing for a top-level
    /// version marker.
    auto value(int depth, bool top) -> std::optional<boost::json::value> {
        if (depth > k_max_depth) {
            fail("values nested too deeply");
        }
        skip_space();
        std::vector<std::string> annotations;
        for (;;) {
            // An annotation is a symbol followed by `::`.
            const auto save = _at;
            std::string sym;
            if (peek() == '\'' && _s.substr(_at, 3) != "'''") {
                ++_at;
                quoted_body(sym, "'");
            } else if (ident_start(peek())) {
                sym = std::string(identifier());
            } else {
                break;
            }
            skip_space();
            if (_s.substr(_at, 2) == "::") {
                _at += 2;
                skip_space();
                annotations.push_back(std::move(sym));
                continue;
            }
            _at = save;
            break;
        }
        const auto set_type = [&]() -> std::string_view {
            for (const auto& a : annotations) {
                if (a == "$dynamodb_SS") {
                    return "SS";
                }
                if (a == "$dynamodb_NS") {
                    return "NS";
                }
                if (a == "$dynamodb_BS") {
                    return "BS";
                }
            }
            return {};
        };
        const auto c = peek();
        if (c == '{' && peek(1) == '{') {
            return attr("B", boost::json::string(lob()));
        }
        if (c == '{') {
            ++_at;
            boost::json::object m;
            for (;;) {
                skip_space();
                if (peek() == '}') {
                    ++_at;
                    break;
                }
                std::string name;
                if (peek() == '"') {
                    ++_at;
                    quoted_body(name, "\"");
                } else if (_s.substr(_at, 3) == "'''") {
                    name = long_strings();
                } else if (peek() == '\'') {
                    ++_at;
                    quoted_body(name, "'");
                } else if (ident_start(peek())) {
                    name = std::string(identifier());
                } else {
                    fail("expected a field name");
                }
                skip_space();
                if (peek() != ':') {
                    fail("expected ':' after field " + name);
                }
                ++_at;
                auto v = value(depth + 1, false);
                m[name] = std::move(*v);
                skip_space();
                if (peek() == ',') {
                    ++_at;
                } else if (peek() != '}') {
                    fail("expected ',' or '}' in a struct");
                }
            }
            return attr("M", std::move(m));
        }
        if (c == '[') {
            ++_at;
            const auto st = set_type();
            boost::json::array list;
            for (;;) {
                skip_space();
                if (peek() == ']') {
                    ++_at;
                    break;
                }
                auto v = value(depth + 1, false);
                if (st.empty()) {
                    list.push_back(std::move(*v));
                } else {
                    // A set's members are bare scalars of the set's type.
                    const auto& member = v->as_object();
                    const auto* const want = st == "SS" ? "S" : st == "NS" ? "N" : "B";
                    const auto* inner = member.if_contains(want);
                    if (inner == nullptr) {
                        fail(std::string("a ") + std::string(st) + " member is not " + want);
                    }
                    list.push_back(*inner);
                }
                skip_space();
                if (peek() == ',') {
                    ++_at;
                } else if (peek() != ']') {
                    fail("expected ',' or ']' in a list");
                }
            }
            return attr(st.empty() ? "L" : st, std::move(list));
        }
        if (c == '(') {
            fail("s-expressions have no DynamoDB type");
        }
        if (c == '"') {
            ++_at;
            std::string s;
            quoted_body(s, "\"");
            return attr("S", boost::json::string(s));
        }
        if (_s.substr(_at, 3) == "'''") {
            return attr("S", boost::json::string(long_strings()));
        }
        if (c == '\'') {
            ++_at;
            std::string s;
            quoted_body(s, "'");
            return attr("S", boost::json::string(s));
        }
        if ((c >= '0' && c <= '9') || (c == '-' && peek(1) >= '0' && peek(1) <= '9')) {
            // A timestamp starts with four digits and a dash or `T`.
            if (c != '-' && _at + 4 < _s.size() && (_s[_at + 4] == '-' || _s[_at + 4] == 'T') &&
                std::all_of(_s.begin() + static_cast<std::ptrdiff_t>(_at),
                            _s.begin() + static_cast<std::ptrdiff_t>(_at + 4),
                            [](char d) { return d >= '0' && d <= '9'; })) {
                fail("timestamps have no DynamoDB type");
            }
            return attr("N", boost::json::string(number()));
        }
        if (c == '+' || c == '-') {
            fail("infinities have no DynamoDB type");
        }
        if (ident_start(c)) {
            const auto word = identifier();
            if (word == "null") {
                if (peek() == '.') {
                    ++_at;
                    const auto t = identifier();
                    if (t != "null") {
                        fail("typed null null." + std::string(t) + " has no DynamoDB type");
                    }
                }
                return attr("NULL", true);
            }
            if (word == "true" || word == "false") {
                return attr("BOOL", word == "true");
            }
            if (word == "nan") {
                fail("nan has no DynamoDB type");
            }
            if (top && annotations.empty() && word.starts_with("$ion_")) {
                return std::nullopt;
            }
            return attr("S", boost::json::string(word));
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    std::string_view _s;
    std::size_t _at = 0;
};

// -- CSV ----------------------------------------------------------------------

/// Calls `row(fields)` for each record of RFC 4180 CSV with `delimiter`:
/// fields may be double-quoted, a quote inside one doubled; records end at
/// LF or CRLF. Blank lines are skipped. Returns false from `row` to stop.
template<typename Row> void read_csv(std::string_view text, char delimiter, Row row) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    bool any = false;
    std::size_t i = 0;
    const auto end_record = [&] {
        fields.push_back(std::move(field));
        field.clear();
        const bool blank = fields.size() == 1 && fields[0].empty() && !any;
        bool go = true;
        if (!blank) {
            go = row(fields);
        }
        fields.clear();
        any = false;
        return go;
    };
    while (i < text.size()) {
        const auto c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    i += 2;
                    continue;
                }
                quoted = false;
                ++i;
                continue;
            }
            field.push_back(c);
            ++i;
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
            any = true;
            ++i;
        } else if (c == delimiter) {
            fields.push_back(std::move(field));
            field.clear();
            any = true;
            ++i;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            ++i;
            if (!end_record()) {
                return;
            }
        } else {
            field.push_back(c);
            any = true;
            ++i;
        }
    }
    if (quoted) {
        throw format_error("CSV ends inside a quoted field");
    }
    if (any || !field.empty()) {
        (void)end_record();
    }
}

}  // namespace kythira::dynamodb::export_formats
