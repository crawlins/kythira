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

#include <cstddef>
#include <cstdint>
#include <cstring>
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
    /// Bytes held unparsed across `consume()` calls before the connection is
    /// declared abusive.
    std::size_t _max_buffered_bytes = 64u * 1024u * 1024u;
};

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
class resp_parser {
public:
    resp_parser() = default;
    explicit resp_parser(resp_parser_limits limits) : _limits(limits) {}

    /// Append `bytes` and return every command that is now complete, in order.
    /// Throws `resp_protocol_error` on malformed input or a limit breach; the
    /// parser is unusable afterwards and the caller must close the connection.
    auto consume(std::span<const std::byte> bytes) -> std::vector<resp_command> {
        if (_buffer.size() - _consumed + bytes.size() > _limits._max_buffered_bytes) {
            throw resp_protocol_error("too many unparsed bytes buffered");
        }
        _buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::vector<resp_command> out;
        while (true) {
            auto cmd = try_parse_one();
            if (!cmd.has_value()) {
                break;
            }
            out.push_back(std::move(*cmd));
        }
        compact();
        return out;
    }

    auto consume(std::string_view text) -> std::vector<resp_command> {
        return consume(std::as_bytes(std::span<const char>(text.data(), text.size())));
    }

    /// Bytes received but not yet part of a complete command.
    [[nodiscard]] auto buffered_bytes() const noexcept -> std::size_t {
        return _buffer.size() - _consumed;
    }

    [[nodiscard]] auto limits() const noexcept -> const resp_parser_limits& { return _limits; }

private:
    // Attempt to parse one complete command starting at `_consumed`. Returns
    // nullopt (and leaves `_consumed` untouched) when more bytes are needed.
    auto try_parse_one() -> std::optional<resp_command> {
        std::size_t pos = _consumed;
        if (pos >= _buffer.size()) {
            return std::nullopt;
        }
        if (_buffer[pos] != '*') {
            return try_parse_inline();
        }
        auto count_line = read_line(pos);
        if (!count_line.has_value()) {
            return std::nullopt;
        }
        auto count = parse_integer(count_line->substr(1), "invalid multibulk length");
        if (count < 0) {
            // `*-1\r\n` is a null array; Redis treats it as an empty request.
            _consumed = pos;
            return resp_command{};
        }
        if (static_cast<std::size_t>(count) > _limits._max_multibulk_elements) {
            throw resp_protocol_error("invalid multibulk length");
        }
        resp_command cmd;
        cmd._argv.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
            auto len_line = read_line(pos);
            if (!len_line.has_value()) {
                return std::nullopt;
            }
            if (len_line->empty() || (*len_line)[0] != '$') {
                throw resp_protocol_error(
                    std::string("expected '$', got '") +
                    (len_line->empty() ? std::string("") : len_line->substr(0, 1)) + "'");
            }
            auto len = parse_integer(len_line->substr(1), "invalid bulk length");
            if (len < 0 || static_cast<std::size_t>(len) > _limits._max_bulk_len) {
                throw resp_protocol_error("invalid bulk length");
            }
            auto need = static_cast<std::size_t>(len) + 2;
            if (_buffer.size() - pos < need) {
                return std::nullopt;
            }
            if (_buffer[pos + static_cast<std::size_t>(len)] != '\r' ||
                _buffer[pos + static_cast<std::size_t>(len) + 1] != '\n') {
                throw resp_protocol_error("bulk string not terminated by CRLF");
            }
            cmd._argv.emplace_back(_buffer.data() + pos, static_cast<std::size_t>(len));
            pos += need;
        }
        _consumed = pos;
        return cmd;
    }

    // Inline command: a single line of whitespace-separated words. Only used
    // by hand-written probes; quoting is intentionally not supported.
    auto try_parse_inline() -> std::optional<resp_command> {
        std::size_t pos = _consumed;
        auto line = read_line(pos);
        if (!line.has_value()) {
            if (_buffer.size() - _consumed > 64u * 1024u) {
                throw resp_protocol_error("too big inline request");
            }
            return std::nullopt;
        }
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
        _consumed = pos;
        return cmd;
    }

    // Read up to and including CRLF at `pos`, advancing `pos` past it.
    // Returns the line without the terminator, or nullopt if incomplete.
    auto read_line(std::size_t& pos) -> std::optional<std::string> {
        auto nl = _buffer.find('\n', pos);
        if (nl == std::string::npos) {
            return std::nullopt;
        }
        if (nl == pos || _buffer[nl - 1] != '\r') {
            throw resp_protocol_error("expected CRLF line terminator");
        }
        std::string line = _buffer.substr(pos, nl - 1 - pos);
        pos = nl + 1;
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
            value = value * 10 + (text[i] - '0');
        }
        return negative ? -value : value;
    }

    auto compact() -> void {
        if (_consumed == 0) {
            return;
        }
        if (_consumed == _buffer.size()) {
            _buffer.clear();
        } else if (_consumed > _buffer.size() / 2) {
            _buffer.erase(0, _consumed);
        } else {
            return;
        }
        _consumed = 0;
    }

    resp_parser_limits _limits{};
    std::string _buffer;
    std::size_t _consumed = 0;
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

/// Length of one complete reply at the front of `data`, or 0 if more bytes
/// are needed. Used by the forwarding client, which relays a peer gateway's
/// reply verbatim and therefore only has to find its end, never decode it.
/// Throws `resp_protocol_error` on a reply that is not RESP at all.
[[nodiscard]] inline auto resp_reply_length(std::string_view data) -> std::size_t {
    if (data.empty()) {
        return 0;
    }
    auto line_end = [&](std::size_t from) -> std::size_t {
        auto nl = data.find("\r\n", from);
        return nl == std::string_view::npos ? 0 : nl + 2;
    };
    auto count_after = [&](std::size_t from, std::size_t end) -> std::int64_t {
        auto text = data.substr(from, end - 2 - from);
        if (text.empty()) {
            throw resp_protocol_error("empty length in reply");
        }
        std::int64_t v = 0;
        bool neg = false;
        std::size_t i = 0;
        if (text[0] == '-') {
            neg = true;
            i = 1;
        }
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                throw resp_protocol_error("bad length in reply");
            }
            v = v * 10 + (text[i] - '0');
        }
        return neg ? -v : v;
    };
    switch (data[0]) {
        case '+':
        case '-':
        case ':':
        case '_':
        case ',':
        case '#':
            return line_end(1);
        case '$':
        case '!':
        case '=': {
            auto end = line_end(1);
            if (end == 0) {
                return 0;
            }
            auto n = count_after(1, end);
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
            auto end = line_end(1);
            if (end == 0) {
                return 0;
            }
            auto n = count_after(1, end);
            if (n < 0) {
                return end;  // null array
            }
            std::size_t elements = static_cast<std::size_t>(n);
            if (data[0] == '%' || data[0] == '|') {
                elements *= 2;
            }
            std::size_t pos = end;
            for (std::size_t i = 0; i < elements; ++i) {
                auto len = resp_reply_length(data.substr(pos));
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
