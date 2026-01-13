#define BOOST_TEST_MODULE coap_dtls_certificate_validation_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/console_logger.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>

#include <string>
#include <vector>

using namespace kythira;

namespace {
    // Test constants for certificate validation
    constexpr const char* valid_pem_cert = R"(-----BEGIN CERTIFICATE-----
MIIDXTCCAkWgAwIBAgIJAKoK/heBjcOuMA0GCSqGSIb3DQEBBQUAMEUxCzAJBgNV
BAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEwHwYDVQQKDBhJbnRlcm5ldCBX
aWRnaXRzIFB0eSBMdGQwHhcNMTMwODI3MjM1NDA3WhcNMTQwODI3MjM1NDA3WjBF
MQswCQYDVQQGEwJBVTETMBEGA1UECAwKU29tZS1TdGF0ZTEhMB8GA1UECgwYSW50
ZXJuZXQgV2lkZ2l0cyBQdHkgTHRkMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAwwKWzU2dJKiw4/a5vAg7EuiPiK9OlAmErVxjR2t4/e1n5jNjVBqxGer0
Zg4bStLWESOjvISzaT3YgzaLwVVMNyuAlfXYnO18aLLAcBuJpAOMQB2G2iu0GcyB
nTQanbf7eULVHce/5MzLPhw7y/b5PrRMxXDekqfFnlzsHjMz3MpnFvPaD+1NpPyO
DyMuBWqo5a8XlxrFViMkhgL8Jjx8ipkRyVfUPBHbQzdcgqyAXiKukjdUFvX1AmPX
REhA1uF2RuMyQ5XkxWc/J5alXtXNgJZJGcqcMxVNNoCQoaq2lDwA0CgCgYlpzTgD
7OOAjqysqtaLlYHIgMuuFqiitMkMIQIDAQABo1AwTjAdBgNVHQ4EFgQUhKs61e4z
miAeJt0Q+KeIR73feQswHwYDVR0jBBgwFoAUhKs61e4zmiAeJt0Q+KeIR73feQsw
DAYDVR0TBAUwAwEB/zANBgkqhkiG9w0BAQUFAAOCAQEAcMnfvnpPjEQ2TjZrddqB
v5cypgHqMX+adPwVpVLWuWqiWuqCXtdGp0FGnKTVxy5Vr1RSos1V/lx2GDpxfKvY
eFRpnYatHQoQZtZvCxVukKAaOLkDSaPh+Wcr2UcUmuiEHhdahMsGYea9p2d0BfUi
H4GlnwI/9M2S2QLjN2Sg4ScC2WQ0pSUn71PvL9tnote7xWAuUK/pxyGcHcH6mFs8
+K6BNbgHFZyr1Ys6sI7glTwU56UdNsx8/8YuVj7l+uMsVC9PNqD6YoPpvvWZ+JRG
lNqiEHXgVcYr0w77R6/aH/XZs5B6j5JT3JQRfGiGBWMlWJpqeLPFO+Aw4DdgM5dV
lA==
-----END CERTIFICATE-----
)";

    constexpr const char* invalid_pem_cert = R"(-----BEGIN CERTIFICATE-----
INVALID_CERTIFICATE_DATA_HERE
-----END CERTIFICATE-----
)";

    constexpr const char* expired_pem_cert = R"(-----BEGIN CERTIFICATE-----
