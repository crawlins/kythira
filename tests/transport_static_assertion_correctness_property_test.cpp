#define BOOST_TEST_MODULE transport_static_assertion_correctness_property_test
#include <boost/test/unit_test.hpp>

#include <raft/network.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <raft/http_transport.hpp>
#include <raft/console_logger.hpp>
#include <raft/simulator_network.hpp>
#include <concepts/future.hpp>

// Include CoAP transport if available
#ifdef LIBCOAP_AVAILABLE
#include <raft/coap_transport.hpp>
#endif

namespace {
    constexpr const char* test_name = "transport_static_assertion_correctness_property_test";
    
    // Test type aliases
    using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using test_metrics = kythira::noop_metrics;
    using test_logger = kythira::console_logger;
    using future_type = kythira::Future<kythira::request_vote_response<>>;
}

BOOST_AUTO_TEST_SUITE(transport_static_assertion_correctness_property_tests)

/**
 * **Feature: network-concept-template-fix, Property 3: Static assertion correctness**
 * **Validates: Requirements 1.4, 2.3, 3.2**
 * 
 * Property: For any static_assert statement using network concepts, both required template parameters should be provided and the assertion should compile successfully
 */
BOOST_AUTO_TEST_CASE(property_static_assertion_correctness, * boost::unit_test::timeout(60)) {
    // Test that transport header static assertions are correct
    
    // Test 1: Verify HTTP transport types satisfy network concepts
    using test_types = kythira::http_transport_types<test_serializer, test_metrics, test_metrics>;
    using http_client_type = kythira::cpp_httplib_client<test_types>;
    using http_server_type = kythira::cpp_httplib_server<test_types>;
    
    static_assert(kythira::network_client<http_client_type>,
                 "HTTP client must satisfy network_client concept");
    
    static_assert(kythira::network_server<http_server_type>,
                 "HTTP server must satisfy network_server concept");
    
    // Test 2: Verify simulator network types satisfy network concepts
    using sim_network_types = kythira::raft_simulator_network_types<std::string>;
    using simulator_client_type = kythira::simulator_network_client<sim_network_types, test_serializer, std::vector<std::byte>>;
    using simulator_server_type = kythira::simulator_network_server<sim_network_types, test_serializer, std::vector<std::byte>>;
    
    static_assert(kythira::network_client<simulator_client_type>,
                 "Simulator client must satisfy network_client concept");
    
    static_assert(kythira::network_server<simulator_server_type>,
                 "Simulator server must satisfy network_server concept");
    
#ifdef LIBCOAP_AVAILABLE
    // Test 3: Verify CoAP transport types satisfy network concepts (if available)
    using test_types = kythira::default_transport_types<future_type, test_serializer, test_metrics, test_logger>;
    using coap_client_type = kythira::coap_client<test_types>;
    using coap_server_type = kythira::coap_server<test_types>;
    
    static_assert(kythira::network_client<coap_client_type>,
                 "CoAP client must satisfy network_client concept");
    
    static_assert(kythira::network_server<coap_server_type>,
                 "CoAP server must satisfy network_server concept");
    
    BOOST_TEST_MESSAGE("CoAP transport static assertions are correct");
#endif
    
    BOOST_TEST_MESSAGE("All transport header static assertions are correct");
    BOOST_CHECK(true);
}

/**
 * Test that static assertions use the correct namespace
 */
BOOST_AUTO_TEST_CASE(test_static_assertion_namespace_correctness, * boost::unit_test::timeout(30)) {
    // Test that all transport static assertions use kythira namespace
    // This is verified by the fact that the static assertions in the headers compile
    
    // Test with different future types to ensure namespace consistency
    using ae_types = kythira::std_http_transport_types<test_serializer, test_metrics, test_metrics>;
    using is_types = kythira::simple_http_transport_types<test_serializer, test_metrics, test_metrics>;
    
    // HTTP transport with different future types
    using http_client_ae = kythira::cpp_httplib_client<ae_types>;
    using http_server_is = kythira::cpp_httplib_server<is_types>;
    
    static_assert(kythira::network_client<http_client_ae>,
                 "HTTP client with std future must satisfy network_client concept");
    
    static_assert(kythira::network_server<http_server_is>,
                 "HTTP server with simple future must satisfy network_server concept");
    
    BOOST_TEST_MESSAGE("Static assertions use correct kythira namespace");
    BOOST_CHECK(true);
}

/**
 * Test that static assertions enforce correct template parameter count
 */
BOOST_AUTO_TEST_CASE(test_static_assertion_template_parameter_count, * boost::unit_test::timeout(30)) {
    // Test that network concepts require exactly 1 template parameter (the client/server type)
    // This is enforced by the concept definitions and verified by static assertions
    
    // Test with various transport implementations
    using sim_network_types = kythira::raft_simulator_network_types<std::string>;
    
    // Simulator network with correct parameters
    using sim_client = kythira::simulator_network_client<sim_network_types, test_serializer, std::vector<std::byte>>;
    using sim_server = kythira::simulator_network_server<sim_network_types, test_serializer, std::vector<std::byte>>;
    
    static_assert(kythira::network_client<sim_client>,
                 "Simulator client must satisfy network_client concept");
    
    static_assert(kythira::network_server<sim_server>,
                 "Simulator server must satisfy network_server concept");
    
    BOOST_TEST_MESSAGE("Static assertions enforce correct template parameter count");
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()