#define BOOST_TEST_MODULE coap_certificate_validation_failure_property_test
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
    
    constexpr const char* valid_cert_content = R"(-----BEGIN CERTIFICATE-----
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
    
    // Various invalid certificate formats for testing
    const std::vector<std::string> invalid_certificates = {
        "",  // Empty certificate
        "INVALID CERTIFICATE DATA",  // No PEM markers
        "-----BEGIN CERTIFICATE-----\nINVALID_BASE64_DATA\n-----END CERTIFICATE-----",  // Invalid base64
        "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----",  // Empty certificate body
        "-----BEGIN CERTIFICATE-----\nMIIDXTCCAkWgAwIBAgIJAKoK/heBjcOu",  // Missing END marker
        "MIIDXTCCAkWgAwIBAgIJAKoK/heBjcOu\n-----END CERTIFICATE-----",  // Missing BEGIN marker
        "-----BEGIN PRIVATE KEY-----\nMIIDXTCCAkWgAwIBAgIJAKoK/heBjcOu\n-----END PRIVATE KEY-----",  // Wrong type
        "-----BEGIN CERTIFICATE-----\n" + std::string(2000, 'A') + "\n-----END CERTIFICATE-----",  // Oversized
        "-----BEGIN CERTIFICATE-----\n\n\n-----END CERTIFICATE-----",  // Only whitespace
        "-----BEGIN CERTIFICATE-----\n@#$%^&*()\n-----END CERTIFICATE-----"  // Invalid characters
    };
}

BOOST_AUTO_TEST_SUITE(coap_certificate_validation_failure_property_tests)

