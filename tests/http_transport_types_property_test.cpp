#define BOOST_TEST_MODULE http_transport_types_property_test

#include <boost/test/unit_test.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 8080;
    constexpr std::uint64_t test_node_id = 1;
    constexpr const char* test_node_url = "http://localhost:8080";
}

BOOST_AUTO_TEST_SUITE(http_transport_types_property_tests)

// **Feature: http-transport, Property 11: Types template parameter conformance**
// **Validates: Requirements 18.6, 18.7, 18.8, 18.9**
BOOST_AUTO_TEST_CASE(test_transport_types_concept_conformance, * boost::unit_test::timeout(30)) {
    // Test that http_transport_types satisfies the transport_types concept
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    // Static assertions to verify concept conformance
    static_assert(kythira::transport_types<test_types>,
                  "http_transport_types must satisfy transport_types concept");
    
    // Verify that the types provide required member types
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::request_vote_response<>>, 
                                folly::Future<kythira::request_vote_response<>>>,
                  "future_template must be correctly defined for request_vote_response");
    
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::append_entries_response<>>, 
                                folly::Future<kythira::append_entries_response<>>>,
                  "future_template must be correctly defined for append_entries_response");
    
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::install_snapshot_response<>>, 
                                folly::Future<kythira::install_snapshot_response<>>>,
                  "future_template must be correctly defined for install_snapshot_response");
    
    static_assert(std::is_same_v<typename test_types::serializer_type,
                                kythira::json_rpc_serializer<std::vector<std::byte>>>,
                  "serializer_type must be correctly defined");
    
    static_assert(std::is_same_v<typename test_types::metrics_type,
                                kythira::noop_metrics>,
                  "metrics_type must be correctly defined");
    
    static_assert(std::is_same_v<typename test_types::executor_type,
                                folly::CPUThreadPoolExecutor>,
                  "executor_type must be correctly defined");
    
    // Verify that the serializer_type satisfies rpc_serializer concept
    static_assert(kythira::rpc_serializer<typename test_types::serializer_type, std::vector<std::byte>>,
                  "serializer_type must satisfy rpc_serializer concept");
    
    // Verify that the metrics_type satisfies metrics concept
    static_assert(kythira::metrics<typename test_types::metrics_type>,
                  "metrics_type must satisfy metrics concept");
    
    // Verify that the future_template satisfies future concept for all required response types
    static_assert(future<typename test_types::template future_template<kythira::request_vote_response<>>, kythira::request_vote_response<>>,
                  "future_template must satisfy future concept for request_vote_response");
    
    static_assert(future<typename test_types::template future_template<kythira::append_entries_response<>>, kythira::append_entries_response<>>,
                  "future_template must satisfy future concept for append_entries_response");
    
    static_assert(future<typename test_types::template future_template<kythira::install_snapshot_response<>>, kythira::install_snapshot_response<>>,
                  "future_template must satisfy future concept for install_snapshot_response");
    
    BOOST_TEST(true); // Test passes if static_asserts compile
}

BOOST_AUTO_TEST_CASE(test_client_uses_transport_types, * boost::unit_test::timeout(30)) {
    // Test that cpp_httplib_client can be instantiated with transport_types
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    // Verify that the client template accepts transport_types
    static_assert(kythira::transport_types<test_types>,
                  "test_types must satisfy transport_types concept");
    
    // Test client construction
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    typename test_types::metrics_type metrics;
    
    // This should compile if the template parameter is correctly defined
    kythira::cpp_httplib_client<test_types> client(
        std::move(node_map), config, metrics);
    
    BOOST_TEST(true); // Test passes if construction succeeds
}

BOOST_AUTO_TEST_CASE(test_server_uses_transport_types, * boost::unit_test::timeout(30)) {
    // Test that cpp_httplib_server can be instantiated with transport_types
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    // Verify that the server template accepts transport_types
    static_assert(kythira::transport_types<test_types>,
                  "test_types must satisfy transport_types concept");
    
    // Test server construction
    kythira::cpp_httplib_server_config config;
    typename test_types::metrics_type metrics;
    
    // This should compile if the template parameter is correctly defined
    kythira::cpp_httplib_server<test_types> server(
        test_bind_address, test_bind_port, config, metrics);
    
    BOOST_TEST(true); // Test passes if construction succeeds
}

BOOST_AUTO_TEST_CASE(test_network_concepts_with_transport_types, * boost::unit_test::timeout(30)) {
    // Test that HTTP transport classes satisfy network concepts with transport_types
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    using client_type = kythira::cpp_httplib_client<test_types>;
    using server_type = kythira::cpp_httplib_server<test_types>;
    using future_type = typename test_types::template future_template<kythira::request_vote_response<>>;
    
    // Verify that the client satisfies network_client concept
    static_assert(kythira::network_client<client_type, future_type>,
                  "cpp_httplib_client with transport_types must satisfy network_client concept");
    
    // Verify that the server satisfies network_server concept
    static_assert(kythira::network_server<server_type, future_type>,
                  "cpp_httplib_server with transport_types must satisfy network_server concept");
    
    BOOST_TEST(true); // Test passes if static_asserts compile
}

