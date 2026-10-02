// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file resp_protocol.hpp
/// @brief RESP2/RESP3 wire codec for the Redis-compatible gateway
///        (.kiro/specs/redis-compatible-kv/ design Component 1).
///
/// This header does no I/O. `resp_parser` turns bytes into commands and
/// `resp_writer` turns replies into bytes; the gateway owns the sockets.
/// Keeping the codec free of sockets is what lets the unit tests feed it one
/// byte at a time and assert on the exact bytes that come back.
///
/// The request grammar is deliberately the subset redis-rs 1.2 (sccache's
/// client, via OpenDAL) puts on the wire: every request is a multibulk array
/// of bulk strings (`*<n>\r\n$<len>\r\n<bytes>\r\n...`). Inline commands
/// (`PING\r\n`) are accepted too because `redis-cli` and shell probes send
/// them and refusing them makes the gateway needlessly hard to poke at.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// One decoded request: argv[0] is the command name as sent (case preserved).
struct resp_command {
    std::vector<std::string> _argv;
};

/// Parser limits (Requirement 1.6 / design Component 1). A violation is a
/// protocol error: the gateway answers `-ERR Protocol error: <reason>` and
/// closes the connection, exactly as Redis does.
struct resp_parser_limits {
    /// `proto-max-bulk-len` in Redis terms. sccache's largest object is a
    /// compiler output, so 32 MiB is generous; the value-size limit the
    /// gateway enforces on SET is separate and smaller.
    std::size_t _max_bulk_len = 32u * 1024u * 1024u;
    /// Largest multibulk element count. The widest command in the closure is
    /// `HELLO 3 AUTH user pass SETNAME name` (7 elements).
    std::size_t _max_multibulk_elements = 64;
    /// Bytes of the command being assembled plus bytes held unparsed across
    /// `consume()` calls before the connection is declared abusive.
    std::size_t _max_buffered_bytes = 64u * 1024u * 1024u;
    /// Longest line: an inline command, or a `*<n>` / `$<len>` header.
    /// Redis's PROTO_INLINE_MAX_SIZE. Bounds the CRLF search, which would
    /// otherwise rescan an ever-growing unterminated line on every read.
    std::size_t _max_line_len = 64u * 1024u;
};

/// Limits for a connection that has not authenticated yet, derived from the
/// configured ones. Mirrors what Redis applies to unauthenticated clients
/// (multibulk length 10, bulk length 16 KiB): nothing in the pre-auth
/// surface (AUTH, HELLO, QUIT, RESET) needs more, and without it any
/// stranger could make the gateway buffer a 32 MiB argument per connection.
/// The buffered-byte cap leaves room for one largest pre-auth command plus
/// one 64 KiB socket read.
[[nodiscard]] inline auto resp_pre_auth_limits(const resp_parser_limits& configured)
    -> resp_parser_limits {
    resp_parser_limits l = configured;
    l._max_bulk_len = std::min<std::size_t>(l._max_bulk_len, 16u * 1024u);
    l._max_multibulk_elements = std::min<std::size_t>(l._max_multibulk_elements, 10);
    l._max_buffered_bytes = std::min<std::size_t>(l._max_buffered_bytes, 512u * 1024u);
    return l;
}

/// Thrown by `resp_parser::consume` when the byte stream violates the grammar
/// or a limit. `what()` is the text after `-ERR Protocol error: `.
class resp_protocol_error : public std::exception {
public:
    explicit resp_protocol_error(std::string reason) : _reason(std::move(reason)) {}
    [[nodiscard]] auto what() const noexcept -> const char* override { return _reason.c_str(); }

private:
    std::string _reason;
};

