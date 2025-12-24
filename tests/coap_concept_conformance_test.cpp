#define BOOST_TEST_MODULE coap_concept_conformance_test
#include <boost/test/included/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/network.hpp>
#include <raft/types.hpp>

// Only include CoAP transport if libcoap is available
#ifdef LIBCOAP_AVAILABLE
#include <raft/coap_transport.hpp>
#endif

namespace {
    constexpr const char* test_name = "coap_concept_conformance_test";
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 5683;
    constexpr std::uint64_t test_node_id = 1;
    constexpr const char* test_endpoint = "coap://127.0.0.1:5683";
    
    // Test serializer type alias
    using test_serializer = raft::json_rpc_serializer<std::vector<std::byte>>;
    using test_metrics = raft::noop_metrics;
    
#ifdef LIBCOAP_AVAILABLE
    using future_type = kythira::Future<raft::request_vote_response<>>;
    using test_client = kythira::coap_client<future_type, test_serializer, test_metrics, raft::console_logger>;
    using test_server = kythira::coap_server<future_type, test_serializer, test_metrics, raft::console_logger>;
#endif
}

BOOST_AUTO_TEST_SUITE(coap_concept_conformance_tests)

#ifdef LIBCOAP_AVAILABLE
// Test that coap_client satisfies network_client concept
BOOST_AUTO_TEST_CASE(test_coap_client_network_client_concept, * boost::unit_test::timeout(15)) {
    // Static assertion to verify concept satisfaction
    static_assert(kythira::network_client<test_client, future_type>, 
                  "coap_client must satisfy network_client concept");
    
    BOOST_TEST_MESSAGE("coap_client satisfies network_client concept");
    BOOST_TEST(true);
}

// Test that coap_server satisfies network_server concept
BOOST_AUTO_TEST_CASE(test_coap_server_network_server_concept, * boost::unit_test::timeout(15)) {
    // Static assertion to verify concept satisfaction
    static_assert(kythira::network_server<test_server>, 
                  "coap_server must satisfy network_server concept");
    
    BOOST_TEST_MESSAGE("coap_server satisfies network_server concept");
    BOOST_TEST(true);
}
#endif

// Test RPC serializer integration with coap_client
BOOST_AUTO_TEST_CASE(test_coap_client_rpc_serializer_integration, * boost::unit_test::timeout(15)) {
    // Verify that the serializer satisfies rpc_serializer concept
    static_assert(raft::rpc_serializer<test_serializer, std::vector<std::byte>>, 
                  "json_rpc_serializer must satisfy rpc_serializer concept");
    
#ifdef LIBCOAP_AVAILABLE
    // Test client instantiation with serializer
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {test_node_id, test_endpoint}
    };
    
    raft::coap_client_config config;
    test_metrics metrics;
    
    // This should compile without errors if concepts are satisfied
    test_client client(std::move(endpoints), config, metrics);
    
    BOOST_TEST_MESSAGE("coap_client integrates correctly with rpc_serializer");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping client instantiation test");
#endif
    BOOST_TEST(true);
}

// Test RPC serializer integration with coap_server
BOOST_AUTO_TEST_CASE(test_coap_server_rpc_serializer_integration, * boost::unit_test::timeout(15)) {
    // Verify that the serializer satisfies rpc_serializer concept
    static_assert(raft::rpc_serializer<test_serializer, std::vector<std::byte>>, 
                  "json_rpc_serializer must satisfy rpc_serializer concept");
    
#ifdef LIBCOAP_AVAILABLE
    // Test server instantiation with serializer
    raft::coap_server_config config;
    test_metrics metrics;
    
    // This should compile without errors if concepts are satisfied
    test_server server(test_bind_address, test_bind_port, config, metrics);
    
    BOOST_TEST_MESSAGE("coap_server integrates correctly with rpc_serializer");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping server instantiation test");
#endif
    BOOST_TEST(true);
}

// Test metrics concept integration
BOOST_AUTO_TEST_CASE(test_metrics_concept_integration, * boost::unit_test::timeout(15)) {
    // Verify that noop_metrics satisfies metrics concept
    static_assert(raft::metrics<test_metrics>, 
                  "noop_metrics must satisfy metrics concept");
    
#ifdef LIBCOAP_AVAILABLE
    // Test that both client and server can use metrics
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {test_node_id, test_endpoint}
    };
    
    raft::coap_client_config client_config;
    raft::coap_server_config server_config;
    test_metrics client_metrics;
    test_metrics server_metrics;
    
    // These should compile without errors if metrics concept is satisfied
    test_client client(std::move(endpoints), client_config, client_metrics);
    test_server server(test_bind_address, test_bind_port, server_config, server_metrics);
    
    BOOST_TEST_MESSAGE("CoAP transport integrates correctly with metrics concept");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping transport instantiation test");
#endif
    BOOST_TEST(true);
}

