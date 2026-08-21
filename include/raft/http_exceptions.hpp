// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdexcept>
#include <string>

namespace kythira {

// Base exception class for HTTP transport errors
class http_transport_error : public std::runtime_error {
public:
    explicit http_transport_error(const std::string& message) : std::runtime_error(message) {}
};

// Exception for HTTP client errors (4xx status codes)
class http_client_error : public http_transport_error {
public:
    http_client_error(int status_code, const std::string& message, std::string accept_post = {})
        : http_transport_error(message),
          _status_code(status_code),
          _accept_post(std::move(accept_post)) {}

    [[nodiscard]] auto status_code() const -> int { return _status_code; }

    /// @brief The peer's `Accept-Post` header value, verbatim, or empty when it
    ///        sent none.
    ///
    /// Only ever populated on a 415: it is the rejecting server naming the
    /// request media types it *would* have taken (W3C Linked Data Platform 1.0
    /// §7.1), which lets a client's retry converge in one round trip instead of
    /// walking its whole preference list. Carried on the exception rather than
    /// returned alongside it because in the async transports the 415 *is* the
    /// exception — the response object is gone by the time the retry decides
    /// what to send next.
    ///
    /// Defaulted to empty rather than required, so this is additive: every
    /// existing construction site keeps compiling, and a peer that sends no
    /// such header (which is most of them — an unmodified single-serializer
    /// node emits nothing) falls back to the blind walk unchanged.
    [[nodiscard]] auto accept_post() const -> const std::string& { return _accept_post; }

private:
    int _status_code;
    std::string _accept_post;
};

// Exception for HTTP server errors (5xx status codes)
class http_server_error : public http_transport_error {
public:
    http_server_error(int status_code, const std::string& message)
        : http_transport_error(message), _status_code(status_code) {}

    [[nodiscard]] auto status_code() const -> int { return _status_code; }

private:
    int _status_code;
};

// Exception for HTTP timeout errors
class http_timeout_error : public http_transport_error {
public:
    explicit http_timeout_error(const std::string& message) : http_transport_error(message) {}
};

// Exception for serialization/deserialization errors
class serialization_error : public http_transport_error {
public:
    explicit serialization_error(const std::string& message) : http_transport_error(message) {}
};

/// @brief A peer asked for, or sent, a media type no registered serializer
///        handles.
///
/// Distinct from `serialization_error`, which means a payload of a media type
/// we *do* support failed to decode. The two map to different HTTP responses —
/// 415 for this, 400 for that — so a transport must be able to tell them apart
/// without inspecting the message text. Carries the offending media type so the
/// handler need not re-parse the header to build its response.
class unsupported_media_type_error : public http_transport_error {
public:
    explicit unsupported_media_type_error(const std::string& media_type)
        : http_transport_error("unsupported media type: " + media_type), _media_type(media_type) {}

    [[nodiscard]] auto media_type() const -> const std::string& { return _media_type; }

private:
    std::string _media_type;
};

// Exception for SSL configuration errors
class ssl_configuration_error : public http_transport_error {
public:
    explicit ssl_configuration_error(const std::string& message) : http_transport_error(message) {}
};

// Exception for certificate validation errors
class certificate_validation_error : public http_transport_error {
public:
    explicit certificate_validation_error(const std::string& message)
        : http_transport_error(message) {}
};

// Exception for SSL context errors
class ssl_context_error : public http_transport_error {
public:
    explicit ssl_context_error(const std::string& message) : http_transport_error(message) {}
};

}  // namespace kythira
