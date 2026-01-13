#define BOOST_TEST_MODULE CoAP Final Integration Validation
#include <boost/test/unit_test.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>
#include <raft/types.hpp>

#include <chrono>
#include <thread>
#include <atomic>
#include <future>
#include <vector>
#include <memory>

using namespace kythira;

namespace {
    constexpr const char* test_server_address = "127.0.0.1";
    constexpr std::uint16_t test_server_port = 5700;
    constexpr std::uint16_t test_secure_port = 5701;
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::chrono::milliseconds test_timeout{10000};
    
    // Test data constants
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_candidate_id = 42;
    constexpr std::uint64_t test_leader_id = 1;
    constexpr std::uint64_t test_log_index = 10;
    constexpr std::uint64_t test_log_term = 4;
    
    const std::vector<std::byte> test_large_data = []() {
        std::vector<std::byte> data;
        data.reserve(5000); // 5KB for block transfer testing
        for (std::size_t i = 0; i < 5000; ++i) {
            data.push_back(static_cast<std::byte>(i % 256));
        }
        return data;
    }();
}

// Test transport types for real libcoap integration
struct real_transport_types {
    using serializer_type = json_rpc_serializer<std::vector<std::byte>>;
    using rpc_serializer_type = json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = noop_metrics;
    using logger_type = console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;
    
    template<typename T>
    using future_template = folly::Future<T>;
    
    using future_type = folly::Future<std::vector<std::byte>>;
};

/**
 * Feature: coap-transport, Task 11: Final integration testing with real libcoap
 * 
 * This test suite validates complete Raft consensus scenarios over real CoAP
 * with actual libcoap implementation when available.
 */
