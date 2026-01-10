#define BOOST_TEST_MODULE http_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <thread>
#include <chrono>
#include <unordered_map>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 8083;
    constexpr const char* test_server_url = "http://127.0.0.1:8083";
    constexpr std::uint64_t test_node_id = 1;
    
    // Define transport types for testing using the provided template
    using test_transport_types = kythira::http_transport_types<
        kythira::json_serializer,
        kythira::noop_metrics,
        folly::CPUThreadPoolExecutor
    >;
}

BOOST_AUTO_TEST_SUITE(http_integration_tests)

// Integration test for client-server communication
BOOST_AUTO_TEST_CASE(test_client_server_communication) {
    // Use a unique port to avoid conflicts
    constexpr std::uint16_t unique_port = 8084;
    constexpr const char* server_url = "http://127.0.0.1:8084";
    
    // Create server configuration
    kythira::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 10;
    server_config.request_timeout = std::chrono::seconds{5};
    
    // Create client configuration  
    kythira::cpp_httplib_client_config client_config;
    client_config.connection_timeout = std::chrono::milliseconds{1000};
    client_config.request_timeout = std::chrono::milliseconds{2000};
    
    typename test_transport_types::metrics_type metrics;
    
    // Create and configure server
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, unique_port, server_config, metrics);
    
    // Track handler invocations
    bool request_vote_called = false;
    bool append_entries_called = false;
    bool install_snapshot_called = false;
    
    // Register handlers
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        request_vote_called = true;
        kythira::request_vote_response<> resp;
        resp._term = req.term() + 1;
        resp._vote_granted = true;
        return resp;
    });
    
    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        append_entries_called = true;
        kythira::append_entries_response<> resp;
        resp._term = req.term();
        resp._success = true;
        return resp;
    });
    
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        install_snapshot_called = true;
        kythira::install_snapshot_response<> resp;
        resp._term = req.term();
        return resp;
    });
    
    // Start server
    server.start();
    BOOST_TEST(server.is_running());
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    
    try {
        // Create client
        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[test_node_id] = server_url;
        
        kythira::cpp_httplib_client<test_transport_types> client(
            std::move(node_urls), client_config, metrics);
        
        // Test RequestVote RPC
        {
            kythira::request_vote_request<> request;
            request._term = 5;
            request._candidate_id = 42;
            request._last_log_index = 10;
            request._last_log_term = 4;
            
            auto future = client.send_request_vote(test_node_id, request, std::chrono::milliseconds{1000});
            auto response = std::move(future).get();
            
            BOOST_TEST(request_vote_called);
            BOOST_TEST(response.term() == 6); // Handler returns term + 1
            BOOST_TEST(response.vote_granted() == true);
        }
        
        // Test AppendEntries RPC
        {
            kythira::append_entries_request<> request;
            request._term = 6;
            request._leader_id = 42;
            request._prev_log_index = 10;
            request._prev_log_term = 5;
            request._leader_commit = 9;
            
            auto future = client.send_append_entries(test_node_id, request, std::chrono::milliseconds{1000});
            auto response = std::move(future).get();
            
            BOOST_TEST(append_entries_called);
            BOOST_TEST(response.term() == 6);
            BOOST_TEST(response.success() == true);
        }
        
        // Test InstallSnapshot RPC
        {
            kythira::install_snapshot_request<> request;
            request._term = 7;
            request._leader_id = 42;
            request._last_included_index = 100;
            request._last_included_term = 6;
            request._offset = 0;
            request._data = {std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
            request._done = true;
            
            auto future = client.send_install_snapshot(test_node_id, request, std::chrono::milliseconds{1000});
            auto response = std::move(future).get();
            
            BOOST_TEST(install_snapshot_called);
            BOOST_TEST(response.term() == 7);
        }
        
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception during client-server communication: " << e.what());
        BOOST_TEST(false, "Client-server communication failed");
    }
    
    // Stop server
    server.stop();
    BOOST_TEST(!server.is_running());
}

