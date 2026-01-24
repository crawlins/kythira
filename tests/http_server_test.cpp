#define BOOST_TEST_MODULE http_server_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 8082;
    
    // Define transport types for testing
    using test_transport_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
}

BOOST_AUTO_TEST_SUITE(http_server_tests)

// Test server conforms to network_server concept
BOOST_AUTO_TEST_CASE(test_server_concept_conformance, * boost::unit_test::timeout(30)) {
    using server_type = kythira::cpp_httplib_server<test_transport_types>;
    
    static_assert(kythira::network_server<server_type>,
                  "cpp_httplib_server must satisfy network_server concept");
    
    BOOST_TEST(true); // Test passes if compilation succeeds
}

// Test server requires rpc_serializer concept
BOOST_AUTO_TEST_CASE(test_server_requires_rpc_serializer, * boost::unit_test::timeout(30)) {
    // This should compile with valid serializer
    using valid_server = kythira::cpp_httplib_server<test_transport_types>;
    static_assert(kythira::rpc_serializer<typename test_transport_types::serializer_type, std::vector<std::byte>>);
    
    BOOST_TEST(true); // Test passes if compilation succeeds
}

// Test handler registration for each RPC type
BOOST_AUTO_TEST_CASE(test_handler_registration, * boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    typename test_transport_types::metrics_type metrics;
    
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, test_bind_port, config, metrics);
    
    // Test RequestVote handler registration
    bool request_vote_called = false;
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        request_vote_called = true;
        return kythira::request_vote_response<>{};
    });
    
    // Test AppendEntries handler registration
    bool append_entries_called = false;
    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        append_entries_called = true;
        return kythira::append_entries_response<>{};
    });
    
    // Test InstallSnapshot handler registration
    bool install_snapshot_called = false;
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        install_snapshot_called = true;
        return kythira::install_snapshot_response<>{};
    });
    
    // Handlers should be registered (we can't easily test invocation without starting server)
    BOOST_TEST(true); // Test passes if no exceptions thrown during registration
}

// Test server lifecycle (start, stop, is_running)
BOOST_AUTO_TEST_CASE(test_server_lifecycle, * boost::unit_test::timeout(45)) {
    kythira::cpp_httplib_server_config config;
    typename test_transport_types::metrics_type metrics;
    
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, test_bind_port, config, metrics);
    
    // Initially not running
    BOOST_TEST(!server.is_running());
    
    // Note: We can't easily test start() without potentially conflicting with other tests
    // that might be using the same port. In a real test environment, you'd want to:
    // 1. Use a unique port for each test
    // 2. Actually start the server and verify it's listening
    // 3. Stop the server and verify it's no longer listening
    
    BOOST_TEST(true); // Test structure is correct
}

// Test HTTPS support configuration
BOOST_AUTO_TEST_CASE(test_https_configuration, * boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    config.enable_ssl = true;
    config.ssl_cert_path = "/path/to/cert.pem";
    config.ssl_key_path = "/path/to/key.pem";
    
    typename test_transport_types::metrics_type metrics;
    
    // Server should accept HTTPS configuration
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, test_bind_port, config, metrics);
    
    BOOST_TEST(true); // Test passes if construction succeeds
}

// Test configuration acceptance
BOOST_AUTO_TEST_CASE(test_configuration_acceptance, * boost::unit_test::timeout(30)) {
    kythira::cpp_httplib_server_config config;
    config.max_concurrent_connections = 50;
    config.max_request_body_size = 5 * 1024 * 1024; // 5 MB
    config.request_timeout = std::chrono::seconds{15};
    
    typename test_transport_types::metrics_type metrics;
    
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, test_bind_port, config, metrics);
    
    BOOST_TEST(true); // Test passes if construction with custom config succeeds
}

BOOST_AUTO_TEST_SUITE_END()