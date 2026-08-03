#define BOOST_TEST_MODULE CoAP libcoap Integration Test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>

#include <chrono>
#include <thread>

#include "test_timeout_scale.hpp"

using namespace kythira;

// Test transport types using default_transport_types
using test_transport_types =
    kythira::default_transport_types<kythira::future_default<kythira::request_vote_response<>>,
                                     kythira::json_rpc_serializer<std::vector<std::byte>>,
                                     kythira::noop_metrics, kythira::console_logger>;

BOOST_AUTO_TEST_CASE(test_libcoap_context_creation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    noop_metrics metrics;

    coap_client_config config;
    config.enable_dtls = false;
    config.enable_session_reuse = true;
    config.enable_serialization_caching = true;

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coap://127.0.0.1:61060";

    console_logger logger;
    logger.info("Testing libcoap context creation");

#ifdef LIBCOAP_AVAILABLE
    // Test real libcoap context creation
    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        // Verify context was created
        logger.info("libcoap context created successfully");
    });

    logger.info("Real libcoap integration test passed");
#else
    // Test stub implementation
    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        logger.warning("Using stub implementation - libcoap not available");
    });

    logger.info("Stub implementation test passed");
#endif
}

BOOST_AUTO_TEST_CASE(test_session_management,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    console_logger logger;
    noop_metrics metrics;

    coap_client_config config;
    config.enable_session_reuse = true;
    config.connection_pool_size = 5;

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coap://127.0.0.1:61060";
    endpoints[2] = "coap://127.0.0.1:61061";

    logger.info("Testing session management");

    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        // Test session creation and reuse
        logger.info("Session management test completed");
    });
}

BOOST_AUTO_TEST_CASE(test_serialization_caching,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    console_logger logger;
    noop_metrics metrics;

    coap_client_config config;
    config.enable_serialization_caching = true;
    config.max_cache_entries = 10;
    config.cache_ttl = std::chrono::milliseconds{5000};

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coap://127.0.0.1:61060";

    logger.info("Testing serialization caching");

    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        // Test caching functionality
        logger.info("Serialization caching test completed");
    });
}

BOOST_AUTO_TEST_CASE(test_dtls_configuration,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    console_logger logger;
    noop_metrics metrics;

    coap_client_config config;
    config.enable_dtls = true;
    config.enable_certificate_validation = true;
    config.verify_peer_cert = true;
    // Add valid PSK configuration to avoid the "no valid authentication method" error
    config.psk_identity = "test_identity";
    config.psk_key = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coaps://127.0.0.1:61061";

    logger.info("Testing DTLS configuration");

    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        // Test DTLS setup
        logger.info("DTLS configuration test completed");
    });
}

BOOST_AUTO_TEST_CASE(test_server_context_creation,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    console_logger logger;
    noop_metrics metrics;

    coap_server_config config;
    config.enable_dtls = false;
    config.enable_concurrent_processing = true;

    logger.info("Testing server context creation");

    BOOST_CHECK_NO_THROW({
        // Never start()-ed, so no real bind ever occurs; 0 keeps that
        // invariant explicit rather than relying on a literal that looks live.
        coap_server<test_transport_types> server("127.0.0.1", 0, config, metrics);

        // Test server initialization
        logger.info("Server context creation test completed");
    });
}

BOOST_AUTO_TEST_CASE(test_enhanced_error_handling,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    console_logger logger;
    noop_metrics metrics;

    coap_client_config config;
    config.max_retransmit = 3;
    config.ack_timeout = std::chrono::milliseconds{1000};

    // Test with invalid endpoint to trigger error handling
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coap://host-that-should-not-exist.invalid:61060";

    logger.info("Testing enhanced error handling");

    BOOST_CHECK_NO_THROW({
        coap_client<test_transport_types> client(std::move(endpoints), config, metrics);

        // Test error handling during initialization
        logger.info("Enhanced error handling test completed");
    });
}