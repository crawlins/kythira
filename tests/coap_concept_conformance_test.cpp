#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_concept_conformance_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>

// Only include CoAP transport if libcoap is available
#ifdef LIBCOAP_AVAILABLE
#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>

#endif

namespace {
constexpr const char* test_name = "coap_concept_conformance_test";
constexpr const char* test_bind_address = "127.0.0.1";
// Servers built with this port in this file are never start()-ed (only
// constructed/type-checked), so no real bind ever occurs; 0 keeps that
// invariant explicit rather than relying on a literal that looks live.
constexpr std::uint16_t test_bind_port = 0;
constexpr std::uint64_t test_node_id = 1;
// Client-only target -- some tests below do call real send_* methods
// against this endpoint, but no coap_server anywhere in this file is ever
// started(), so nothing ever listens here regardless of the port number.
// A fixed literal well outside the ephemeral range (32768-60999) and
// distinct from every other coap_*_test.cpp's assigned literal is safe.
constexpr const char* test_endpoint = "coap://127.0.0.1:61030";

// Test serializer type alias
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using test_metrics = kythira::noop_metrics;

#ifdef LIBCOAP_AVAILABLE
// coap_client has three RPCs with three distinct Response types, so Types::
// future_template<T> must be genuinely parameterized over T. The deprecated
// default_transport_types alias always yields the single FutureType it was
// constructed with regardless of T (see its doc comment in
// coap_transport.hpp), which only works for request_vote -- send_append_
// entries()/send_install_snapshot() would return a Future<append_entries_
// response<>>/Future<install_snapshot_response<>> that can't convert to the
// fixed Future<request_vote_response<>> the function is declared to return.
// This local struct matches the pattern used by every other coap_*_test.cpp
// (e.g. coap_content_format_property_test.cpp).
struct test_types {
    using serializer_type = test_serializer;
    using rpc_serializer_type = test_serializer;
    using metrics_type = test_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};
using test_client = kythira::coap_client<test_types>;
using test_server = kythira::coap_server<test_types>;
#endif
}

BOOST_AUTO_TEST_SUITE(coap_concept_conformance_tests)

#ifdef LIBCOAP_AVAILABLE
// Test that coap_client satisfies network_client concept
BOOST_AUTO_TEST_CASE(test_coap_client_network_client_concept,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Static assertion to verify concept satisfaction
    static_assert(kythira::network_client<test_client>,
                  "coap_client must satisfy network_client concept");

    BOOST_TEST_MESSAGE("coap_client satisfies network_client concept");
    BOOST_TEST(true);
}

// Test that coap_server satisfies network_server concept
BOOST_AUTO_TEST_CASE(test_coap_server_network_server_concept,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Static assertion to verify concept satisfaction
    static_assert(kythira::network_server<test_server>,
                  "coap_server must satisfy network_server concept");

    BOOST_TEST_MESSAGE("coap_server satisfies network_server concept");
    BOOST_TEST(true);
}
#endif

