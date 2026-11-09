// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_query_protocol.hpp
/// @brief The AWS Query protocol, as IAM and STS speak it
///        (`.kiro/specs/iam-compatible-auth/` Requirement 9.1, design
///        Component 8).
///
///  - `form`: an `application/x-www-form-urlencoded` body (or a GET query
///    string) decoded, with AWS's member-list flattening read back:
///    `Tags.member.1.Key=k&Tags.member.1.Value=v` and
///    `PolicyArns.member.2=arn:...`.
///  - `xml_writer`: a small escaping XML writer, and `response_xml` /
///    `error_xml` for the two document shapes every action answers with.
///  - `marker_sealer`: opaque `Marker` values, sealed with an HMAC so a
///    client can neither forge a list position nor replay one list's
///    marker against another.
///
/// Plain functions over strings, so it has no Raft or IAM dependency and
/// any Query-protocol service can use it.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::aws_query {

namespace detail {

[[nodiscard]] constexpr auto hex_digit(char c) -> int {
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

/// Form decoding: '+' is a space and %XX a byte. A '%' without two hex
/// digits after it makes the whole body unreadable.
[[nodiscard]] inline auto form_decode(std::string_view in) -> std::optional<std::string> {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size()) {
                return std::nullopt;
            }
            const int hi = hex_digit(in[i + 1]);
            const int lo = hex_digit(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

[[nodiscard]] inline auto parse_index(std::string_view s) -> std::optional<std::size_t> {
    std::size_t v = 0;
    if (s.empty() || s.front() == '0') {
        return std::nullopt;  // member indices start at 1
    }
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

inline constexpr std::string_view k_b64url =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] inline auto b64url_encode(std::span<const unsigned char> in) -> std::string {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char b : in) {
        acc = (acc << 8U) | b;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += k_b64url[(acc >> static_cast<unsigned>(bits)) & 0x3FU];
        }
    }
    if (bits > 0) {
        out += k_b64url[(acc << static_cast<unsigned>(6 - bits)) & 0x3FU];
    }
    return out;
}

[[nodiscard]] inline auto b64url_decode(std::string_view in)
    -> std::optional<std::vector<unsigned char>> {
    std::vector<unsigned char> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        const auto pos = k_b64url.find(c);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        acc = (acc << 6U) | static_cast<std::uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    // Leftover bits must be padding zeros, and a whole leftover sextet
    // means the input was cut.
    if (bits >= 6 || (acc & ((1U << static_cast<unsigned>(bits)) - 1U)) != 0) {
        return std::nullopt;
    }
    return out;
}

}  // namespace detail

/// A decoded Query request's parameters.
class form {
public:
    /// The parameters of `body`, or empty when it is not a well-formed
    /// form or names a parameter twice (AWS refuses both).
    [[nodiscard]] static auto parse(std::string_view body) -> std::optional<form> {
        form f;
        std::size_t start = 0;
        while (start <= body.size()) {
            const auto amp = body.find('&', start);
            const auto part = body.substr(
                start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
            if (!part.empty()) {
                const auto eq = part.find('=');
                auto key = detail::form_decode(part.substr(0, eq));
                auto value = detail::form_decode(
                    eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1));
                if (!key || !value || key->empty()) {
                    return std::nullopt;
                }
                if (!f._params.emplace(std::move(*key), std::move(*value)).second) {
                    return std::nullopt;
                }
            }
            if (amp == std::string_view::npos) {
                break;
            }
            start = amp + 1;
        }
        return f;
    }

    [[nodiscard]] auto get(std::string_view name) const -> std::optional<std::string_view> {
        const auto it = _params.find(name);
        if (it == _params.end()) {
            return std::nullopt;
        }
        return std::string_view(it->second);
    }

    [[nodiscard]] auto contains(std::string_view name) const -> bool {
        return _params.find(name) != _params.end();
    }

