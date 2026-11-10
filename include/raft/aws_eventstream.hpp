// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_eventstream.hpp
/// @brief AWS event-stream framing (`application/vnd.amazon.eventstream`),
///        as Kinesis `SubscribeToShard` streams it
///        (`.kiro/specs/kinesis-compatible-streams/` Requirement 4.2, task 2).
///
/// A message on the wire:
///
/// ```
/// total length   u32 BE   12 + headers + payload + 4
/// headers length u32 BE
/// prelude CRC    u32 BE   CRC-32 of the 8 bytes above
/// headers        name length u8, name, type u8, value
/// payload
/// message CRC    u32 BE   CRC-32 of everything before it
/// ```
///
/// Header value types: 0 true, 1 false, 2 i8, 3 i16, 4 i32, 5 i64 (all
/// big-endian), 6 bytes and 7 UTF-8 string (u16 length + data), 8 timestamp
/// (i64 epoch milliseconds), 9 UUID (16 bytes). The CRC is CRC-32 (IEEE),
/// the same one `x-amz-crc32` uses.
///
/// Limits are aws-c-event-stream's: a message of at most 16 MiB, at most
/// 128 KiB of headers, names of at most 255 bytes, byte and string values of
/// at most 32767 bytes. The encoder refuses a message over them and the
/// decoder refuses a prelude announcing one, before buffering it.
///
/// The service side of an event stream sends three kinds of message,
/// distinguished by `:message-type`: `event` (with `:event-type` and
/// `:content-type`), `exception` (a modeled error, with `:exception-type`)
/// and `error` (an unmodeled one, with `:error-code` and `:error-message`).
/// `make_event`, `make_exception` and `make_error` build them.

#include <raft/crc32.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::aws_eventstream {

inline constexpr std::string_view k_content_type = "application/vnd.amazon.eventstream";
inline constexpr std::size_t k_prelude_bytes = 12;
inline constexpr std::size_t k_min_message_bytes = k_prelude_bytes + 4;
inline constexpr std::size_t k_max_message_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t k_max_headers_bytes = std::size_t{128} * 1024;
inline constexpr std::size_t k_max_header_name_bytes = 255;
inline constexpr std::size_t k_max_header_value_bytes = 32767;

class eventstream_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class header_type : std::uint8_t {
    bool_true = 0,
    bool_false = 1,
    byte = 2,
    int16 = 3,
    int32 = 4,
    int64 = 5,
    bytes = 6,
    string = 7,
    timestamp = 8,
    uuid = 9,
};

/// Epoch milliseconds, kept apart from a plain i64 so the type survives a
/// round trip.
struct timestamp_ms {
    std::int64_t value = 0;
    friend auto operator==(const timestamp_ms&, const timestamp_ms&) -> bool = default;
};

using uuid = std::array<std::byte, 16>;

/// Alternatives in the order of `header_type`, except that both booleans
/// share `bool`.
using header_value = std::variant<bool, std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                                  std::vector<std::byte>, std::string, timestamp_ms, uuid>;

struct header {
    std::string name;
    header_value value;
    friend auto operator==(const header&, const header&) -> bool = default;
};

struct message {
    std::vector<header> headers;
    std::vector<std::byte> payload;

    /// The first header called `name`, if any.
    [[nodiscard]] auto find(std::string_view name) const -> const header_value* {
        for (const auto& h : headers) {
            if (h.name == name) {
                return &h.value;
            }
        }
        return nullptr;
    }

    /// A string header's value, if `name` is present and a string.
    [[nodiscard]] auto string_header(std::string_view name) const
        -> std::optional<std::string_view> {
        const auto* v = find(name);
        if (v == nullptr) {
            return std::nullopt;
        }
        const auto* s = std::get_if<std::string>(v);
        return s == nullptr ? std::nullopt : std::optional<std::string_view>(*s);
    }

    friend auto operator==(const message&, const message&) -> bool = default;
};

namespace detail {

inline void put_be(std::vector<std::byte>& out, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(static_cast<std::byte>((v >> (static_cast<unsigned>(i) * 8U)) & 0xFFU));
    }
}

[[nodiscard]] inline auto get_be(std::span<const std::byte> in, std::size_t at, int n)
    -> std::uint64_t {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) {
        v = (v << 8U) | static_cast<std::uint8_t>(in[at + static_cast<std::size_t>(i)]);
    }
    return v;
}

inline void put_text(std::vector<std::byte>& out, std::string_view s) {
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
}