/// Incremental request parser. Feed it whatever arrived on the socket; it
/// returns every complete command and keeps the tail of a partial one.
///
/// Pipelining is the normal case, not an edge case: redis-rs writes its whole
/// handshake (`AUTH`, `SELECT`, `CLIENT SETINFO` x2) before reading anything,
/// so a single `consume()` routinely yields several commands.
///
/// The parser keeps its place inside a partial command: header lines and
/// finished bulk strings are taken off the buffer as soon as they are whole,
/// and the CRLF search resumes where the last one stopped. Every byte is
/// therefore examined and copied a bounded number of times however it is
/// split across reads. (The first version restarted from the command's
/// first byte on each read and re-copied every finished argument, so a
/// 63 MB request trickled in 64 KiB reads cost seconds of CPU.)
class resp_parser {
public:
    resp_parser() = default;
    explicit resp_parser(resp_parser_limits limits) : _limits(limits) {}

    /// Append `bytes` and return every command that is now complete, in order.
    /// Throws `resp_protocol_error` on malformed input or a limit breach; the
    /// parser is unusable afterwards and the caller must close the connection.
    auto consume(std::span<const std::byte> bytes) -> std::vector<resp_command> {
        feed(bytes);
        std::vector<resp_command> out;
        while (auto cmd = next()) {
            out.push_back(std::move(*cmd));
        }
        return out;
    }

    auto consume(std::string_view text) -> std::vector<resp_command> {
        return consume(std::as_bytes(std::span<const char>(text.data(), text.size())));
    }

