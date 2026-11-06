// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Streaming SigV4 payloads (`.kiro/specs/iam-compatible-auth/`
// Requirement 3.4, design Component 3): the `aws-chunked` bodies an S3
// client sends when x-amz-content-sha256 is one of
//
//   STREAMING-AWS4-HMAC-SHA256-PAYLOAD          signed chunks
//   STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER  signed chunks, signed trailer
//   STREAMING-UNSIGNED-PAYLOAD-TRAILER          plain chunks, unsigned trailer
//
// The wire forms, as AWS's S3 signing pages give them:
//
//   signed:    <hex size>;chunk-signature=<sig>\r\n<data>\r\n ...
//              0;chunk-signature=<sig>\r\n\r\n
//   + trailer: ... 0;chunk-signature=<sig>\r\n
//              <name>:<value>\r\n ... x-amz-trailer-signature:<sig>\r\n\r\n
//   unsigned:  <hex size>\r\n<data>\r\n ... 0\r\n<name>:<value>\r\n ... \r\n
//
// Each chunk's string to sign chains on the previous signature, the seed
// being the request's own:
//
//   AWS4-HMAC-SHA256-PAYLOAD \n <x-amz-date> \n <scope> \n <previous sig> \n
//   <sha256("")> \n <sha256(chunk data)>
//
// and the trailer's on the final chunk's:
//
//   AWS4-HMAC-SHA256-TRAILER \n <x-amz-date> \n <scope> \n <previous sig> \n
//   <sha256("name:value\n" for each trailer, in order)>
//
// `chunk_decoder` is a push parser: feed it bytes as they arrive and it
// appends payload to the caller's buffer only once the chunk holding it is
// whole and, in the signed forms, verified (the "released only after it has
// verified" rule). It never holds more than one chunk plus a header line,
// and every length it reads is bounded by `chunk_limits`.
//
// What it leaves to the front end: the trailer *values*. An unsigned
// trailer carries a checksum (x-amz-checksum-crc32 and friends) that is the
// payload's only integrity check, and checking it is the S3 layer's job,
// since it knows which algorithms it supports. `trailers()` hands them over.
// S3's 8 KiB minimum for non-final chunks is not enforced either: it is an
// S3 service rule, not part of the encoding, and nothing here depends on it.

#include <raft/sigv4_verifier.hpp>

#include <openssl/crypto.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::sigv4 {

/// Bounds on what one stream may make the decoder hold.
struct chunk_limits {
    std::size_t max_chunk_size = 16U * 1024U * 1024U;  // bytes of data in one chunk
    std::size_t max_line = 4096;                       // a chunk header or trailer line
    std::size_t max_trailer_bytes = 16U * 1024U;       // every trailer line together
    std::size_t max_trailers = 16;
};

class chunk_decoder {
public:
    enum class status : std::uint8_t {
        need_more,  // feed more bytes
        done,       // the final chunk (and trailer) arrived and verified
        failed,     // see error(); the request must be refused
    };

