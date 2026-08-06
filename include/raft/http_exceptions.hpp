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
    http_client_error(int status_code, const std::string& message)
        : http_transport_error(message), _status_code(status_code) {}

    [[nodiscard]] auto status_code() const -> int { return _status_code; }

private:
    int _status_code;
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