    /// `Name.member.N` values in index order. Empty when the list is
    /// absent; `std::nullopt` when an index is not a positive number.
    [[nodiscard]] auto members(std::string_view name) const
        -> std::optional<std::vector<std::string>> {
        const auto prefix = std::string(name) + ".member.";
        std::map<std::size_t, std::string> by_index;
        for (auto it = _params.lower_bound(prefix);
             it != _params.end() && it->first.starts_with(prefix); ++it) {
            const auto rest = std::string_view(it->first).substr(prefix.size());
            const auto idx = detail::parse_index(rest);
            if (!idx) {
                return std::nullopt;
            }
            by_index.emplace(*idx, it->second);
        }
        std::vector<std::string> out;
        out.reserve(by_index.size());
        for (auto& [_, v] : by_index) {
            out.push_back(std::move(v));
        }
        return out;
    }

    /// `Name.member.N.Field` structures in index order, each a map from
    /// field to value. `std::nullopt` for a bad index or a field that is
    /// not one of `fields`.
    [[nodiscard]] auto member_structs(std::string_view name,
                                      std::span<const std::string_view> fields) const
        -> std::optional<std::vector<std::map<std::string, std::string, std::less<>>>> {
        const auto prefix = std::string(name) + ".member.";
        std::map<std::size_t, std::map<std::string, std::string, std::less<>>> by_index;
        for (auto it = _params.lower_bound(prefix);
             it != _params.end() && it->first.starts_with(prefix); ++it) {
            const auto rest = std::string_view(it->first).substr(prefix.size());
            const auto dot = rest.find('.');
            if (dot == std::string_view::npos) {
                return std::nullopt;
            }
            const auto idx = detail::parse_index(rest.substr(0, dot));
            const auto field = rest.substr(dot + 1);
            if (!idx || std::ranges::find(fields, field) == fields.end()) {
                return std::nullopt;
            }
            by_index[*idx].emplace(std::string(field), it->second);
        }
        std::vector<std::map<std::string, std::string, std::less<>>> out;
        out.reserve(by_index.size());
        for (auto& [_, v] : by_index) {
            out.push_back(std::move(v));
        }
        return out;
    }

    [[nodiscard]] auto params() const -> const std::map<std::string, std::string, std::less<>>& {
        return _params;
    }

private:
    std::map<std::string, std::string, std::less<>> _params;
};

/// RFC 3986 percent-encoding: everything but the unreserved characters
/// (A-Z a-z 0-9 - . _ ~) becomes %XX. IAM returns policy documents this way
/// inside its XML (`AssumeRolePolicyDocument`, `PolicyDocument`, `Document`).
[[nodiscard]] inline auto url_encode(std::string_view s) -> std::string {
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4U];
            out += hex[c & 0x0FU];
        }
    }
    return out;
}

/// Text for XML character data or an attribute value. Characters XML 1.0
/// cannot carry at all (C0 controls other than tab, newline and carriage
/// return) are dropped.
[[nodiscard]] inline auto xml_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            case '\t':
            case '\n':
            case '\r':
                out += c;
                break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) {
                    out += c;
                }
        }
    }
    return out;
}

/// Builds an XML document element by element.
class xml_writer {
public:
    auto open(std::string_view tag) -> xml_writer& {
        _out += '<';
        _out += tag;
        _out += '>';
        _stack.emplace_back(tag);
        return *this;
    }

    auto close() -> xml_writer& {
        _out += "</";
        _out += _stack.back();
        _out += '>';
        _stack.pop_back();
        return *this;
    }

    /// `<tag>text</tag>`, escaped.
    auto element(std::string_view tag, std::string_view text) -> xml_writer& {
        _out += '<';
        _out += tag;
        _out += '>';
        _out += xml_escape(text);
        _out += "</";
        _out += tag;
        _out += '>';
        return *this;
    }

    /// `<tag>true</tag>` or `false`. Not an `element` overload: a string
    /// literal converts to bool before it does to string_view.
    auto boolean(std::string_view tag, bool value) -> xml_writer& {
        return element(tag, std::string_view(value ? "true" : "false"));
    }

    /// Appends already-written XML.
    auto raw(std::string_view xml) -> xml_writer& {
        _out += xml;
        return *this;
    }

    [[nodiscard]] auto depth() const -> std::size_t { return _stack.size(); }
    [[nodiscard]] auto str() const -> const std::string& { return _out; }

private:
    std::string _out;
    std::vector<std::string> _stack;
};