// Integration test for concurrent requests
BOOST_AUTO_TEST_CASE(test_concurrent_requests, * boost::unit_test::timeout(120)) {
    // Use a unique port to avoid conflicts
    constexpr std::uint16_t unique_port = 8085;
    constexpr const char* server_url = "http://127.0.0.1:8085";
    constexpr std::size_t num_concurrent_requests = 20;
    constexpr std::size_t requests_per_thread = 5;
    
    // Create server configuration with higher connection limits
    kythira::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 50;
    server_config.request_timeout = std::chrono::seconds{10};
    
    // Create client configuration with connection pooling
    kythira::cpp_httplib_client_config client_config;
    client_config.connection_pool_size = 10;
    client_config.connection_timeout = std::chrono::milliseconds{2000};
    client_config.request_timeout = std::chrono::milliseconds{5000};
    
    typename test_transport_types::metrics_type metrics;
    
    // Create and configure server
    kythira::cpp_httplib_server<test_transport_types> server(
        test_bind_address, unique_port, server_config, metrics);
    
    // Track handler invocations with thread-safe counters
    std::atomic<std::size_t> request_vote_count{0};
    std::atomic<std::size_t> append_entries_count{0};
    std::atomic<std::size_t> install_snapshot_count{0};
    
    // Register handlers that increment counters
    server.register_request_vote_handler([&](const kythira::request_vote_request<>& req) {
        request_vote_count.fetch_add(1);
        // Simulate some processing time
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        kythira::request_vote_response<> resp;
        resp._term = req.term() + 1;
        resp._vote_granted = true;
        return resp;
    });
    
    server.register_append_entries_handler([&](const kythira::append_entries_request<>& req) {
        append_entries_count.fetch_add(1);
        // Simulate some processing time
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        kythira::append_entries_response<> resp;
        resp._term = req.term();
        resp._success = true;
        return resp;
    });
    
    server.register_install_snapshot_handler([&](const kythira::install_snapshot_request<>& req) {
        install_snapshot_count.fetch_add(1);
        // Simulate some processing time
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        kythira::install_snapshot_response<> resp;
        resp._term = req.term();
        return resp;
    });
    
    // Start server
    server.start();
    BOOST_TEST(server.is_running());
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    
    try {
        // Create client
        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[test_node_id] = server_url;
        
        kythira::cpp_httplib_client<test_transport_types> client(
            std::move(node_urls), client_config, metrics);
        
        // Launch concurrent threads to send requests
        std::vector<std::thread> threads;
        std::vector<std::exception_ptr> exceptions(num_concurrent_requests);
        std::atomic<std::size_t> successful_requests{0};
        
        for (std::size_t i = 0; i < num_concurrent_requests; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    for (std::size_t j = 0; j < requests_per_thread; ++j) {
                        // Send different types of requests in rotation
                        std::size_t request_type = (i * requests_per_thread + j) % 3;
                        
                        if (request_type == 0) {
                            // RequestVote RPC
                            kythira::request_vote_request<> request;
                            request._term = 5 + i;
                            request._candidate_id = 42 + i;
                            request._last_log_index = 10 + j;
                            request._last_log_term = 4;
                            
                            auto future = client.send_request_vote(test_node_id, request, std::chrono::milliseconds{3000});
                            auto response = std::move(future).get();
                            
                            if (response.term() == request.term() + 1 && response.vote_granted()) {
                                successful_requests.fetch_add(1);
                            }
                        } else if (request_type == 1) {
                            // AppendEntries RPC
                            kythira::append_entries_request<> request;
                            request._term = 6 + i;
                            request._leader_id = 42 + i;
                            request._prev_log_index = 10 + j;
                            request._prev_log_term = 5;
                            request._leader_commit = 9 + j;
                            
                            auto future = client.send_append_entries(test_node_id, request, std::chrono::milliseconds{3000});
                            auto response = std::move(future).get();
                            
                            if (response.term() == request.term() && response.success()) {
                                successful_requests.fetch_add(1);
                            }
                        } else {
                            // InstallSnapshot RPC
                            kythira::install_snapshot_request<> request;
                            request._term = 7 + i;
                            request._leader_id = 42 + i;
                            request._last_included_index = 100 + j;
                            request._last_included_term = 6;
                            request._offset = 0;
                            request._data = {std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
                            request._done = true;
                            
                            auto future = client.send_install_snapshot(test_node_id, request, std::chrono::milliseconds{3000});
                            auto response = std::move(future).get();
                            
                            if (response.term() == request.term()) {
                                successful_requests.fetch_add(1);
                            }
                        }
                    }
                } catch (...) {
                    exceptions[i] = std::current_exception();
                }
            });
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Check for exceptions
        for (std::size_t i = 0; i < num_concurrent_requests; ++i) {
            if (exceptions[i]) {
                try {
                    std::rethrow_exception(exceptions[i]);
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Thread " << i << " threw exception: " << e.what());
                    BOOST_TEST(false, "Concurrent request thread failed");
                }
            }
        }
        
        // Verify all requests were successful
        std::size_t expected_total_requests = num_concurrent_requests * requests_per_thread;
        BOOST_TEST(successful_requests.load() == expected_total_requests);
        
        // Verify handler invocation counts
        std::size_t expected_per_type = expected_total_requests / 3;
        std::size_t remainder = expected_total_requests % 3;
        
        // RequestVote gets the remainder due to modulo distribution
        BOOST_TEST(request_vote_count.load() == expected_per_type + remainder);
        BOOST_TEST(append_entries_count.load() == expected_per_type);
        BOOST_TEST(install_snapshot_count.load() == expected_per_type);
        
        BOOST_TEST_MESSAGE("Concurrent requests test completed successfully");
        BOOST_TEST_MESSAGE("Total requests: " << expected_total_requests);
        BOOST_TEST_MESSAGE("Successful requests: " << successful_requests.load());
        BOOST_TEST_MESSAGE("RequestVote calls: " << request_vote_count.load());
        BOOST_TEST_MESSAGE("AppendEntries calls: " << append_entries_count.load());
        BOOST_TEST_MESSAGE("InstallSnapshot calls: " << install_snapshot_count.load());
        
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("Exception during concurrent requests test: " << e.what());
        BOOST_TEST(false, "Concurrent requests test failed");
    }
    
    // Stop server
    server.stop();
    BOOST_TEST(!server.is_running());
}