    /// Append `bytes` without parsing. Use with `next()` when the caller must
    /// act on one command before the next is parsed (the gateway does, so a
    /// connection's limits can change when AUTH succeeds).
    auto feed(std::span<const std::byte> bytes) -> void {
        if (buffered_bytes() + bytes.size() > _limits._max_buffered_bytes) {
            throw resp_protocol_error("too many unparsed bytes buffered");
        }
        compact();
        _buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    auto feed(std::string_view text) -> void {
        feed(std::as_bytes(std::span<const char>(text.data(), text.size())));
    }

    /// Parse at most one command from what has been fed. Returns nullopt
    /// when more bytes are needed. Throws as `consume()` does.
    auto next() -> std::optional<resp_command> {
        while (true) {
            if (!_in_multibulk) {
                if (_consumed >= _buffer.size()) {
                    return std::nullopt;
                }
                if (_buffer[_consumed] != '*') {
                    return parse_inline();
                }
                auto line = read_line("too big mbulk count string");
                if (!line.has_value()) {
                    return std::nullopt;
                }
                auto count =
                    parse_integer(std::string_view(*line).substr(1), "invalid multibulk length");
                if (count <= 0) {
                    // `*-1\r\n` is a null array and `*0\r\n` an empty one;
                    // Redis treats both as an empty request.
                    _pending_raw = 0;
                    return resp_command{};
                }
                if (static_cast<std::size_t>(count) > _limits._max_multibulk_elements) {
                    throw resp_protocol_error("invalid multibulk length");
                }
                _in_multibulk = true;
                _remaining = static_cast<std::size_t>(count);
                _pending = resp_command{};
                _pending._argv.reserve(_remaining);
                _bulk_len.reset();
                continue;
            }
            if (_remaining == 0) {
                resp_command done = std::move(_pending);
                _pending = resp_command{};
                _in_multibulk = false;
                _pending_raw = 0;
                return done;
            }
            if (!_bulk_len.has_value()) {
                auto line = read_line("too big bulk count string");
                if (!line.has_value()) {
                    return std::nullopt;
                }
                if (line->empty() || (*line)[0] != '$') {
                    throw resp_protocol_error(
                        std::string("expected '$', got '") +
                        (line->empty() ? std::string("") : line->substr(0, 1)) + "'");
                }
                auto len = parse_integer(std::string_view(*line).substr(1), "invalid bulk length");
                if (len < 0 || static_cast<std::size_t>(len) > _limits._max_bulk_len) {
                    throw resp_protocol_error("invalid bulk length");
                }
                _bulk_len = static_cast<std::size_t>(len);
            }
            // O(1) while the bulk is still arriving: no rescan, no copy.
            auto len = *_bulk_len;
            if (_buffer.size() - _consumed < len + 2) {
                return std::nullopt;
            }
            if (_buffer[_consumed + len] != '\r' || _buffer[_consumed + len + 1] != '\n') {
                throw resp_protocol_error("bulk string not terminated by CRLF");
            }
            _pending._argv.emplace_back(_buffer.data() + _consumed, len);
            _consumed += len + 2;
            _scan = _consumed;
            _pending_raw += len + 2;
            _bulk_len.reset();
            --_remaining;
        }
    }

    /// Bytes received but not yet part of a complete command, including the
    /// already-decoded part of the command being assembled.
    [[nodiscard]] auto buffered_bytes() const noexcept -> std::size_t {
        return _buffer.size() - _consumed + _pending_raw;
    }

    [[nodiscard]] auto limits() const noexcept -> const resp_parser_limits& { return _limits; }

    /// Change the limits for everything parsed from now on (the gateway
    /// relaxes them once a connection authenticates). Bytes already buffered
    /// are not re-checked against the new buffered-byte cap until the next
    /// `feed()`.
    auto set_limits(const resp_parser_limits& limits) noexcept -> void { _limits = limits; }

private:
    // Inline command: a single line of whitespace-separated words. Only used
    // by hand-written probes; quoting is intentionally not supported.
    auto parse_inline() -> std::optional<resp_command> {
        auto line = read_line("too big inline request");
        if (!line.has_value()) {
            return std::nullopt;
        }
        _pending_raw = 0;
        resp_command cmd;
        std::size_t i = 0;
        while (i < line->size()) {
            while (i < line->size() && ((*line)[i] == ' ' || (*line)[i] == '\t')) {
                ++i;
            }
            std::size_t start = i;
            while (i < line->size() && (*line)[i] != ' ' && (*line)[i] != '\t') {
                ++i;
            }
            if (i > start) {
                cmd._argv.push_back(line->substr(start, i - start));
            }
        }
        return cmd;
    }

    // Take one CRLF-terminated line off the front of the buffer, without the
    // terminator, or return nullopt if it is not complete yet. The search
    // resumes at `_scan`, so an incomplete line is never rescanned.
    auto read_line(const char* too_long) -> std::optional<std::string> {
        auto from = std::max(_scan, _consumed);
        auto nl = _buffer.find('\n', from);
        if (nl == std::string::npos) {
            _scan = _buffer.size();
            if (_buffer.size() - _consumed > _limits._max_line_len) {
                throw resp_protocol_error(too_long);
            }
            return std::nullopt;
        }
        if (nl == _consumed || _buffer[nl - 1] != '\r') {
            throw resp_protocol_error("expected CRLF line terminator");
        }
        if (nl - 1 - _consumed > _limits._max_line_len) {
            throw resp_protocol_error(too_long);
        }
        std::string line = _buffer.substr(_consumed, nl - 1 - _consumed);
        _pending_raw += nl + 1 - _consumed;
        _consumed = nl + 1;
        _scan = _consumed;
        return line;
    }

    static auto parse_integer(std::string_view text, const char* reason) -> std::int64_t {
        if (text.empty() || text.size() > 20) {
            throw resp_protocol_error(reason);
        }
        std::size_t i = 0;
        bool negative = false;
        if (text[0] == '-') {
            negative = true;
            i = 1;
            if (text.size() == 1) {
                throw resp_protocol_error(reason);
            }
        }
        std::int64_t value = 0;
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                throw resp_protocol_error(reason);
            }
            auto digit = static_cast<std::int64_t>(text[i] - '0');
            if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
                throw resp_protocol_error(reason);
            }
            value = value * 10 + digit;
        }
        return negative ? -value : value;
    }

    // Drop the consumed prefix once it is at least half the buffer, so each
    // byte is moved O(1) times amortised.
    auto compact() -> void {
        if (_consumed == 0) {
            return;
        }
        if (_consumed == _buffer.size()) {
            _buffer.clear();
        } else if (_consumed >= _buffer.size() / 2) {
            _buffer.erase(0, _consumed);
        } else {
            return;
        }
        _scan -= std::min(_scan, _consumed);
        _consumed = 0;
    }

    resp_parser_limits _limits{};
    std::string _buffer;
    /// Start of the unparsed bytes in `_buffer`.
    std::size_t _consumed = 0;
    /// Where the next CRLF search starts (>= `_consumed` when meaningful).
    std::size_t _scan = 0;

    // The multibulk command being assembled, if any.
    bool _in_multibulk = false;
    resp_command _pending;
    std::size_t _remaining = 0;
    std::optional<std::size_t> _bulk_len;
    /// Wire bytes of `_pending` already taken off the buffer; counted
    /// against `_max_buffered_bytes` so splitting a request does not
    /// sidestep the cap.
    std::size_t _pending_raw = 0;
};

