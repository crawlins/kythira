#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <random>
#include <chrono>
#include <stdexcept>
#include <raft/coap_exceptions.hpp>

namespace kythira {

/// @brief Maximum CoAP token length, in bytes.
///
/// RFC 7252 §5.3.1 gives the Token Length field 4 bits and reserves values
/// 9-15, so a token is 0-8 bytes. libcoap enforces this in coap_send(), which
/// *drops* an over-long PDU rather than failing the call that built it -- so
/// exceeding this is silent at the point of use and has to be prevented at the
/// point of generation.
///
/// Declared here, rather than used from `coap3/coap_pdu.h`, because the token
/// helpers below and coap_client::generate_message_token() are all compiled in
/// the stub build too, where libcoap's headers are not included at all.
/// coap_transport.hpp static_asserts this against COAP_TOKEN_DEFAULT_MAX
/// wherever libcoap is actually present, so the two cannot drift.
inline constexpr std::size_t coap_max_token_length = 8;

namespace coap_utils {

// Endpoint parsing utilities
struct parsed_endpoint {
    std::string scheme;  // "coap" or "coaps"
    std::string host;    // hostname or IP address
    std::uint16_t port;  // port number
    std::string path;    // resource path (optional)

    parsed_endpoint() = default;
    parsed_endpoint(std::string s, std::string h, std::uint16_t p, std::string path = "")
        : scheme(std::move(s)), host(std::move(h)), port(p), path(std::move(path)) {}
};

inline auto parse_coap_endpoint(const std::string& endpoint) -> parsed_endpoint {
    parsed_endpoint result;

    if (endpoint.empty()) {
        throw coap_network_error("Empty endpoint");
    }

    // Parse scheme
    std::string rest;
    if (endpoint.find("coaps://") == 0) {
        result.scheme = "coaps";
        rest = endpoint.substr(8);  // Remove "coaps://"
    } else if (endpoint.find("coap://") == 0) {
        result.scheme = "coap";
        rest = endpoint.substr(7);  // Remove "coap://"
    } else {
        throw coap_network_error("Invalid scheme - must be coap:// or coaps://");
    }

    // Parse path if present
    auto slash_pos = rest.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port = rest.substr(0, slash_pos);
        result.path = rest.substr(slash_pos);  // Include the leading slash
    } else {
        host_port = rest;
    }

    // Parse host and port
    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        result.host = host_port.substr(0, colon_pos);
        auto port_str = host_port.substr(colon_pos + 1);
        try {
            int port_int = std::stoi(port_str);
            if (port_int <= 0 || port_int > 65535) {
                throw coap_network_error("Invalid port number - must be 1-65535");
            }
            result.port = static_cast<std::uint16_t>(port_int);
        } catch (const std::exception&) {
            throw coap_network_error("Invalid port number");
        }
    } else {
        result.host = host_port;
        result.port = (result.scheme == "coaps") ? 5684 : 5683;  // Default ports
    }

    return result;
}

inline auto format_coap_endpoint(const parsed_endpoint& endpoint) -> std::string {
    if (endpoint.scheme != "coap" && endpoint.scheme != "coaps") {
        throw coap_network_error("Invalid scheme - must be coap or coaps");
    }
    if (endpoint.host.empty()) {
        throw coap_network_error("Empty host");
    }
    if (endpoint.port == 0) {
        throw coap_network_error("Invalid port - must be non-zero");
    }

    return endpoint.scheme + "://" + endpoint.host + ":" + std::to_string(endpoint.port) +
           endpoint.path;
}

inline auto is_valid_coap_endpoint(const std::string& endpoint) -> bool {
    if (endpoint.empty()) {
        return false;
    }

    if (endpoint.find("coap://") != 0 && endpoint.find("coaps://") != 0) {
        return false;
    }

    // Try to parse and validate
    try {
        auto parsed = parse_coap_endpoint(endpoint);
        return parsed.port > 0 && parsed.port <= 65535 && !parsed.host.empty();
    } catch (...) {
        return false;
    }
}

// Token generation utilities
inline auto generate_coap_token(std::size_t length = 4) -> std::vector<std::byte> {
    if (length == 0) {
        throw coap_transport_error("Token length must be at least 1");
    }
    if (length > coap_max_token_length) {
        throw coap_transport_error("Token length must be at most " +
                                   std::to_string(coap_max_token_length) + " bytes");
    }

    std::vector<std::byte> token(length);

    // Use current time and random number for token generation
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> dis(0, 255);

    for (std::size_t i = 0; i < length; ++i) {
        token[i] = static_cast<std::byte>(dis(gen));
    }

    return token;
}

inline auto is_valid_coap_token(const std::vector<std::byte>& token) -> bool {
    return token.size() <= coap_max_token_length;
}

/// @brief Render a request counter as a fixed-width, printable CoAP token.
///
/// The result is *always* exactly `coap_max_token_length` bytes, for every
/// input. That invariant is the whole point: the previous scheme built tokens
/// as `"token_" + std::to_string(n)`, which is 7 bytes at n=0 and 9 bytes from
/// n=100 onward. Nothing rejected the over-long token -- coap_add_token()
/// accepted it and coap_send() silently dropped the PDU -- so a client stopped
/// transmitting after its 100th request with only a libcoap warning to show
/// for it. A width that cannot vary with the counter removes that failure mode
/// by construction rather than by check.
///
/// Lowercase hex keeps tokens printable, which matters because the transport
/// logs the token on ~45 paths and uses it as an `_pending_requests` map key.
/// Most-significant nibble first, so lexicographic order matches numeric order
/// when reading logs.
inline auto format_sequential_token(std::uint32_t counter) -> std::string {
    // Eight hex digits carry exactly the 32 bits of `counter`. If the cap ever
    // changes, this mapping has to be revisited rather than silently truncate.
    static_assert(coap_max_token_length == 8,
                  "format_sequential_token encodes a uint32_t as exactly 8 hex digits");
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string token(coap_max_token_length, '0');
    for (std::size_t i = 0; i < coap_max_token_length; ++i) {
        const auto shift = 4U * static_cast<unsigned>(coap_max_token_length - 1U - i);
        token[i] = hex_digits[(counter >> shift) & 0xFU];
    }
    return token;
}