/// `<{Action}Response xmlns=ns><{Action}Result>result</{Action}Result>
/// <ResponseMetadata><RequestId/></ResponseMetadata></{Action}Response>`.
/// An empty `result` leaves the Result element out, as AWS does for
/// actions that return nothing (DeleteUser and the like).
[[nodiscard]] inline auto response_xml(std::string_view action, std::string_view ns,
                                       std::string_view result, std::string_view request_id)
    -> std::string {
    std::string out = "<";
    out += action;
    out += "Response xmlns=\"";
    out += xml_escape(ns);
    out += "\">";
    if (!result.empty()) {
        out += "<";
        out += action;
        out += "Result>";
        out += result;
        out += "</";
        out += action;
        out += "Result>";
    }
    out += "<ResponseMetadata><RequestId>";
    out += xml_escape(request_id);
    out += "</RequestId></ResponseMetadata></";
    out += action;
    out += "Response>";
    return out;
}

/// AWS's Query error document. `sender` is the fault type: Sender for a
/// client error, Receiver for a server one.
[[nodiscard]] inline auto error_xml(std::string_view ns, bool sender, std::string_view code,
                                    std::string_view message, std::string_view request_id)
    -> std::string {
    std::string out = "<ErrorResponse xmlns=\"";
    out += xml_escape(ns);
    out += "\"><Error><Type>";
    out += sender ? "Sender" : "Receiver";
    out += "</Type><Code>";
    out += xml_escape(code);
    out += "</Code><Message>";
    out += xml_escape(message);
    out += "</Message></Error><RequestId>";
    out += xml_escape(request_id);
    out += "</RequestId></ErrorResponse>";
    return out;
}

/// Pagination markers: `base64url(last key || HMAC(context, last key)[:16])`.
/// `context` names the list (the action and whatever scopes it: account,
/// path prefix, user), so a marker opens only for the list that issued it.
class marker_sealer {
public:
    static constexpr std::size_t k_tag_size = 16;

    explicit marker_sealer(std::span<const unsigned char> key) : _key(key.begin(), key.end()) {}

    [[nodiscard]] auto seal(std::string_view context, std::string_view last_key) const
        -> std::string {
        std::vector<unsigned char> buf(last_key.begin(), last_key.end());
        const auto t = tag(context, last_key);
        buf.insert(buf.end(), t.begin(), t.begin() + k_tag_size);
        return detail::b64url_encode(buf);
    }

    /// The last key `marker` was sealed over, or empty when it is not one
    /// this sealer issued for `context`.
    [[nodiscard]] auto open(std::string_view context, std::string_view marker) const
        -> std::optional<std::string> {
        const auto raw = detail::b64url_decode(marker);
        if (!raw || raw->size() < k_tag_size) {
            return std::nullopt;
        }
        const auto key_len = raw->size() - k_tag_size;
        std::string last_key(raw->begin(), raw->begin() + static_cast<std::ptrdiff_t>(key_len));
        const auto t = tag(context, last_key);
        if (CRYPTO_memcmp(t.data(), raw->data() + key_len, k_tag_size) != 0) {
            return std::nullopt;
        }
        return last_key;
    }

private:
    [[nodiscard]] auto tag(std::string_view context, std::string_view last_key) const
        -> std::array<unsigned char, EVP_MAX_MD_SIZE> {
        // The context is length-prefixed, so ("ab", "c") and ("a", "bc")
        // cannot share a tag.
        std::string msg = std::to_string(context.size());
        msg += ':';
        msg += context;
        msg += last_key;
        std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
        unsigned int len = 0;
        HMAC(EVP_sha256(), _key.data(), static_cast<int>(_key.size()),
             reinterpret_cast<const unsigned char*>(msg.data()),  // NOLINT
             msg.size(), out.data(), &len);
        return out;
    }

    std::vector<unsigned char> _key;
};

/// What a `MaxItems` parameter asks for: the default when absent, or the
/// value when it is a whole number in [1, 1000]; empty otherwise.
[[nodiscard]] inline auto max_items(const form& f, std::size_t fallback = 100)
    -> std::optional<std::size_t> {
    const auto v = f.get("MaxItems");
    if (!v) {
        return fallback;
    }
    std::size_t n = 0;
    const auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
    if (ec != std::errc{} || p != v->data() + v->size() || n < 1 || n > 1000) {
        return std::nullopt;
    }
    return n;
}

}  // namespace kythira::aws_query
