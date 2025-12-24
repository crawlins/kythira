#pragma once

#include <raft/coap_transport.hpp>
#include <raft/coap_exceptions.hpp>

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <random>
#include <algorithm>
#include <chrono>
#include <iostream>

namespace raft::coap_utils {

// Configuration validation functions
auto validate_client_config(const coap_client_config& config) -> void {
    // Validate timeout values
    if (config.ack_timeout.count() <= 0) {
        throw coap_transport_error("Client ack_timeout must be positive");
    }
    
    if (config.ack_random_factor_ms.count() < 0) {
        throw coap_transport_error("Client ack_random_factor_ms must be non-negative");
    }
    
    if (config.max_retransmit == 0) {
        throw coap_transport_error("Client max_retransmit must be at least 1");
    }
    
    if (config.max_retransmit > 20) {
        throw coap_transport_error("Client max_retransmit should not exceed 20 to prevent excessive delays");
    }
    
    // Validate DTLS configuration
    if (config.enable_dtls) {
        bool has_cert_auth = !config.cert_file.empty() && !config.key_file.empty();
        bool has_psk_auth = !config.psk_identity.empty() && !config.psk_key.empty();
        
        if (!has_cert_auth && !has_psk_auth) {
            throw coap_security_error("DTLS enabled but no valid authentication method configured (certificate or PSK)");
        }
        
        if (has_cert_auth && has_psk_auth) {
            throw coap_security_error("Cannot configure both certificate and PSK authentication simultaneously");
        }
        
        // Validate PSK parameters if PSK auth is used
        if (has_psk_auth) {
            if (config.psk_key.size() < 4 || config.psk_key.size() > 64) {
                throw coap_security_error("PSK key length must be between 4 and 64 bytes");
            }
            
            if (config.psk_identity.length() > 128) {
                throw coap_security_error("PSK identity length must not exceed 128 characters");
            }
        }
    }
    
    // Validate block transfer configuration
    if (config.enable_block_transfer) {
        if (!is_valid_block_size(config.max_block_size)) {
            throw coap_transport_error("Invalid block size: " + std::to_string(config.max_block_size) + 
                                     ". Must be a power of 2 between 16 and 1024");
        }
    }
    
    // Validate connection management parameters
    if (config.max_sessions == 0) {
        throw coap_transport_error("Client max_sessions must be at least 1");
    }
    
    if (config.session_timeout.count() <= 0) {
        throw coap_transport_error("Client session_timeout must be positive");
    }
    
    // Validate exponential backoff parameters
    if (config.exponential_backoff_factor <= 1.0) {
        throw coap_transport_error("Client exponential_backoff_factor must be greater than 1.0");
    }
    
    if (config.exponential_backoff_factor > 10.0) {
        throw coap_transport_error("Client exponential_backoff_factor should not exceed 10.0 to prevent excessive delays");
    }
}

auto validate_server_config(const coap_server_config& config) -> void {
    // Validate session limits
    if (config.max_concurrent_sessions == 0) {
        throw coap_transport_error("Server max_concurrent_sessions must be at least 1");
    }
    
    if (config.max_request_size == 0) {
        throw coap_transport_error("Server max_request_size must be at least 1");
    }
    
    if (config.max_request_size > 64 * 1024 * 1024) { // 64 MB limit
        throw coap_transport_error("Server max_request_size should not exceed 64 MB");
    }
    
    if (config.session_timeout.count() <= 0) {
        throw coap_transport_error("Server session_timeout must be positive");
    }
    
    // Validate DTLS configuration
    if (config.enable_dtls) {
        bool has_cert_auth = !config.cert_file.empty() && !config.key_file.empty();
        bool has_psk_auth = !config.psk_identity.empty() && !config.psk_key.empty();
        
        if (!has_cert_auth && !has_psk_auth) {
            throw coap_security_error("Server DTLS enabled but no valid authentication method configured (certificate or PSK)");
        }
        
        if (has_cert_auth && has_psk_auth) {
            throw coap_security_error("Server cannot configure both certificate and PSK authentication simultaneously");
        }
        
        // Validate PSK parameters if PSK auth is used
        if (has_psk_auth) {
            if (config.psk_key.size() < 4 || config.psk_key.size() > 64) {
                throw coap_security_error("Server PSK key length must be between 4 and 64 bytes");
            }
            
            if (config.psk_identity.length() > 128) {
                throw coap_security_error("Server PSK identity length must not exceed 128 characters");
            }
        }
    }
    
    // Validate block transfer configuration
    if (config.enable_block_transfer) {
        if (!is_valid_block_size(config.max_block_size)) {
            throw coap_transport_error("Server invalid block size: " + std::to_string(config.max_block_size) + 
                                     ". Must be a power of 2 between 16 and 1024");
        }
    }
    
    // Validate multicast configuration
    if (config.enable_multicast) {
        if (config.multicast_address.empty()) {
            throw coap_transport_error("Server multicast enabled but multicast_address is empty");
        }
        
        if (config.multicast_port == 0) {
            throw coap_transport_error("Server multicast_port must be non-zero when multicast is enabled");
        }
        
        // Basic validation for IPv4 multicast address format
        if (config.multicast_address.find("224.") == 0 || 
            config.multicast_address.find("239.") == 0) {
            // Valid IPv4 multicast range
        } else if (config.multicast_address.find("ff") == 0 || 
                   config.multicast_address.find("FF") == 0) {
            // Valid IPv6 multicast range
        } else {
            throw coap_transport_error("Server invalid multicast address: " + config.multicast_address);
        }
    }
    
    // Validate exponential backoff parameters
    if (config.exponential_backoff_factor <= 1.0) {
        throw coap_transport_error("Server exponential_backoff_factor must be greater than 1.0");
    }
    
    if (config.exponential_backoff_factor > 10.0) {
        throw coap_transport_error("Server exponential_backoff_factor should not exceed 10.0 to prevent excessive delays");
    }
}

// Endpoint parsing utilities
auto parse_coap_endpoint(const std::string& endpoint) -> parsed_endpoint {
    if (endpoint.empty()) {
        throw coap_network_error("Empty endpoint");
    }
    
    // Regular expression to parse CoAP URI: (coap|coaps)://host:port[/path]
    std::regex coap_uri_regex(R"(^(coaps?)://([^:/]+)(?::(\d+))?(?:/(.*))?$)");
    std::smatch matches;
    
    if (!std::regex_match(endpoint, matches, coap_uri_regex)) {
        throw coap_network_error("Invalid CoAP endpoint format: " + endpoint);
    }
    
    parsed_endpoint result;
    result.scheme = matches[1].str();
    result.host = matches[2].str();
    
    // Parse port number
    if (matches[3].matched) {
        try {
            auto port_num = std::stoul(matches[3].str());
            if (port_num == 0 || port_num > 65535) {
                throw coap_network_error("Invalid port number in endpoint: " + endpoint);
            }
            result.port = static_cast<std::uint16_t>(port_num);
        } catch (const std::exception&) {
            throw coap_network_error("Invalid port number in endpoint: " + endpoint);
        }
    } else {
        // Use default ports
        result.port = (result.scheme == "coaps") ? 5684 : 5683;
    }
    
    // Parse path (optional)
    if (matches[4].matched) {
        result.path = "/" + matches[4].str();
    }
    
    // Validate host format (basic validation)
    if (result.host.empty()) {
        throw coap_network_error("Empty host in endpoint: " + endpoint);
    }
    
    return result;
}

auto format_coap_endpoint(const parsed_endpoint& endpoint) -> std::string {
    if (endpoint.scheme.empty() || endpoint.host.empty()) {
        throw coap_network_error("Invalid endpoint: scheme and host are required");
    }
    
    if (endpoint.scheme != "coap" && endpoint.scheme != "coaps") {
        throw coap_network_error("Invalid scheme: " + endpoint.scheme + ". Must be 'coap' or 'coaps'");
    }
    
    if (endpoint.port == 0 || endpoint.port > 65535) {
        throw coap_network_error("Invalid port: " + std::to_string(endpoint.port));
    }
    
    std::string result = endpoint.scheme + "://" + endpoint.host + ":" + std::to_string(endpoint.port);
    
    if (!endpoint.path.empty()) {
        if (endpoint.path[0] != '/') {
            result += "/";
        }
        result += endpoint.path;
    }
    
    return result;
}

auto is_valid_coap_endpoint(const std::string& endpoint) -> bool {
    try {
        parse_coap_endpoint(endpoint);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Token generation utilities
auto generate_coap_token() -> std::vector<std::byte> {
    // Generate a random token of default length (4 bytes)
    return generate_coap_token(4);
}

auto generate_coap_token(std::size_t length) -> std::vector<std::byte> {
    if (length == 0 || length > 8) {
        throw coap_transport_error("CoAP token length must be between 1 and 8 bytes");
    }
    
    std::vector<std::byte> token(length);
    
    // Use a random number generator with current time as seed
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint8_t> dis(0, 255);
    
    for (std::size_t i = 0; i < length; ++i) {
        token[i] = static_cast<std::byte>(dis(gen));
    }
    
    return token;
}

auto is_valid_coap_token(const std::vector<std::byte>& token) -> bool {
    // CoAP tokens must be between 0 and 8 bytes in length
    return token.size() <= 8;
}

// CoAP option handling utilities
auto get_content_format_for_serializer(const std::string& serializer_name) -> coap_content_format {
    // Convert serializer name to lowercase for comparison
    std::string lower_name = serializer_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    if (lower_name == "json" || lower_name == "json_serializer") {
        return coap_content_format::application_json;
    } else if (lower_name == "cbor" || lower_name == "cbor_serializer") {
        return coap_content_format::application_cbor;
    } else if (lower_name == "xml" || lower_name == "xml_serializer") {
        return coap_content_format::application_xml;
    } else if (lower_name == "text" || lower_name == "text_serializer") {
        return coap_content_format::text_plain;
    } else {
        // Default to CBOR for unknown serializers
        return coap_content_format::application_cbor;
    }
}

auto content_format_to_string(coap_content_format format) -> std::string {
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
            return "unknown";
    }
}

auto parse_content_format(std::uint16_t format_value) -> coap_content_format {
    switch (format_value) {
        case 0:
            return coap_content_format::text_plain;
        case 40:
            return coap_content_format::application_link_format;
        case 41:
            return coap_content_format::application_xml;
        case 42:
            return coap_content_format::application_octet_stream;
        case 47:
            return coap_content_format::application_exi;
        case 50:
            return coap_content_format::application_json;
        case 60:
            return coap_content_format::application_cbor;
        default:
            throw coap_protocol_error("Unknown content format: " + std::to_string(format_value));
    }
}

// Block option utilities
auto calculate_block_size_szx(std::size_t block_size) -> std::uint8_t {
    // CoAP block size is encoded as SZX where actual size = 2^(SZX+4)
    // Valid block sizes: 16, 32, 64, 128, 256, 512, 1024 (SZX 0-6)
    
    if (block_size < 16 || block_size > 1024) {
        throw coap_transport_error("Block size must be between 16 and 1024 bytes");
    }
    
    // Check if block_size is a power of 2
    if ((block_size & (block_size - 1)) != 0) {
        throw coap_transport_error("Block size must be a power of 2");
    }
    
    // Calculate SZX
    std::uint8_t szx = 0;
    std::size_t size = block_size;
    while (size > 16) {
        size >>= 1;
        szx++;
    }
    
    if (szx > 6) {
        throw coap_transport_error("Block size too large, maximum is 1024 bytes");
    }
    
    return szx;
}

auto szx_to_block_size(std::uint8_t szx) -> std::size_t {
    if (szx > 6) {
        throw coap_transport_error("Invalid SZX value: " + std::to_string(szx) + ". Must be 0-6");
    }
    
    return 1U << (szx + 4);  // 2^(SZX+4)
}

auto is_valid_block_size(std::size_t block_size) -> bool {
    try {
        calculate_block_size_szx(block_size);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace raft::coap_utils