inline void encode_header(std::vector<std::byte>& out, const header& h) {
    if (h.name.empty() || h.name.size() > k_max_header_name_bytes) {
        throw eventstream_error("aws_eventstream: header name must be 1-255 bytes");
    }
    out.push_back(static_cast<std::byte>(h.name.size()));
    put_text(out, h.name);
    std::visit(
        [&out]<typename T>(const T& v) {
            if constexpr (std::is_same_v<T, bool>) {
                out.push_back(
                    static_cast<std::byte>(v ? header_type::bool_true : header_type::bool_false));
            } else if constexpr (std::is_same_v<T, std::int8_t>) {
                out.push_back(static_cast<std::byte>(header_type::byte));
                out.push_back(static_cast<std::byte>(v));
            } else if constexpr (std::is_same_v<T, std::int16_t>) {
                out.push_back(static_cast<std::byte>(header_type::int16));
                put_be(out, static_cast<std::uint16_t>(v), 2);
            } else if constexpr (std::is_same_v<T, std::int32_t>) {
                out.push_back(static_cast<std::byte>(header_type::int32));
                put_be(out, static_cast<std::uint32_t>(v), 4);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out.push_back(static_cast<std::byte>(header_type::int64));
                put_be(out, static_cast<std::uint64_t>(v), 8);
            } else if constexpr (std::is_same_v<T, std::vector<std::byte>> ||
                                 std::is_same_v<T, std::string>) {
                if (v.size() > k_max_header_value_bytes) {
                    throw eventstream_error("aws_eventstream: header value over 32767 bytes");
                }
                out.push_back(static_cast<std::byte>(
                    std::is_same_v<T, std::string> ? header_type::string : header_type::bytes));
                put_be(out, v.size(), 2);
                for (const auto c : v) {
                    out.push_back(static_cast<std::byte>(c));
                }
            } else if constexpr (std::is_same_v<T, timestamp_ms>) {
                out.push_back(static_cast<std::byte>(header_type::timestamp));
                put_be(out, static_cast<std::uint64_t>(v.value), 8);
            } else {
                out.push_back(static_cast<std::byte>(header_type::uuid));
                out.insert(out.end(), v.begin(), v.end());
            }
        },
        h.value);
}

