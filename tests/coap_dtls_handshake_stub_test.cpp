#define BOOST_TEST_MODULE coap_dtls_handshake_stub_test
#include <boost/test/unit_test.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>

using namespace kythira;

// Use the correct transport types for testing
using test_transport_types = kythira::default_transport_types<
    kythira::Future<kythira::request_vote_response<>>,
    kythira::json_rpc_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    kythira::console_logger
>;

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 19683;
    constexpr const char* test_endpoint = "coaps://127.0.0.1:5684";
}

/**
 * Test DTLS handshake stub methods for client
 * 
 * Validates: Requirements 6.1, 6.3, 6.4, 11.4
 */
BOOST_AUTO_TEST_CASE(test_client_dtls_handshake_stubs, * boost::unit_test::timeout(30)) {
    // Create client configuration with DTLS enabled
    coap_client_config client_config;
    client_config.enable_dtls = true;
    client_config.psk_identity = "test_client";
    client_config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    
    // Create test types and client
    test_transport_types::metrics_type metrics;
    
    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {1, test_endpoint}
    };
    
    // Create client
    coap_client<test_transport_types> client(
        node_endpoints,
        client_config,
        metrics
    );
    
    // Test handshake initiation
    bool initiate_result = client.initiate_dtls_handshake(test_endpoint);
    BOOST_CHECK(initiate_result == true);
    
    // Test handshake completion
    bool complete_result = client.complete_dtls_handshake(test_endpoint);
    BOOST_CHECK(complete_result == true);
}

/**
 * Test DTLS handshake stub methods for client without DTLS
 * 
 * Validates: Requirements 6.1, 6.3, 6.4, 11.4
 */
BOOST_AUTO_TEST_CASE(test_client_dtls_handshake_stubs_disabled, * boost::unit_test::timeout(30)) {
    // Create client configuration with DTLS disabled
    coap_client_config client_config;
    client_config.enable_dtls = false;
    
    // Create test types and client
    test_transport_types::metrics_type metrics;
    
    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {1, test_endpoint}
    };
    
    // Create client
    coap_client<test_transport_types> client(
        node_endpoints,
        client_config,
        metrics
    );
    
    // Test handshake initiation should return false when DTLS is disabled
    bool initiate_result = client.initiate_dtls_handshake(test_endpoint);
    BOOST_CHECK(initiate_result == false);
    
    // Test handshake completion should return false when DTLS is disabled
    bool complete_result = client.complete_dtls_handshake(test_endpoint);
    BOOST_CHECK(complete_result == false);
}

/**
 * Test DTLS handshake stub methods for server
 * 
 * Validates: Requirements 6.1, 6.3, 6.4, 11.4
 */
BOOST_AUTO_TEST_CASE(test_server_dtls_handshake_stubs, * boost::unit_test::timeout(30)) {
    // Create server configuration with DTLS enabled
    coap_server_config server_config;
    server_config.enable_dtls = true;
    server_config.psk_identity = "test_server";
    server_config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    
    // Create test types and server
    test_transport_types::metrics_type metrics;
    
    // Create server
    coap_server<test_transport_types> server(
        test_bind_address,
        test_bind_port,
        server_config,
        metrics
    );
    
    // Test handshake initiation with null session (stub should handle gracefully)
    bool initiate_result = server.initiate_dtls_handshake(nullptr);
    
#ifdef LIBCOAP_AVAILABLE
    // With libcoap, null session should return false
    BOOST_CHECK(initiate_result == false);
#else
    // Without libcoap (stub), should return true
    BOOST_CHECK(initiate_result == true);
#endif
    
    // Test handshake completion with null session (stub should handle gracefully)
    bool complete_result = server.complete_dtls_handshake(nullptr);
    
#ifdef LIBCOAP_AVAILABLE
    // With libcoap, null session should return false
    BOOST_CHECK(complete_result == false);
#else
    // Without libcoap (stub), should return true
    BOOST_CHECK(complete_result == true);
#endif
}

/**
 * Test DTLS handshake stub methods for server without DTLS
 * 
 * Validates: Requirements 6.1, 6.3, 6.4, 11.4
 */
BOOST_AUTO_TEST_CASE(test_server_dtls_handshake_stubs_disabled, * boost::unit_test::timeout(30)) {
    // Create server configuration with DTLS disabled
    coap_server_config server_config;
    server_config.enable_dtls = false;
    
    // Create test types and server
    test_transport_types::metrics_type metrics;
    
    // Create server
    coap_server<test_transport_types> server(
        test_bind_address,
        test_bind_port + 1,
        server_config,
        metrics
    );
    
    // Test handshake initiation should return false when DTLS is disabled
    bool initiate_result = server.initiate_dtls_handshake(nullptr);
    BOOST_CHECK(initiate_result == false);
    
    // Test handshake completion should return false when DTLS is disabled
    bool complete_result = server.complete_dtls_handshake(nullptr);
    BOOST_CHECK(complete_result == false);
}
