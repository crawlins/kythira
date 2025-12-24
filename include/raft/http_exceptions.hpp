#pragma once

#include <stdexcept>
#include <string>

namespace raft {

// Base exception class for HTTP transport errors
class http_transport_error : public std::runtime_error {
public:
    explicit http_transport_error(const std::string& message)
        : std::runtime_error(message) {}
};

// Exception for HTTP client errors (4xx status codes)
class http_client_error : public http_transport_error {
public:
    http_client_error(int status_code, const std::string& message)
        : http_transport_error(message)
        , _status_code(status_code) {}
    
    auto status_code() const -> int {
        return _status_code;
    }

private:
    int _status_code;
};

// Exception for HTTP server errors (5xx status codes)
class http_server_error : public http_transport_error {
public:
    http_server_error(int status_code, const std::string& message)
        : http_transport_error(message)
        , _status_code(status_code) {}
    
    auto status_code() const -> int {
        return _status_code;
    }

private:
    int _status_code;
};

// Exception for HTTP timeout errors
class http_timeout_error : public http_transport_error {
public:
    explicit http_timeout_error(const std::string& message)
        : http_transport_error(message) {}
};

// Exception for serialization/deserialization errors
class serialization_error : public http_transport_error {
public:
    explicit serialization_error(const std::string& message)
        : http_transport_error(message) {}
};

} // namespace raft