// CoAP option handling utilities
enum class coap_content_format : std::uint16_t {  // NOLINT(performance-enum-size)
    text_plain = 0,
    application_link_format = 40,
    application_xml = 41,
    application_octet_stream = 42,
    application_exi = 47,
    application_json = 50,
    application_cbor = 60,
    // No IANA-registered CoAP Content-Format exists for Amazon Ion, so this
    // uses a value from RFC 7252 §12.3's experimental/private-use range
    // (65000-65535). A single value covers both binary and text Ion: Ion's
    // binary version marker makes the two encodings self-describing to a
    // receiver, so no separate number per encoding is needed
    // (ion-rpc-serializer spec, Requirement 6.3). If IANA ever registers an
    // official Ion Content-Format, migrate to it (design.md Future Enhancements).
    application_ion = 65000
};

inline auto get_content_format_for_serializer(const std::string& serializer_name)
    -> coap_content_format {
    // Check for Ion variants ("ion-binary"/"ion-text") first. Checked before
    // JSON: "json" and "ion" never collide as substrings, so order is safe.
    if (serializer_name.find("ion") != std::string::npos ||
        serializer_name.find("ION") != std::string::npos) {
        return coap_content_format::application_ion;
    }
    // Check for JSON variants
    if (serializer_name.find("json") != std::string::npos ||
        serializer_name.find("JSON") != std::string::npos) {
        return coap_content_format::application_json;
    }
    // Check for CBOR variants
    if (serializer_name.find("cbor") != std::string::npos ||
        serializer_name.find("CBOR") != std::string::npos) {
        return coap_content_format::application_cbor;
    }
    // Check for Protocol Buffers variants (protobuf_rpc_serializer::name() ==
    // "protobuf"). No IANA CoAP content-format is registered for protobuf, so
    // label it as the generic binary octet-stream (registered format 42) rather
    // than mislabeling it as CBOR -- correct and interoperable on both the HTTP
    // (application/octet-stream Content-Type) and CoAP wire.
    if (serializer_name.find("protobuf") != std::string::npos ||
        serializer_name.find("PROTOBUF") != std::string::npos) {
        return coap_content_format::application_octet_stream;
    }
    // Check for XML
    if (serializer_name == "xml" || serializer_name == "XML") {
        return coap_content_format::application_xml;
    }
    // Check for text/plain
    if (serializer_name == "text" || serializer_name == "TEXT") {
        return coap_content_format::text_plain;
    }

    // Default to CBOR for unknown serializers
    return coap_content_format::application_cbor;
}

inline auto content_format_to_string(coap_content_format format) -> std::string {
    switch (format) {
        case coap_content_format::text_plain:
            return "text/plain";
        case coap_content_format::application_link_format:
            return "application/link-format";
        case coap_content_format::application_xml:
            return "application/xml";
        case coap_content_format::application_octet_stream:
            return "application/octet-stream";
        case coap_content_format::application_exi:
            return "application/exi";
        case coap_content_format::application_json:
            return "application/json";
        case coap_content_format::application_cbor:
            return "application/cbor";
        case coap_content_format::application_ion:
            return "application/ion";
        default:
            return "application/octet-stream";
    }
}

inline auto parse_content_format(std::uint16_t format_value) -> coap_content_format {
    // Validate that the format value is one of the known formats
    switch (format_value) {
        case 0:      // text_plain
        case 40:     // application_link_format
        case 41:     // application_xml
        case 42:     // application_octet_stream
        case 47:     // application_exi
        case 50:     // application_json
        case 60:     // application_cbor
        case 65000:  // application_ion (private-use range, RFC 7252 §12.3)
            return static_cast<coap_content_format>(format_value);
        default:
            throw coap_protocol_error("Unknown content format value: " +
                                      std::to_string(format_value));
    }
}

// Block option utilities
inline auto calculate_block_size_szx(std::size_t block_size) -> std::uint8_t {
    // Validate block size
    if (block_size < 16) {
        throw coap_transport_error("Block size must be at least 16 bytes");
    }
    if (block_size > 1024) {
        throw coap_transport_error("Block size must be at most 1024 bytes");
    }
    if ((block_size & (block_size - 1)) != 0) {
        throw coap_transport_error("Block size must be a power of 2");
    }

    // Calculate SZX from block size (SZX = log2(block_size) - 4)
    std::uint8_t szx = 0;
    std::size_t size = block_size;
    while (size > 16) {
        size >>= 1;
        szx++;
    }
    return szx;
}

inline auto szx_to_block_size(std::uint8_t szx) -> std::size_t {
    // Validate SZX value
    if (szx > 6) {
        throw coap_transport_error("SZX value must be 0-6 (block sizes 16-1024)");
    }

    // Convert SZX to block size (block_size = 2^(SZX+4))
    return 1U << (szx + 4);
}

inline auto is_valid_block_size(std::size_t block_size) -> bool {
    // Valid block sizes are powers of 2 from 16 to 1024
    return block_size >= 16 && block_size <= 1024 && (block_size & (block_size - 1)) == 0;
}

}  // namespace coap_utils
}  // namespace kythira