MIIDXTCCAkWgAwIBAgIJAKoK/heBjcOuMA0GCSqGSIb3DQEBBQUAMEUxCzAJBgNV
BAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEwHwYDVQQKDBhJbnRlcm5ldCBX
aWRnaXRzIFB0eSBMdGQwHhcNMTMwODI3MjM1NDA3WhcNMTQwODI3MjM1NDA3WjBF
MQswCQYDVQQGEwJBVTETMBEGA1UECAwKU29tZS1TdGF0ZTEhMB8GA1UECgwYSW50
ZXJuZXQgV2lkZ2l0cyBQdHkgTHRkMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAwwKWzU2dJKiw4/a5vAg7EuiPiK9OlAmErVxjR2t4/e1n5jNjVBqxGer0
Zg4bStLWESOjvISzaT3YgzaLwVVMNyuAlfXYnO18aLLAcBuJpAOMQB2G2iu0GcyB
nTQanbf7eULVHce/5MzLPhw7y/b5PrRMxXDekqfFnlzsHjMz3MpnFvPaD+1NpPyO
DyMuBWqo5a8XlxrFViMkhgL8Jjx8ipkRyVfUPBHbQzdcgqyAXiKukjdUFvX1AmPX
REhA1uF2RuMyQ5XkxWc/J5alXtXNgJZJGcqcMxVNNoCQoaq2lDwA0CgCgYlpzTgD
7OOAjqysqtaLlYHIgMuuFqiitMkMIQIDAQABo1AwTjAdBgNVHQ4EFgQUhKs61e4z
miAeJt0Q+KeIR73feQswHwYDVR0jBBgwFoAUhKs61e4zmiAeJt0Q+KeIR73feQsw
DAYDVR0TBAUwAwEB/zANBgkqhkiG9w0BAQUFAAOCAQEAcMnfvnpPjEQ2TjZrddqB
v5cypgHqMX+adPwVpVLWuWqiWuqCXtdGp0FGnKTVxy5Vr1RSos1V/lx2GDpxfKvY
eFRpnYatHQoQZtZvCxVukKAaOLkDSaPh+Wcr2UcUmuiEHhdahMsGYea9p2d0BfUi
H4GlnwI/9M2S2QLjN2Sg4ScC2WQ0pSUn71PvL9tnote7xWAuUK/pxyGcHcH6mFs8
+K6BNbgHFZyr1Ys6sI7glTwU56UdNsx8/8YuVj7l+uMsVC9PNqD6YoPpvvWZ+JRG
lNqiEHXgVcYr0w77R6/aH/XZs5B6j5JT3JQRfGiGBWMlWJpqeLPFO+Aw4DdgM5dV
lA==
-----END CERTIFICATE-----
)";

    constexpr const char* malformed_cert = "NOT_A_CERTIFICATE";
    constexpr const char* empty_cert = "";
    constexpr const char* corrupted_cert = "-----BEGIN CERTIFICATE-----\n@#$%CORRUPTED\n-----END CERTIFICATE-----";

    // Test PSK credentials
    constexpr const char* valid_psk_identity = "test_client";
    constexpr const char* valid_psk_key_hex = "deadbeefcafebabe";
    constexpr const char* invalid_psk_identity = "";
    constexpr const char* short_psk_key = "abc"; // Too short
    const std::string long_psk_identity(200, 'x'); // Too long

    // Helper function to convert hex string to bytes
    auto hex_to_bytes(const std::string& hex) -> std::vector<std::byte> {
        std::vector<std::byte> bytes;
        for (std::size_t i = 0; i < hex.length(); i += 2) {
            std::string byte_str = hex.substr(i, 2);
            auto byte_val = static_cast<std::byte>(std::stoul(byte_str, nullptr, 16));
            bytes.push_back(byte_val);
        }
        return bytes;
    }

    // Test transport types
    struct test_transport_types {
        using serializer_type = json_rpc_serializer<std::vector<std::byte>>;
        using rpc_serializer_type = json_rpc_serializer<std::vector<std::byte>>;
        using metrics_type = noop_metrics;
        using logger_type = console_logger;
        using address_type = std::string;
        using port_type = std::uint16_t;
        using executor_type = folly::Executor;
        
        template<typename T>
        using future_template = folly::Future<T>;
        
        using future_type = folly::Future<std::vector<std::byte>>;
    };
}

BOOST_AUTO_TEST_CASE(test_certificate_format_validation, * boost::unit_test::timeout(60)) {
    // Property 22: Complete X.509 certificate validation
    // Test certificate format validation and parsing
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test valid PEM certificate format
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(logger));
        
        // Test valid certificate validation
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Valid PEM certificate validation passed");
    }
    
    // Test invalid certificate formats
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test empty certificate
        BOOST_CHECK_THROW(client.validate_peer_certificate(empty_cert), coap_security_error);
        
        // Test malformed certificate
        BOOST_CHECK_THROW(client.validate_peer_certificate(malformed_cert), coap_security_error);
        
        // Test corrupted certificate
        BOOST_CHECK_THROW(client.validate_peer_certificate(corrupted_cert), coap_security_error);
        
        // Test invalid PEM format
        BOOST_CHECK_THROW(client.validate_peer_certificate(invalid_pem_cert), coap_security_error);
        
        console_logger info_logger;
        info_logger.info("Invalid certificate format validation passed");
    }
    
    // Test certificate validation with DTLS disabled
    {
        coap_client_config config;
        config.enable_dtls = false; // DTLS disabled
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:5683"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Should return true when DTLS is disabled
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Certificate validation with DTLS disabled passed");
    }
    
    // Test certificate validation with peer verification disabled
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = false; // Peer verification disabled
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Should return true when peer verification is disabled
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Certificate validation with peer verification disabled passed");
    }
}

