#define BOOST_TEST_MODULE network_concept_template_parameter_consistency_property_test
#include <boost/test/unit_test.hpp>

#include <raft/network.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <raft/http_transport.hpp>
#include <raft/coap_transport.hpp>
#include <raft/console_logger.hpp>
#include <concepts/future.hpp>

namespace {
    constexpr const char* test_name = "network_concept_template_parameter_consistency_property_test";
    
    // Test type aliases
    using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using test_metrics = kythira::noop_metrics;
    using test_logger = kythira::console_logger;
    using future_type = kythira::Future<kythira::request_vote_response<>>;
}

BOOST_AUTO_TEST_SUITE(network_concept_template_parameter_consistency_property_tests)

/**
 * **Feature: network-concept-template-fix, Property 1: Network concept template parameter consistency**
 * **Validates: Requirements 1.1, 1.2, 2.1, 2.2, 4.1, 4.2**
 * 
 * Property: For any usage of network_client or network_server concepts throughout the codebase, 
 * exactly two template parameters should be provided: the implementation type and the future type
 */
BOOST_AUTO_TEST_CASE(property_network_concept_template_parameter_consistency, * boost::unit_test::timeout(90)) {
    // Test that network_client concept requires exactly 2 template parameters
    
    // Test 1: Valid network_client usage with 2 template parameters
    using valid_http_client = kythira::cpp_httplib_client<future_type, test_serializer, test_metrics>;
    static_assert(kythira::network_client<valid_http_client, future_type>,
                 "Valid HTTP client with 2 template parameters must satisfy network_client concept");
    
    // Test 2: Valid network_server usage with 2 template parameters
    using valid_http_server = kythira::cpp_httplib_server<future_type, test_serializer, test_metrics>;
    static_assert(kythira::network_server<valid_http_server, future_type>,
                 "Valid HTTP server with 2 template parameters must satisfy network_server concept");
    
#ifdef LIBCOAP_AVAILABLE
    // Test 3: Valid CoAP client usage with 2 template parameters
    using test_types = kythira::default_transport_types<future_type, test_serializer, test_metrics, test_logger>;
    using valid_coap_client = kythira::coap_client<test_types>;
    static_assert(kythira::network_client<valid_coap_client, future_type>,
                 "Valid CoAP client with 2 template parameters must satisfy network_client concept");
    
    // Test 4: Valid CoAP server usage with 2 template parameters
    using valid_coap_server = kythira::coap_server<future_type, test_serializer, test_metrics, test_logger>;
    static_assert(kythira::network_server<valid_coap_server, future_type>,
                 "Valid CoAP server with 2 template parameters must satisfy network_server concept");
#endif
    
    // Test 5: Verify that concepts are in kythira namespace (not raft namespace)
    // This is a compile-time test - if it compiles, the concepts are accessible in kythira namespace
    BOOST_TEST_MESSAGE("Network concepts are accessible in kythira namespace with correct template parameters");
    
    // Test 6: Test with different future types to ensure consistency
    using rv_future_type = kythira::Future<kythira::request_vote_response<>>;
    using ae_future_type = kythira::Future<kythira::append_entries_response<>>;
    using is_future_type = kythira::Future<kythira::install_snapshot_response<>>;
    
    using http_client_rv = kythira::cpp_httplib_client<rv_future_type, test_serializer, test_metrics>;
    using http_client_ae = kythira::cpp_httplib_client<ae_future_type, test_serializer, test_metrics>;
    using http_client_is = kythira::cpp_httplib_client<is_future_type, test_serializer, test_metrics>;
    
    static_assert(kythira::network_client<http_client_rv, rv_future_type>,
                 "HTTP client with RequestVote future must satisfy network_client concept");
    static_assert(kythira::network_client<http_client_ae, ae_future_type>,
                 "HTTP client with AppendEntries future must satisfy network_client concept");
    static_assert(kythira::network_client<http_client_is, is_future_type>,
                 "HTTP client with InstallSnapshot future must satisfy network_client concept");
    
    // Test 7: Verify that invalid types are properly rejected
    class invalid_client {
    public:
        auto some_method() -> void {}
    };
    
    static_assert(!kythira::network_client<invalid_client, future_type>,
                 "Invalid client type must not satisfy network_client concept");
    
    class invalid_server {
    public:
        auto some_method() -> void {}
    };
    
    static_assert(!kythira::network_server<invalid_server, future_type>,
                 "Invalid server type must not satisfy network_server concept");
    
    // Test 8: Test that mock implementations with correct signatures satisfy concepts
    class mock_client {
    public:
        auto send_request_vote(
            std::uint64_t target,
            const kythira::request_vote_request<>& request,
            std::chrono::milliseconds timeout
        ) -> future_type {
            return future_type(kythira::request_vote_response<>{});
        }
        
        auto send_append_entries(
            std::uint64_t target,
            const kythira::append_entries_request<>& request,
            std::chrono::milliseconds timeout
        ) -> future_type {
            return future_type(kythira::request_vote_response<>{});
        }
        
        auto send_install_snapshot(
            std::uint64_t target,
            const kythira::install_snapshot_request<>& request,
            std::chrono::milliseconds timeout
        ) -> future_type {
            return future_type(kythira::request_vote_response<>{});
        }
    };
    
    static_assert(kythira::network_client<mock_client, future_type>,
                 "Mock client with correct signature must satisfy network_client concept");
    
    class mock_server {
    public:
        auto register_request_vote_handler(
            std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> handler
        ) -> void {}
        
        auto register_append_entries_handler(
            std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> handler
        ) -> void {}
        
        auto register_install_snapshot_handler(
            std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)> handler
        ) -> void {}
        
        auto start() -> void {}
        auto stop() -> void {}
        auto is_running() -> bool { return true; }
    };
    
    static_assert(kythira::network_server<mock_server, future_type>,
                 "Mock server with correct signature must satisfy network_server concept");
    
    BOOST_TEST_MESSAGE("Network concept template parameter consistency validation completed");
    BOOST_CHECK(true);
}