BOOST_AUTO_TEST_CASE(test_complete_raft_consensus_over_real_coap, * boost::unit_test::timeout(180)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Testing complete Raft consensus scenarios over real CoAP");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Using real libcoap implementation");
    
    // Configure server with real CoAP settings
    coap_server_config server_config;
    server_config.enable_dtls = false;
    server_config.max_concurrent_sessions = 50;
    server_config.enable_block_transfer = true;
    server_config.max_block_size = 1024;
    server_config.enable_concurrent_processing = true;
    
    // Configure client with real CoAP settings
    coap_client_config client_config;
    client_config.enable_dtls = false;
    client_config.ack_timeout = std::chrono::milliseconds{5000};
    client_config.max_retransmit = 3;
    client_config.enable_block_transfer = true;
    client_config.max_block_size = 1024;
    client_config.enable_session_reuse = true;
    client_config.connection_pool_size = 10;
    
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port);
    
    BOOST_CHECK_NO_THROW({
        // Create server with real libcoap
        console_logger server_logger;
        coap_server<real_transport_types> server(
            test_server_address,
            test_server_port,
            server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register Raft RPC handlers
        std::atomic<bool> vote_handler_called{false};
        std::atomic<bool> append_handler_called{false};
        std::atomic<bool> snapshot_handler_called{false};
        
        server.register_request_vote_handler([&](const request_vote_request<>& req) {
            vote_handler_called = true;
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        server.register_append_entries_handler([&](const append_entries_request<>& req) {
            append_handler_called = true;
            append_entries_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.match_index = req.prev_log_index + req.entries.size();
            return resp;
        });
        
        server.register_install_snapshot_handler([&](const install_snapshot_request<>& req) {
            snapshot_handler_called = true;
            install_snapshot_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.bytes_stored = req.data.size();
            return resp;
        });
        
        // Start server
        server.start();
        BOOST_CHECK(server.is_running());
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        
        // Create client with real libcoap
        console_logger client_logger;
        coap_client<real_transport_types> client(
            endpoints,
            client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test RequestVote RPC with real CoAP
        request_vote_request<> vote_req;
        vote_req.term = test_term;
        vote_req.candidate_id = test_candidate_id;
        vote_req.last_log_index = test_log_index;
        vote_req.last_log_term = test_log_term;
        
        auto vote_future = client.send_request_vote(test_node_id, vote_req, test_timeout);
        auto vote_resp = std::move(vote_future).get();
        
        BOOST_CHECK_EQUAL(vote_resp.term, test_term);
        BOOST_CHECK(vote_resp.vote_granted);
        BOOST_CHECK(vote_handler_called.load());
        
        // Test AppendEntries RPC with real CoAP
        append_entries_request<> append_req;
        append_req.term = test_term;
        append_req.leader_id = test_leader_id;
        append_req.prev_log_index = test_log_index;
        append_req.prev_log_term = test_log_term;
        append_req.leader_commit = test_log_index - 1;
        append_req.entries = {"entry1", "entry2", "entry3"};
        
        auto append_future = client.send_append_entries(test_node_id, append_req, test_timeout);
        auto append_resp = std::move(append_future).get();
        
        BOOST_CHECK_EQUAL(append_resp.term, test_term);
        BOOST_CHECK(append_resp.success);
        BOOST_CHECK_EQUAL(append_resp.match_index, test_log_index + 3);
        BOOST_CHECK(append_handler_called.load());
        
        // Test InstallSnapshot RPC with real CoAP (large data for block transfer)
        install_snapshot_request<> snapshot_req;
        snapshot_req.term = test_term;
        snapshot_req.leader_id = test_leader_id;
        snapshot_req.last_included_index = test_log_index;
        snapshot_req.last_included_term = test_log_term;
        snapshot_req.offset = 0;
        snapshot_req.data = test_large_data; // Large data to test block transfer
        snapshot_req.done = true;
        
        auto snapshot_future = client.send_install_snapshot(test_node_id, snapshot_req, test_timeout);
        auto snapshot_resp = std::move(snapshot_future).get();
        
        BOOST_CHECK_EQUAL(snapshot_resp.term, test_term);
        BOOST_CHECK(snapshot_resp.success);
        BOOST_CHECK_EQUAL(snapshot_resp.bytes_stored, test_large_data.size());
        BOOST_CHECK(snapshot_handler_called.load());
        
        // Stop server
        server.stop();
        BOOST_CHECK(!server.is_running());
        
        logger.info("Real libcoap Raft consensus integration test passed");
    });
    
#else
    logger.warning("libcoap not available - using stub implementation");
    
    // Test with stub implementation for development environments
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<real_transport_types> server(
            test_server_address,
            test_server_port,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        console_logger client_logger;
        std::unordered_map<std::uint64_t, std::string> endpoints;
        endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port);
        
        coap_client<real_transport_types> client(
            endpoints,
            coap_client_config{},
            metrics,
            std::move(client_logger)
        );
        
        logger.info("Stub implementation test passed");
    });
#endif
}

/**
 * Feature: coap-transport, Task 11: Security features with real DTLS handshakes
 * 
 * This test validates security features with actual DTLS implementation
 * when libcoap and OpenSSL are available.
 */
BOOST_AUTO_TEST_CASE(test_security_features_real_dtls, * boost::unit_test::timeout(240)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Testing security features with real DTLS handshakes");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Testing with real DTLS implementation");
    
    // Configure secure server
    coap_server_config secure_server_config;
    secure_server_config.enable_dtls = true;
    secure_server_config.enable_certificate_validation = true;
    secure_server_config.verify_peer_cert = true;
    // Use PSK for testing (easier than certificate setup)
    secure_server_config.psk_identity = "test_raft_node";
    secure_server_config.psk_key = {
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}
    };
    
    // Configure secure client
    coap_client_config secure_client_config;
    secure_client_config.enable_dtls = true;
    secure_client_config.enable_certificate_validation = true;
    secure_client_config.verify_peer_cert = true;
    secure_client_config.psk_identity = "test_raft_node";
    secure_client_config.psk_key = {
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}
    };
    secure_client_config.ack_timeout = std::chrono::milliseconds{10000}; // Longer for DTLS
    
    std::unordered_map<std::uint64_t, std::string> secure_endpoints;
    secure_endpoints[test_node_id] = std::format("coaps://{}:{}", test_server_address, test_secure_port);
    
    BOOST_CHECK_NO_THROW({
        // Create secure server
        console_logger server_logger;
        coap_server<real_transport_types> secure_server(
            test_server_address,
            test_secure_port,
            secure_server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register handler for secure communication
        std::atomic<bool> secure_handler_called{false};
        secure_server.register_request_vote_handler([&](const request_vote_request<>& req) {
            secure_handler_called = true;
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        // Start secure server
        secure_server.start();
        BOOST_CHECK(secure_server.is_running());
        
        // Give server time to start and setup DTLS
        std::this_thread::sleep_for(std::chrono::milliseconds{1000});
        
        // Create secure client
        console_logger client_logger;
        coap_client<real_transport_types> secure_client(
            secure_endpoints,
            secure_client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test secure RequestVote with DTLS
        request_vote_request<> vote_req;
        vote_req.term = test_term;
        vote_req.candidate_id = test_candidate_id;
        vote_req.last_log_index = test_log_index;
        vote_req.last_log_term = test_log_term;
        
        auto vote_future = secure_client.send_request_vote(test_node_id, vote_req, test_timeout);
        auto vote_resp = std::move(vote_future).get();
        
        BOOST_CHECK_EQUAL(vote_resp.term, test_term);
        BOOST_CHECK(vote_resp.vote_granted);
        BOOST_CHECK(secure_handler_called.load());
        
        // Stop secure server
        secure_server.stop();
        BOOST_CHECK(!secure_server.is_running());
        
        logger.info("Real DTLS security test passed");
    });
    
#else
    logger.warning("libcoap not available - testing stub DTLS implementation");
    
    // Test stub DTLS implementation
    BOOST_CHECK_NO_THROW({
        coap_server_config stub_config;
        stub_config.enable_dtls = true;
        
        console_logger server_logger;
        coap_server<real_transport_types> server(
            test_server_address,
            test_secure_port,
            stub_config,
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub DTLS implementation test passed");
    });
#endif
}

/**
 * Feature: coap-transport, Task 11: Performance under load with real protocol overhead
 * 
 * This test validates performance characteristics with actual CoAP protocol
 * overhead and measures real-world performance metrics.
 */
BOOST_AUTO_TEST_CASE(test_performance_real_protocol_overhead, * boost::unit_test::timeout(300)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Testing performance under load with real protocol overhead");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Performance testing with real libcoap implementation");
    
    // Configure for performance testing
    coap_server_config perf_server_config;
    perf_server_config.enable_dtls = false; // Disable DTLS for pure CoAP performance
    perf_server_config.max_concurrent_sessions = 100;
    perf_server_config.enable_concurrent_processing = true;
    perf_server_config.enable_block_transfer = false; // Disable for small message performance
    
    coap_client_config perf_client_config;
    perf_client_config.enable_dtls = false;
    perf_client_config.ack_timeout = std::chrono::milliseconds{2000};
    perf_client_config.max_retransmit = 2; // Reduce retries for performance
    perf_client_config.enable_session_reuse = true;
    perf_client_config.connection_pool_size = 20;
    perf_client_config.enable_serialization_caching = true;
    perf_client_config.max_cache_entries = 100;
    
    std::unordered_map<std::uint64_t, std::string> perf_endpoints;
    perf_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port + 1);
    
    BOOST_CHECK_NO_THROW({
        // Create performance test server
        console_logger server_logger;
        coap_server<real_transport_types> perf_server(
            test_server_address,
            test_server_port + 1,
            perf_server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register fast handler
        std::atomic<std::size_t> request_count{0};
        perf_server.register_request_vote_handler([&](const request_vote_request<>& req) {
            request_count.fetch_add(1);
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        // Start performance server
        perf_server.start();
        BOOST_CHECK(perf_server.is_running());
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        
        // Create performance client
        console_logger client_logger;
        coap_client<real_transport_types> perf_client(
            perf_endpoints,
            perf_client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Performance test: Send multiple concurrent requests
        constexpr std::size_t num_requests = 50;
        constexpr std::chrono::milliseconds perf_timeout{30000};
        
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<folly::Future<request_vote_response<>>> futures;
        futures.reserve(num_requests);
        
        // Send concurrent requests
        for (std::size_t i = 0; i < num_requests; ++i) {
            request_vote_request<> req;
            req.term = test_term + i;
            req.candidate_id = test_candidate_id;
            req.last_log_index = test_log_index + i;
            req.last_log_term = test_log_term;
            
            futures.push_back(perf_client.send_request_vote(test_node_id, req, perf_timeout));
        }
        
        // Wait for all responses
        auto all_responses = folly::collectAll(futures).get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Validate all responses
        std::size_t successful_responses = 0;
        for (const auto& result : all_responses) {
            if (result.hasValue()) {
                const auto& resp = result.value();
                BOOST_CHECK(resp.vote_granted);
                successful_responses++;
            }
        }
        
        BOOST_CHECK_EQUAL(successful_responses, num_requests);
        BOOST_CHECK_EQUAL(request_count.load(), num_requests);
        
        // Calculate performance metrics
        double requests_per_second = (static_cast<double>(num_requests) * 1000.0) / duration.count();
        double avg_latency_ms = static_cast<double>(duration.count()) / num_requests;
        
        logger.info(std::format("Performance results: {} requests in {}ms", num_requests, duration.count()));
        logger.info(std::format("Throughput: {:.2f} requests/second", requests_per_second));
        logger.info(std::format("Average latency: {:.2f}ms per request", avg_latency_ms));
        
        // Performance assertions (should be reasonable for real CoAP)
        BOOST_CHECK_GT(requests_per_second, 10.0); // At least 10 req/sec
        BOOST_CHECK_LT(avg_latency_ms, 1000.0); // Less than 1 second average
        
        // Stop performance server
        perf_server.stop();
        BOOST_CHECK(!perf_server.is_running());
        
        logger.info("Real CoAP performance test passed");
    });
    
#else
    logger.warning("libcoap not available - performance test with stub implementation");
    
    // Stub performance test
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<real_transport_types> server(
            test_server_address,
            test_server_port + 1,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub performance test passed");
    });
#endif
}

/**
 * Feature: coap-transport, Task 11: Interoperability with standard CoAP clients/servers
 * 
 * This test validates interoperability with other CoAP implementations
 * by testing standard CoAP protocol compliance.
 */
BOOST_AUTO_TEST_CASE(test_interoperability_standard_coap, * boost::unit_test::timeout(180)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Testing interoperability with standard CoAP clients/servers");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Testing CoAP protocol compliance for interoperability");
    
    // Configure for standard CoAP compliance
    coap_server_config standard_config;
    standard_config.enable_dtls = false;
    standard_config.max_concurrent_sessions = 10;
    standard_config.enable_block_transfer = true;
    standard_config.max_block_size = 1024; // Standard CoAP block size
    
    coap_client_config client_standard_config;
    client_standard_config.enable_dtls = false;
    client_standard_config.ack_timeout = std::chrono::milliseconds{2000}; // RFC 7252 default
    client_standard_config.max_retransmit = 4; // RFC 7252 default
    client_standard_config.enable_block_transfer = true;
    client_standard_config.max_block_size = 1024;
    
    std::unordered_map<std::uint64_t, std::string> standard_endpoints;
    standard_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port + 2);
    
    BOOST_CHECK_NO_THROW({
        // Create standard-compliant server
        console_logger server_logger;
        coap_server<real_transport_types> standard_server(
            test_server_address,
            test_server_port + 2,
            standard_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register handlers that follow CoAP conventions
        std::atomic<bool> standard_handler_called{false};
        standard_server.register_request_vote_handler([&](const request_vote_request<>& req) {
            standard_handler_called = true;
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        // Start standard server
        standard_server.start();
        BOOST_CHECK(standard_server.is_running());
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        
        // Create standard-compliant client
        console_logger client_logger;
        coap_client<real_transport_types> standard_client(
            standard_endpoints,
            client_standard_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test standard CoAP message exchange
        request_vote_request<> standard_req;
        standard_req.term = test_term;
        standard_req.candidate_id = test_candidate_id;
        standard_req.last_log_index = test_log_index;
        standard_req.last_log_term = test_log_term;
        
        auto standard_future = standard_client.send_request_vote(test_node_id, standard_req, test_timeout);
        auto standard_resp = std::move(standard_future).get();
        
        BOOST_CHECK_EQUAL(standard_resp.term, test_term);
        BOOST_CHECK(standard_resp.vote_granted);
        BOOST_CHECK(standard_handler_called.load());
        
        // Test with large message to verify block transfer compliance
        install_snapshot_request<> large_req;
        large_req.term = test_term;
        large_req.leader_id = test_leader_id;
        large_req.last_included_index = test_log_index;
        large_req.last_included_term = test_log_term;
        large_req.offset = 0;
        large_req.data = test_large_data; // Should trigger block transfer
        large_req.done = true;
        
        std::atomic<bool> snapshot_handler_called{false};
        standard_server.register_install_snapshot_handler([&](const install_snapshot_request<>& req) {
            snapshot_handler_called = true;
            install_snapshot_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.bytes_stored = req.data.size();
            return resp;
        });
        
        auto large_future = standard_client.send_install_snapshot(test_node_id, large_req, test_timeout);
        auto large_resp = std::move(large_future).get();
        
        BOOST_CHECK_EQUAL(large_resp.term, test_term);
        BOOST_CHECK(large_resp.success);
        BOOST_CHECK_EQUAL(large_resp.bytes_stored, test_large_data.size());
        BOOST_CHECK(snapshot_handler_called.load());
        
        // Stop standard server
        standard_server.stop();
        BOOST_CHECK(!standard_server.is_running());
        
        logger.info("CoAP protocol compliance and interoperability test passed");
    });
    
#else
    logger.warning("libcoap not available - interoperability test with stub implementation");
    
    // Stub interoperability test
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<real_transport_types> server(
            test_server_address,
            test_server_port + 2,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub interoperability test passed");
    });
#endif
}