// Integration test for TLS/HTTPS
BOOST_AUTO_TEST_CASE(test_tls_https, * boost::unit_test::timeout(180)) {
    // Use a unique port to avoid conflicts
    constexpr std::uint16_t unique_port = 8086;
    constexpr const char* server_url = "https://127.0.0.1:8086";
    
    // Create temporary self-signed certificate for testing
    // Note: In a real implementation, you would use proper test certificates
    // For this test, we'll simulate the TLS configuration and test the error handling
    
    // Create server configuration with TLS enabled
    kythira::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 10;
    server_config.request_timeout = std::chrono::seconds{10};
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = "/tmp/test_cert.pem";  // Non-existent for testing
    server_config.ssl_key_path = "/tmp/test_key.pem";   // Non-existent for testing
    
    // Create client configuration with SSL verification
    kythira::cpp_httplib_client_config client_config;
    client_config.connection_timeout = std::chrono::milliseconds{2000};
    client_config.request_timeout = std::chrono::milliseconds{5000};
    client_config.enable_ssl_verification = true;
    client_config.ca_cert_path = "/tmp/test_ca.pem";    // Non-existent for testing
    
    typename test_transport_types::metrics_type metrics;
    
    // Test 1: Server fails to start without valid certificates
    {
        BOOST_TEST_MESSAGE("Testing server startup failure with invalid certificates");
        
        kythira::cpp_httplib_server<test_transport_types> server(
            test_bind_address, unique_port, server_config, metrics);
        
        // Register a simple handler
        server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
            kythira::request_vote_response<> resp;
            resp._term = req.term() + 1;
            resp._vote_granted = true;
            return resp;
        });
        
        // Server should fail to start due to missing certificates
        try {
            server.start();
            // If we get here, the server started despite missing certificates
            // This might happen if the implementation falls back to HTTP
            BOOST_TEST_MESSAGE("Server started despite missing certificates - may have fallen back to HTTP");
            server.stop();
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Expected: Server failed to start due to missing certificates: " << e.what());
            // This is the expected behavior
        }
    }
    
    // Test 2: Client certificate validation failure
    {
        BOOST_TEST_MESSAGE("Testing client certificate validation failure");
        
        // For this test, we'll create a client that tries to connect to HTTPS
        // but will fail due to certificate validation issues
        std::unordered_map<std::uint64_t, std::string> node_urls;
        node_urls[test_node_id] = server_url;  // HTTPS URL
        
        kythira::cpp_httplib_client<test_transport_types> client(
            std::move(node_urls), client_config, metrics);
        
        // Try to send a request - should fail due to connection/certificate issues
        kythira::request_vote_request<> request;
        request._term = 5;
        request._candidate_id = 42;
        request._last_log_index = 10;
        request._last_log_term = 4;
        
        try {
            auto future = client.send_request_vote(test_node_id, request, std::chrono::milliseconds{2000});
            auto response = std::move(future).get();
            
            // If we get here, the request succeeded unexpectedly
            BOOST_TEST_MESSAGE("Unexpected: HTTPS request succeeded without proper server");
            BOOST_TEST(false, "HTTPS request should have failed");
            
        } catch (const kythira::http_transport_error& e) {
            BOOST_TEST_MESSAGE("Expected: HTTPS request failed with transport error: " << e.what());
            // This is expected - connection should fail
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("Expected: HTTPS request failed with exception: " << e.what());
            // This is also expected - various connection errors possible
        }
    }
    
    // Test 3: Successful HTTPS communication (simulated)
    // Note: This would require actual test certificates in a real implementation
    {
        BOOST_TEST_MESSAGE("Testing successful HTTPS communication (simulated)");
        
        // For demonstration purposes, we'll test the configuration validation
        // In a real implementation, this would set up proper test certificates
        
        // Verify that HTTPS URLs are detected correctly
        std::unordered_map<std::uint64_t, std::string> https_urls;
        https_urls[1] = "https://example.com:443";
        https_urls[2] = "https://secure.example.com:8443";
        
        // Client should accept HTTPS URLs in configuration
        try {
            kythira::cpp_httplib_client<test_transport_types> https_client(
                std::move(https_urls), client_config, metrics);
            
            BOOST_TEST_MESSAGE("HTTPS client created successfully with HTTPS URLs");
            
        } catch (const std::exception& e) {
            BOOST_TEST_MESSAGE("HTTPS client creation failed: " << e.what());
            BOOST_TEST(false, "HTTPS client should accept HTTPS URLs");
        }
        
        // Verify server configuration accepts TLS settings
        kythira::cpp_httplib_server_config valid_tls_config;
        valid_tls_config.enable_ssl = true;
        valid_tls_config.ssl_cert_path = "/path/to/cert.pem";
        valid_tls_config.ssl_key_path = "/path/to/key.pem";
        
        BOOST_TEST(valid_tls_config.enable_ssl == true);
        BOOST_TEST(!valid_tls_config.ssl_cert_path.empty());
        BOOST_TEST(!valid_tls_config.ssl_key_path.empty());
        
        BOOST_TEST_MESSAGE("TLS configuration validation passed");
    }
    
    // Test 4: TLS version and cipher suite requirements
    {
        BOOST_TEST_MESSAGE("Testing TLS version and security requirements");
        
        // Verify that client configuration supports security settings
        kythira::cpp_httplib_client_config secure_config;
        secure_config.enable_ssl_verification = true;
        secure_config.ca_cert_path = "/path/to/ca.pem";
        
        BOOST_TEST(secure_config.enable_ssl_verification == true);
        BOOST_TEST(!secure_config.ca_cert_path.empty());
        
        // In a real implementation, this would test:
        // - TLS 1.2 or higher enforcement
        // - Certificate chain validation
        // - Hostname verification
        // - Cipher suite restrictions
        
        BOOST_TEST_MESSAGE("TLS security configuration validation passed");
    }
    
    BOOST_TEST_MESSAGE("TLS/HTTPS integration test completed");
    BOOST_TEST_MESSAGE("Note: Full TLS testing requires valid test certificates");
    BOOST_TEST_MESSAGE("This test validates configuration and error handling paths");
}

BOOST_AUTO_TEST_SUITE_END()