// Test RPC serializer integration with coap_client
BOOST_AUTO_TEST_CASE(test_coap_client_rpc_serializer_integration,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Verify that the serializer satisfies rpc_serializer concept
    static_assert(kythira::rpc_serializer<test_serializer, std::vector<std::byte>>,
                  "json_rpc_serializer must satisfy rpc_serializer concept");

#ifdef LIBCOAP_AVAILABLE
    // Test client instantiation with serializer
    std::unordered_map<std::uint64_t, std::string> endpoints = {{test_node_id, test_endpoint}};

    kythira::coap_client_config config;
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
BOOST_AUTO_TEST_CASE(test_coap_server_rpc_serializer_integration,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Verify that the serializer satisfies rpc_serializer concept
    static_assert(kythira::rpc_serializer<test_serializer, std::vector<std::byte>>,
                  "json_rpc_serializer must satisfy rpc_serializer concept");

#ifdef LIBCOAP_AVAILABLE
    // Test server instantiation with serializer
    kythira::coap_server_config config;
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
BOOST_AUTO_TEST_CASE(test_metrics_concept_integration,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Verify that noop_metrics satisfies metrics concept
    static_assert(kythira::metrics<test_metrics>, "noop_metrics must satisfy metrics concept");

#ifdef LIBCOAP_AVAILABLE
    // Test that both client and server can use metrics
    std::unordered_map<std::uint64_t, std::string> endpoints = {{test_node_id, test_endpoint}};

    kythira::coap_client_config client_config;
    kythira::coap_server_config server_config;
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
BOOST_AUTO_TEST_CASE(test_network_client_concept_requirements,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
#ifdef LIBCOAP_AVAILABLE
    std::unordered_map<std::uint64_t, std::string> endpoints = {{test_node_id, test_endpoint}};

    kythira::coap_client_config config;
    test_metrics metrics;
    test_client client(std::move(endpoints), config, metrics);

    // Test that all required methods exist and have correct signatures
    std::uint64_t target = test_node_id;
    std::chrono::milliseconds timeout{5000};

    // Create test requests
    kythira::request_vote_request<> rv_request{1, 2, 3, 4};
    kythira::append_entries_request<> ae_request{1, 2, 3, 4, {}, 5};
    kythira::install_snapshot_request<> is_request{1, 2, 3, 4, {}};

    // Test that methods return the correct future types
    auto rv_future = client.send_request_vote(target, rv_request, timeout);
    auto ae_future = client.send_append_entries(target, ae_request, timeout);
    auto is_future = client.send_install_snapshot(target, is_request, timeout);

    // Verify return types (these will be checked at compile time)
    static_assert(std::same_as<decltype(rv_future),
                               kythira::future_default<kythira::request_vote_response<>>>);
    static_assert(std::same_as<decltype(ae_future),
                               kythira::future_default<kythira::append_entries_response<>>>);
    static_assert(std::same_as<decltype(is_future),
                               kythira::future_default<kythira::install_snapshot_response<>>>);

    BOOST_TEST_MESSAGE("network_client concept requirements verified");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping network_client method signature test");
#endif
    BOOST_TEST(true);
}

// Test network_server concept requirements in detail
BOOST_AUTO_TEST_CASE(test_network_server_concept_requirements,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
#ifdef LIBCOAP_AVAILABLE
    kythira::coap_server_config config;
    test_metrics metrics;
    test_server server(test_bind_address, test_bind_port, config, metrics);

    // Test that all required methods exist and have correct signatures

    // Create test handlers
    auto rv_handler =
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
        return kythira::request_vote_response<>{req.term(), false};
    };

    auto ae_handler =
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
        return kythira::append_entries_response<>{req.term(), false};
    };

    auto is_handler =
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
        return kythira::install_snapshot_response<>{req.term()};
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
BOOST_AUTO_TEST_CASE(test_non_conforming_types,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Test that a non-conforming serializer does not satisfy rpc_serializer concept
    // Make it not satisfy by using a non-byte data type
    class non_serializer {
    public:
        // Missing required methods
        auto serialize(int x) -> std::vector<std::byte> { return {}; }
    };

    static_assert(!kythira::rpc_serializer<non_serializer, std::vector<int>>,
                  "non_serializer must not satisfy rpc_serializer concept");

    // Test that a non-conforming metrics class does not satisfy metrics concept
    class non_metrics {
    public:
        // Missing required methods
        auto add_one() -> void {}
    };

    static_assert(!kythira::metrics<non_metrics>, "non_metrics must not satisfy metrics concept");

    BOOST_TEST_MESSAGE("Non-conforming types correctly rejected by concepts");
    BOOST_TEST(true);
}

// Test template parameter constraints
BOOST_AUTO_TEST_CASE(test_template_parameter_constraints,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
#ifdef LIBCOAP_AVAILABLE
    // Verify that coap_client and coap_server have proper template constraints

    // This should compile - valid template parameters
    using valid_client = kythira::coap_client<test_types>;
    using valid_server = kythira::coap_server<test_types>;

    // Verify the types are instantiable
    static_assert(
        std::is_constructible_v<valid_client, std::unordered_map<std::uint64_t, std::string>,
                                kythira::coap_client_config, test_metrics>);

    static_assert(std::is_constructible_v<valid_server, std::string, std::uint16_t,
                                          kythira::coap_server_config, test_metrics>);

    BOOST_TEST_MESSAGE("Template parameter constraints verified");
#else
    BOOST_TEST_MESSAGE("libcoap not available - skipping template constraint test");
#endif
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()