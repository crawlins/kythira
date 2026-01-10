#define BOOST_TEST_MODULE http_exceptions_test
#include <boost/test/unit_test.hpp>

#include <raft/http_exceptions.hpp>

#include <stdexcept>
#include <string>

BOOST_AUTO_TEST_SUITE(http_exceptions_test)

// Test http_transport_error base class
BOOST_AUTO_TEST_CASE(test_http_transport_error_construction, * boost::unit_test::timeout(15)) {
    const std::string error_message = "Transport error occurred";
    kythira::http_transport_error error(error_message);
    
    BOOST_CHECK_EQUAL(error.what(), error_message);
}

BOOST_AUTO_TEST_CASE(test_http_transport_error_inheritance, * boost::unit_test::timeout(15)) {
    kythira::http_transport_error error("Test error");
    
    // Should be catchable as std::runtime_error
    try {
        throw error;
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Test error");
    }
}

// Test http_client_error (4xx status codes)
BOOST_AUTO_TEST_CASE(test_http_client_error_construction, * boost::unit_test::timeout(15)) {
    constexpr int status_code_404 = 404;
    const std::string error_message = "Not Found";
    kythira::http_client_error error(status_code_404, error_message);
    
    BOOST_CHECK_EQUAL(error.status_code(), status_code_404);
    BOOST_CHECK_EQUAL(error.what(), error_message);
}

BOOST_AUTO_TEST_CASE(test_http_client_error_inheritance, * boost::unit_test::timeout(15)) {
    constexpr int status_code_400 = 400;
    kythira::http_client_error error(status_code_400, "Bad Request");
    
    // Should be catchable as http_transport_error
    try {
        throw error;
    } catch (const kythira::http_transport_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Bad Request");
    }
}

BOOST_AUTO_TEST_CASE(test_http_client_error_various_status_codes, * boost::unit_test::timeout(15)) {
    constexpr int status_code_400 = 400;
    constexpr int status_code_401 = 401;
    constexpr int status_code_403 = 403;
    constexpr int status_code_404 = 404;
    constexpr int status_code_429 = 429;
    
    kythira::http_client_error error_400(status_code_400, "Bad Request");
    kythira::http_client_error error_401(status_code_401, "Unauthorized");
    kythira::http_client_error error_403(status_code_403, "Forbidden");
    kythira::http_client_error error_404(status_code_404, "Not Found");
    kythira::http_client_error error_429(status_code_429, "Too Many Requests");
    
    BOOST_CHECK_EQUAL(error_400.status_code(), status_code_400);
    BOOST_CHECK_EQUAL(error_401.status_code(), status_code_401);
    BOOST_CHECK_EQUAL(error_403.status_code(), status_code_403);
    BOOST_CHECK_EQUAL(error_404.status_code(), status_code_404);
    BOOST_CHECK_EQUAL(error_429.status_code(), status_code_429);
}

// Test http_server_error (5xx status codes)
BOOST_AUTO_TEST_CASE(test_http_server_error_construction, * boost::unit_test::timeout(15)) {
    constexpr int status_code_500 = 500;
    const std::string error_message = "Internal Server Error";
    kythira::http_server_error error(status_code_500, error_message);
    
    BOOST_CHECK_EQUAL(error.status_code(), status_code_500);
    BOOST_CHECK_EQUAL(error.what(), error_message);
}

BOOST_AUTO_TEST_CASE(test_http_server_error_inheritance, * boost::unit_test::timeout(15)) {
    constexpr int status_code_503 = 503;
    kythira::http_server_error error(status_code_503, "Service Unavailable");
    
    // Should be catchable as http_transport_error
    try {
        throw error;
    } catch (const kythira::http_transport_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Service Unavailable");
    }
}

BOOST_AUTO_TEST_CASE(test_http_server_error_various_status_codes, * boost::unit_test::timeout(15)) {
    constexpr int status_code_500 = 500;
    constexpr int status_code_502 = 502;
    constexpr int status_code_503 = 503;
    constexpr int status_code_504 = 504;
    
    kythira::http_server_error error_500(status_code_500, "Internal Server Error");
    kythira::http_server_error error_502(status_code_502, "Bad Gateway");
    kythira::http_server_error error_503(status_code_503, "Service Unavailable");
    kythira::http_server_error error_504(status_code_504, "Gateway Timeout");
    
    BOOST_CHECK_EQUAL(error_500.status_code(), status_code_500);
    BOOST_CHECK_EQUAL(error_502.status_code(), status_code_502);
    BOOST_CHECK_EQUAL(error_503.status_code(), status_code_503);
    BOOST_CHECK_EQUAL(error_504.status_code(), status_code_504);
}

// Test http_timeout_error
BOOST_AUTO_TEST_CASE(test_http_timeout_error_construction, * boost::unit_test::timeout(15)) {
    const std::string error_message = "Request timeout after 5000ms";
    kythira::http_timeout_error error(error_message);
    
    BOOST_CHECK_EQUAL(error.what(), error_message);
}

BOOST_AUTO_TEST_CASE(test_http_timeout_error_inheritance, * boost::unit_test::timeout(15)) {
    kythira::http_timeout_error error("Connection timeout");
    
    // Should be catchable as http_transport_error
    try {
        throw error;
    } catch (const kythira::http_transport_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Connection timeout");
    }
}

// Test serialization_error
BOOST_AUTO_TEST_CASE(test_serialization_error_construction, * boost::unit_test::timeout(15)) {
    const std::string error_message = "Failed to deserialize JSON response";
    kythira::serialization_error error(error_message);
    
    BOOST_CHECK_EQUAL(error.what(), error_message);
}

BOOST_AUTO_TEST_CASE(test_serialization_error_inheritance, * boost::unit_test::timeout(15)) {
    kythira::serialization_error error("Invalid JSON format");
    
    // Should be catchable as http_transport_error
    try {
        throw error;
    } catch (const kythira::http_transport_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "Invalid JSON format");
    }
}

// Test exception hierarchy
BOOST_AUTO_TEST_CASE(test_exception_hierarchy, * boost::unit_test::timeout(15)) {
    constexpr int status_code_404 = 404;
    constexpr int status_code_500 = 500;
    
    // All exceptions should be catchable as http_transport_error
    try {
        throw kythira::http_client_error(status_code_404, "Not Found");
    } catch (const kythira::http_transport_error&) {
        // Expected
    }
    
    try {
        throw kythira::http_server_error(status_code_500, "Internal Error");
    } catch (const kythira::http_transport_error&) {
        // Expected
    }
    
    try {
        throw kythira::http_timeout_error("Timeout");
    } catch (const kythira::http_transport_error&) {
        // Expected
    }
    
    try {
        throw kythira::serialization_error("Serialization failed");
    } catch (const kythira::http_transport_error&) {
        // Expected
    }
    
    // All exceptions should be catchable as std::runtime_error
    try {
        throw kythira::http_transport_error("Base error");
    } catch (const std::runtime_error&) {
        // Expected
    }
}

// Test that status_code is preserved correctly
BOOST_AUTO_TEST_CASE(test_status_code_preservation, * boost::unit_test::timeout(15)) {
    constexpr int client_status = 418;  // I'm a teapot
    constexpr int server_status = 507;  // Insufficient Storage
    
    kythira::http_client_error client_error(client_status, "Teapot error");
    kythira::http_server_error server_error(server_status, "Storage error");
    
    BOOST_CHECK_EQUAL(client_error.status_code(), client_status);
    BOOST_CHECK_EQUAL(server_error.status_code(), server_status);
}

BOOST_AUTO_TEST_SUITE_END()
