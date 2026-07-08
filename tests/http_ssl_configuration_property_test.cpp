#define BOOST_TEST_MODULE http_ssl_configuration_property_test

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include "ca_test_fixture.hpp"
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <filesystem>
#include <fstream>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 8443;
constexpr std::uint64_t test_node_id = 1;
constexpr const char* test_node_url = "https://localhost:8443";

// Real, cryptographically valid certificate material (replaces the previous
// hand-authored placeholder PEM constants — see Requirement 6.1/14.5 of the
// certificate-authority spec).
struct cert_material {
    raft::testing::ca_test_fixture fixture;

    auto leaf() -> const raft::testing::temp_cert_files& {
        return fixture.bootstrap_client("leaf", {"localhost"}, {"127.0.0.1"}, /*server_auth=*/true,
                                        /*client_auth=*/true);
    }

    auto write_ca_file() -> std::string {
        auto path = std::filesystem::temp_directory_path() /
                    ("test_ca_" + std::to_string(std::random_device{}()) + ".pem");
        std::ofstream file(path);
        file << fixture.root_certificate_pem();
        return path.string();
    }
};

// Helper to create temporary certificate files
auto create_temp_cert_file(const std::string& content) -> std::string {
    auto temp_path = std::filesystem::temp_directory_path() /
                     ("test_cert_" + std::to_string(std::random_device{}()));

    std::ofstream file(temp_path);
    file << content;
    file.close();

    return temp_path.string();
}

// Helper to clean up temporary files
auto cleanup_temp_file(const std::string& path) -> void {
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
}
}

BOOST_AUTO_TEST_SUITE(http_ssl_configuration_property_tests)

// **Feature: http-transport, Property 13: SSL certificate loading validation**
// **Validates: Requirements 10.6, 10.7, 10.12**
BOOST_AUTO_TEST_CASE(test_ssl_certificate_loading_validation, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Test client with valid, real SSL certificate paths. ca_cert_path is
    // required here: validate_certificate_files() validates the presented
    // certificate's chain unconditionally, against system CAs when
    // ca_cert_path is empty — which a self-signed test CA can never satisfy.
    cert_material material;
    const auto& files = material.leaf();
    auto ca_cert_path = material.write_ca_file();

    kythira::cpp_httplib_client_config client_config;
    client_config.client_cert_path = files.cert_path();
    client_config.client_key_path = files.key_path();
    client_config.ca_cert_path = ca_cert_path;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);  // Valid certificate files construct successfully.
}

BOOST_AUTO_TEST_CASE(test_ssl_certificate_loading_failure_cases, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Test invalid certificate file path
    kythira::cpp_httplib_client_config client_config;
    client_config.client_cert_path = "/nonexistent/certificate.pem";
    client_config.client_key_path = "/nonexistent/key.pem";

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;

    // This should fail with invalid certificate paths
    BOOST_CHECK_THROW(
        kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_ssl_certificate_mismatch, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // A real, valid certificate paired with a real, valid — but different —
    // private key: a genuine mismatch, not merely unparseable content.
    cert_material material;
    const auto& cert_files = material.leaf();
    const auto& other_files = material.fixture.bootstrap_client("other", {"other.example.com"});

    kythira::cpp_httplib_client_config client_config;
    client_config.client_cert_path = cert_files.cert_path();
    client_config.client_key_path = other_files.key_path();

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

// **Feature: http-transport, Property 14: Certificate chain verification**
// **Validates: Requirements 10.8**
BOOST_AUTO_TEST_CASE(test_certificate_chain_verification, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    cert_material material;
    const auto& files = material.leaf();
    auto ca_cert_path = material.write_ca_file();

    kythira::cpp_httplib_client_config client_config;
    client_config.client_cert_path = files.cert_path();
    client_config.client_key_path = files.key_path();
    client_config.ca_cert_path = ca_cert_path;

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    // The leaf genuinely chains to this CA, so this must succeed cleanly.
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);

    cleanup_temp_file(ca_cert_path);
}

// **Feature: http-transport, Property 15: Cipher suite restriction enforcement**
// **Validates: Requirements 10.13, 14.10, 14.14**
BOOST_AUTO_TEST_CASE(test_cipher_suite_restriction_enforcement, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Test valid cipher suites
    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256";
    client_config.min_tls_version = "TLSv1.2";
    client_config.max_tls_version = "TLSv1.3";

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_client<test_types> client(std::move(node_map), client_config, metrics);
    BOOST_TEST(true);  // Valid cipher suites construct successfully.
}

BOOST_AUTO_TEST_CASE(test_invalid_cipher_suites, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Test invalid cipher suites
    kythira::cpp_httplib_client_config client_config;
    client_config.cipher_suites = "INVALID-CIPHER-SUITE:ANOTHER-INVALID-CIPHER";

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_tls_version_constraints, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    // Test invalid TLS version range (min > max)
    kythira::cpp_httplib_client_config client_config;
    client_config.min_tls_version = "TLSv1.3";
    client_config.max_tls_version = "TLSv1.2";

    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW(
        (kythira::cpp_httplib_client<test_types>(std::move(node_map), client_config, metrics)),
        kythira::ssl_configuration_error);
}

// **Feature: http-transport, Property 16: Client certificate authentication**
// **Validates: Requirements 10.10, 10.11**
BOOST_AUTO_TEST_CASE(test_client_certificate_authentication, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    cert_material material;
    const auto& files = material.leaf();
    auto ca_cert_path = material.write_ca_file();

    // Test server with client certificate authentication enabled
    kythira::cpp_httplib_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = files.cert_path();
    server_config.ssl_key_path = files.key_path();
    server_config.ca_cert_path = ca_cert_path;
    server_config.require_client_cert = true;

    typename test_types::metrics_type metrics;
    kythira::cpp_httplib_server<test_types> server(test_bind_address, test_bind_port, server_config,
                                                   metrics);
    BOOST_TEST(true);  // Real, valid material constructs successfully.

    cleanup_temp_file(ca_cert_path);
}

BOOST_AUTO_TEST_CASE(test_client_cert_auth_without_ca, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    cert_material material;
    const auto& files = material.leaf();

    // Test server with client certificate authentication but no CA certificate
    kythira::cpp_httplib_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = files.cert_path();
    server_config.ssl_key_path = files.key_path();
    server_config.require_client_cert = true;
    // Missing ca_cert_path - should fail

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW((kythira::cpp_httplib_server<test_types>(test_bind_address, test_bind_port,
                                                               server_config, metrics)),
                      kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_CASE(test_ssl_disabled_with_ssl_config, *boost::unit_test::timeout(60)) {
    using test_types =
        kythira::http_transport_types<kythira::json_rpc_serializer<std::vector<std::byte>>,
                                      kythira::noop_metrics, folly::CPUThreadPoolExecutor>;

    cert_material material;
    const auto& files = material.leaf();

    // Test server with SSL disabled but SSL configuration provided
    kythira::cpp_httplib_server_config server_config;
    server_config.enable_ssl = false;                 // SSL disabled
    server_config.ssl_cert_path = files.cert_path();  // But SSL config provided
    server_config.ssl_key_path = files.key_path();

    typename test_types::metrics_type metrics;
    BOOST_CHECK_THROW((kythira::cpp_httplib_server<test_types>(test_bind_address, test_bind_port,
                                                               server_config, metrics)),
                      kythira::ssl_configuration_error);
}

BOOST_AUTO_TEST_SUITE_END()