// Test network_client concept requirements in detail
BOOST_AUTO_TEST_CASE(test_network_client_concept_requirements, * boost::unit_test::timeout(30)) {
#ifdef LIBCOAP_AVAILABLE
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {test_node_id, test_endpoint}
    };
    
    raft::coap_client_config config;
    test_metrics metrics;
    test_client client(std::move(endpoints), config, metrics);
    
    // Test that all required methods exist and have correct signatures
    std::uint64_t target = test_node_id;
    std::chrono::milliseconds timeout{5000};
    
    // Create test requests
    raft::request_vote_request<> rv_request{1, 2, 3, 4};
    raft::append_entries_request<> ae_request{1, 2, 3, 4, {}, 5};
    raft::install_snapshot_request<> is_request{1, 2, 3, 4, {}};
    
    // Test that methods return the correct future types
    auto rv_future = client.send_request_vote(target, rv_request, timeout);
    auto ae_future = client.send_append_entries(target, ae_request, timeout);
    auto is_future = client.send_install_snapshot(target, is_request, timeout);
    
    // Verify return types (these will be checked at compile time)
    static_assert(std::same_as<decltype(rv_future), folly::Future<raft::request_vote_response<>>>);
    static_assert(std::same_as<decltype(ae_future), folly::Future<raft::append_entries_response<>>>);
    static_assert(std::same_as<decltype(is_future), folly::Future<raft::install_snapshot_response<>>>);
    
    BOOST_TEST_MESSAGE("network_client concept requirements verified");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping network_client method signature test");
#endif
    BOOST_TEST(true);
}

// Test network_server concept requirements in detail
BOOST_AUTO_TEST_CASE(test_network_server_concept_requirements, * boost::unit_test::timeout(30)) {
#ifdef LIBCOAP_AVAILABLE
    raft::coap_server_config config;
    test_metrics metrics;
    test_server server(test_bind_address, test_bind_port, config, metrics);
    
    // Test that all required methods exist and have correct signatures
    
    // Create test handlers
    auto rv_handler = [](const raft::request_vote_request<>& req) -> raft::request_vote_response<> {
        return raft::request_vote_response<>{req.term(), false};
    };
    
    auto ae_handler = [](const raft::append_entries_request<>& req) -> raft::append_entries_response<> {
        return raft::append_entries_response<>{req.term(), false};
    };
    
    auto is_handler = [](const raft::install_snapshot_request<>& req) -> raft::install_snapshot_response<> {
        return raft::install_snapshot_response<>{req.term()};
    };
    
    // Test handler registration methods
    server.register_request_vote_handler(rv_handler);
    server.register_append_entries_handler(ae_handler);
    server.register_install_snapshot_handler(is_handler);
    
    // Test lifecycle methods
    static_assert(std::same_as<decltype(server.start()), void>);
    static_assert(std::same_as<decltype(server.stop()), void>);
    static_assert(std::convertible_to<decltype(server.is_running()), bool>);
    
    BOOST_TEST_MESSAGE("network_server concept requirements verified");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping network_server method signature test");
#endif
    BOOST_TEST(true);
}

// Test that non-conforming types do not satisfy concepts
BOOST_AUTO_TEST_CASE(test_non_conforming_types, * boost::unit_test::timeout(15)) {
    // Test that a non-conforming serializer does not satisfy rpc_serializer concept
    class non_serializer {
    public:
        // Missing required methods
        auto serialize(int x) -> std::vector<std::byte> { return {}; }
    };
    
    static_assert(!raft::rpc_serializer<non_serializer, std::vector<std::byte>>, 
                  "non_serializer must not satisfy rpc_serializer concept");
    
    // Test that a non-conforming metrics class does not satisfy metrics concept
    class non_metrics {
    public:
        // Missing required methods
        auto add_one() -> void {}
    };
    
    static_assert(!raft::metrics<non_metrics>, 
                  "non_metrics must not satisfy metrics concept");
    
    BOOST_TEST_MESSAGE("Non-conforming types correctly rejected by concepts");
    BOOST_TEST(true);
}

// Test template parameter constraints
BOOST_AUTO_TEST_CASE(test_template_parameter_constraints, * boost::unit_test::timeout(15)) {
#ifdef LIBCOAP_AVAILABLE
    // Verify that coap_client and coap_server have proper template constraints
    
    // This should compile - valid template parameters
    using valid_client = raft::coap_client<test_serializer, test_metrics>;
    using valid_server = raft::coap_server<test_serializer, test_metrics>;
    
    // Verify the types are instantiable
    static_assert(std::is_constructible_v<valid_client, 
                  std::unordered_map<std::uint64_t, std::string>, 
                  raft::coap_client_config, 
                  test_metrics>);
    
    static_assert(std::is_constructible_v<valid_server, 
                  std::string, 
                  std::uint16_t, 
                  raft::coap_server_config, 
                  test_metrics>);
    
    BOOST_TEST_MESSAGE("Template parameter constraints verified");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping template constraint test");
#endif
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()