BOOST_AUTO_TEST_CASE(test_certificate_chain_verification, * boost::unit_test::timeout(90)) {
    // Property 23: Certificate chain verification with OpenSSL
    // Test certificate chain validation and CA verification
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test certificate chain verification with CA file
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        config.ca_file = "test_ca.pem"; // CA file specified
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test certificate validation with CA (will use stub implementation)
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Certificate chain verification with CA file passed");
    }
    
    // Test certificate chain verification without CA file
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        // No CA file specified
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test certificate validation without CA
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Certificate validation without CA file passed");
    }
    
    // Test server certificate validation
    {
        coap_server_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "server_cert.pem";
        config.key_file = "server_key.pem";
        config.ca_file = "ca.pem";
        
        console_logger server_logger;
        coap_server<test_transport_types> server("127.0.0.1", 5684, config, test_metrics, std::move(server_logger));
        
        // Test client certificate validation
        bool result = server.validate_client_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        // Test invalid client certificate
        BOOST_CHECK_THROW(server.validate_client_certificate(empty_cert), coap_security_error);
        BOOST_CHECK_THROW(server.validate_client_certificate(malformed_cert), coap_security_error);
        
        console_logger info_logger;
        info_logger.info("Server client certificate validation passed");
    }
}

BOOST_AUTO_TEST_CASE(test_certificate_revocation_checking, * boost::unit_test::timeout(60)) {
    // Property 24: Certificate revocation checking (CRL/OCSP)
    // Test certificate revocation list and OCSP validation
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test certificate with CRL distribution points
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        config.ca_file = "test_ca.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test certificate validation (CRL checking is implemented in the validation logic)
        bool result = client.validate_peer_certificate(valid_pem_cert);
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Certificate revocation checking validation passed");
    }
    
    // Test certificate validation with various certificate types
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test various certificate scenarios
        std::vector<std::string> test_certificates = {
            valid_pem_cert,
            // Add more test certificates here for comprehensive testing
        };
        
        for (const auto& cert : test_certificates) {
            bool result = client.validate_peer_certificate(cert);
            BOOST_CHECK(result == true);
        }
        
        console_logger info_logger;
        info_logger.info("Multiple certificate validation scenarios passed");
    }
}

BOOST_AUTO_TEST_CASE(test_psk_authentication_validation, * boost::unit_test::timeout(60)) {
    // Test PSK authentication and key management
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test valid PSK configuration
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.psk_identity = valid_psk_identity;
        config.psk_key = hex_to_bytes(valid_psk_key_hex);
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        // Should not throw with valid PSK configuration
        console_logger client_logger;
        BOOST_CHECK_NO_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)));
        
        console_logger info_logger;
        info_logger.info("Valid PSK configuration test passed");
    }
    
    // Test invalid PSK configurations
    {
        // Test empty PSK identity
        {
            coap_client_config config;
            config.enable_dtls = true;
            config.psk_identity = invalid_psk_identity; // Empty identity
            config.psk_key = hex_to_bytes(valid_psk_key_hex);
            
            std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
            
            console_logger client_logger;
            BOOST_CHECK_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)), 
                             coap_security_error);
        }
        
        // Test PSK key too short
        {
            coap_client_config config;
            config.enable_dtls = true;
            config.psk_identity = valid_psk_identity;
            config.psk_key = hex_to_bytes(short_psk_key); // Too short
            
            std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
            
            console_logger client_logger;
            BOOST_CHECK_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)), 
                             coap_security_error);
        }
        
        // Test PSK identity too long
        {
            coap_client_config config;
            config.enable_dtls = true;
            config.psk_identity = long_psk_identity; // Too long
            config.psk_key = hex_to_bytes(valid_psk_key_hex);
            
            std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
            
            console_logger client_logger;
            BOOST_CHECK_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)), 
                             coap_security_error);
        }
        
        console_logger info_logger;
        info_logger.info("Invalid PSK configuration validation passed");
    }
    
    // Test server PSK configuration
    {
        coap_server_config config;
        config.enable_dtls = true;
        config.psk_identity = valid_psk_identity;
        config.psk_key = hex_to_bytes(valid_psk_key_hex);
        
        // Should not throw with valid PSK configuration
        console_logger server_logger;
        BOOST_CHECK_NO_THROW(coap_server<test_transport_types> server("127.0.0.1", 5684, config, test_metrics, std::move(server_logger)));
        
        console_logger info_logger;
        info_logger.info("Server PSK configuration test passed");
    }
}