    /// Sets the decoder up for `c`, a claim `parse()` accepted with a
    /// streaming payload mode, and `req`, the request it came from (for
    /// x-amz-trailer and x-amz-decoded-content-length). `key` is the
    /// claim's signing key; the signed forms need it, the unsigned one
    /// ignores it. A setup problem leaves the decoder `failed` from the
    /// start, so a caller checks `state()` before feeding.
    chunk_decoder(const claim& c, const signing_key* key, const request& req,
                  error_protocol protocol, chunk_limits limits = {})
        : _protocol(protocol),
          _limits(limits),
          _mode(c.payload),
          _trailer_form(_mode == payload_mode::streaming_signed_trailer ||
                        _mode == payload_mode::streaming_unsigned_trailer) {
        const bool signed_form = _mode == payload_mode::streaming_signed ||
                                 _mode == payload_mode::streaming_signed_trailer;
        if (!signed_form && _mode != payload_mode::streaming_unsigned_trailer) {
            fail(malformed("the request does not declare a streaming payload"));
            return;
        }
        if (signed_form) {
            if (key == nullptr) {
                fail(signature_mismatch(_protocol));
                return;
            }
            _key = *key;
            _have_key = true;
            _previous = c.signature;
            _prefix = std::string("\n") + c.amz_date + "\n" + c.scope.text() + "\n";
        }
        const auto headers = detail::index_headers(req);
        if (auto len = headers.single("x-amz-decoded-content-length")) {
            std::uint64_t n = 0;
            if (len->empty() || len->size() > 19) {
                fail(malformed("x-amz-decoded-content-length is not a number"));
                return;
            }
            for (char ch : *len) {
                if (ch < '0' || ch > '9') {
                    fail(malformed("x-amz-decoded-content-length is not a number"));
                    return;
                }
                n = n * 10 + static_cast<std::uint64_t>(ch - '0');
            }
            _decoded_length = n;
        } else if (signed_form && _protocol == error_protocol::s3) {
            fail(aws_error{"MissingContentLength",
                           "You must provide the Content-Length HTTP header.", 411});
            return;
        }
        if (const auto* declared = headers.find("x-amz-trailer")) {
            if (!_trailer_form) {
                fail(malformed("x-amz-trailer needs a trailer payload mode"));
                return;
            }
            for (auto v : *declared) {
                for (auto name : detail::split(v, ',')) {
                    const auto n = detail::lower(detail::trim(name));
                    if (n.empty() || n == "x-amz-trailer-signature" ||
                        !_declared.insert(n).second) {
                        fail(malformed("x-amz-trailer names an invalid or repeated trailer"));
                        return;
                    }
                }
            }
            if (_declared.size() > _limits.max_trailers) {
                fail(malformed("too many trailers"));
                return;
            }
        }
    }

    chunk_decoder(const chunk_decoder&) = default;
    auto operator=(const chunk_decoder&) -> chunk_decoder& = default;
    chunk_decoder(chunk_decoder&&) = default;
    auto operator=(chunk_decoder&&) -> chunk_decoder& = default;
    ~chunk_decoder() { OPENSSL_cleanse(_key.data(), _key.size()); }

    /// Consumes `in`, appending verified payload to `out`. Bytes after the
    /// end of the stream are an error.
    auto feed(std::string_view in, std::string& out) -> status {
        if (_state == phase::failed) {
            return status::failed;
        }
        if (_state == phase::done) {
            if (!in.empty()) {
                fail(malformed("data after the final chunk"));
            }
            return state();
        }
        _buf.append(in);
        while (step(out)) {
        }
        // What stays buffered is bounded: a line, or one chunk and its CRLF.
        return state();
    }

    /// Call at the end of the body: a stream that has not reached its final
    /// chunk is truncated.
    auto finish() -> status {
        if (_state != phase::done && _state != phase::failed) {
            fail(aws_error{"IncompleteBody", "The request body terminated unexpectedly", 400});
        }
        return state();
    }

    [[nodiscard]] auto state() const -> status {
        switch (_state) {
            case phase::done:
                return status::done;
            case phase::failed:
                return status::failed;
            default:
                return status::need_more;
        }
    }

    [[nodiscard]] auto error() const -> const aws_error& { return _error; }

    /// The trailer lines received, names lower-cased, in arrival order,
    /// without x-amz-trailer-signature. Complete once `done`.
    [[nodiscard]] auto trailers() const -> const std::vector<std::pair<std::string, std::string>>& {
        return _trailers;
    }

    /// Payload bytes released so far.
    [[nodiscard]] auto decoded_bytes() const -> std::uint64_t { return _released; }

private:
    enum class phase : std::uint8_t {
        header,
        data,
        trailer,
        done,
        failed
    };

    [[nodiscard]] auto signed_form() const -> bool { return _have_key; }

    [[nodiscard]] auto malformed(std::string what) const -> aws_error {
        return aws_error{"InvalidRequest", "Invalid aws-chunked encoding: " + what, 400};
    }