/// Parses the header block; throws on any malformation.
[[nodiscard]] inline auto decode_headers(std::span<const std::byte> in) -> std::vector<header> {
    std::vector<header> out;
    std::size_t at = 0;
    const auto need = [&](std::size_t n) {
        if (in.size() - at < n) {
            throw eventstream_error("aws_eventstream: truncated header");
        }
    };
    while (at < in.size()) {
        const auto name_len = static_cast<std::size_t>(in[at]);
        ++at;
        if (name_len == 0) {
            throw eventstream_error("aws_eventstream: empty header name");
        }
        need(name_len + 1);
        header h;
        h.name.assign(reinterpret_cast<const char*>(in.data() + at), name_len);
        at += name_len;
        const auto type = static_cast<std::uint8_t>(in[at]);
        ++at;
        switch (static_cast<header_type>(type)) {
            case header_type::bool_true:
                h.value = true;
                break;
            case header_type::bool_false:
                h.value = false;
                break;
            case header_type::byte:
                need(1);
                h.value = static_cast<std::int8_t>(in[at]);
                at += 1;
                break;
            case header_type::int16:
                need(2);
                h.value = static_cast<std::int16_t>(get_be(in, at, 2));
                at += 2;
                break;
            case header_type::int32:
                need(4);
                h.value = static_cast<std::int32_t>(get_be(in, at, 4));
                at += 4;
                break;
            case header_type::int64:
                need(8);
                h.value = static_cast<std::int64_t>(get_be(in, at, 8));
                at += 8;
                break;
            case header_type::bytes:
            case header_type::string: {
                need(2);
                const auto len = static_cast<std::size_t>(get_be(in, at, 2));
                at += 2;
                if (len > k_max_header_value_bytes) {
                    throw eventstream_error("aws_eventstream: header value over 32767 bytes");
                }
                need(len);
                if (type == static_cast<std::uint8_t>(header_type::string)) {
                    h.value = std::string(reinterpret_cast<const char*>(in.data() + at), len);
                } else {
                    h.value =
                        std::vector<std::byte>(in.begin() + static_cast<std::ptrdiff_t>(at),
                                               in.begin() + static_cast<std::ptrdiff_t>(at + len));
                }
                at += len;
                break;
            }
            case header_type::timestamp:
                need(8);
                h.value = timestamp_ms{static_cast<std::int64_t>(get_be(in, at, 8))};
                at += 8;
                break;
            case header_type::uuid: {
                need(16);
                uuid u{};
                std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(at), 16, u.begin());
                h.value = u;
                at += 16;
                break;
            }
            default:
                throw eventstream_error("aws_eventstream: unknown header type");
        }
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace detail

/// One message's bytes. Throws `eventstream_error` when it breaks a limit.
[[nodiscard]] inline auto encode(const message& m) -> std::vector<std::byte> {
    std::vector<std::byte> headers;
    for (const auto& h : m.headers) {
        detail::encode_header(headers, h);
    }
    if (headers.size() > k_max_headers_bytes) {
        throw eventstream_error("aws_eventstream: headers over 128 KiB");
    }
    const std::size_t total = k_prelude_bytes + headers.size() + m.payload.size() + 4;
    if (total > k_max_message_bytes) {
        throw eventstream_error("aws_eventstream: message over 16 MiB");
    }
    std::vector<std::byte> out;
    out.reserve(total);
    detail::put_be(out, total, 4);
    detail::put_be(out, headers.size(), 4);
    detail::put_be(out, kythira::crc32(std::span(out).first(8)), 4);
    out.insert(out.end(), headers.begin(), headers.end());
    out.insert(out.end(), m.payload.begin(), m.payload.end());
    detail::put_be(out, kythira::crc32(out), 4);
    return out;
}

/// Splits a byte stream into messages, as a client reading a response body
/// does. Feed it bytes as they arrive and take messages with `next()`. Any
/// fault (bad CRC, a length out of range, malformed headers) throws
/// `eventstream_error`, after which the stream is unusable: an event stream
/// has no way to resynchronise.
class decoder {
public:
    void feed(std::span<const std::byte> bytes) {
        if (_failed) {
            throw eventstream_error("aws_eventstream: decoder already failed");
        }
        _buf.insert(_buf.end(), bytes.begin(), bytes.end());
    }

    /// The next complete message, or nothing until more bytes arrive.
    [[nodiscard]] auto next() -> std::optional<message> {
        if (_failed) {
            throw eventstream_error("aws_eventstream: decoder already failed");
        }
        const std::span<const std::byte> avail(_buf.data() + _start, _buf.size() - _start);
        if (avail.size() < k_prelude_bytes) {
            return std::nullopt;
        }
        try {
            const auto total = static_cast<std::size_t>(detail::get_be(avail, 0, 4));
            const auto hlen = static_cast<std::size_t>(detail::get_be(avail, 4, 4));
            const auto pcrc = static_cast<std::uint32_t>(detail::get_be(avail, 8, 4));
            if (kythira::crc32(avail.first(8)) != pcrc) {
                throw eventstream_error("aws_eventstream: prelude CRC mismatch");
            }
            if (total < k_min_message_bytes || total > k_max_message_bytes) {
                throw eventstream_error("aws_eventstream: message length out of range");
            }
            if (hlen > k_max_headers_bytes || hlen > total - k_min_message_bytes) {
                throw eventstream_error("aws_eventstream: headers length out of range");
            }
            if (avail.size() < total) {
                return std::nullopt;
            }
            const auto whole = avail.first(total);
            const auto mcrc = static_cast<std::uint32_t>(detail::get_be(whole, total - 4, 4));
            if (kythira::crc32(whole.first(total - 4)) != mcrc) {
                throw eventstream_error("aws_eventstream: message CRC mismatch");
            }
            message m;
            m.headers = detail::decode_headers(whole.subspan(k_prelude_bytes, hlen));
            const auto payload =
                whole.subspan(k_prelude_bytes + hlen, total - k_min_message_bytes - hlen);
            m.payload.assign(payload.begin(), payload.end());
            _start += total;
            if (_start > _buf.size() / 2) {
                _buf.erase(_buf.begin(), _buf.begin() + static_cast<std::ptrdiff_t>(_start));
                _start = 0;
            }
            return m;
        } catch (...) {
            _failed = true;
            throw;
        }
    }

    /// Bytes received but not yet returned as a message.
    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t {
        return _buf.size() - _start;
    }

private:
    std::vector<std::byte> _buf;
    std::size_t _start = 0;
    bool _failed = false;
};

/// One message from a complete buffer; throws unless `bytes` is exactly one.
[[nodiscard]] inline auto decode(std::span<const std::byte> bytes) -> message {
    decoder d;
    d.feed(bytes);
    auto m = d.next();
    if (!m || d.pending_bytes() != 0) {
        throw eventstream_error("aws_eventstream: not exactly one message");
    }
    return std::move(*m);
}

/// An `event` message: `:message-type event`, `:event-type`, `:content-type`.
[[nodiscard]] inline auto make_event(std::string event_type, std::string content_type,
                                     std::vector<std::byte> payload) -> message {
    message m;
    m.headers.push_back({":message-type", std::string("event")});
    m.headers.push_back({":event-type", std::move(event_type)});
    m.headers.push_back({":content-type", std::move(content_type)});
    m.payload = std::move(payload);
    return m;
}

/// A modeled error: `:message-type exception`, `:exception-type`,
/// `:content-type`, the error shape in the payload.
[[nodiscard]] inline auto make_exception(std::string exception_type, std::string content_type,
                                         std::vector<std::byte> payload) -> message {
    message m;
    m.headers.push_back({":message-type", std::string("exception")});
    m.headers.push_back({":exception-type", std::move(exception_type)});
    m.headers.push_back({":content-type", std::move(content_type)});
    m.payload = std::move(payload);
    return m;
}

/// An unmodeled error: `:message-type error`, `:error-code`,
/// `:error-message`, no payload.
[[nodiscard]] inline auto make_error(std::string code, std::string text) -> message {
    message m;
    m.headers.push_back({":message-type", std::string("error")});
    m.headers.push_back({":error-code", std::move(code)});
    m.headers.push_back({":error-message", std::move(text)});
    return m;
}

}  // namespace kythira::aws_eventstream
