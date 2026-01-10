#define BOOST_TEST_MODULE CoAPConfigTest
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_utils.hpp>
#include <raft/coap_exceptions.hpp>

#include <chrono>
#include <vector>
#include <string>

using namespace raft;
using namespace kythira::coap_utils;

// Stream operator for coap_content_format (for testing)
inline auto operator<<(std::ostream& os, coap_content_format format) -> std::ostream& {
    return os << content_format_to_string(format);
}

namespace {
    // Test constants
    constexpr std::chrono::milliseconds valid_timeout{2000};
    constexpr std::chrono::milliseconds invalid_timeout{-1000};
    constexpr std::size_t valid_max_retransmit = 4;
    constexpr std::size_t invalid_max_retransmit = 0;
    constexpr std::size_t valid_max_sessions = 100;
    constexpr std::size_t invalid_max_sessions = 0;
    constexpr std::chrono::seconds valid_session_timeout{300};
    constexpr std::chrono::seconds invalid_session_timeout{-300};
    constexpr std::size_t valid_block_size = 1024;
    constexpr std::size_t invalid_block_size = 100;
    constexpr double valid_backoff_factor = 2.0;
    constexpr double invalid_backoff_factor_low = 0.5;
    constexpr double invalid_backoff_factor_high = 15.0;
    
    constexpr const char* valid_cert_file = "/path/to/cert.pem";
    constexpr const char* valid_key_file = "/path/to/key.pem";
    constexpr const char* valid_ca_file = "/path/to/ca.pem";
    constexpr const char* valid_psk_identity = "test_identity";
    constexpr const char* valid_multicast_address = "224.0.1.187";
    constexpr std::uint16_t valid_multicast_port = 5683;
    constexpr std::uint16_t invalid_multicast_port = 0;
}

BOOST_AUTO_TEST_SUITE(CoAPClientConfigTests)

BOOST_AUTO_TEST_CASE(test_valid_client_config, * boost::unit_test::timeout(15)) {
    coap_client_config config;
    config.ack_timeout = valid_timeout;
    config.max_retransmit = valid_max_retransmit;
    config.max_sessions = valid_max_sessions;
    config.session_timeout = valid_session_timeout;
    config.max_block_size = valid_block_size;
    config.exponential_backoff_factor = valid_backoff_factor;
    
    BOOST_CHECK_NO_THROW(validate_client_config(config));
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_timeout, * boost::unit_test::timeout(15)) {
    coap_client_config config;
    config.ack_timeout = invalid_timeout;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_max_retransmit, * boost::unit_test::timeout(15)) {
    coap_client_config config;
    config.max_retransmit = invalid_max_retransmit;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_excessive_max_retransmit) {
    coap_client_config config;
    config.max_retransmit = 25; // Too high
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_max_sessions) {
    coap_client_config config;
    config.max_sessions = invalid_max_sessions;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_session_timeout) {
    coap_client_config config;
    config.session_timeout = invalid_session_timeout;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_block_size) {
    coap_client_config config;
    config.enable_block_transfer = true;
    config.max_block_size = invalid_block_size;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_backoff_factor_low) {
    coap_client_config config;
    config.exponential_backoff_factor = invalid_backoff_factor_low;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_invalid_backoff_factor_high) {
    coap_client_config config;
    config.exponential_backoff_factor = invalid_backoff_factor_high;
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_no_auth_method) {
    coap_client_config config;
    config.enable_dtls = true;
    // No certificate or PSK configured
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_security_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_certificate_auth) {
    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = valid_cert_file;
    config.key_file = valid_key_file;
    config.ca_file = valid_ca_file;
    
    BOOST_CHECK_NO_THROW(validate_client_config(config));
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_psk_auth) {
    coap_client_config config;
    config.enable_dtls = true;
    config.psk_identity = valid_psk_identity;
    config.psk_key = std::vector<std::byte>(16, std::byte{0x42}); // 16 bytes
    
    BOOST_CHECK_NO_THROW(validate_client_config(config));
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_both_auth_methods) {
    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = valid_cert_file;
    config.key_file = valid_key_file;
    config.psk_identity = valid_psk_identity;
    config.psk_key = std::vector<std::byte>(16, std::byte{0x42});
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_security_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_psk_key_too_short) {
    coap_client_config config;
    config.enable_dtls = true;
    config.psk_identity = valid_psk_identity;
    config.psk_key = std::vector<std::byte>(2, std::byte{0x42}); // Too short
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_security_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_psk_key_too_long) {
    coap_client_config config;
    config.enable_dtls = true;
    config.psk_identity = valid_psk_identity;
    config.psk_key = std::vector<std::byte>(100, std::byte{0x42}); // Too long
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_security_error);
}

