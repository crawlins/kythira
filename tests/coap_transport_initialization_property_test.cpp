#define BOOST_TEST_MODULE coap_transport_initialization_property_test
#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>

// Use the correct serializer type alias
using test_serializer = raft::json_rpc_serializer<std::vector<std::byte>>;

#include <unordered_map>
#include <string>
#include <chrono>

namespace {
    constexpr const char* test_coap_endpoint = "coap://127.0.0.1:5683";
    constexpr const char* test_coaps_endpoint = "coaps://127.0.0.1:5684";
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::uint16_t test_bind_port = 5683;
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::size_t property_test_iterations = 10;
}

BOOST_AUTO_TEST_SUITE(coap_transport_initialization_property_tests)

// **Feature: coap-transport, Property 1: Transport initialization creates required components**
// **Validates: Requirements 1.1**
// Property: For any valid configuration, initializing the CoAP transport should create
// both client and server components with the specified configuration parameters.
BOOST_AUTO_TEST_CASE(property_transport_initialization_creates_components, * boost::unit_test::timeout(60)) {
    // Test multiple configurations to verify initialization robustness
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        try {
            // Test CoAP client initialization
            {
                raft::coap_client_config client_config;
                client_config.ack_timeout = std::chrono::milliseconds{2000 + i * 100};
                client_config.max_retransmit = 4 + (i % 3);
                client_config.max_block_size = 1024 + (i * 256);
                client_config.enable_dtls = (i % 2 == 0);
                client_config.max_sessions = 100 + (i * 10);
                
                // Configure PSK when DTLS is enabled
                if (client_config.enable_dtls) {
                    client_config.psk_identity = "test_client";
                    client_config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
                }
                
                raft::noop_metrics metrics;
                
                std::unordered_map<std::uint64_t, std::string> node_endpoints;
                node_endpoints[test_node_id + i] = std::format("coap://127.0.0.1:{}", 5683 + i);
                
                // Verify client can be constructed with valid configuration
                // Note: This test verifies the interface exists and can be instantiated
                // The actual CoAP functionality will be tested in implementation-specific tests
                bool client_created = false;
                try {
                    raft::console_logger logger;
                    raft::coap_client<test_serializer, raft::noop_metrics, raft::console_logger> client(
                        std::move(node_endpoints), client_config, metrics, std::move(logger));
                    client_created = true;
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Client creation failed: " << e.what());
                }
                BOOST_REQUIRE(client_created);
                
                BOOST_TEST_MESSAGE("CoAP client initialization test " << i << " passed");
            }
            
            // Test CoAP server initialization
            {
                raft::coap_server_config server_config;
                server_config.max_concurrent_sessions = 200 + (i * 20);
                server_config.max_request_size = (64 + i) * 1024;
                server_config.enable_dtls = (i % 2 == 1);
                server_config.max_block_size = 1024 + (i * 128);
                server_config.enable_multicast = (i % 3 == 0);
                
                // Configure PSK when DTLS is enabled
                if (server_config.enable_dtls) {
                    server_config.psk_identity = "test_server";
                    server_config.psk_key = {std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
                }
                
                raft::noop_metrics metrics;
                
                std::uint16_t port = test_bind_port + static_cast<std::uint16_t>(i);
                
                // Verify server can be constructed with valid configuration
                bool server_created = false;
                try {
                    raft::console_logger logger;
                    raft::coap_server<test_serializer, raft::noop_metrics, raft::console_logger> server(
                        test_bind_address, port, server_config, metrics, std::move(logger));
                    server_created = true;
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Server creation failed: " << e.what());
                }
                BOOST_REQUIRE(server_created);
                
                BOOST_TEST_MESSAGE("CoAP server initialization test " << i << " passed");
            }
            
            // Test configuration validation - verify different parameter combinations work
            {
                raft::coap_client_config config;
                
                // Test timeout configurations
                config.ack_timeout = std::chrono::milliseconds{1000 + i * 500};
                config.ack_random_factor_ms = std::chrono::milliseconds{500 + i * 100};
                
                // Test retransmission parameters
                config.max_retransmit = 1 + (i % 8); // Valid range: 1-8
                
                // Test block transfer settings
                config.enable_block_transfer = (i % 2 == 0);
                config.max_block_size = 256 << (i % 4); // Powers of 2: 256, 512, 1024, 2048
                
                // Test session management
                config.max_sessions = 10 + (i * 5);
                config.session_timeout = std::chrono::seconds{60 + i * 30};
                
                // Verify configuration is accepted
                std::unordered_map<std::uint64_t, std::string> endpoints;
                endpoints[1] = test_coap_endpoint;
                
                raft::noop_metrics metrics;
                
                bool config_client_created = false;
                try {
                    raft::console_logger logger;
                    raft::coap_client<test_serializer, raft::noop_metrics, raft::console_logger> client(
                        std::move(endpoints), config, metrics, std::move(logger));
                    config_client_created = true;
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Config client creation failed: " << e.what());
                }
                BOOST_REQUIRE(config_client_created);
                
                BOOST_TEST_MESSAGE("Configuration validation test " << i << " passed");
            }
            
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Exception during initialization test " << i << ": " << e.what());
            BOOST_TEST(false, "Transport initialization property test failed");
        }
    }
    
    // Test DTLS configuration variations
    for (std::size_t i = 0; i < 3; ++i) {
        try {
            raft::coap_client_config dtls_config;
            dtls_config.enable_dtls = true;
            
            switch (i) {
                case 0:
                    // Certificate-based authentication
                    dtls_config.cert_file = "/path/to/cert.pem";
                    dtls_config.key_file = "/path/to/key.pem";
                    dtls_config.ca_file = "/path/to/ca.pem";
                    dtls_config.verify_peer_cert = true;
                    break;
                case 1:
                    // PSK-based authentication
                    dtls_config.psk_identity = "test_identity";
                    dtls_config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
                    break;
                case 2:
                    // Mixed configuration (should still be valid for construction)
                    dtls_config.cert_file = "/path/to/cert.pem";
                    dtls_config.key_file = "/path/to/key.pem";
                    dtls_config.psk_identity = "backup_identity";
                    dtls_config.psk_key = {std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
                    dtls_config.verify_peer_cert = false;
                    break;
            }
            
            std::unordered_map<std::uint64_t, std::string> endpoints;
            endpoints[1] = test_coaps_endpoint;
            
            raft::noop_metrics metrics;
            
            bool dtls_client_created = false;
            try {
                raft::console_logger logger;
                raft::coap_client<test_serializer, raft::noop_metrics, raft::console_logger> client(
                    std::move(endpoints), dtls_config, metrics, std::move(logger));
                dtls_client_created = true;
            } catch (const std::exception& e) {
                BOOST_TEST_MESSAGE("DTLS client creation failed: " << e.what());
            }
            BOOST_REQUIRE(dtls_client_created);
            
            BOOST_TEST_MESSAGE("DTLS configuration test " << i << " passed");
            
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Exception during DTLS configuration test " << i << ": " << e.what());
            BOOST_TEST(false, "DTLS configuration test failed");
        }
    }
    
    // Test multicast configuration
    try {
        raft::coap_server_config multicast_config;
        multicast_config.enable_multicast = true;
        multicast_config.multicast_address = "224.0.1.187";
        multicast_config.multicast_port = 5683;
        
        raft::noop_metrics metrics;
        
        bool multicast_server_created = false;
        try {
            raft::console_logger logger;
            raft::coap_server<test_serializer, raft::noop_metrics, raft::console_logger> server(
                test_bind_address, test_bind_port, multicast_config, metrics, std::move(logger));
            multicast_server_created = true;
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Multicast server creation failed: " << e.what());
        }
        BOOST_REQUIRE(multicast_server_created);
        
        BOOST_TEST_MESSAGE("Multicast configuration test passed");
        
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception during multicast configuration test: " << e.what());
        BOOST_TEST(false, "Multicast configuration test failed");
    }
    
    BOOST_TEST_MESSAGE("All transport initialization property tests completed successfully");
}

// Test that the CoAP transport classes satisfy the required concepts
BOOST_AUTO_TEST_CASE(test_concept_satisfaction, * boost::unit_test::timeout(15)) {
    // Verify that coap_client satisfies network_client concept
    using future_type = kythira::Future<raft::request_vote_response<>>;
    static_assert(kythira::network_client<kythira::coap_client<future_type, test_serializer, raft::noop_metrics, raft::console_logger>, future_type>);
    
    // Verify that coap_server satisfies network_server concept
    static_assert(kythira::network_server<kythira::coap_server<future_type, test_serializer, raft::noop_metrics, raft::console_logger>>);
    
    // Verify that json_serializer satisfies rpc_serializer concept
    static_assert(raft::rpc_serializer<test_serializer, std::vector<std::byte>>);
    
    // Verify that noop_metrics satisfies metrics concept
    static_assert(raft::metrics<raft::noop_metrics>);
    
    BOOST_TEST(true); // Test passes if static_asserts compile
    BOOST_TEST_MESSAGE("All concept satisfaction tests passed");
}

// Test exception types are properly defined
BOOST_AUTO_TEST_CASE(test_exception_types, * boost::unit_test::timeout(15)) {
    // Test that all CoAP exception types can be constructed and thrown
    try {
        throw raft::coap_transport_error("Base transport error");
    } catch (const raft::coap_transport_error& e) {
        BOOST_TEST(std::string(e.what()) == "Base transport error");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_client_error(0x80, "Client error"); // 4.00 Bad Request
    } catch (const raft::coap_client_error& e) {
        BOOST_TEST(e.response_code() == 0x80);
        BOOST_TEST(std::string(e.what()) == "Client error");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_server_error(0xA0, "Server error"); // 5.00 Internal Server Error
    } catch (const raft::coap_server_error& e) {
        BOOST_TEST(e.response_code() == 0xA0);
        BOOST_TEST(std::string(e.what()) == "Server error");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_timeout_error("Timeout occurred");
    } catch (const raft::coap_timeout_error& e) {
        BOOST_TEST(std::string(e.what()) == "Timeout occurred");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_security_error("DTLS handshake failed");
    } catch (const raft::coap_security_error& e) {
        BOOST_TEST(std::string(e.what()) == "DTLS handshake failed");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_protocol_error("Invalid CoAP message");
    } catch (const raft::coap_protocol_error& e) {
        BOOST_TEST(std::string(e.what()) == "Invalid CoAP message");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    try {
        throw raft::coap_network_error("Network unreachable");
    } catch (const raft::coap_network_error& e) {
        BOOST_TEST(std::string(e.what()) == "Network unreachable");
    } catch (...) {
        BOOST_TEST(false, "Wrong exception type caught");
    }
    
    BOOST_TEST_MESSAGE("All exception type tests passed");
}

BOOST_AUTO_TEST_SUITE_END()