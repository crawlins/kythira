#define BOOST_TEST_MODULE coap_multicast_address_validation_test
#include <boost/test/unit_test.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/types.hpp>
#include <raft/metrics.hpp>

// Use the correct transport types for testing
using test_transport_types = kythira::default_transport_types<
    kythira::Future<kythira::request_vote_response<>>,
    kythira::json_rpc_serializer<std::vector<std::byte>>,
    kythira::noop_metrics,
    kythira::console_logger
>;

// Test fixture for CoAP multicast address validation
struct coap_multicast_validation_fixture {
    using client_type = kythira::coap_client<test_transport_types>;
    using server_type = kythira::coap_server<test_transport_types>;
    
    kythira::coap_client_config client_config;
    kythira::coap_server_config server_config;
    
    coap_multicast_validation_fixture() {
        // Configure client
        client_config.enable_dtls = false;
        client_config.max_sessions = 10;
        
        // Configure server
        server_config.enable_dtls = false;
        server_config.enable_multicast = true;
        server_config.multicast_address = "224.0.1.187";
        server_config.multicast_port = 5683;
    }
};

BOOST_FIXTURE_TEST_SUITE(coap_multicast_address_validation_suite, coap_multicast_validation_fixture, * boost::unit_test::timeout(30))

// Test valid multicast addresses
BOOST_AUTO_TEST_CASE(test_valid_multicast_addresses, * boost::unit_test::timeout(15)) {
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://localhost:5683"}};
    client_type client(endpoints, client_config, test_transport_types::metrics_type{});
    
    // Test valid multicast addresses in the 224.0.0.0/4 range
    BOOST_CHECK(client.is_valid_multicast_address("224.0.0.0"));
    BOOST_CHECK(client.is_valid_multicast_address("224.0.1.187"));
    BOOST_CHECK(client.is_valid_multicast_address("224.255.255.255"));
    BOOST_CHECK(client.is_valid_multicast_address("225.0.0.1"));
    BOOST_CHECK(client.is_valid_multicast_address("230.1.2.3"));
    BOOST_CHECK(client.is_valid_multicast_address("235.100.200.50"));
    BOOST_CHECK(client.is_valid_multicast_address("239.255.255.255"));
}

// Test invalid multicast addresses
BOOST_AUTO_TEST_CASE(test_invalid_multicast_addresses, * boost::unit_test::timeout(15)) {
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://localhost:5683"}};
    client_type client(endpoints, client_config, test_transport_types::metrics_type{});
    
    // Test invalid addresses
    BOOST_CHECK(!client.is_valid_multicast_address(""));  // Empty string
    BOOST_CHECK(!client.is_valid_multicast_address("192.168.1.1"));  // Unicast address
    BOOST_CHECK(!client.is_valid_multicast_address("10.0.0.1"));  // Private address
    BOOST_CHECK(!client.is_valid_multicast_address("223.255.255.255"));  // Just below multicast range
    BOOST_CHECK(!client.is_valid_multicast_address("240.0.0.0"));  // Just above multicast range
    BOOST_CHECK(!client.is_valid_multicast_address("255.255.255.255"));  // Broadcast address
    BOOST_CHECK(!client.is_valid_multicast_address("224"));  // Incomplete address
    BOOST_CHECK(!client.is_valid_multicast_address("224.0"));  // Incomplete address
    BOOST_CHECK(!client.is_valid_multicast_address("224.0.0"));  // Incomplete address
    BOOST_CHECK(!client.is_valid_multicast_address("invalid"));  // Not an IP address
    BOOST_CHECK(!client.is_valid_multicast_address("224.0.0.0.0"));  // Too many octets
}

// Test edge cases
BOOST_AUTO_TEST_CASE(test_multicast_address_edge_cases, * boost::unit_test::timeout(15)) {
    std::unordered_map<std::uint64_t, std::string> endpoints = {{1, "coap://localhost:5683"}};
    client_type client(endpoints, client_config, test_transport_types::metrics_type{});
    
    // Test boundary values
    BOOST_CHECK(client.is_valid_multicast_address("224.0.0.0"));  // Lower bound
    BOOST_CHECK(client.is_valid_multicast_address("239.255.255.255"));  // Upper bound
    
    // Test just outside boundaries
    BOOST_CHECK(!client.is_valid_multicast_address("223.255.255.255"));  // Just below
    BOOST_CHECK(!client.is_valid_multicast_address("240.0.0.0"));  // Just above
    
    // Test malformed addresses
    BOOST_CHECK(!client.is_valid_multicast_address("224.0.0."));  // Trailing dot
    BOOST_CHECK(!client.is_valid_multicast_address(".224.0.0.0"));  // Leading dot
    BOOST_CHECK(!client.is_valid_multicast_address("224..0.0.0"));  // Double dot
    BOOST_CHECK(!client.is_valid_multicast_address("224.0.0.0 "));  // Trailing space
    BOOST_CHECK(!client.is_valid_multicast_address(" 224.0.0.0"));  // Leading space
}

// Test server multicast address validation
BOOST_AUTO_TEST_CASE(test_server_multicast_validation, * boost::unit_test::timeout(15)) {
    kythira::coap_server_config config;
    config.enable_dtls = false;
    config.enable_multicast = true;
    config.multicast_address = "224.0.1.187";
    config.multicast_port = 5683;
    
    server_type server("0.0.0.0", 5683, config, test_transport_types::metrics_type{});
    
    // Test valid multicast addresses
    BOOST_CHECK(server.is_valid_multicast_address("224.0.1.187"));
    BOOST_CHECK(server.is_valid_multicast_address("239.255.255.255"));
    
    // Test invalid addresses
    BOOST_CHECK(!server.is_valid_multicast_address("192.168.1.1"));
    BOOST_CHECK(!server.is_valid_multicast_address(""));
    BOOST_CHECK(!server.is_valid_multicast_address("invalid"));
}

BOOST_AUTO_TEST_SUITE_END()
