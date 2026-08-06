#pragma once

#include <stdexcept>
#include <string>
#include <cstdint>

namespace kythira {

// Base exception class for CoAP transport errors
class coap_transport_error : public std::runtime_error {
public:
    explicit coap_transport_error(const std::string& message) : std::runtime_error(message) {}
};

// Exception for CoAP client errors (4.xx response codes)
class coap_client_error : public coap_transport_error {
public:
    coap_client_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message), _response_code(response_code) {}

    [[nodiscard]] auto response_code() const -> std::uint8_t { return _response_code; }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP server errors (5.xx response codes)
class coap_server_error : public coap_transport_error {
public:
    coap_server_error(std::uint8_t response_code, const std::string& message)
        : coap_transport_error(message), _response_code(response_code) {}

    [[nodiscard]] auto response_code() const -> std::uint8_t { return _response_code; }

private:
    std::uint8_t _response_code;
};

// Exception for CoAP timeout errors
class coap_timeout_error : public coap_transport_error {
public:
    explicit coap_timeout_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP security/DTLS errors
class coap_security_error : public coap_transport_error {
public:
    explicit coap_security_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP protocol errors
class coap_protocol_error : public coap_transport_error {
public:
    explicit coap_protocol_error(const std::string& message) : coap_transport_error(message) {}
};

// Exception for CoAP network errors
class coap_network_error : public coap_transport_error {
public:
    explicit coap_network_error(const std::string& message) : coap_transport_error(message) {}
};

/// @brief A peer asked for, or sent, a media type no registered serializer
///        handles — the CoAP analogue of `unsupported_media_type_error`.
///
/// Separate from the HTTP type rather than shared, because the two hierarchies
/// are separate all the way up (`coap_transport_error` vs
/// `http_transport_error`) and a CoAP handler catching
/// `const coap_transport_error&` must not miss this one. It carries the media
/// type rather than a `coap_content_format` for the same reason the registry is
/// keyed that way: the media type is what the registry dispatches on, and the
/// Content-Format number is a CoAP-side encoding of it that not every media
/// type has (see `media_type_to_coap_content_format`).
///
/// Distinct from `coap_protocol_error`, which means a malformed or
/// unintelligible PDU. This one means a well-formed request naming an encoding
/// this node does not speak, and the two get different responses: 4.15
/// Unsupported Content-Format / 4.06 Not Acceptable for this, 4.00 Bad Request
/// for that.
class coap_unsupported_content_format_error : public coap_transport_error {
public:
    explicit coap_unsupported_content_format_error(const std::string& media_type)
        : coap_transport_error("unsupported content format: " + media_type),
          _media_type(media_type) {}

    [[nodiscard]] auto media_type() const -> const std::string& { return _media_type; }

private:
    std::string _media_type;
};

}  // namespace kythira
