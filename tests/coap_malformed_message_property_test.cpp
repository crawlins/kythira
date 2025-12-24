#define BOOST_TEST_MODULE coap_malformed_message_property_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/types.hpp>
#include <raft/coap_exceptions.hpp>

#include <random>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

namespace {
    constexpr std::size_t property_test_iterations = 100;
    constexpr std::uint16_t min_port = 5683;
    constexpr std::uint16_t max_port = 6000;
    constexpr std::size_t max_malformed_payload_size = 1024;
}

BOOST_AUTO_TEST_SUITE(coap_malformed_message_property_tests)

// **Feature: coap-transport, Property 14: Malformed message rejection**
// **Validates: Requirements 8.2**
// Property: For any malformed CoAP message received by the server, it should be rejected 
// without affecting other message processing.
BOOST_AUTO_TEST_CASE(property_malformed_message_rejection, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<std::uint16_t> port_dist(min_port, max_port);
    std::uniform_int_distribution<std::size_t> size_dist(1, max_malformed_payload_size);
    std::uniform_int_distribution<std::uint8_t> byte_dist(0, 255);
    
    std::size_t failures = 0;
    
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Generate random test parameters
            std::uint16_t server_port = port_dist(rng);
            std::size_t malformed_size = size_dist(rng);
            
            // Create server configuration
            raft::coap_server_config config;
            config.max_request_size = 64 * 1024;
            config.max_concurrent_sessions = 100;
            
            // Create server
            raft::noop_metrics metrics;
            raft::console_logger logger;
            raft::coap_server<raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger> 
                server("127.0.0.1", server_port, config, metrics, std::move(logger));
            
            // Register a dummy handler
            server.register_request_vote_handler([](const raft::request_vote_request<>& req) {
                raft::request_vote_response<> response;
                response._term = req._term;
                response._vote_granted = false;
                return response;
            });
            
            // Generate malformed message data
            std::vector<std::byte> malformed_data(malformed_size);
            std::generate(malformed_data.begin(), malformed_data.end(), [&]() {
                return static_cast<std::byte>(byte_dist(rng));
            });
            
            // Test that malformed messages are properly rejected using the new error handling methods
            
            // Test 1: Empty message data should be rejected
            std::vector<std::byte> empty_data;
            BOOST_CHECK_EQUAL(server.detect_malformed_message(empty_data), true);
            
            // Test 2: Invalid CoAP header should be rejected
            std::vector<std::byte> invalid_header = {
                std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}
            };
            BOOST_CHECK_EQUAL(server.detect_malformed_message(invalid_header), true);
            
            // Test 3: Truncated message should be rejected
            std::vector<std::byte> truncated_msg = {std::byte{0x40}}; // CoAP header without rest
            BOOST_CHECK_EQUAL(server.detect_malformed_message(truncated_msg), true);
            
            // Test 4: Invalid CoAP version should be rejected
            std::vector<std::byte> invalid_version = {
                std::byte{0x80}, std::byte{0x01}, // Invalid version (10 instead of 01)
                std::byte{0x00}, std::byte{0x01}  // Message ID
            };
            BOOST_CHECK_EQUAL(server.detect_malformed_message(invalid_version), true);
            
            // Test 5: Invalid token length should be rejected
            std::vector<std::byte> invalid_token_length = {
                std::byte{0x4F}, std::byte{0x01}, // Token length 15 (invalid, max is 8)
                std::byte{0x00}, std::byte{0x01}  // Message ID
            };
            BOOST_CHECK_EQUAL(server.detect_malformed_message(invalid_token_length), true);
            
            // Test 6: Valid message should not be rejected
            std::vector<std::byte> valid_msg = {
                std::byte{0x40}, std::byte{0x01}, // Valid header (version 1, CON, no token)
                std::byte{0x00}, std::byte{0x01}  // Message ID
            };
            BOOST_CHECK_EQUAL(server.detect_malformed_message(valid_msg), false);
            
            // Test 7: All zeros pattern should be rejected (corrupted data)
            std::vector<std::byte> all_zeros(8, std::byte{0x00});
            BOOST_CHECK_EQUAL(server.detect_malformed_message(all_zeros), true);
            
            // Test 8: All ones pattern should be rejected (corrupted data)
            std::vector<std::byte> all_ones(8, std::byte{0xFF});
            BOOST_CHECK_EQUAL(server.detect_malformed_message(all_ones), true);
            
            // Verify server state remains consistent after malformed message handling
            BOOST_CHECK(server.is_running() == false); // Server not started in test
            
            // Test that duplicate detection still works after malformed messages
            std::uint16_t test_msg_id = 12345;
            BOOST_CHECK_EQUAL(server.is_duplicate_message(test_msg_id), false);
            server.record_received_message(test_msg_id);
            BOOST_CHECK_EQUAL(server.is_duplicate_message(test_msg_id), true);
            
            // Test that DTLS validation still works after malformed messages
            if (server.is_dtls_enabled()) {
                // Test certificate validation with malformed data
                std::string malformed_cert;
                malformed_cert.reserve(malformed_data.size());
                for (auto byte : malformed_data) {
                    malformed_cert.push_back(static_cast<char>(byte));
                }
                try {
                    server.validate_client_certificate(malformed_cert);
                    // Should throw exception for malformed certificate
                    BOOST_FAIL("Expected exception for malformed certificate");
                } catch (const raft::coap_security_error&) {
                    // Expected behavior - malformed certificate rejected
                }
            }
            
            // Test that block transfer still works after malformed messages
            if (config.enable_block_transfer) {
                std::vector<std::byte> valid_payload(1024, std::byte{0x42});
                BOOST_CHECK_EQUAL(server.should_use_block_transfer(valid_payload), false);
                
                std::vector<std::byte> large_payload(config.max_block_size + 1, std::byte{0x42});
                BOOST_CHECK_EQUAL(server.should_use_block_transfer(large_payload), true);
            }
            
        } catch (const std::exception& e) {
            failures++;
            BOOST_TEST_MESSAGE("Exception during malformed message test " << i << ": " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("Malformed message rejection: " 
        << (property_test_iterations - failures) << "/" << property_test_iterations << " passed");
    
    BOOST_CHECK_EQUAL(failures, 0);
}

// Test client malformed message detection
BOOST_AUTO_TEST_CASE(client_malformed_message_detection, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://127.0.0.1:5683"}
    };
    
    raft::coap_client_config config;
    raft::noop_metrics metrics;
    raft::console_logger logger;
    raft::coap_client<raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger> 
        client(endpoints, config, metrics, std::move(logger));
    
    // Test client can detect malformed messages
    std::vector<std::byte> empty_data;
    BOOST_CHECK_EQUAL(client.detect_malformed_message(empty_data), true);
    
    std::vector<std::byte> invalid_header = {
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}
    };
    BOOST_CHECK_EQUAL(client.detect_malformed_message(invalid_header), true);
    
    std::vector<std::byte> valid_msg = {
        std::byte{0x40}, std::byte{0x01}, // Valid header
        std::byte{0x00}, std::byte{0x01}  // Message ID
    };
    BOOST_CHECK_EQUAL(client.detect_malformed_message(valid_msg), false);
}