    void fail(aws_error e) {
        _error = std::move(e);
        _state = phase::failed;
        _buf.clear();
    }

    /// One CRLF-terminated line off the buffer, or nothing yet.
    auto take_line() -> std::optional<std::string> {
        const auto pos = _buf.find("\r\n");
        if (pos == std::string::npos) {
            if (_buf.size() > _limits.max_line) {
                fail(malformed("line too long"));
            }
            return std::nullopt;
        }
        if (pos > _limits.max_line) {
            fail(malformed("line too long"));
            return std::nullopt;
        }
        std::string line = _buf.substr(0, pos);
        _buf.erase(0, pos + 2);
        return line;
    }

    [[nodiscard]] auto sign(std::string_view kind, std::string_view hashed) const -> std::string {
        std::string sts(kind);
        sts += _prefix;
        sts += _previous;
        sts += '\n';
        sts += hashed;
        return detail::to_hex(detail::hmac_sha256(_key, sts));
    }

    [[nodiscard]] static auto same(std::string_view a, std::string_view b) -> bool {
        return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    /// Advances one step; false when it needs more input or stopped.
    auto step(std::string& out) -> bool {
        switch (_state) {
            case phase::header:
                return step_header();
            case phase::data:
                return step_data(out);
            case phase::trailer:
                return step_trailer();
            default:
                return false;
        }
    }

    auto step_header() -> bool {
        auto line = take_line();
        if (!line) {
            return false;
        }
        std::string_view l(*line);
        std::string_view size_text = l;
        if (signed_form()) {
            constexpr std::string_view ext = ";chunk-signature=";
            const auto semi = l.find(';');
            if (semi == std::string_view::npos || l.substr(semi, ext.size()) != ext) {
                fail(malformed("a chunk has no chunk-signature"));
                return false;
            }
            size_text = l.substr(0, semi);
            _chunk_signature = std::string(l.substr(semi + ext.size()));
            if (!detail::is_lower_hex(_chunk_signature, 64)) {
                fail(malformed("a chunk-signature is not 64 hex digits"));
                return false;
            }
        }
        if (size_text.empty() || size_text.size() > 16) {
            fail(malformed("bad chunk size"));
            return false;
        }
        std::uint64_t n = 0;
        for (char ch : size_text) {
            int d = 0;
            if (ch >= '0' && ch <= '9') {
                d = ch - '0';
            } else if (ch >= 'a' && ch <= 'f') {
                d = ch - 'a' + 10;
            } else if (ch >= 'A' && ch <= 'F') {
                d = ch - 'A' + 10;
            } else {
                fail(malformed("bad chunk size"));
                return false;
            }
            n = (n << 4U) | static_cast<std::uint64_t>(d);
        }
        if (n > _limits.max_chunk_size) {
            fail(malformed("chunk larger than " + std::to_string(_limits.max_chunk_size) +
                           " bytes"));
            return false;
        }
        if (_decoded_length && _released + n > *_decoded_length) {
            fail(aws_error{"IncompleteBody",
                           "The decoded body is longer than x-amz-decoded-content-length", 400});
            return false;
        }
        _chunk_size = static_cast<std::size_t>(n);
        if (n == 0 && _trailer_form) {
            // The final chunk: no data line follows, the trailer does.
            if (signed_form() && !verify_chunk({})) {
                return false;
            }
            _state = phase::trailer;
            return true;
        }
        _state = phase::data;
        return true;
    }

    auto verify_chunk(std::string_view data) -> bool {
        const auto expected = sign("AWS4-HMAC-SHA256-PAYLOAD",
                                   std::string(k_empty_sha256) + "\n" + detail::sha256_hex(data));
        if (!same(expected, _chunk_signature)) {
            fail(signature_mismatch(_protocol));
            return false;
        }
        _previous = expected;
        return true;
    }

    auto step_data(std::string& out) -> bool {
        if (_buf.size() < _chunk_size + 2) {
            return false;
        }
        if (_buf.compare(_chunk_size, 2, "\r\n") != 0) {
            fail(malformed("chunk data is not followed by CRLF"));
            return false;
        }
        const std::string_view data(_buf.data(), _chunk_size);
        if (signed_form() && !verify_chunk(data)) {
            return false;
        }
        out.append(data);
        _released += _chunk_size;
        _buf.erase(0, _chunk_size + 2);
        if (_chunk_size == 0) {
            return finish_stream();
        }
        _state = phase::header;
        return true;
    }

    auto step_trailer() -> bool {
        auto line = take_line();
        if (!line) {
            return false;
        }
        if (line->empty()) {
            if (_mode == payload_mode::streaming_signed_trailer && !_trailer_signed) {
                fail(signature_mismatch_with(_protocol, "The trailer is not signed."));
                return false;
            }
            for (const auto& name : _declared) {
                if (std::ranges::find(_trailers, name,
                                      &std::pair<std::string, std::string>::first) ==
                    _trailers.end()) {
                    fail(malformed("declared trailer " + name + " is missing"));
                    return false;
                }
            }
            return finish_stream();
        }
        if (_trailer_signed) {
            fail(malformed("a trailer follows x-amz-trailer-signature"));
            return false;
        }
        _trailer_bytes += line->size() + 2;
        if (_trailer_bytes > _limits.max_trailer_bytes) {
            fail(malformed("trailer too large"));
            return false;
        }
        const auto colon = line->find(':');
        if (colon == std::string::npos || colon == 0) {
            fail(malformed("a trailer line has no name"));
            return false;
        }
        auto name = detail::lower(detail::trim(std::string_view(*line).substr(0, colon)));
        std::string value(detail::trim(std::string_view(*line).substr(colon + 1)));
        if (name == "x-amz-trailer-signature") {
            if (_mode != payload_mode::streaming_signed_trailer) {
                fail(malformed("an unsigned trailer carries a signature"));
                return false;
            }
            std::string canonical;
            for (const auto& [k, v] : _trailers) {
                canonical += k;
                canonical += ':';
                canonical += v;
                canonical += '\n';
            }
            const auto expected = sign("AWS4-HMAC-SHA256-TRAILER", detail::sha256_hex(canonical));
            if (!same(expected, value)) {
                fail(signature_mismatch(_protocol));
                return false;
            }
            _trailer_signed = true;
            return true;
        }
        if (!_declared.contains(name)) {
            fail(malformed("trailer " + name + " was not declared in x-amz-trailer"));
            return false;
        }
        if (std::ranges::find(_trailers, name, &std::pair<std::string, std::string>::first) !=
            _trailers.end()) {
            fail(malformed("trailer " + name + " repeated"));
            return false;
        }
        _trailers.emplace_back(std::move(name), std::move(value));
        return true;
    }

    auto finish_stream() -> bool {
        if (_decoded_length && _released != *_decoded_length) {
            fail(aws_error{"IncompleteBody",
                           "The decoded body is shorter than x-amz-decoded-content-length", 400});
            return false;
        }
        _state = phase::done;
        if (!_buf.empty()) {
            fail(malformed("data after the final chunk"));
        }
        return false;
    }

    error_protocol _protocol;
    chunk_limits _limits;
    payload_mode _mode;
    bool _trailer_form = false;
    bool _have_key = false;
    signing_key _key{};
    std::string _prefix;    // "\n<amz-date>\n<scope>\n"
    std::string _previous;  // the last verified signature (the seed first)
    std::optional<std::uint64_t> _decoded_length;
    std::set<std::string> _declared;

    phase _state = phase::header;
    std::string _buf;
    std::size_t _chunk_size = 0;
    std::string _chunk_signature;
    std::uint64_t _released = 0;
    std::size_t _trailer_bytes = 0;
    bool _trailer_signed = false;
    std::vector<std::pair<std::string, std::string>> _trailers;
    aws_error _error;
};

}  // namespace kythira::sigv4