/// Reply encoder. RESP2 by default; `set_version(3)` after a successful
/// `HELLO 3` switches the null encoding (`$-1\r\n` -> `_\r\n`) and lets
/// `map()` emit a real map instead of a flat array.
class resp_writer {
public:
    explicit resp_writer(int version = 2) : _version(version) {}

    auto set_version(int version) noexcept -> void { _version = version; }
    [[nodiscard]] auto version() const noexcept -> int { return _version; }

    [[nodiscard]] auto simple_string(std::string_view s) const -> std::string {
        std::string out;
        out.reserve(s.size() + 3);
        out += '+';
        out += s;
        out += "\r\n";
        return out;
    }

    /// `text` includes the error prefix, e.g. `ERR unknown command`.
    [[nodiscard]] auto error(std::string_view text) const -> std::string {
        std::string out;
        out.reserve(text.size() + 3);
        out += '-';
        out += text;
        out += "\r\n";
        return out;
    }

    [[nodiscard]] auto integer(std::int64_t v) const -> std::string {
        return ":" + std::to_string(v) + "\r\n";
    }

    [[nodiscard]] auto bulk(std::string_view s) const -> std::string {
        std::string out;
        out.reserve(s.size() + 16);
        out += '$';
        out += std::to_string(s.size());
        out += "\r\n";
        out += s;
        out += "\r\n";
        return out;
    }

    [[nodiscard]] auto bulk(std::span<const std::byte> s) const -> std::string {
        return bulk(std::string_view(reinterpret_cast<const char*>(s.data()), s.size()));
    }

    [[nodiscard]] auto null() const -> std::string { return _version >= 3 ? "_\r\n" : "$-1\r\n"; }

    [[nodiscard]] auto array(const std::vector<std::string>& encoded_elements) const
        -> std::string {
        std::string out = "*" + std::to_string(encoded_elements.size()) + "\r\n";
        for (const auto& e : encoded_elements) {
            out += e;
        }
        return out;
    }

    /// Emit a map of bulk keys to already-encoded values. RESP3 uses `%`;
    /// RESP2 clients get a flat array of alternating key/value, which is how
    /// Redis itself downgrades HELLO's reply.
    [[nodiscard]] auto map(const std::vector<std::pair<std::string, std::string>>& entries) const
        -> std::string {
        std::string out;
        if (_version >= 3) {
            out += "%" + std::to_string(entries.size()) + "\r\n";
        } else {
            out += "*" + std::to_string(entries.size() * 2) + "\r\n";
        }
        for (const auto& [k, v] : entries) {
            out += bulk(k);
            out += v;
        }
        return out;
    }

private:
    int _version;
};

/// Bounds on a reply `resp_reply_length` will measure. The forwarding client
/// reads replies from a peer gateway over plain TCP; without these a
/// malformed or hostile peer could drive unbounded recursion, overflow the
/// length arithmetic into a bogus short length, or make every read rescan
/// an unterminated line.
struct resp_reply_limits {
    /// Aggregate nesting. Nothing the gateway forwards nests at all.
    std::size_t _max_depth = 8;
    /// Largest bulk string, and largest aggregate element count.
    std::size_t _max_bulk_len = 512u * 1024u * 1024u;
    std::size_t _max_elements = 1024u * 1024u;
    /// Longest type/length/simple-string line, CRLF excluded.
    std::size_t _max_line_len = 64u * 1024u;
};