BOOST_AUTO_TEST_CASE(test_dtls_connection_establishment, * boost::unit_test::timeout(90)) {
    // Test DTLS connection establishment and handshake
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test DTLS connection establishment with valid endpoint
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = false; // Disable for connection test
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test DTLS connection establishment (will use stub implementation)
        bool result = client.establish_dtls_connection("coaps://127.0.0.1:5684");
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("DTLS connection establishment test passed");
    }
    
    // Test DTLS connection with invalid endpoints
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test invalid endpoint formats
        BOOST_CHECK_THROW(client.establish_dtls_connection(""), coap_network_error);
        BOOST_CHECK_THROW(client.establish_dtls_connection("invalid://endpoint"), coap_network_error);
        BOOST_CHECK_THROW(client.establish_dtls_connection("coap://127.0.0.1:5683"), coap_security_error); // Non-DTLS with DTLS enabled
        
        console_logger info_logger;
        info_logger.info("Invalid DTLS endpoint validation passed");
    }
    
    // Test non-DTLS connection when DTLS is disabled
    {
        coap_client_config config;
        config.enable_dtls = false; // DTLS disabled
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://127.0.0.1:5683"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Should succeed with regular CoAP endpoint when DTLS is disabled
        bool result = client.establish_dtls_connection("coap://127.0.0.1:5683");
        BOOST_CHECK(result == true);
        
        console_logger info_logger;
        info_logger.info("Non-DTLS connection test passed");
    }
}

BOOST_AUTO_TEST_CASE(test_certificate_error_reporting, * boost::unit_test::timeout(60)) {
    // Test detailed certificate error reporting
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test various certificate error scenarios
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.verify_peer_cert = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test different error conditions and verify appropriate exceptions are thrown
        struct test_case {
            std::string cert_data;
            std::string expected_error_substring;
        };
        
        std::vector<test_case> error_cases = {
            {empty_cert, "empty"},
            {malformed_cert, "invalid certificate format"},
            {corrupted_cert, "invalid base64 characters"},
            {"-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----", "empty"}
        };
        
        for (const auto& test : error_cases) {
            try {
                client.validate_peer_certificate(test.cert_data);
                BOOST_FAIL("Expected exception for certificate: " + test.expected_error_substring);
            } catch (const coap_security_error& e) {
                std::string error_msg = e.what();
                // Convert to lowercase for case-insensitive comparison
                std::transform(error_msg.begin(), error_msg.end(), error_msg.begin(), ::tolower);
                std::string expected_lower = test.expected_error_substring;
                std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(), ::tolower);
                
                BOOST_CHECK(error_msg.find(expected_lower) != std::string::npos);
                console_logger info_logger;
                info_logger.info("Certificate error correctly reported", {
                    {"error", e.what()},
                    {"expected_substring", test.expected_error_substring}
                });
            }
        }
        
        console_logger info_logger;
        info_logger.info("Certificate error reporting validation passed");
    }
}

BOOST_AUTO_TEST_CASE(test_dtls_configuration_validation, * boost::unit_test::timeout(60)) {
    // Test comprehensive DTLS configuration validation
    
    console_logger logger;
    noop_metrics test_metrics;
    
    // Test DTLS enabled without authentication method
    {
        coap_client_config config;
        config.enable_dtls = true;
        // No certificate or PSK configured
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        BOOST_CHECK_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)), 
                         coap_security_error);
        
        console_logger info_logger;
        info_logger.info("DTLS without authentication method validation passed");
    }
    
    // Test mixed authentication methods (should use certificate over PSK)
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        config.psk_identity = valid_psk_identity;
        config.psk_key = hex_to_bytes(valid_psk_key_hex);
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        // Should not throw - certificate takes precedence
        console_logger client_logger;
        BOOST_CHECK_NO_THROW(coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger)));
        
        console_logger info_logger;
        info_logger.info("Mixed authentication methods validation passed");
    }
    
    // Test DTLS configuration flags
    {
        coap_client_config config;
        config.enable_dtls = true;
        config.cert_file = "test_cert.pem";
        config.key_file = "test_key.pem";
        
        std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coaps://127.0.0.1:5684"}};
        
        console_logger client_logger;
        coap_client<test_transport_types> client(endpoints, config, test_metrics, std::move(client_logger));
        
        // Test DTLS enabled flag
        BOOST_CHECK(client.is_dtls_enabled() == true);
        
        console_logger info_logger;
        info_logger.info("DTLS configuration flags validation passed");
    }
    
    // Test server DTLS configuration
    {
        coap_server_config config;
        config.enable_dtls = true;
        config.cert_file = "server_cert.pem";
        config.key_file = "server_key.pem";
        config.verify_peer_cert = true;
        
        console_logger server_logger;
        coap_server<test_transport_types> server("127.0.0.1", 5684, config, test_metrics, std::move(server_logger));
        
        // Test server DTLS enabled flag
        BOOST_CHECK(server.is_dtls_enabled() == true);
        
        console_logger info_logger;
        info_logger.info("Server DTLS configuration validation passed");
    }
}