BOOST_AUTO_TEST_CASE(test_client_config_dtls_psk_identity_too_long) {
    coap_client_config config;
    config.enable_dtls = true;
    config.psk_identity = std::string(200, 'x'); // Too long
    config.psk_key = std::vector<std::byte>(16, std::byte{0x42});
    
    BOOST_CHECK_THROW(validate_client_config(config), coap_security_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CoAPServerConfigTests)

BOOST_AUTO_TEST_CASE(test_valid_server_config) {
    coap_server_config config;
    config.max_concurrent_sessions = 200;
    config.max_request_size = 64 * 1024;
    config.session_timeout = valid_session_timeout;
    config.max_block_size = valid_block_size;
    config.exponential_backoff_factor = valid_backoff_factor;
    
    BOOST_CHECK_NO_THROW(validate_server_config(config));
}

BOOST_AUTO_TEST_CASE(test_server_config_invalid_max_sessions) {
    coap_server_config config;
    config.max_concurrent_sessions = 0;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_invalid_max_request_size_zero) {
    coap_server_config config;
    config.max_request_size = 0;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_invalid_max_request_size_too_large) {
    coap_server_config config;
    config.max_request_size = 128 * 1024 * 1024; // 128 MB, too large
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_invalid_session_timeout) {
    coap_server_config config;
    config.session_timeout = invalid_session_timeout;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_multicast_enabled_valid) {
    coap_server_config config;
    config.enable_multicast = true;
    config.multicast_address = valid_multicast_address;
    config.multicast_port = valid_multicast_port;
    
    BOOST_CHECK_NO_THROW(validate_server_config(config));
}

BOOST_AUTO_TEST_CASE(test_server_config_multicast_empty_address) {
    coap_server_config config;
    config.enable_multicast = true;
    config.multicast_address = "";
    config.multicast_port = valid_multicast_port;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_multicast_invalid_port) {
    coap_server_config config;
    config.enable_multicast = true;
    config.multicast_address = valid_multicast_address;
    config.multicast_port = invalid_multicast_port;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_multicast_invalid_address) {
    coap_server_config config;
    config.enable_multicast = true;
    config.multicast_address = "192.168.1.1"; // Not a multicast address
    config.multicast_port = valid_multicast_port;
    
    BOOST_CHECK_THROW(validate_server_config(config), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_server_config_multicast_ipv6_address) {
    coap_server_config config;
    config.enable_multicast = true;
    config.multicast_address = "ff02::1"; // IPv6 multicast
    config.multicast_port = valid_multicast_port;
    
    BOOST_CHECK_NO_THROW(validate_server_config(config));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EndpointParsingTests)

BOOST_AUTO_TEST_CASE(test_parse_coap_endpoint_basic) {
    const std::string endpoint = "coap://example.com:5683";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coap");
    BOOST_CHECK_EQUAL(parsed.host, "example.com");
    BOOST_CHECK_EQUAL(parsed.port, 5683);
    BOOST_CHECK(parsed.path.empty());
}

BOOST_AUTO_TEST_CASE(test_parse_coaps_endpoint_basic) {
    const std::string endpoint = "coaps://secure.example.com:5684";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coaps");
    BOOST_CHECK_EQUAL(parsed.host, "secure.example.com");
    BOOST_CHECK_EQUAL(parsed.port, 5684);
    BOOST_CHECK(parsed.path.empty());
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_with_path) {
    const std::string endpoint = "coap://example.com:5683/raft/request_vote";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coap");
    BOOST_CHECK_EQUAL(parsed.host, "example.com");
    BOOST_CHECK_EQUAL(parsed.port, 5683);
    BOOST_CHECK_EQUAL(parsed.path, "/raft/request_vote");
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_default_coap_port) {
    const std::string endpoint = "coap://example.com";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coap");
    BOOST_CHECK_EQUAL(parsed.host, "example.com");
    BOOST_CHECK_EQUAL(parsed.port, 5683); // Default CoAP port
    BOOST_CHECK(parsed.path.empty());
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_default_coaps_port) {
    const std::string endpoint = "coaps://example.com";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coaps");
    BOOST_CHECK_EQUAL(parsed.host, "example.com");
    BOOST_CHECK_EQUAL(parsed.port, 5684); // Default CoAPS port
    BOOST_CHECK(parsed.path.empty());
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_ipv4_address) {
    const std::string endpoint = "coap://192.168.1.100:5683";
    auto parsed = parse_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(parsed.scheme, "coap");
    BOOST_CHECK_EQUAL(parsed.host, "192.168.1.100");
    BOOST_CHECK_EQUAL(parsed.port, 5683);
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_invalid_scheme) {
    const std::string endpoint = "http://example.com:5683";
    
    BOOST_CHECK_THROW(parse_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_empty) {
    const std::string endpoint = "";
    
    BOOST_CHECK_THROW(parse_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_invalid_port) {
    const std::string endpoint = "coap://example.com:99999";
    
    BOOST_CHECK_THROW(parse_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_parse_endpoint_zero_port) {
    const std::string endpoint = "coap://example.com:0";
    
    BOOST_CHECK_THROW(parse_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_format_coap_endpoint) {
    parsed_endpoint endpoint{"coap", "example.com", 5683, "/test/path"};
    auto formatted = format_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(formatted, "coap://example.com:5683/test/path");
}

BOOST_AUTO_TEST_CASE(test_format_coaps_endpoint) {
    parsed_endpoint endpoint{"coaps", "secure.example.com", 5684};
    auto formatted = format_coap_endpoint(endpoint);
    
    BOOST_CHECK_EQUAL(formatted, "coaps://secure.example.com:5684");
}

BOOST_AUTO_TEST_CASE(test_format_endpoint_invalid_scheme) {
    parsed_endpoint endpoint{"https", "example.com", 443};
    
    BOOST_CHECK_THROW(format_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_format_endpoint_empty_host) {
    parsed_endpoint endpoint{"coap", "", 5683};
    
    BOOST_CHECK_THROW(format_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_format_endpoint_invalid_port) {
    parsed_endpoint endpoint{"coap", "example.com", 0};
    
    BOOST_CHECK_THROW(format_coap_endpoint(endpoint), coap_network_error);
}

BOOST_AUTO_TEST_CASE(test_is_valid_coap_endpoint) {
    BOOST_CHECK(is_valid_coap_endpoint("coap://example.com:5683"));
    BOOST_CHECK(is_valid_coap_endpoint("coaps://secure.example.com:5684"));
    BOOST_CHECK(is_valid_coap_endpoint("coap://192.168.1.100:5683/path"));
    
    BOOST_CHECK(!is_valid_coap_endpoint(""));
    BOOST_CHECK(!is_valid_coap_endpoint("http://example.com"));
    BOOST_CHECK(!is_valid_coap_endpoint("coap://example.com:99999"));
    BOOST_CHECK(!is_valid_coap_endpoint("invalid"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TokenGenerationTests)

BOOST_AUTO_TEST_CASE(test_generate_coap_token_default_length) {
    auto token = generate_coap_token();
    
    BOOST_CHECK_EQUAL(token.size(), 4); // Default length
    BOOST_CHECK(is_valid_coap_token(token));
}

BOOST_AUTO_TEST_CASE(test_generate_coap_token_custom_length) {
    for (std::size_t length = 1; length <= 8; ++length) {
        auto token = generate_coap_token(length);
        
        BOOST_CHECK_EQUAL(token.size(), length);
        BOOST_CHECK(is_valid_coap_token(token));
    }
}

BOOST_AUTO_TEST_CASE(test_generate_coap_token_invalid_length_zero) {
    BOOST_CHECK_THROW(generate_coap_token(0), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_generate_coap_token_invalid_length_too_large) {
    BOOST_CHECK_THROW(generate_coap_token(9), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_generate_coap_token_uniqueness) {
    // Generate multiple tokens and check they are different
    auto token1 = generate_coap_token();
    auto token2 = generate_coap_token();
    
    BOOST_CHECK(token1 != token2);
}

BOOST_AUTO_TEST_CASE(test_is_valid_coap_token) {
    // Valid tokens (0-8 bytes)
    BOOST_CHECK(is_valid_coap_token({})); // Empty token is valid
    BOOST_CHECK(is_valid_coap_token(std::vector<std::byte>(1, std::byte{0x42})));
    BOOST_CHECK(is_valid_coap_token(std::vector<std::byte>(8, std::byte{0x42})));
    
    // Invalid token (too long)
    BOOST_CHECK(!is_valid_coap_token(std::vector<std::byte>(9, std::byte{0x42})));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ContentFormatTests)

BOOST_AUTO_TEST_CASE(test_get_content_format_for_serializer) {
    BOOST_CHECK(get_content_format_for_serializer("json") == coap_content_format::application_json);
    BOOST_CHECK(get_content_format_for_serializer("JSON") == coap_content_format::application_json);
    BOOST_CHECK(get_content_format_for_serializer("json_serializer") == coap_content_format::application_json);
    
    BOOST_CHECK(get_content_format_for_serializer("cbor") == coap_content_format::application_cbor);
    BOOST_CHECK(get_content_format_for_serializer("CBOR") == coap_content_format::application_cbor);
    BOOST_CHECK(get_content_format_for_serializer("cbor_serializer") == coap_content_format::application_cbor);
    
    BOOST_CHECK(get_content_format_for_serializer("xml") == coap_content_format::application_xml);
    BOOST_CHECK(get_content_format_for_serializer("text") == coap_content_format::text_plain);
    
    // Unknown serializer defaults to CBOR
    BOOST_CHECK(get_content_format_for_serializer("unknown") == coap_content_format::application_cbor);
}

BOOST_AUTO_TEST_CASE(test_content_format_to_string) {
    BOOST_CHECK_EQUAL(content_format_to_string(coap_content_format::text_plain), "text/plain");
    BOOST_CHECK_EQUAL(content_format_to_string(coap_content_format::application_json), "application/json");
    BOOST_CHECK_EQUAL(content_format_to_string(coap_content_format::application_cbor), "application/cbor");
    BOOST_CHECK_EQUAL(content_format_to_string(coap_content_format::application_xml), "application/xml");
}

BOOST_AUTO_TEST_CASE(test_parse_content_format) {
    BOOST_CHECK(parse_content_format(0) == coap_content_format::text_plain);
    BOOST_CHECK(parse_content_format(50) == coap_content_format::application_json);
    BOOST_CHECK(parse_content_format(60) == coap_content_format::application_cbor);
    BOOST_CHECK(parse_content_format(41) == coap_content_format::application_xml);
    
    BOOST_CHECK_THROW(parse_content_format(999), coap_protocol_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(BlockOptionTests)

BOOST_AUTO_TEST_CASE(test_calculate_block_size_szx) {
    BOOST_CHECK_EQUAL(calculate_block_size_szx(16), 0);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(32), 1);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(64), 2);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(128), 3);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(256), 4);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(512), 5);
    BOOST_CHECK_EQUAL(calculate_block_size_szx(1024), 6);
}

BOOST_AUTO_TEST_CASE(test_calculate_block_size_szx_invalid) {
    BOOST_CHECK_THROW(calculate_block_size_szx(8), coap_transport_error);   // Too small
    BOOST_CHECK_THROW(calculate_block_size_szx(2048), coap_transport_error); // Too large
    BOOST_CHECK_THROW(calculate_block_size_szx(100), coap_transport_error);  // Not power of 2
}

BOOST_AUTO_TEST_CASE(test_szx_to_block_size) {
    BOOST_CHECK_EQUAL(szx_to_block_size(0), 16);
    BOOST_CHECK_EQUAL(szx_to_block_size(1), 32);
    BOOST_CHECK_EQUAL(szx_to_block_size(2), 64);
    BOOST_CHECK_EQUAL(szx_to_block_size(3), 128);
    BOOST_CHECK_EQUAL(szx_to_block_size(4), 256);
    BOOST_CHECK_EQUAL(szx_to_block_size(5), 512);
    BOOST_CHECK_EQUAL(szx_to_block_size(6), 1024);
}

BOOST_AUTO_TEST_CASE(test_szx_to_block_size_invalid) {
    BOOST_CHECK_THROW(szx_to_block_size(7), coap_transport_error);
    BOOST_CHECK_THROW(szx_to_block_size(255), coap_transport_error);
}

BOOST_AUTO_TEST_CASE(test_is_valid_block_size) {
    BOOST_CHECK(is_valid_block_size(16));
    BOOST_CHECK(is_valid_block_size(32));
    BOOST_CHECK(is_valid_block_size(64));
    BOOST_CHECK(is_valid_block_size(128));
    BOOST_CHECK(is_valid_block_size(256));
    BOOST_CHECK(is_valid_block_size(512));
    BOOST_CHECK(is_valid_block_size(1024));
    
    BOOST_CHECK(!is_valid_block_size(8));    // Too small
    BOOST_CHECK(!is_valid_block_size(2048)); // Too large
    BOOST_CHECK(!is_valid_block_size(100));  // Not power of 2
}

BOOST_AUTO_TEST_CASE(test_block_option_parse_and_encode) {
    // Test parsing and encoding of block options
    std::uint32_t option_value = (1 << 23) | (2 << 20) | 42; // More=1, SZX=2, Block=42
    
    auto parsed = block_option::parse(option_value);
    BOOST_CHECK_EQUAL(parsed.block_number, 42);
    BOOST_CHECK_EQUAL(parsed.more_blocks, true);
    BOOST_CHECK_EQUAL(parsed.block_size, 64); // SZX=2 -> 2^(2+4) = 64
    
    auto encoded = parsed.encode();
    BOOST_CHECK_EQUAL(encoded, option_value);
}

BOOST_AUTO_TEST_CASE(test_block_option_no_more_blocks) {
    std::uint32_t option_value = (0 << 23) | (3 << 20) | 10; // More=0, SZX=3, Block=10
    
    auto parsed = block_option::parse(option_value);
    BOOST_CHECK_EQUAL(parsed.block_number, 10);
    BOOST_CHECK_EQUAL(parsed.more_blocks, false);
    BOOST_CHECK_EQUAL(parsed.block_size, 128); // SZX=3 -> 2^(3+4) = 128
    
    auto encoded = parsed.encode();
    BOOST_CHECK_EQUAL(encoded, option_value);
}

BOOST_AUTO_TEST_SUITE_END()