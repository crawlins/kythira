#define BOOST_TEST_MODULE http_client_comprehensive_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
    constexpr const char* test_http_url = "http://localhost:8080";
    constexpr const char* test_https_url = "https://localhost:8443";
    constexpr std::uint64_t test_node_id_1 = 1;
    constexpr std::uint64_t test_node_id_2 = 2;
    constexpr std::uint64_t test_node_id_3 = 3;
    
    // Define transport types for testing
    using test_transport_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
}

BOOST_AUTO_TEST_SUITE(http_client_comprehensive_tests)

// Test client construction with multiple nodes
BOOST_AUTO_TEST_CASE(test_multi_node_construction, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id_1] = test_http_url;
    // Skip HTTPS URLs when OpenSSL is not available
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    node_map[test_node_id_2] = test_https_url;
#else
    node_map[test_node_id_2] = "http://localhost:8081";
#endif
    node_map[test_node_id_3] = "http://remote.example.com:9090";
    
    kythira::cpp_httplib_client_config config;
    config.connection_pool_size = 15;
    config.connection_timeout = std::chrono::milliseconds{2000};
    config.request_timeout = std::chrono::milliseconds{8000};
    config.keep_alive_timeout = std::chrono::milliseconds{45000};
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    config.enable_ssl_verification = true;
    config.ca_cert_path = "/etc/ssl/certs/ca-certificates.crt";
#else
    config.enable_ssl_verification = false;
    config.ca_cert_path = "";
#endif
    config.user_agent = "raft-test-client/1.0";
    
    kythira::noop_metrics metrics;
    
    // Test construction with multiple nodes
    kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test client with empty node map
BOOST_AUTO_TEST_CASE(test_empty_node_map_construction, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> empty_node_map;
    
    kythira::cpp_httplib_client_config config;
    kythira::noop_metrics metrics;
    
    // Should be able to construct with empty map
    kythira::cpp_httplib_client<test_transport_types> client(empty_node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test client with various URL formats
BOOST_AUTO_TEST_CASE(test_various_url_formats, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[1] = "http://localhost:8080";
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    node_map[2] = "https://localhost:8443";
#else
    node_map[2] = "http://localhost:8081";
#endif
    node_map[3] = "http://192.168.1.100:9000";
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    node_map[4] = "https://example.com:443";
#else
    node_map[4] = "http://example.com:8080";
#endif
    node_map[5] = "http://node-5.cluster.local:8080";
    
    kythira::cpp_httplib_client_config config;
    kythira::noop_metrics metrics;
    
    // Test construction with various URL formats
    kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test client configuration edge cases
BOOST_AUTO_TEST_CASE(test_configuration_edge_cases, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id_1] = test_http_url;
    
    // Test with minimal timeouts
    {
        kythira::cpp_httplib_client_config config;
        config.connection_pool_size = 1;
        config.connection_timeout = std::chrono::milliseconds{1};
        config.request_timeout = std::chrono::milliseconds{1};
        config.keep_alive_timeout = std::chrono::milliseconds{1};
        
        kythira::noop_metrics metrics;
        
        kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
        
        BOOST_CHECK(true);  // Construction succeeded
    }
    
    // Test with maximum timeouts
    {
        kythira::cpp_httplib_client_config config;
        config.connection_pool_size = 1000;
        config.connection_timeout = std::chrono::milliseconds{60000};
        config.request_timeout = std::chrono::milliseconds{300000};
        config.keep_alive_timeout = std::chrono::milliseconds{600000};
        
        kythira::noop_metrics metrics;
        
        kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
        
        BOOST_CHECK(true);  // Construction succeeded
    }
}

// Test SSL configuration options
BOOST_AUTO_TEST_CASE(test_ssl_configuration, * boost::unit_test::timeout(30)) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id_1] = test_https_url;
    
    // Test with SSL verification enabled
    {
        kythira::cpp_httplib_client_config config;
        config.enable_ssl_verification = true;
        config.ca_cert_path = "/etc/ssl/certs/ca-certificates.crt";
        
        kythira::noop_metrics metrics;
        
        kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
        
        BOOST_CHECK(true);  // Construction succeeded
    }
    
    // Test with SSL verification disabled
    {
        kythira::cpp_httplib_client_config config;
        config.enable_ssl_verification = false;
        config.ca_cert_path = "";  // Empty path
        
        kythira::noop_metrics metrics;
        
        kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
        
        BOOST_CHECK(true);  // Construction succeeded
    }
#else
    BOOST_TEST_MESSAGE("OpenSSL support not available, skipping SSL configuration test");
    BOOST_CHECK(true);  // Test passes when SSL is not available
#endif
}

// Test user agent configuration
BOOST_AUTO_TEST_CASE(test_user_agent_configuration, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id_1] = test_http_url;
    
    kythira::cpp_httplib_client_config config;
    config.user_agent = "custom-raft-client/2.1.0 (Linux; x86_64)";
    
    kythira::noop_metrics metrics;
    
    kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test that client properly handles move semantics
BOOST_AUTO_TEST_CASE(test_move_semantics, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id_1] = test_http_url;
    
    kythira::cpp_httplib_client_config config;
    kythira::noop_metrics metrics;
    
    // Test move construction
    auto create_client = [&]() {
        return kythira::cpp_httplib_client<test_transport_types>(node_map, config, metrics);
    };
    
    auto client = create_client();
    BOOST_CHECK(true);  // Move construction succeeded
}

// Test client with large node maps
BOOST_AUTO_TEST_CASE(test_large_node_map, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> large_node_map;
    
    // Create a map with 100 nodes
    for (std::uint64_t i = 1; i <= 100; ++i) {
        large_node_map[i] = "http://node-" + std::to_string(i) + ".cluster.local:8080";
    }
    
    kythira::cpp_httplib_client_config config;
    config.connection_pool_size = 50;  // Smaller than node count
    
    kythira::noop_metrics metrics;
    
    kythira::cpp_httplib_client<test_transport_types> client(large_node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction with large node map succeeded
}

BOOST_AUTO_TEST_SUITE_END()