BOOST_AUTO_TEST_CASE(test_type_aliases_work_correctly, * boost::unit_test::timeout(30)) {
    // Test that type aliases in the classes work correctly
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    using client_type = kythira::cpp_httplib_client<test_types>;
    using server_type = kythira::cpp_httplib_server<test_types>;
    
    // Verify that type aliases are correctly defined
    static_assert(std::is_same_v<typename client_type::template future_template<kythira::request_vote_response<>>, 
                                typename test_types::template future_template<kythira::request_vote_response<>>>,
                  "Client future_template alias must match transport_types future_template");
    
    static_assert(std::is_same_v<typename client_type::serializer_type, typename test_types::serializer_type>,
                  "Client serializer_type alias must match transport_types serializer_type");
    
    static_assert(std::is_same_v<typename client_type::metrics_type, typename test_types::metrics_type>,
                  "Client metrics_type alias must match transport_types metrics_type");
    
    static_assert(std::is_same_v<typename server_type::template future_template<kythira::request_vote_response<>>, 
                                typename test_types::template future_template<kythira::request_vote_response<>>>,
                  "Server future_template alias must match transport_types future_template");
    
    static_assert(std::is_same_v<typename server_type::serializer_type, typename test_types::serializer_type>,
                  "Server serializer_type alias must match transport_types serializer_type");
    
    static_assert(std::is_same_v<typename server_type::metrics_type, typename test_types::metrics_type>,
                  "Server metrics_type alias must match transport_types metrics_type");
    
    BOOST_TEST(true); // Test passes if static_asserts compile
}

// **Feature: http-transport, Property 12: Template template parameter future type correctness**
// **Validates: Requirements 19.2, 19.3, 19.4, 19.7, 19.9**
BOOST_AUTO_TEST_CASE(test_template_template_parameter_future_type_correctness, * boost::unit_test::timeout(30)) {
    // Test that different RPC methods return correctly typed futures
    using test_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    using client_type = kythira::cpp_httplib_client<test_types>;
    
    // Verify that future_template can be instantiated with different response types
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::request_vote_response<>>,
                                folly::Future<kythira::request_vote_response<>>>,
                  "future_template<request_vote_response> must be folly::Future<request_vote_response>");
    
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::append_entries_response<>>,
                                folly::Future<kythira::append_entries_response<>>>,
                  "future_template<append_entries_response> must be folly::Future<append_entries_response>");
    
    static_assert(std::is_same_v<typename test_types::template future_template<kythira::install_snapshot_response<>>,
                                folly::Future<kythira::install_snapshot_response<>>>,
                  "future_template<install_snapshot_response> must be folly::Future<install_snapshot_response>");
    
    // Test that client methods return correctly typed futures
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    typename test_types::metrics_type metrics;
    
    client_type client(std::move(node_map), config, metrics);
    
    // Create dummy requests
    kythira::request_vote_request<> vote_request;
    kythira::append_entries_request<> append_request;
    kythira::install_snapshot_request<> snapshot_request;
    
    std::chrono::milliseconds timeout{1000};
    
    // Test return types (these should compile with correct types)
    auto vote_future = client.send_request_vote(test_node_id, vote_request, timeout);
    auto append_future = client.send_append_entries(test_node_id, append_request, timeout);
    auto snapshot_future = client.send_install_snapshot(test_node_id, snapshot_request, timeout);
    
    // Verify return types are correctly typed
    static_assert(std::is_same_v<decltype(vote_future), 
                                typename test_types::template future_template<kythira::request_vote_response<>>>,
                  "send_request_vote must return future_template<request_vote_response>");
    
    static_assert(std::is_same_v<decltype(append_future), 
                                typename test_types::template future_template<kythira::append_entries_response<>>>,
                  "send_append_entries must return future_template<append_entries_response>");
    
    static_assert(std::is_same_v<decltype(snapshot_future), 
                                typename test_types::template future_template<kythira::install_snapshot_response<>>>,
                  "send_install_snapshot must return future_template<install_snapshot_response>");
    
    BOOST_TEST(true); // Test passes if static_asserts compile
}

BOOST_AUTO_TEST_CASE(test_alternative_future_implementations, * boost::unit_test::timeout(30)) {
    // Test that alternative future implementations work with transport_types
    using std_types = kythira::std_http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
    
    // Verify that std_http_transport_types satisfies transport_types concept
    static_assert(kythira::transport_types<std_types>,
                  "std_http_transport_types must satisfy transport_types concept");
    
    // Verify that future_template works with std::future
    static_assert(std::is_same_v<typename std_types::template future_template<kythira::request_vote_response<>>,
                                std::future<kythira::request_vote_response<>>>,
                  "std_types future_template must use std::future");
    
    // Test that client can be instantiated with std::future types
    using std_client_type = kythira::cpp_httplib_client<std_types>;
    
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_node_url;
    
    kythira::cpp_httplib_client_config config;
    typename std_types::metrics_type metrics;
    
    // This should compile if the template template parameter works correctly
    std_client_type client(std::move(node_map), config, metrics);
    
    BOOST_TEST(true); // Test passes if construction succeeds
}

BOOST_AUTO_TEST_SUITE_END()