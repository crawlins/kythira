#define BOOST_TEST_MODULE http_config_test
#include <raft/http_transport.hpp>
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(http_config_tests)

// Test client configuration structure
BOOST_AUTO_TEST_CASE(test_client_config_defaults) {
    kythira::cpp_httplib_client_config config;
    
    // Verify default values match design document
    BOOST_CHECK_EQUAL(config.connection_pool_size, 10);
    BOOST_CHECK_EQUAL(config.connection_timeout.count(), 5000);
    BOOST_CHECK_EQUAL(config.request_timeout.count(), 10000);
    BOOST_CHECK_EQUAL(config.keep_alive_timeout.count(), 60000);
    BOOST_CHECK_EQUAL(config.enable_ssl_verification, true);
    BOOST_CHECK_EQUAL(config.ca_cert_path, "");
    BOOST_CHECK_EQUAL(config.user_agent, "raft-cpp-httplib/1.0");
}

// Test client configuration customization
BOOST_AUTO_TEST_CASE(test_client_config_customization) {
    constexpr std::size_t custom_pool_size = 20;
    constexpr std::chrono::milliseconds custom_connection_timeout{10000};
    constexpr std::chrono::milliseconds custom_request_timeout{20000};
    constexpr std::chrono::milliseconds custom_keep_alive_timeout{120000};
    constexpr const char* custom_ca_cert_path = "/etc/ssl/certs/ca-bundle.crt";
    constexpr const char* custom_user_agent = "my-raft-client/2.0";
    
    kythira::cpp_httplib_client_config config;
    config.connection_pool_size = custom_pool_size;
    config.connection_timeout = custom_connection_timeout;
    config.request_timeout = custom_request_timeout;
    config.keep_alive_timeout = custom_keep_alive_timeout;
    config.enable_ssl_verification = false;
    config.ca_cert_path = custom_ca_cert_path;
    config.user_agent = custom_user_agent;
    
    BOOST_CHECK_EQUAL(config.connection_pool_size, custom_pool_size);
    BOOST_CHECK_EQUAL(config.connection_timeout.count(), custom_connection_timeout.count());
    BOOST_CHECK_EQUAL(config.request_timeout.count(), custom_request_timeout.count());
    BOOST_CHECK_EQUAL(config.keep_alive_timeout.count(), custom_keep_alive_timeout.count());
    BOOST_CHECK_EQUAL(config.enable_ssl_verification, false);
    BOOST_CHECK_EQUAL(config.ca_cert_path, custom_ca_cert_path);
    BOOST_CHECK_EQUAL(config.user_agent, custom_user_agent);
}

// Test server configuration structure
BOOST_AUTO_TEST_CASE(test_server_config_defaults) {
    kythira::cpp_httplib_server_config config;
    
    // Verify default values match design document
    BOOST_CHECK_EQUAL(config.max_concurrent_connections, 100);
    BOOST_CHECK_EQUAL(config.max_request_body_size, 10 * 1024 * 1024);  // 10 MB
    BOOST_CHECK_EQUAL(config.request_timeout.count(), 30);
    BOOST_CHECK_EQUAL(config.enable_ssl, false);
    BOOST_CHECK_EQUAL(config.ssl_cert_path, "");
    BOOST_CHECK_EQUAL(config.ssl_key_path, "");
}

// Test server configuration customization
BOOST_AUTO_TEST_CASE(test_server_config_customization) {
    constexpr std::size_t custom_max_connections = 200;
    constexpr std::size_t custom_max_body_size = 20 * 1024 * 1024;  // 20 MB
    constexpr std::chrono::seconds custom_request_timeout{60};
    constexpr const char* custom_ssl_cert_path = "/etc/ssl/certs/server.crt";
    constexpr const char* custom_ssl_key_path = "/etc/ssl/private/server.key";
    
    kythira::cpp_httplib_server_config config;
    config.max_concurrent_connections = custom_max_connections;
    config.max_request_body_size = custom_max_body_size;
    config.request_timeout = custom_request_timeout;
    config.enable_ssl = true;
    config.ssl_cert_path = custom_ssl_cert_path;
    config.ssl_key_path = custom_ssl_key_path;
    
    BOOST_CHECK_EQUAL(config.max_concurrent_connections, custom_max_connections);
    BOOST_CHECK_EQUAL(config.max_request_body_size, custom_max_body_size);
    BOOST_CHECK_EQUAL(config.request_timeout.count(), custom_request_timeout.count());
    BOOST_CHECK_EQUAL(config.enable_ssl, true);
    BOOST_CHECK_EQUAL(config.ssl_cert_path, custom_ssl_cert_path);
    BOOST_CHECK_EQUAL(config.ssl_key_path, custom_ssl_key_path);
}

// Test that configuration structures are copyable
BOOST_AUTO_TEST_CASE(test_config_copyable) {
    kythira::cpp_httplib_client_config client_config;
    client_config.connection_pool_size = 15;
    client_config.user_agent = "test-agent";
    
    auto client_config_copy = client_config;
    BOOST_CHECK_EQUAL(client_config_copy.connection_pool_size, 15);
    BOOST_CHECK_EQUAL(client_config_copy.user_agent, "test-agent");
    
    kythira::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 150;
    server_config.enable_ssl = true;
    
    auto server_config_copy = server_config;
    BOOST_CHECK_EQUAL(server_config_copy.max_concurrent_connections, 150);
    BOOST_CHECK_EQUAL(server_config_copy.enable_ssl, true);
}

BOOST_AUTO_TEST_SUITE_END()
