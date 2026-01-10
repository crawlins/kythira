#pragma once

#include <stdexcept>
#include <string>
#include <cstdint>

namespace kythira {

// Base exception class for CoAP transport errors
class coap_transport_error : public std::runtime_error {
public:
    explicit coap_transport_error(const std::string& message)
        : std::runtime_error(message) {}
};

// Exception for CoAP client errors (4.xx response codes)
class coap_client_error : public coap_transport_error {
public:
    coap_client_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message)
        , _response_code(response_code) {}
    
    auto response_code() const -> std::uint8_t {
        return _response_code;
    }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP server errors (5.xx response codes)
class coap_server_error : public coap_transport_error {
public:
    coap_server_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message)
        , _response_code(response_code) {}
    
    auto response_code() const -> std::uint8_t {
        return _response_code;
    }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP timeout errors
class coap_timeout_error : public coap_transport_error {
public:
    explicit coap_timeout_error(const std::string& message)
        : coap_transport_error(message) {}
};

// Exception for CoAP security/DTLS errors
class coap_security_error : public coap_transport_error {
public:
    explicit coap_security_error(const std::string& message)
        : coap_transport_error(message) {}
};

// Exception for CoAP protocol errors
class coap_protocol_error : public coap_transport_error {
public:
    explicit coap_protocol_error(const std::string& message)
        : coap_transport_error(message) {}
};

// Exception for CoAP network errors
class coap_network_error : public coap_transport_error {
public:
    explicit coap_network_error(const std::string& message)
        : coap_transport_error(message) {}
};

} // namespace kythira