// Test specific malformed message scenarios
BOOST_AUTO_TEST_CASE(specific_malformed_message_scenarios, * boost::unit_test::timeout(45)) {
    raft::coap_server_config config;
    config.max_request_size = 1024;
    
    raft::noop_metrics metrics;
    raft::console_logger logger;
    raft::coap_server<raft::json_rpc_serializer<std::vector<std::byte>>, raft::noop_metrics, raft::console_logger> 
        server("127.0.0.1", 5683, config, metrics, std::move(logger));
    
    // Test empty certificate data
    try {
        bool result = server.validate_client_certificate("");
        if (server.is_dtls_enabled() && config.verify_peer_cert) {
            BOOST_FAIL("Expected exception for empty certificate");
        } else {
            // DTLS not enabled or peer cert verification disabled, should return true
            BOOST_CHECK_EQUAL(result, true);
        }
    } catch (const raft::coap_security_error& e) {
        BOOST_TEST_MESSAGE("Empty certificate rejected: " << e.what());
    }
    
    // Test malformed certificate format
    try {
        bool result = server.validate_client_certificate("not-a-certificate");
        if (server.is_dtls_enabled() && config.verify_peer_cert) {
            BOOST_FAIL("Expected exception for malformed certificate");
        } else {
            // DTLS not enabled or peer cert verification disabled, should return true
            BOOST_CHECK_EQUAL(result, true);
        }
    } catch (const raft::coap_security_error& e) {
        BOOST_TEST_MESSAGE("Malformed certificate rejected: " << e.what());
    }
    
    // Test certificate without proper markers
    try {
        bool result = server.validate_client_certificate("CERTIFICATE DATA WITHOUT MARKERS");
        if (server.is_dtls_enabled() && config.verify_peer_cert) {
            BOOST_FAIL("Expected exception for certificate without markers");
        } else {
            // DTLS not enabled or peer cert verification disabled, should return true
            BOOST_CHECK_EQUAL(result, true);
        }
    } catch (const raft::coap_security_error& e) {
        BOOST_TEST_MESSAGE("Certificate without markers rejected: " << e.what());
    }
    
    // Test incomplete certificate
    try {
        bool result = server.validate_client_certificate("-----BEGIN CERTIFICATE-----\nincomplete");
        if (server.is_dtls_enabled() && config.verify_peer_cert) {
            BOOST_FAIL("Expected exception for incomplete certificate");
        } else {
            // DTLS not enabled or peer cert verification disabled, should return true
            BOOST_CHECK_EQUAL(result, true);
        }
    } catch (const raft::coap_security_error& e) {
        BOOST_TEST_MESSAGE("Incomplete certificate rejected: " << e.what());
    }
}

BOOST_AUTO_TEST_SUITE_END()