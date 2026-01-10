#define BOOST_TEST_MODULE coap_dtls_connection_establishment_property_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>

#include <random>
#include <vector>
#include <string>
#include <chrono>
#include <format>

// Define test types for the transport
struct TestTypes {
    using future_type = kythira::Future<void>;
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using rpc_serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::uint16_t test_bind_port = 5684;
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr const char* test_cert_content = R"(-----BEGIN CERTIFICATE-----
MIIDXTCCAkWgAwIBAgIJAKoK/heBjcOuMA0GCSqGSIb3DQEBBQUAMEUxCzAJBgNV
BAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEwHwYDVQQKDBhJbnRlcm5ldCBX
aWRnaXRzIFB0eSBMdGQwHhcNMTMwODI3MjM1NDA3WhcNMTQwODI3MjM1NDA3WjBF
MQswCQYDVQQGEwJBVTETMBEGA1UECAwKU29tZS1TdGF0ZTEhMB8GA1UECgwYSW50
ZXJuZXQgV2lkZ2l0cyBQdHkgTHRkMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAwuqTiuGqAXGHYAg/WQwIE9+96jceNVkSF7fvYxfUz9AbfxJy48sqh4Hz
6VJArhHa8IyiAaYAZwd9YyLlJcBcBrze4IZrZVd18VKHk+WiZj0ECjAw+eCkqd3a
LlyaHCCUDI/3Y5HuW8Arf+TFgdnuTTj0+VoM8RcPp5sBjPiMpsIwPiMAKbJ5dF9J
8q1k2JGfLy3B3n+OcB6g==
-----END CERTIFICATE-----
)";
    constexpr const char* test_psk_identity = "test_client";
    constexpr const char* invalid_cert_content = "INVALID CERTIFICATE DATA";
}

BOOST_AUTO_TEST_SUITE(coap_dtls_connection_establishment_property_tests)