namespace resp_detail {

[[nodiscard]] inline auto reply_length(std::string_view data, const resp_reply_limits& limits,
                                       std::size_t depth) -> std::size_t {
    if (data.empty()) {
        return 0;
    }
    if (depth > limits._max_depth) {
        throw resp_protocol_error("reply nested too deeply");
    }
    // End of the line starting at 0 (one past its CRLF), or 0 if incomplete.
    // The search never looks further than the longest permitted line.
    auto line_end = [&]() -> std::size_t {
        auto window = data.substr(0, limits._max_line_len + 2);
        auto nl = window.find("\r\n", 1);
        if (nl == std::string_view::npos) {
            if (window.size() == limits._max_line_len + 2) {
                throw resp_protocol_error("reply line too long");
            }
            return 0;
        }
        return nl + 2;
    };
    // The signed length on the line [1, end - 2), bounded by `max`.
    auto count_on = [&](std::size_t end, std::size_t max) -> std::int64_t {
        auto text = data.substr(1, end - 3);
        if (text.empty()) {
            throw resp_protocol_error("empty length in reply");
        }
        if (text == "-1") {
            return -1;
        }
        std::uint64_t v = 0;
        for (char c : text) {
            if (c < '0' || c > '9') {
                throw resp_protocol_error("bad length in reply");
            }
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
            if (v > max) {
                throw resp_protocol_error("length in reply exceeds limit");
            }
        }
        return static_cast<std::int64_t>(v);
    };
    switch (data[0]) {
        case '+':
        case '-':
        case ':':
        case '_':
        case ',':
        case '#':
            return line_end();
        case '$':
        case '!':
        case '=': {
            auto end = line_end();
            if (end == 0) {
                return 0;
            }
            auto n = count_on(end, limits._max_bulk_len);
            if (n < 0) {
                return end;  // null bulk
            }
            auto total = end + static_cast<std::size_t>(n) + 2;
            return data.size() >= total ? total : 0;
        }
        case '*':
        case '~':
        case '>':
        case '%':
        case '|': {
            auto end = line_end();
            if (end == 0) {
                return 0;
            }
            auto n = count_on(end, limits._max_elements);
            if (n < 0) {
                return end;  // null array
            }
            std::size_t elements = static_cast<std::size_t>(n);
            if (data[0] == '%' || data[0] == '|') {
                elements *= 2;
            }
            std::size_t pos = end;
            for (std::size_t i = 0; i < elements; ++i) {
                auto len = reply_length(data.substr(pos), limits, depth + 1);
                if (len == 0) {
                    return 0;
                }
                pos += len;
            }
            return pos;
        }
        default:
            throw resp_protocol_error("unexpected reply type byte");
    }
}

}  // namespace resp_detail

/// Length of one complete reply at the front of `data`, or 0 if more bytes
/// are needed. Used by the forwarding client, which relays a peer gateway's
/// reply verbatim and therefore only has to find its end, never decode it.
/// Throws `resp_protocol_error` on a reply that is not RESP at all or that
/// breaks `limits`. The caller still bounds how much it buffers in total.
[[nodiscard]] inline auto resp_reply_length(std::string_view data,
                                            const resp_reply_limits& limits = {}) -> std::size_t {
    return resp_detail::reply_length(data, limits, 0);
}

/// ASCII case-insensitive comparison for command names; Redis commands are
/// matched case-insensitively and redis-rs sends them upper-case.
[[nodiscard]] inline auto resp_iequals(std::string_view a, std::string_view b) noexcept -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'a' && ca <= 'z') {
            ca = static_cast<unsigned char>(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = static_cast<unsigned char>(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline auto resp_to_upper(std::string_view s) -> std::string {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

}  // namespace kythira
