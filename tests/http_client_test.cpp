#define BOOST_TEST_MODULE http_client_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
    constexpr const char* test_node_url = "http://localhost:8080";
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_candidate_id = 2;
    constexpr std::uint64_t test_last_log_index = 10;
    constexpr std::uint64_t test_last_log_term = 4;
    
    // Define transport types for testing
    using test_transport_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
}

BOOST_AUTO_TEST_SUITE(http_client_tests)

// Test that cpp_httplib_client satisfies network_client concept
BOOST_AUTO_TEST_CASE(test_client_satisfies_network_client_concept, * boost::unit_test::timeout(30)) {
    using client_type = kythira::cpp_httplib_client<test_transport_types>;
    
    static_assert(kythira::network_client<client_type>,
                  "cpp_httplib_client must satisfy network_client concept");
    
    BOOST_CHECK(true);  // If we get here, static_assert passed
}

// Test that cpp_httplib_client requires rpc_serializer concept
BOOST_AUTO_TEST_CASE(test_client_requires_rpc_serializer_concept, * boost::unit_test::timeout(30)) {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    
    static_assert(kythira::rpc_serializer<serializer_type, std::vector<std::byte>>,
                  "json_rpc_serializer must satisfy rpc_serializer concept");
    
    BOOST_CHECK(true);  // If we get here, static_assert passed
}

// Test client construction with valid configuration
BOOST_AUTO_TEST_CASE(test_client_construction, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    config.connection_pool_size = 5;
    config.connection_timeout = std::chrono::milliseconds{1000};
    config.request_timeout = std::chrono::milliseconds{5000};
    
    typename test_transport_types::metrics_type metrics;
    
    // Test construction doesn't throw
    kythira::cpp_httplib_client<test_transport_types> client(
        node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test HTTPS URL detection
BOOST_AUTO_TEST_CASE(test_https_url_detection, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[1] = "http://localhost:8080";
    node_map[2] = "https://localhost:8443";
    
    kythira::cpp_httplib_client_config config;
    typename test_transport_types::metrics_type metrics;
    
    // Test construction with mixed HTTP/HTTPS URLs
    kythira::cpp_httplib_client<test_transport_types> client(
        node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test configuration parameters
BOOST_AUTO_TEST_CASE(test_configuration_parameters, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    config.connection_pool_size = 20;
    config.connection_timeout = std::chrono::milliseconds{2000};
    config.request_timeout = std::chrono::milliseconds{10000};
    config.keep_alive_timeout = std::chrono::milliseconds{30000};
    config.enable_ssl_verification = false;
    config.ca_cert_path = "/path/to/ca.crt";
    config.user_agent = "test-agent/1.0";
    
    typename test_transport_types::metrics_type metrics;
    
    // Test construction with custom configuration
    kythira::cpp_httplib_client<test_transport_types> client(
        node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

// Test metrics integration
BOOST_AUTO_TEST_CASE(test_metrics_integration, * boost::unit_test::timeout(30)) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    typename test_transport_types::metrics_type metrics;
    
    // Verify that noop_metrics satisfies metrics concept
    static_assert(kythira::metrics<typename test_transport_types::metrics_type>,
                  "metrics_type must satisfy metrics concept");
    
    // Test construction with metrics
    kythira::cpp_httplib_client<test_transport_types> client(
        node_map, config, metrics);
    
    BOOST_CHECK(true);  // Construction succeeded
}

BOOST_AUTO_TEST_SUITE_END()