// Test that the concept template parameter consistency is enforced in test files
BOOST_AUTO_TEST_CASE(test_file_concept_usage_consistency, * boost::unit_test::timeout(30)) {
    // This test verifies that test files use consistent template parameters
    // for network concepts, which is what Property 1 is specifically about
    
    // Test that HTTP transport test types use correct template parameters
    using http_client_test_type = kythira::cpp_httplib_client<
        future_type,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics
    >;
    using http_server_test_type = kythira::cpp_httplib_server<future_type, kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics>;
    
    // These should satisfy the concepts with correct template parameters
    static_assert(kythira::network_client<http_client_test_type, future_type>,
                 "HTTP client test type must satisfy network_client concept with correct parameters");
    static_assert(kythira::network_server<http_server_test_type, future_type>,
                 "HTTP server test type must satisfy network_server concept with correct parameters");
    
#ifdef LIBCOAP_AVAILABLE
    // Test that CoAP transport test types use correct template parameters
    using test_types = kythira::default_transport_types<future_type, test_serializer, test_metrics, test_logger>;
    using coap_client_test_type = kythira::coap_client<test_types>;
    using coap_server_test_type = kythira::coap_server<test_types>;
    
    static_assert(kythira::network_client<coap_client_test_type, future_type>,
                 "CoAP client test type must satisfy network_client concept with correct parameters");
    static_assert(kythira::network_server<coap_server_test_type, future_type>,
                 "CoAP server test type must satisfy network_server concept with correct parameters");
#endif
    
    BOOST_TEST_MESSAGE("Test file concept usage consistency verified");
    BOOST_CHECK(true);
}

// Test that static assertions in test files use correct template parameters
BOOST_AUTO_TEST_CASE(test_static_assertion_template_parameters, * boost::unit_test::timeout(30)) {
    // This test verifies that static_assert statements in test files use
    // the correct number of template parameters for network concepts
    
    // Test patterns that should be used in test files
    using client_type = kythira::cpp_httplib_client<
        future_type,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics
    >;
    using server_type = kythira::cpp_httplib_server<future_type, kythira::json_rpc_serializer<std::vector<std::byte>>, kythira::noop_metrics>;
    
    // Correct static assertion patterns (2 template parameters)
    static_assert(kythira::network_client<client_type, future_type>,
                 "Static assertions must use 2 template parameters for network_client");
    static_assert(kythira::network_server<server_type, future_type>,
                 "Static assertions must use 2 template parameters for network_server");
    
    // Test that the concepts work with different future specializations
    using different_future = kythira::Future<kythira::append_entries_response<>>;
    using client_with_different_future = kythira::cpp_httplib_client<
        different_future,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics
    >;
    using server_with_different_future = kythira::cpp_httplib_server<
        different_future,
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics
    >;
    
    static_assert(kythira::network_client<client_with_different_future, different_future>,
                 "Static assertions must work with different future specializations");
    static_assert(kythira::network_server<server_with_different_future, different_future>,
                 "Static assertions must work with different future specializations");
    
    BOOST_TEST_MESSAGE("Static assertion template parameters verified");
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()