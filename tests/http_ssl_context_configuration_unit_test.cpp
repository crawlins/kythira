#define BOOST_TEST_MODULE http_ssl_context_configuration_unit_test

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 8443;
constexpr std::uint64_t test_node_id = 1;
constexpr const char* test_node_url = "https://localhost:8443";

// Valid cipher suites for testing
constexpr const char* valid_cipher_suites =
    "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256";
constexpr const char* invalid_cipher_suites = "INVALID-CIPHER-SUITE:ANOTHER-INVALID-CIPHER";

// TLS version strings
constexpr const char* tls_v12 = "TLSv1.2";
constexpr const char* tls_v13 = "TLSv1.3";
constexpr const char* tls_v10 = "TLSv1.0";  // Below security requirements
constexpr const char* invalid_tls_version = "TLSv9.9";
}

BOOST_AUTO_TEST_SUITE(http_ssl_context_configuration_unit_tests)

// **Requirement 14: cpp_httplib_server/client SSL context configuration**
// **Validates: Requirements 10.9, 10.13, 10.14**
//
// None of these cases configure a client certificate — they exercise pure
// cipher-suite/TLS-version validation, which has no dependency on the
// certificate-authority framework (no cert files are ever loaded here).

BOOST_AUTO_TEST_CASE(test_cipher_suite_restriction_enforcement, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = valid_cipher_suites;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);  // Valid cipher suites construct successfully.
}

BOOST_AUTO_TEST_CASE(test_invalid_cipher_suite_rejection, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = invalid_cipher_suites;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_tls_version_constraint_enforcement, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    kythira::cpp_httplib_client_config client_config;
    client_config.min_tls_version = tls_v12;
    client_config.max_tls_version = tls_v13;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);  // Valid TLS version range constructs successfully.
}

BOOST_AUTO_TEST_CASE(test_invalid_tls_version_range_rejection, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // min > max is invalid.
    kythira::cpp_httplib_client_config client_config;
    client_config.min_tls_version = tls_v13;
    client_config.max_tls_version = tls_v12;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_minimum_security_standards_enforcement, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // TLS 1.0 is below the project's minimum security standard.
    kythira::cpp_httplib_client_config client_config;
    client_config.min_tls_version = tls_v10;
    client_config.max_tls_version = tls_v12;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_invalid_tls_version_string_rejection, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    kythira::cpp_httplib_client_config client_config;
    client_config.min_tls_version = invalid_tls_version;
    client_config.max_tls_version = tls_v13;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_ssl_context_creation_and_configuration, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = valid_cipher_suites;
    client_config.min_tls_version = tls_v12;
    client_config.max_tls_version = tls_v13;
    client_config.enable_ssl_verification = true;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);  // Comprehensive, valid configuration constructs successfully.
}

BOOST_AUTO_TEST_CASE(test_ssl_context_error_handling, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Multiple invalid parameters at once.
    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = invalid_cipher_suites;
    client_config.min_tls_version = invalid_tls_version;
    client_config.max_tls_version = tls_v13;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_server_ssl_context_configuration, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Certificate files that don't exist — a genuine configuration failure,
    // independent of the certificate-authority framework.
    kythira::cpp_httplib_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = "/path/to/server.crt";
    server_config.ssl_key_path = "/path/to/server.key";
    server_config.cipher_suites = valid_cipher_suites;
    server_config.min_tls_version = tls_v12;
    server_config.max_tls_version = tls_v13;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW((kythira::cpp_httplib_server<test_types>(test_bind_address, test_bind_port,
                                                               server_config, metrics)),
                      kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_empty_cipher_suites_allowed, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Empty cipher suites means "use defaults" and must be accepted.
    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = "";
    client_config.min_tls_version = tls_v12;
    client_config.max_tls_version = tls_v13;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(test_empty_tls_versions_allowed, *boost::unit_test::timeout(30)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Empty TLS version bounds means "use defaults" and must be accepted.
    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = valid_cipher_suites;
    client_config.min_tls_version = "";
    client_config.max_tls_version = "";

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()
