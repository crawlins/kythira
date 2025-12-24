#define BOOST_TEST_MODULE http_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>

#include <thread>
#include <chrono>
#include <unordered_map>

namespace {
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 8083;
    constexpr const char* test_server_url = "http://127.0.0.1:8083";
    constexpr std::uint64_t test_node_id = 1;
}

BOOST_AUTO_TEST_SUITE(http_integration_tests)

// Integration test for client-server communication
BOOST_AUTO_TEST_CASE(test_client_server_communication) {
    // Use a unique port to avoid conflicts
    constexpr std::uint16_t unique_port = 8084;
    constexpr const char* server_url = "http://127.0.0.1:8084";
    
    // Create server configuration
    raft::cpp_httplib_server_config server_config;
    server_config.max_concurrent_connections = 10;
    server_config.request_timeout = std::chrono::seconds{5};
    
    // Create client configuration  
    raft::cpp_httplib_client_config client_config;
    client_config.connection_timeout = std::chrono::milliseconds{1000};
    client_config.request_timeout = std::chrono::milliseconds{2000};
    
    raft::noop_metrics metrics;
    
    // Create and configure server
    raft::cpp_httplib_server<raft::json_serializer, raft::noop_metrics> server(
        test_bind_address, unique_port, server_config, metrics);
    
    // Track handler invocations
    bool request_vote_called = false;
    bool append_entries_called = false;
    bool install_snapshot_called = false;
    
    // Register handlers
    server.register_request_vote_handler([&](const raft::request_vote_request<>& req) {
        request_vote_called = true;
        raft::request_vote_response<> resp;
        resp._term = req.term() + 1;
        resp._vote_granted = true;
        return resp;
    });
    
    server.register_append_entries_handler([&](const raft::append_entries_request<>& req) {
        append_entries_called = true;
        raft::append_entries_response<> resp;
        resp._term = req.term();
        resp._success = true;
        return resp;
    });
    
    server.register_install_snapshot_handler([&](const raft::install_snapshot_request<>& req) {
        install_snapshot_called = true;
        raft::install_snapshot_response<> resp;
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
        
        raft::cpp_httplib_client<raft::json_serializer, raft::noop_metrics> client(
            std::move(node_urls), client_config, metrics);
        
        // Test RequestVote RPC
        {
            raft::request_vote_request<> request;
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
            raft::append_entries_request<> request;
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
            raft::install_snapshot_request<> request;
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
BOOST_AUTO_TEST_CASE(test_concurrent_requests, * boost::unit_test::disabled()) {
    // Note: This test is disabled because it requires:
    // 1. Server setup and coordination
    // 2. Multiple client threads sending concurrent requests
    // 3. Verification that all requests are handled correctly
    // 4. Connection pooling verification
    
    BOOST_TEST_MESSAGE("Integration test: Concurrent requests");
    BOOST_TEST_MESSAGE("This test requires server setup and thread coordination");
    BOOST_TEST_MESSAGE("Test should verify concurrent request handling and connection pooling");
}

// Integration test for TLS/HTTPS
BOOST_AUTO_TEST_CASE(test_tls_https, * boost::unit_test::disabled()) {
    // Note: This test is disabled because it requires:
    // 1. TLS certificate generation or test certificates
    // 2. Server configured with TLS
    // 3. Client configured for HTTPS
    // 4. Certificate validation testing
    
    BOOST_TEST_MESSAGE("Integration test: TLS/HTTPS communication");
    BOOST_TEST_MESSAGE("This test requires TLS certificates and HTTPS configuration");
    BOOST_TEST_MESSAGE("Test should verify secure communication and certificate validation");
}

BOOST_AUTO_TEST_SUITE_END()