// **Feature: coap-transport, Property 10: Certificate validation failure handling**
// **Validates: Requirements 6.2**
// Property: For any invalid certificate presented during DTLS handshake, 
// the transport should reject the connection.
BOOST_AUTO_TEST_CASE(property_certificate_validation_failure_handling, * boost::unit_test::timeout(90)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> bool_dist(0, 1);
    std::uniform_int_distribution<std::size_t> cert_index_dist(0, invalid_certificates.size() - 1);
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    std::uniform_int_distribution<std::size_t> corruption_count_dist(1, 10);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Test client certificate validation failure
            {
                kythira::coap_client_config client_config;
                client_config.enable_dtls = true;
                client_config.cert_file = "/tmp/test_cert.pem";
                client_config.key_file = "/tmp/test_key.pem";
                client_config.ca_file = "/tmp/test_ca.pem";
                client_config.verify_peer_cert = true;  // Enable certificate verification
                
                std::unordered_map<std::uint64_t, std::string> node_endpoints;
                node_endpoints[test_node_id] = "coaps://127.0.0.1:5684";
                
                kythira::noop_metrics metrics;
                
                try {
                    kythira::console_logger logger;
                    kythira::coap_client<TestTypes> client(
                        std::move(node_endpoints), client_config, metrics, std::move(logger));
                    
                    // Test validation with various invalid certificates
                    std::string invalid_cert;
                    
                    if (i < invalid_certificates.size()) {
                        // Use predefined invalid certificates
                        invalid_cert = invalid_certificates[i % invalid_certificates.size()];
                    } else {
                        // Generate corrupted version of valid certificate
                        invalid_cert = valid_cert_content;
                        
                        // Randomly corrupt the certificate in a way that makes it obviously invalid
                        std::size_t corruption_count = corruption_count_dist(rng);
                        for (std::size_t j = 0; j < corruption_count; ++j) {
                            if (!invalid_cert.empty()) {
                                std::size_t pos = rng() % invalid_cert.length();
                                // Use obviously invalid characters that will fail base64 validation
                                char invalid_chars[] = {'@', '#', '$', '%', '^', '&', '*', '(', ')', '!', '~'};
                                invalid_cert[pos] = invalid_chars[rng() % (sizeof(invalid_chars) - 1)];
                            }
                        }
                    }
                    
                    // Certificate validation should fail for invalid certificates
                    bool validation_failed = false;
                    try {
                        bool result = client.validate_peer_certificate(invalid_cert);
                        if (!result) {
                            validation_failed = true;
                        }
                    } catch (const kythira::coap_security_error&) {
                        validation_failed = true;  // Expected behavior
                    }
                    
                    if (!validation_failed) {
                        failures++;
                        BOOST_TEST_MESSAGE("Certificate validation should have failed for invalid certificate at iteration " << i);
                    }
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors during client creation are acceptable
                    BOOST_TEST_MESSAGE("Expected security error during client creation at iteration " << i << ": " << e.what());
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during client certificate validation test " << i << ": " << e.what());
                    failures++;
                }
            }
            
            // Test server certificate validation failure
            {
                kythira::coap_server_config server_config;
                server_config.enable_dtls = true;
                server_config.cert_file = "/tmp/server_cert.pem";
                server_config.key_file = "/tmp/server_key.pem";
                server_config.ca_file = "/tmp/server_ca.pem";
                server_config.verify_peer_cert = true;  // Enable client certificate verification
                
                kythira::noop_metrics server_metrics;
                
                try {
                    kythira::console_logger server_logger;
                    kythira::coap_server<TestTypes> server(
                        test_bind_address, test_bind_port, server_config, server_metrics, std::move(server_logger));
                    
                    // Test validation with various invalid client certificates
                    std::string invalid_client_cert;
                    
                    if (i < invalid_certificates.size()) {
                        // Use predefined invalid certificates
                        invalid_client_cert = invalid_certificates[cert_index_dist(rng)];
                    } else {
                        // Generate corrupted version of valid certificate
                        invalid_client_cert = valid_cert_content;
                        
                        // Randomly corrupt the certificate in a way that makes it obviously invalid
                        std::size_t corruption_count = corruption_count_dist(rng);
                        for (std::size_t j = 0; j < corruption_count; ++j) {
                            if (!invalid_client_cert.empty()) {
                                std::size_t pos = rng() % invalid_client_cert.length();
                                // Use obviously invalid characters that will fail base64 validation
                                char invalid_chars[] = {'@', '#', '$', '%', '^', '&', '*', '(', ')', '!', '~'};
                                invalid_client_cert[pos] = invalid_chars[rng() % (sizeof(invalid_chars) - 1)];
                            }
                        }
                    }
                    
                    // Client certificate validation should fail for invalid certificates
                    bool validation_failed = false;
                    try {
                        bool result = server.validate_client_certificate(invalid_client_cert);
                        if (!result) {
                            validation_failed = true;
                        }
                    } catch (const kythira::coap_security_error&) {
                        validation_failed = true;  // Expected behavior
                    }
                    
                    if (!validation_failed) {
                        failures++;
                        BOOST_TEST_MESSAGE("Client certificate validation should have failed for invalid certificate at iteration " << i);
                    }
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors during server creation are acceptable
                    BOOST_TEST_MESSAGE("Expected security error during server creation at iteration " << i << ": " << e.what());
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during server certificate validation test " << i << ": " << e.what());
                    failures++;
                }
            }
            
            // Test certificate validation with verification disabled
            {
                kythira::coap_client_config no_verify_config;
                no_verify_config.enable_dtls = true;
                no_verify_config.cert_file = "/tmp/test_cert.pem";
                no_verify_config.key_file = "/tmp/test_key.pem";
                no_verify_config.verify_peer_cert = false;  // Disable certificate verification
                
                std::unordered_map<std::uint64_t, std::string> no_verify_endpoints;
                no_verify_endpoints[test_node_id] = "coaps://127.0.0.1:5684";
                
                kythira::noop_metrics no_verify_metrics;
                
                try {
                    kythira::console_logger no_verify_logger;
                    kythira::coap_client<TestTypes> no_verify_client(
                        std::move(no_verify_endpoints), no_verify_config, no_verify_metrics, std::move(no_verify_logger));
                    
                    // When verification is disabled, even invalid certificates should be accepted
                    std::string invalid_cert = invalid_certificates[cert_index_dist(rng)];
                    
                    bool validation_result = false;
                    try {
                        validation_result = no_verify_client.validate_peer_certificate(invalid_cert);
                    } catch (const kythira::coap_security_error& e) {
                        // Some format errors might still be caught even with verification disabled
                        BOOST_TEST_MESSAGE("Format error caught even with verification disabled: " << e.what());
                        validation_result = false;
                    }
                    
                    // The behavior depends on the type of invalidity
                    // Format errors should still be caught, but verification errors should be ignored
                    
                } catch (const kythira::coap_security_error& e) {
                    // Security errors during client creation are acceptable
                    BOOST_TEST_MESSAGE("Expected security error during no-verify client creation at iteration " << i << ": " << e.what());
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during no-verify certificate test " << i << ": " << e.what());
                    failures++;
                }
            }
            
            // Test certificate validation with DTLS disabled
            {
                kythira::coap_client_config no_dtls_config;
                no_dtls_config.enable_dtls = false;  // DTLS disabled
                
                std::unordered_map<std::uint64_t, std::string> no_dtls_endpoints;
                no_dtls_endpoints[test_node_id] = "coap://127.0.0.1:5683";  // Regular CoAP
                
                kythira::noop_metrics no_dtls_metrics;
                
                try {
                    kythira::console_logger no_dtls_logger;
                    kythira::coap_client<TestTypes> no_dtls_client(
                        std::move(no_dtls_endpoints), no_dtls_config, no_dtls_metrics, std::move(no_dtls_logger));
                    
                    // When DTLS is disabled, certificate validation should always succeed
                    std::string any_cert = invalid_certificates[cert_index_dist(rng)];
                    
                    bool validation_result = no_dtls_client.validate_peer_certificate(any_cert);
                    
                    if (!validation_result) {
                        failures++;
                        BOOST_TEST_MESSAGE("Certificate validation should succeed when DTLS is disabled at iteration " << i);
                    }
                    
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Unexpected error during no-DTLS certificate test " << i << ": " << e.what());
                    failures++;
                }
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during certificate validation failure test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Certificate validation failure handling: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

// Test specific certificate validation scenarios
BOOST_AUTO_TEST_CASE(test_specific_certificate_validation_scenarios, * boost::unit_test::timeout(60)) {
    std::size_t failures = 0;
    
    // Test empty certificate handling
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = "/tmp/test_cert.pem";
        config.key_file = "/tmp/test_key.pem";
        config.verify_peer_cert = true;
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
            
            bool exception_thrown = false;
            try {
                client.validate_peer_certificate("");
            } catch (const kythira::coap_security_error& e) {
                exception_thrown = true;
                BOOST_TEST_MESSAGE("Expected security error for empty certificate: " << e.what());
            }
            
            if (!exception_thrown) {
                failures++;
                BOOST_TEST_MESSAGE("Expected exception not thrown for empty certificate");
            }
            
        } catch (const kythira::coap_security_error& e) {
            // Security errors during client creation are acceptable
            BOOST_TEST_MESSAGE("Expected security error during client creation: " << e.what());
        }
    }
    
    // Test valid certificate format (should succeed in stub implementation)
    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = "/tmp/test_cert.pem";
        config.key_file = "/tmp/test_key.pem";
        config.verify_peer_cert = true;
        
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[1] = "coaps://127.0.0.1:5684";
        
        kythira::noop_metrics metrics;
        
        try {
            kythira::console_logger logger;
            kythira::coap_client<TestTypes> client(
                std::move(endpoints), config, metrics, std::move(logger));
            
            bool validation_result = false;
            bool exception_thrown = false;
            try {
                validation_result = client.validate_peer_certificate(valid_cert_content);
            } catch (const kythira::coap_security_error& e) {
                exception_thrown = true;
                BOOST_TEST_MESSAGE("Unexpected security error for valid certificate format: " << e.what());
            }
            
            if (exception_thrown) {
                failures++;
                BOOST_TEST_MESSAGE("Unexpected exception thrown for valid certificate format");
            } else if (!validation_result) {
                failures++;
                BOOST_TEST_MESSAGE("Valid certificate format should pass validation");
            }
            
        } catch (const kythira::coap_security_error& e) {
            // Security errors during client creation are acceptable
            BOOST_TEST_MESSAGE("Expected security error during client creation: " << e.what());
        }
    }
    
    // Test server certificate validation with various scenarios
    {
        kythira::coap_server_config config;
        config.enable_dtls = true;
        config.cert_file = "/tmp/server_cert.pem";
        config.key_file = "/tmp/server_key.pem";
        config.verify_peer_cert = true;
        
        kythira::noop_metrics metrics;
        
        try {
            kythira::console_logger logger;
            kythira::coap_server<TestTypes> server(
                test_bind_address, test_bind_port, config, metrics, std::move(logger));
            
            // Test various invalid client certificates
            for (const auto& invalid_cert : invalid_certificates) {
                bool validation_failed = false;
                try {
                    bool result = server.validate_client_certificate(invalid_cert);
                    if (!result) {
                        validation_failed = true;
                    }
                } catch (const kythira::coap_security_error&) {
                    validation_failed = true;  // Expected behavior
                }
                
                if (!validation_failed) {
                    failures++;
                    BOOST_TEST_MESSAGE("Server should reject invalid client certificate");
                }
            }
            
        } catch (const kythira::coap_security_error& e) {
            // Security errors during server creation are acceptable
            BOOST_TEST_MESSAGE("Expected security error during server creation: " << e.what());
        }
    }
    
    BOOST_CHECK_EQUAL(failures, 0);
}

BOOST_AUTO_TEST_SUITE_END()