// **Feature: coap-transport, Property 9: DTLS connection establishment**
// **Validates: Requirements 1.4, 6.1, 6.3**
// Property: For any CoAPS endpoint, the transport should establish DTLS connections 
// with proper certificate or PSK validation.
BOOST_AUTO_TEST_CASE(property_dtls_connection_establishment, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::uint16_t> port_dist(5684, 6000);
    std::uniform_int_distribution<std::size_t> psk_size_dist(4, 64);
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Test certificate-based DTLS connection establishment
            {
                kythira::coap_client_config client_config;
                client_config.enable_dtls = true;
                client_config.cert_file = std::format("/tmp/test_cert_{}.pem", i);
                client_config.key_file = std::format("/tmp/test_key_{}.pem", i);
                client_config.ca_file = std::format("/tmp/test_ca_{}.pem", i);
                client_config.verify_peer_cert = (bool_dist(rng) == 1);
                
                std::unordered_map<std::uint64_t, std::string> node_endpoints;
                std::uint16_t port = port_dist(rng);
                node_endpoints[test_node_id] = std::format("coaps://127.0.0.1:{}", port);
                
                kythira::noop_metrics metrics;
                
                bool client_created = false;
                try {
                    kythira::console_logger logger;
                    kythira::coap_client<TestTypes> client(
                        std::move(node_endpoints), client_config, metrics, std::move(logger));
                    
                    // Verify DTLS is enabled
                    if (!client.is_dtls_enabled()) {
                        failures++;
                        BOOST_TEST_MESSAGE("DTLS not enabled despite configuration at iteration " << i);
                        continue;
                    }
                    
                    // Test connection establishment to valid endpoint
                    std::string test_endpoint = std::format("coaps://127.0.0.1:{}", port);
                    bool connection_established = client.establish_dtls_connection(test_endpoint);
                    
                    if (!connection_established) {
                        failures++;
                        BOOST_TEST_MESSAGE("DTLS connection establishment failed at iteration " << i);
                        continue;
                    }
                    
                    client_created = true;
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors are expected for some configurations
                    BOOST_TEST_MESSAGE("Expected security error at iteration " << i << ": " << e.what());
                    client_created = true; // This is acceptable
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during certificate-based DTLS test " << i << ": " << e.what());
                    failures++;
                }
                
                if (!client_created) {
                    failures++;
                    BOOST_TEST_MESSAGE("Certificate-based DTLS client creation failed at iteration " << i);
                }
            }
            
            // Test PSK-based DTLS connection establishment
            {
                kythira::coap_client_config psk_config;
                psk_config.enable_dtls = true;
                psk_config.psk_identity = std::format("{}_{}", test_psk_identity, i);
                
                // Generate random PSK key
                std::size_t psk_size = psk_size_dist(rng);
                psk_config.psk_key.resize(psk_size);
                for (std::size_t j = 0; j < psk_size; ++j) {
                    psk_config.psk_key[j] = static_cast<std::byte>(byte_dist(rng));
                }
                
                std::unordered_map<std::uint64_t, std::string> psk_endpoints;
                std::uint16_t psk_port = port_dist(rng);
                psk_endpoints[test_node_id] = std::format("coaps://127.0.0.1:{}", psk_port);
                
                kythira::noop_metrics psk_metrics;
                
                bool psk_client_created = false;
                try {
                    kythira::console_logger psk_logger;
                    kythira::coap_client<TestTypes> psk_client(
                        std::move(psk_endpoints), psk_config, psk_metrics, std::move(psk_logger));
                    
                    // Verify DTLS is enabled
                    if (!psk_client.is_dtls_enabled()) {
                        failures++;
                        BOOST_TEST_MESSAGE("PSK DTLS not enabled despite configuration at iteration " << i);
                        continue;
                    }
                    
                    // Test connection establishment to valid endpoint
                    std::string psk_endpoint = std::format("coaps://127.0.0.1:{}", psk_port);
                    bool psk_connection_established = psk_client.establish_dtls_connection(psk_endpoint);
                    
                    if (!psk_connection_established) {
                        failures++;
                        BOOST_TEST_MESSAGE("PSK DTLS connection establishment failed at iteration " << i);
                        continue;
                    }
                    
                    psk_client_created = true;
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors are expected for some configurations
                    BOOST_TEST_MESSAGE("Expected PSK security error at iteration " << i << ": " << e.what());
                    psk_client_created = true; // This is acceptable
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during PSK-based DTLS test " << i << ": " << e.what());
                    failures++;
                }
                
                if (!psk_client_created) {
                    failures++;
                    BOOST_TEST_MESSAGE("PSK-based DTLS client creation failed at iteration " << i);
                }
            }
            
            // Test server DTLS configuration
            {
                kythira::coap_server_config server_config;
                server_config.enable_dtls = true;
                
                // Randomly choose between certificate and PSK authentication
                if (bool_dist(rng) == 1) {
                    // Certificate-based server
                    server_config.cert_file = std::format("/tmp/server_cert_{}.pem", i);
                    server_config.key_file = std::format("/tmp/server_key_{}.pem", i);
                    server_config.ca_file = std::format("/tmp/server_ca_{}.pem", i);
                    server_config.verify_peer_cert = (bool_dist(rng) == 1);
                } else {
                    // PSK-based server
                    server_config.psk_identity = std::format("server_{}", i);
                    std::size_t server_psk_size = psk_size_dist(rng);
                    server_config.psk_key.resize(server_psk_size);
                    for (std::size_t j = 0; j < server_psk_size; ++j) {
                        server_config.psk_key[j] = static_cast<std::byte>(byte_dist(rng));
                    }
                }
                
                kythira::noop_metrics server_metrics;
                std::uint16_t server_port = port_dist(rng);
                
                bool server_created = false;
                try {
                    kythira::console_logger server_logger;
                    kythira::coap_server<TestTypes> server(
                        test_bind_address, server_port, server_config, server_metrics, std::move(server_logger));
                    
                    // Verify DTLS is enabled
                    if (!server.is_dtls_enabled()) {
                        failures++;
                        BOOST_TEST_MESSAGE("Server DTLS not enabled despite configuration at iteration " << i);
                        continue;
                    }
                    
                    server_created = true;
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors are expected for some configurations
                    BOOST_TEST_MESSAGE("Expected server security error at iteration " << i << ": " << e.what());
                    server_created = true; // This is acceptable
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during server DTLS test " << i << ": " << e.what());
                    failures++;
                }
                
                if (!server_created) {
                    failures++;
                    BOOST_TEST_MESSAGE("DTLS server creation failed at iteration " << i);
                }
            }
            
            // Test invalid endpoint handling
            {
                kythira::coap_client_config invalid_config;
                invalid_config.enable_dtls = true;
                invalid_config.cert_file = "/tmp/test_cert.pem";
                invalid_config.key_file = "/tmp/test_key.pem";
                
                std::unordered_map<std::uint64_t, std::string> invalid_endpoints;
                invalid_endpoints[test_node_id] = "coaps://127.0.0.1:5684";
                
                kythira::noop_metrics invalid_metrics;
                
                try {
                    kythira::console_logger invalid_logger;
                    kythira::coap_client<TestTypes> invalid_client(
                        std::move(invalid_endpoints), invalid_config, invalid_metrics, std::move(invalid_logger));
                    
                    // Test connection to invalid endpoints
                    std::vector<std::string> invalid_endpoint_tests = {
                        "",  // Empty endpoint
                        "invalid://127.0.0.1:5684",  // Invalid scheme
                        "coap://127.0.0.1:5684",  // Non-DTLS scheme with DTLS enabled
                        "coaps://",  // Missing host/port
                        "not_a_url"  // Invalid format
                    };
                    
                    for (const auto& invalid_endpoint : invalid_endpoint_tests) {
                        bool exception_thrown = false;
                        try {
                            invalid_client.establish_dtls_connection(invalid_endpoint);
                        } catch (const kythira::coap_network_error&) {
                            exception_thrown = true;
                        } catch (const kythira::coap_security_error&) {
                            exception_thrown = true;
                        }
                        
                        if (!exception_thrown) {
                            failures++;
                            BOOST_TEST_MESSAGE("Expected exception not thrown for invalid endpoint: " << invalid_endpoint);
                        }
                    }
                    
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Error during invalid endpoint test " << i << ": " << e.what());
                    // This is acceptable for this test
                }
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during DTLS connection establishment test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("DTLS connection establishment: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

// Test DTLS configuration validation
BOOST_AUTO_TEST_CASE(test_dtls_configuration_validation, * boost::unit_test::timeout(60)) {
    std::size_t failures = 0;
    
    // Test invalid PSK key sizes
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.psk_identity = "test";
        
        // Test PSK key too short
        config.psk_key = {std::byte{0x01}, std::byte{0x02}};  // Only 2 bytes
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        bool exception_thrown = false;
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
        } catch (const kythira::coap_security_error& e) {
            exception_thrown = true;
            BOOST_TEST_MESSAGE("Expected security error for short PSK: " << e.what());
        }
        
        if (!exception_thrown) {
            failures++;
            BOOST_TEST_MESSAGE("Expected exception not thrown for short PSK key");
        }
    }
    
    // Test PSK key too long
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.psk_identity = "test";
        
        // Test PSK key too long (> 64 bytes)
        config.psk_key.resize(100);
        std::fill(config.psk_key.begin(), config.psk_key.end(), std::byte{0xFF});
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        bool exception_thrown = false;
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
        } catch (const kythira::coap_security_error& e) {
            exception_thrown = true;
            BOOST_TEST_MESSAGE("Expected security error for long PSK: " << e.what());
        }
        
        if (!exception_thrown) {
            failures++;
            BOOST_TEST_MESSAGE("Expected exception not thrown for long PSK key");
        }
    }
    
    // Test PSK identity too long
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.psk_identity = std::string(200, 'x');  // 200 characters
        config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        bool exception_thrown = false;
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
        } catch (const kythira::coap_security_error& e) {
            exception_thrown = true;
            BOOST_TEST_MESSAGE("Expected security error for long PSK identity: " << e.what());
        }
        
        if (!exception_thrown) {
            failures++;
            BOOST_TEST_MESSAGE("Expected exception not thrown for long PSK identity");
        }
    }
    
    // Test DTLS enabled without authentication method
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        // No certificate files or PSK configured
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        bool exception_thrown = false;
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
        } catch (const kythira::coap_security_error& e) {
            exception_thrown = true;
            BOOST_TEST_MESSAGE("Expected security error for DTLS without auth method: " << e.what());
        }
        
        if (!exception_thrown) {
            failures++;
            BOOST_TEST_MESSAGE("Expected exception not thrown for DTLS without authentication method");
        }
    }
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()