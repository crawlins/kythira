#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <random>
#include <chrono>

namespace kythira {
namespace coap_utils {

// Endpoint parsing utilities
struct parsed_endpoint {
    std::string scheme;      // "coap" or "coaps"
    std::string host;        // hostname or IP address
    std::uint16_t port;      // port number
    std::string path;        // resource path (optional)
    
    parsed_endpoint() = default;
    parsed_endpoint(std::string s, std::string h, std::uint16_t p, std::string path = "")
        : scheme(std::move(s)), host(std::move(h)), port(p), path(std::move(path)) {}
};

inline auto parse_coap_endpoint(const std::string& endpoint) -> parsed_endpoint {
    parsed_endpoint result;
    
    if (endpoint.empty()) {
        return result;
    }
    
    // Parse scheme
    if (endpoint.find("coaps://") == 0) {
        result.scheme = "coaps";
        auto rest = endpoint.substr(8); // Remove "coaps://"
        
        // Parse host and port
        auto colon_pos = rest.find(':');
        if (colon_pos != std::string::npos) {
            result.host = rest.substr(0, colon_pos);
            auto port_str = rest.substr(colon_pos + 1);
            result.port = static_cast<std::uint16_t>(std::stoi(port_str));
        } else {
            result.host = rest;
            result.port = 5684; // Default CoAPS port
        }
    } else if (endpoint.find("coap://") == 0) {
        result.scheme = "coap";
        auto rest = endpoint.substr(7); // Remove "coap://"
        
        // Parse host and port
        auto colon_pos = rest.find(':');
        if (colon_pos != std::string::npos) {
            result.host = rest.substr(0, colon_pos);
            auto port_str = rest.substr(colon_pos + 1);
            result.port = static_cast<std::uint16_t>(std::stoi(port_str));
        } else {
            result.host = rest;
            result.port = 5683; // Default CoAP port
        }
    }
    
    return result;
}

inline auto format_coap_endpoint(const parsed_endpoint& endpoint) -> std::string {
    return endpoint.scheme + "://" + endpoint.host + ":" + std::to_string(endpoint.port) + endpoint.path;
}

inline auto is_valid_coap_endpoint(const std::string& endpoint) -> bool {
    if (endpoint.empty()) {
        return false;
    }
    
    return endpoint.find("coap://") == 0 || endpoint.find("coaps://") == 0;
}

// Token generation utilities
inline auto generate_coap_token(std::size_t length = 8) -> std::vector<std::byte> {
    if (length > 8) {
        length = 8; // CoAP tokens are max 8 bytes
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
    return token.size() <= 8; // CoAP tokens are max 8 bytes
}

// CoAP option handling utilities
enum class coap_content_format : std::uint16_t {
    text_plain = 0,
    application_link_format = 40,
    application_xml = 41,
    application_octet_stream = 42,
    application_exi = 47,
    application_json = 50,
    application_cbor = 60
};

inline auto get_content_format_for_serializer(const std::string& serializer_name) -> coap_content_format {
    if (serializer_name == "json" || serializer_name == "JSON") {
        return coap_content_format::application_json;
    } else if (serializer_name == "cbor" || serializer_name == "CBOR") {
        return coap_content_format::application_cbor;
    } else {
        return coap_content_format::application_octet_stream; // Default
    }
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
        default:
            return "application/octet-stream";
    }
}

inline auto parse_content_format(std::uint16_t format_value) -> coap_content_format {
    return static_cast<coap_content_format>(format_value);
}

// Block option utilities
inline auto calculate_block_size_szx(std::size_t block_size) -> std::uint8_t {
    // Calculate SZX from block size (SZX = log2(block_size) - 4)
    std::uint8_t szx = 0;
    std::size_t size = block_size;
    while (size > 16 && szx < 7) {
        size >>= 1;
        szx++;
    }
    return szx;
}

inline auto szx_to_block_size(std::uint8_t szx) -> std::size_t {
    // Convert SZX to block size (block_size = 2^(SZX+4))
    if (szx > 7) {
        szx = 7; // Max SZX value
    }
    return 1U << (szx + 4);
}

inline auto is_valid_block_size(std::size_t block_size) -> bool {
    // Valid block sizes are powers of 2 from 16 to 1024
    return block_size >= 16 && block_size <= 1024 && (block_size & (block_size - 1)) == 0;
}

} // namespace coap_utils
} // namespace kythira