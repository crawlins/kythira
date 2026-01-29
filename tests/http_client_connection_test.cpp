#define BOOST_TEST_MODULE http_client_connection_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
    constexpr const char* test_server_url = "http://httpbin.org";  // Public test server
    constexpr std::uint64_t test_node_id = 1;
    constexpr auto test_timeout = std::chrono::milliseconds{5000};
    
    // Define transport types for testing
    using test_transport_types = kythira::http_transport_types<
        kythira::json_rpc_serializer<std::vector<std::byte>>,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
}

BOOST_AUTO_TEST_SUITE(http_client_connection_tests)

// Test that client can handle actual HTTP requests (even if they fail)
BOOST_AUTO_TEST_CASE(test_actual_http_request_handling) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = test_server_url;
    
    kythira::cpp_httplib_client_config config;
    config.connection_timeout = std::chrono::milliseconds{2000};
    config.request_timeout = std::chrono::milliseconds{5000};
    
    kythira::noop_metrics metrics;
    
    kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
    
    // Create a test RequestVote request
    kythira::request_vote_request<> request;
    request._term = 1;
    request._candidate_id = 2;
    request._last_log_index = 0;
    request._last_log_term = 0;
    
    // This should fail because httpbin.org doesn't have our Raft endpoints
    // But it should fail gracefully with an HTTP error, not a crash
    bool exception_caught = false;
    std::string error_message;
    
    try {
        auto future = client.send_request_vote(test_node_id, request, test_timeout);
        auto response = std::move(future).get();
        
        // If we get here, something unexpected happened
        BOOST_TEST_MESSAGE("Unexpected success - httpbin.org responded to Raft RPC");
    } catch (const kythira::http_client_error& e) {
        // Expected: 404 Not Found or similar client error
        exception_caught = true;
        error_message = e.what();
        BOOST_TEST_MESSAGE("Caught expected HTTP client error: " << error_message);
        BOOST_CHECK(e.status_code() >= 400 && e.status_code() < 500);
    } catch (const kythira::http_server_error& e) {
        // Possible: 500 Internal Server Error
        exception_caught = true;
        error_message = e.what();
        BOOST_TEST_MESSAGE("Caught HTTP server error: " << error_message);
        BOOST_CHECK(e.status_code() >= 500);
    } catch (const std::exception& e) {
        // Other errors (connection, timeout, etc.)
        exception_caught = true;
        error_message = e.what();
        BOOST_TEST_MESSAGE("Caught other exception: " << error_message);
    }
    
    // We expect some kind of error since httpbin.org doesn't have Raft endpoints
    BOOST_CHECK(exception_caught);
    BOOST_CHECK(!error_message.empty());
}

// Test connection to non-existent server
BOOST_AUTO_TEST_CASE(test_connection_to_nonexistent_server) {
    std::unordered_map<std::uint64_t, std::string> node_map;
    node_map[test_node_id] = "http://nonexistent.example.com:9999";
    
    kythira::cpp_httplib_client_config config;
    config.connection_timeout = std::chrono::milliseconds{1000};
    config.request_timeout = std::chrono::milliseconds{2000};
    
    kythira::noop_metrics metrics;
    
    kythira::cpp_httplib_client<test_transport_types> client(node_map, config, metrics);
    
    // Create a test RequestVote request
    kythira::request_vote_request<> request;
    request._term = 1;
    request._candidate_id = 2;
    request._last_log_index = 0;
    request._last_log_term = 0;
    
    // This should fail with a connection error
    bool exception_caught = false;
    std::string error_message;
    
    try {
        auto future = client.send_request_vote(test_node_id, request, test_timeout);
        auto response = std::move(future).get();
        
        BOOST_FAIL("Expected connection failure, but request succeeded");
    } catch (const std::exception& e) {
        exception_caught = true;
        error_message = e.what();
        BOOST_TEST_MESSAGE("Caught expected exception: " << error_message);
        
        // Should be a connection-related error
        BOOST_CHECK(error_message.find("failed") != std::string::npos ||
                   error_message.find("refused") != std::string::npos ||
                   error_message.find("connect") != std::string::npos ||
                   error_message.find("resolve") != std::string::npos);
    }
    
    BOOST_CHECK(exception_caught);
}

BOOST_AUTO_TEST_SUITE_END()