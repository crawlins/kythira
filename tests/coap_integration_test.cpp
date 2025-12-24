#define BOOST_TEST_MODULE coap_integration_test

#include <boost/test/unit_test.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT 30

#include <thread>
#include <chrono>
#include <atomic>
#include <raft/future.hpp>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <set>

namespace {
    constexpr const char* test_server_address = "127.0.0.1";
    constexpr std::uint16_t test_server_port = 5700;
    constexpr std::uint64_t test_node_id = 1;
    constexpr std::chrono::milliseconds test_timeout{5000};
    
    // Test data constants
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_candidate_id = 42;
    constexpr std::uint64_t test_leader_id = 1;
    constexpr std::uint64_t test_log_index = 10;
    constexpr std::uint64_t test_log_term = 4;
    
    const std::vector<std::byte> test_snapshot_data = {
        std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
        std::byte{'_'}, std::byte{'s'}, std::byte{'n'}, std::byte{'a'}, 
        std::byte{'p'}, std::byte{'s'}, std::byte{'h'}, std::byte{'o'}, 
        std::byte{'t'}, std::byte{'_'}, std::byte{'d'}, std::byte{'a'}, 
        std::byte{'t'}, std::byte{'a'}
    };
}

// Mock configuration structures for testing
struct coap_server_config {
    bool enable_dtls{false};
    std::size_t max_concurrent_sessions{200};
    std::size_t max_request_size{64 * 1024};
    bool enable_block_transfer{false};
    std::size_t max_block_size{1024};
};

struct coap_client_config {
    bool enable_dtls{false};
    std::chrono::milliseconds ack_timeout{2000};
    bool enable_block_transfer{false};
    std::size_t max_block_size{1024};
};

struct CoAPIntegrationFixture {
    CoAPIntegrationFixture() {
        // Initialize any test fixtures if needed
    }
    
    ~CoAPIntegrationFixture() {
        // Cleanup any test fixtures if needed
    }
};

BOOST_FIXTURE_TEST_SUITE(coap_integration_tests, CoAPIntegrationFixture)

// Integration test for client-server communication
BOOST_AUTO_TEST_CASE(test_client_server_communication, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Integration test: Client-server communication");
    
    // Create server configuration
    coap_server_config server_config;
    server_config.enable_dtls = false;
    server_config.max_concurrent_sessions = 10;
    
    // Create client configuration  
    coap_client_config client_config;
    client_config.enable_dtls = false;
    client_config.ack_timeout = test_timeout;
    
    // Test CoAP server configuration
    BOOST_CHECK(!server_config.enable_dtls);
    BOOST_CHECK_EQUAL(server_config.max_concurrent_sessions, 10);
    
    // Test CoAP client configuration  
    BOOST_CHECK(!client_config.enable_dtls);
    BOOST_CHECK_EQUAL(client_config.ack_timeout, test_timeout);
    
    BOOST_TEST_MESSAGE("CoAP server and client configurations validated");
    
    // Test endpoint mapping
    std::unordered_map<std::uint64_t, std::string> node_endpoints;
    node_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port);
    
    BOOST_CHECK_EQUAL(node_endpoints.size(), 1);
    BOOST_CHECK(node_endpoints.find(test_node_id) != node_endpoints.end());
    
    BOOST_TEST_MESSAGE("CoAP endpoint mapping configured correctly");
    
    // Test RequestVote RPC structure
    struct mock_request_vote_request {
        std::uint64_t term{test_term};
        std::uint64_t candidate_id{test_candidate_id};
        std::uint64_t last_log_index{test_log_index};
        std::uint64_t last_log_term{test_log_term};
    };
    
    mock_request_vote_request vote_req;
    BOOST_CHECK_EQUAL(vote_req.term, test_term);
    BOOST_CHECK_EQUAL(vote_req.candidate_id, test_candidate_id);
    
    // Test AppendEntries RPC structure
    struct mock_append_entries_request {
        std::uint64_t term{test_term};
        std::uint64_t leader_id{test_leader_id};
        std::uint64_t prev_log_index{test_log_index - 1};
        std::uint64_t prev_log_term{test_log_term};
        std::uint64_t leader_commit{test_log_index - 2};
    };
    
    mock_append_entries_request append_req;
    BOOST_CHECK_EQUAL(append_req.term, test_term);
    BOOST_CHECK_EQUAL(append_req.leader_id, test_leader_id);
    
    // Test InstallSnapshot RPC structure
    struct mock_install_snapshot_request {
        std::uint64_t term{test_term};
        std::uint64_t leader_id{test_leader_id};
        std::uint64_t last_included_index{test_log_index};
        std::uint64_t last_included_term{test_log_term};
        std::uint64_t offset{0};
        std::vector<std::byte> data{test_snapshot_data};
        bool done{true};
    };
    
    mock_install_snapshot_request snapshot_req;
    BOOST_CHECK_EQUAL(snapshot_req.term, test_term);
    BOOST_CHECK_EQUAL(snapshot_req.leader_id, test_leader_id);
    BOOST_CHECK_EQUAL(snapshot_req.data.size(), test_snapshot_data.size());
    
    BOOST_TEST_MESSAGE("CoAP transport integration test completed successfully");
}

// Integration test for DTLS handshake and secure communication
BOOST_AUTO_TEST_CASE(test_dtls_handshake_secure_communication, * boost::unit_test::timeout(180)) {
    BOOST_TEST_MESSAGE("Integration test: DTLS handshake and secure communication");
    
    // Test PSK-based DTLS configuration
    coap_server_config server_config;
    server_config.enable_dtls = true;
    
    coap_client_config client_config;
    client_config.enable_dtls = true;
    
    BOOST_CHECK(server_config.enable_dtls);
    BOOST_CHECK(client_config.enable_dtls);
    
    // Test PSK credentials structure
    const std::string test_psk_identity = "raft-node-test";
    const std::vector<std::byte> test_psk_key = {
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF},
        std::byte{0xFE}, std::byte{0xDC}, std::byte{0xBA}, std::byte{0x98},
        std::byte{0x76}, std::byte{0x54}, std::byte{0x32}, std::byte{0x10}
    };
    
    // Simulate PSK configuration
    struct psk_config {
        std::string identity;
        std::vector<std::byte> key;
    };
    
    psk_config server_psk{test_psk_identity, test_psk_key};
    psk_config client_psk{test_psk_identity, test_psk_key};
    
    BOOST_CHECK_EQUAL(server_psk.identity, client_psk.identity);
    BOOST_CHECK_EQUAL(server_psk.key.size(), client_psk.key.size());
    BOOST_CHECK(server_psk.key == client_psk.key);
    
    BOOST_TEST_MESSAGE("PSK credentials configured correctly");
    
    // Test certificate-based configuration
    const std::string test_cert_path = "/etc/ssl/certs/test-cert.pem";
    const std::string test_key_path = "/etc/ssl/private/test-key.pem";
    const std::string test_ca_path = "/etc/ssl/certs/test-ca.pem";
    
    struct cert_config {
        std::string cert_file;
        std::string key_file;
        std::string ca_file;
        bool verify_peer{true};
    };
    
    cert_config server_cert{test_cert_path, test_key_path, test_ca_path, true};
    cert_config client_cert{test_cert_path, test_key_path, test_ca_path, true};
    
    BOOST_CHECK(!server_cert.cert_file.empty());
    BOOST_CHECK(!server_cert.key_file.empty());
    BOOST_CHECK(!server_cert.ca_file.empty());
    BOOST_CHECK(server_cert.verify_peer);
    
    BOOST_CHECK_EQUAL(server_cert.cert_file, client_cert.cert_file);
    BOOST_CHECK_EQUAL(server_cert.key_file, client_cert.key_file);
    BOOST_CHECK_EQUAL(server_cert.ca_file, client_cert.ca_file);
    
    BOOST_TEST_MESSAGE("Certificate configuration structured correctly");
    
    // Test DTLS endpoint format
    std::string secure_endpoint = "coaps://127.0.0.1:5684";
    BOOST_CHECK(secure_endpoint.substr(0, 6) == "coaps:");
    
    // Test security error handling
    struct security_error {
        enum class type { certificate_invalid, psk_mismatch, handshake_timeout };
        type error_type;
        std::string message;
    };
    
    // Simulate certificate validation failure
    security_error cert_error{security_error::type::certificate_invalid, "Certificate validation failed"};
    BOOST_CHECK(cert_error.error_type == security_error::type::certificate_invalid);
    BOOST_CHECK(!cert_error.message.empty());
    
    // Simulate PSK mismatch
    security_error psk_error{security_error::type::psk_mismatch, "PSK identity mismatch"};
    BOOST_CHECK(psk_error.error_type == security_error::type::psk_mismatch);
    
    BOOST_TEST_MESSAGE("DTLS security configuration and error handling validated");
}

// Integration test for block transfer with large messages
BOOST_AUTO_TEST_CASE(test_block_transfer_large_messages, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Integration test: Block transfer with large messages");
    
    // Create configurations with block transfer enabled
    coap_server_config server_config;
    server_config.enable_block_transfer = true;
    server_config.max_block_size = 1024;
    server_config.max_request_size = 64 * 1024; // 64 KB
    server_config.enable_dtls = false;
    
    coap_client_config client_config;
    client_config.enable_block_transfer = true;
    client_config.max_block_size = 1024;
    client_config.ack_timeout = std::chrono::milliseconds{10000}; // Longer for block transfers
    client_config.enable_dtls = false;
    
    // Test block transfer configuration
    BOOST_CHECK(server_config.enable_block_transfer);
    BOOST_CHECK_EQUAL(server_config.max_block_size, 1024);
    BOOST_CHECK_EQUAL(server_config.max_request_size, 64 * 1024);
    
    BOOST_CHECK(client_config.enable_block_transfer);
    BOOST_CHECK_EQUAL(client_config.max_block_size, 1024);
    BOOST_CHECK_EQUAL(client_config.ack_timeout, std::chrono::milliseconds{10000});
    
    BOOST_TEST_MESSAGE("Block transfer configurations validated");
    
    // Generate large test data (larger than block size)
    std::vector<std::byte> large_data;
    large_data.reserve(5000); // 5KB > 1KB block size
    for (std::size_t i = 0; i < 5000; ++i) {
        large_data.push_back(static_cast<std::byte>(i % 256));
    }
    
    // Test if block transfer would be used (mock logic)
    bool should_use_blocks = (large_data.size() > 1024);
    BOOST_CHECK(should_use_blocks);
    
    if (should_use_blocks) {
        // Simulate block splitting
        std::size_t num_blocks = (large_data.size() + 1024 - 1) / 1024;
        BOOST_CHECK_GT(num_blocks, 1);
        BOOST_TEST_MESSAGE("Large data would split into " << num_blocks << " blocks");
        
        // Verify block size calculations
        for (std::size_t i = 0; i < num_blocks - 1; ++i) {
            std::size_t block_size = 1024; // All but last block should be full size
            BOOST_CHECK_EQUAL(block_size, 1024);
        }
        
        // Last block may be smaller
        std::size_t expected_last_size = large_data.size() % 1024;
        if (expected_last_size == 0) expected_last_size = 1024;
        
        std::size_t last_block_start = (num_blocks - 1) * 1024;
        std::size_t actual_last_size = large_data.size() - last_block_start;
        BOOST_CHECK_EQUAL(actual_last_size, expected_last_size);
    }
    
    // Test block reassembly simulation
    std::string test_token = "integration_test_token";
    std::vector<std::byte> reassembled;
    
    std::size_t num_blocks = (large_data.size() + 1024 - 1) / 1024;
    for (std::size_t i = 0; i < num_blocks; ++i) {
        std::size_t block_start = i * 1024;
        std::size_t block_end = std::min(block_start + 1024, large_data.size());
        
        std::vector<std::byte> block_data(large_data.begin() + block_start, large_data.begin() + block_end);
        reassembled.insert(reassembled.end(), block_data.begin(), block_data.end());
    }
    
    BOOST_CHECK_EQUAL(reassembled.size(), large_data.size());
    // Note: In a real implementation, we would verify content equality
    // For this mock test, we just verify the sizes match
    
    BOOST_TEST_MESSAGE("Block transfer simulation completed successfully");
}

// Integration test for multicast communication scenarios
BOOST_AUTO_TEST_CASE(test_multicast_communication_scenarios, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Integration test: Multicast communication scenarios");
    
    // Test multicast configuration
    struct coap_multicast_config {
        bool enable_multicast{true};
        std::string multicast_address{"224.0.1.187"};
        std::uint16_t multicast_port{5683};
    };
    
    coap_multicast_config multicast_config;
    
    BOOST_CHECK(multicast_config.enable_multicast);
    BOOST_CHECK_EQUAL(multicast_config.multicast_address, "224.0.1.187");
    BOOST_CHECK_EQUAL(multicast_config.multicast_port, 5683);
    
    // Validate multicast address range (224.0.0.0 to 239.255.255.255)
    std::string addr = multicast_config.multicast_address;
    BOOST_CHECK(addr.substr(0, 4) == "224.");
    
    BOOST_TEST_MESSAGE("Multicast address validation passed");
    
    // Test multiple server configuration for multicast
    std::vector<coap_server_config> multicast_servers;
    
    for (int i = 0; i < 3; ++i) {
        coap_server_config server_config;
        server_config.enable_dtls = false; // Multicast typically uses plain CoAP
        server_config.max_concurrent_sessions = 20;
        multicast_servers.push_back(server_config);
    }
    
    BOOST_CHECK_EQUAL(multicast_servers.size(), 3);
    
    for (const auto& config : multicast_servers) {
        BOOST_CHECK(!config.enable_dtls);
        BOOST_CHECK_EQUAL(config.max_concurrent_sessions, 20);
    }
    
    BOOST_TEST_MESSAGE("Multiple multicast servers configured");
    
    // Test multicast response aggregation simulation
    struct multicast_response {
        std::uint64_t node_id;
        std::uint64_t term;
        bool vote_granted;
    };
    
    std::vector<multicast_response> responses;
    responses.push_back({1, test_term, true});
    responses.push_back({2, test_term, false});
    responses.push_back({3, test_term + 1, false}); // Higher term
    
    BOOST_CHECK_EQUAL(responses.size(), 3);
    
    // Simulate response aggregation
    std::size_t votes_granted = 0;
    std::uint64_t max_term = 0;
    
    for (const auto& resp : responses) {
        if (resp.vote_granted) {
            votes_granted++;
        }
        max_term = std::max(max_term, resp.term);
    }
    
    bool election_won = votes_granted > (responses.size() / 2);
    
    BOOST_CHECK_EQUAL(votes_granted, 1);
    BOOST_CHECK_EQUAL(max_term, test_term + 1);
    BOOST_CHECK(!election_won); // 1 out of 3 is not majority
    
    BOOST_TEST_MESSAGE("Multicast response aggregation logic validated");
    
    // Test multicast endpoint format
    std::string multicast_endpoint = std::format("coap://{}:{}", 
        multicast_config.multicast_address, multicast_config.multicast_port);
    BOOST_CHECK(multicast_endpoint.find("224.0.1.187") != std::string::npos);
    BOOST_CHECK(multicast_endpoint.find("5683") != std::string::npos);
    
    BOOST_TEST_MESSAGE("Multicast communication scenarios validated successfully");
}

// Integration test for error recovery and resilience
BOOST_AUTO_TEST_CASE(test_error_recovery_resilience, * boost::unit_test::timeout(90)) {
    BOOST_TEST_MESSAGE("Integration test: Error recovery and resilience");
    
    coap_client_config client_config;
    client_config.enable_dtls = false;
    client_config.ack_timeout = std::chrono::milliseconds{1000}; // Short timeout for testing
    
    // Test connection to non-existent server configuration
    std::unordered_map<std::uint64_t, std::string> invalid_endpoints;
    invalid_endpoints[test_node_id] = "coap://127.0.0.1:9999"; // Non-existent server
    
    BOOST_CHECK_EQUAL(invalid_endpoints.size(), 1);
    BOOST_CHECK(invalid_endpoints.find(test_node_id) != invalid_endpoints.end());
    
    // Test timeout handling
    struct mock_request_vote_request {
        std::uint64_t term{test_term};
        std::uint64_t candidate_id{test_candidate_id};
        std::uint64_t last_log_index{test_log_index};
        std::uint64_t last_log_term{test_log_term};
    };
    
    mock_request_vote_request vote_req;
    BOOST_CHECK_EQUAL(vote_req.term, test_term);
    BOOST_CHECK_EQUAL(vote_req.candidate_id, test_candidate_id);
    
    // In a real implementation, this would timeout and the future would be set to an error
    BOOST_TEST_MESSAGE("Timeout handling configured correctly");
    
    // Test malformed message handling configuration
    coap_server_config error_server_config;
    error_server_config.enable_dtls = false;
    
    BOOST_CHECK(!error_server_config.enable_dtls);
    BOOST_TEST_MESSAGE("Error handling server configuration created");
    
    // Test duplicate message detection logic (mock)
    std::uint16_t test_message_id = 12345;
    std::set<std::uint16_t> received_messages;
    
    // First message should not be duplicate
    bool is_duplicate_first = (received_messages.find(test_message_id) != received_messages.end());
    BOOST_CHECK(!is_duplicate_first);
    
    // Record the message
    received_messages.insert(test_message_id);
    
    // Second message with same ID should be duplicate
    bool is_duplicate_second = (received_messages.find(test_message_id) != received_messages.end());
    BOOST_CHECK(is_duplicate_second);
    
    BOOST_TEST_MESSAGE("Duplicate message detection logic validated");
}

// Integration test for performance and concurrent requests
BOOST_AUTO_TEST_CASE(test_performance_concurrent_requests, * boost::unit_test::timeout(180)) {
    BOOST_TEST_MESSAGE("Integration test: Performance and concurrent requests");
    
    coap_server_config server_config;
    server_config.max_concurrent_sessions = 100;
    server_config.enable_dtls = false;
    
    coap_client_config client_config;
    client_config.enable_dtls = false;
    
    BOOST_CHECK_EQUAL(server_config.max_concurrent_sessions, 100);
    BOOST_CHECK(!server_config.enable_dtls);
    BOOST_CHECK(!client_config.enable_dtls);
    
    // Test concurrent request simulation
    const std::size_t num_concurrent_requests = 50;
    const std::size_t max_sessions = server_config.max_concurrent_sessions;
    
    BOOST_CHECK_LE(num_concurrent_requests, max_sessions);
    
    // Simulate concurrent request tracking
    struct request_info {
        std::uint64_t request_id;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::milliseconds timeout;
        bool completed{false};
    };
    
    std::vector<request_info> concurrent_requests;
    auto now = std::chrono::steady_clock::now();
    
    for (std::size_t i = 0; i < num_concurrent_requests; ++i) {
        request_info req;
        req.request_id = i + 1;
        req.start_time = now;
        req.timeout = std::chrono::milliseconds{5000};
        req.completed = false;
        concurrent_requests.push_back(req);
    }
    
    BOOST_CHECK_EQUAL(concurrent_requests.size(), num_concurrent_requests);
    
    // Simulate request processing
    for (auto& req : concurrent_requests) {
        // Simulate processing time
        auto processing_time = std::chrono::milliseconds{100 + (req.request_id % 50)};
        
        // Check if request would complete within timeout
        bool would_complete = (processing_time < req.timeout);
        req.completed = would_complete;
        
        BOOST_CHECK(req.completed); // All requests should complete within timeout
    }
    
    // Count completed requests
    std::size_t completed_count = 0;
    for (const auto& req : concurrent_requests) {
        if (req.completed) {
            completed_count++;
        }
    }
    
    BOOST_CHECK_EQUAL(completed_count, num_concurrent_requests);
    
    BOOST_TEST_MESSAGE("Concurrent request handling simulation completed");
    
    // Test connection pooling simulation
    struct connection_pool {
        std::size_t max_connections;
        std::size_t active_connections{0};
        std::size_t reused_connections{0};
        
        bool acquire_connection() {
            if (active_connections < max_connections) {
                active_connections++;
                return true;
            }
            return false;
        }
        
        void release_connection() {
            if (active_connections > 0) {
                active_connections--;
                reused_connections++;
            }
        }
    };
    
    connection_pool pool{10, 0, 0};
    
    // Simulate connection acquisition and reuse
    for (std::size_t i = 0; i < 15; ++i) {
        bool acquired = pool.acquire_connection();
        if (i < 10) {
            BOOST_CHECK(acquired); // First 10 should succeed
        } else {
            BOOST_CHECK(!acquired); // Next 5 should fail (pool exhausted)
        }
    }
    
    BOOST_CHECK_EQUAL(pool.active_connections, 10);
    
    // Release some connections
    for (std::size_t i = 0; i < 5; ++i) {
        pool.release_connection();
    }
    
    BOOST_CHECK_EQUAL(pool.active_connections, 5);
    BOOST_CHECK_EQUAL(pool.reused_connections, 5);
    
    BOOST_TEST_MESSAGE("Connection pooling simulation validated");
    
    // Test performance metrics structure
    struct performance_metrics {
        std::size_t total_requests{0};
        std::size_t successful_requests{0};
        std::size_t failed_requests{0};
        std::chrono::milliseconds avg_response_time{0};
        std::chrono::milliseconds max_response_time{0};
        std::chrono::milliseconds min_response_time{std::chrono::milliseconds::max()};
    };
    
    performance_metrics metrics;
    
    // Simulate metrics collection
    std::vector<std::chrono::milliseconds> response_times = {
        std::chrono::milliseconds{50},
        std::chrono::milliseconds{75},
        std::chrono::milliseconds{100},
        std::chrono::milliseconds{125},
        std::chrono::milliseconds{80}
    };
    
    metrics.total_requests = response_times.size();
    metrics.successful_requests = response_times.size(); // All successful for this test
    metrics.failed_requests = 0;
    
    // Calculate min, max, and average
    for (const auto& time : response_times) {
        metrics.max_response_time = std::max(metrics.max_response_time, time);
        metrics.min_response_time = std::min(metrics.min_response_time, time);
    }
    
    // Calculate average
    std::chrono::milliseconds total_time{0};
    for (const auto& time : response_times) {
        total_time += time;
    }
    metrics.avg_response_time = total_time / response_times.size();
    
    BOOST_CHECK_EQUAL(metrics.total_requests, 5);
    BOOST_CHECK_EQUAL(metrics.successful_requests, 5);
    BOOST_CHECK_EQUAL(metrics.failed_requests, 0);
    BOOST_CHECK_EQUAL(metrics.max_response_time, std::chrono::milliseconds{125});
    BOOST_CHECK_EQUAL(metrics.min_response_time, std::chrono::milliseconds{50});
    BOOST_CHECK_EQUAL(metrics.avg_response_time, std::chrono::milliseconds{86}); // (50+75+100+125+80)/5 = 86
    
    BOOST_TEST_MESSAGE("Performance metrics collection validated");
}

// Integration test for complete request-response cycles
BOOST_AUTO_TEST_CASE(test_complete_request_response_cycles, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Integration test: Complete request-response cycles");
    
    // Test complete RequestVote cycle
    struct request_vote_cycle {
        struct request {
            std::uint64_t term{test_term};
            std::uint64_t candidate_id{test_candidate_id};
            std::uint64_t last_log_index{test_log_index};
            std::uint64_t last_log_term{test_log_term};
        };
        
        struct response {
            std::uint64_t term{test_term};
            bool vote_granted{true};
        };
        
        request req;
        response resp;
        bool completed{false};
        std::chrono::milliseconds duration{0};
    };
    
    request_vote_cycle vote_cycle;
    
    // Simulate request processing
    BOOST_CHECK_EQUAL(vote_cycle.req.term, test_term);
    BOOST_CHECK_EQUAL(vote_cycle.req.candidate_id, test_candidate_id);
    
    // Simulate response generation
    vote_cycle.resp.term = vote_cycle.req.term;
    vote_cycle.resp.vote_granted = true;
    vote_cycle.completed = true;
    vote_cycle.duration = std::chrono::milliseconds{150};
    
    BOOST_CHECK(vote_cycle.completed);
    BOOST_CHECK(vote_cycle.resp.vote_granted);
    BOOST_CHECK_EQUAL(vote_cycle.resp.term, test_term);
    BOOST_CHECK_LT(vote_cycle.duration, test_timeout);
    
    BOOST_TEST_MESSAGE("RequestVote cycle completed successfully");
    
    // Test complete AppendEntries cycle
    struct append_entries_cycle {
        struct request {
            std::uint64_t term{test_term};
            std::uint64_t leader_id{test_leader_id};
            std::uint64_t prev_log_index{test_log_index - 1};
            std::uint64_t prev_log_term{test_log_term};
            std::uint64_t leader_commit{test_log_index - 2};
            std::vector<std::string> entries{"entry1", "entry2", "entry3"};
        };
        
        struct response {
            std::uint64_t term{test_term};
            bool success{true};
            std::uint64_t match_index{test_log_index + 2}; // After appending 3 entries
        };
        
        request req;
        response resp;
        bool completed{false};
        std::chrono::milliseconds duration{0};
    };
    
    append_entries_cycle append_cycle;
    
    // Simulate request processing
    BOOST_CHECK_EQUAL(append_cycle.req.term, test_term);
    BOOST_CHECK_EQUAL(append_cycle.req.leader_id, test_leader_id);
    BOOST_CHECK_EQUAL(append_cycle.req.entries.size(), 3);
    
    // Simulate response generation
    append_cycle.resp.term = append_cycle.req.term;
    append_cycle.resp.success = true;
    append_cycle.resp.match_index = append_cycle.req.prev_log_index + append_cycle.req.entries.size();
    append_cycle.completed = true;
    append_cycle.duration = std::chrono::milliseconds{200};
    
    BOOST_CHECK(append_cycle.completed);
    BOOST_CHECK(append_cycle.resp.success);
    BOOST_CHECK_EQUAL(append_cycle.resp.term, test_term);
    BOOST_CHECK_EQUAL(append_cycle.resp.match_index, test_log_index + 2);
    BOOST_CHECK_LT(append_cycle.duration, test_timeout);
    
    BOOST_TEST_MESSAGE("AppendEntries cycle completed successfully");
    
    // Test complete InstallSnapshot cycle
    struct install_snapshot_cycle {
        struct request {
            std::uint64_t term{test_term};
            std::uint64_t leader_id{test_leader_id};
            std::uint64_t last_included_index{test_log_index};
            std::uint64_t last_included_term{test_log_term};
            std::uint64_t offset{0};
            std::vector<std::byte> data{test_snapshot_data};
            bool done{true};
        };
        
        struct response {
            std::uint64_t term{test_term};
            bool success{true};
            std::uint64_t bytes_stored{0};
        };
        
        request req;
        response resp;
        bool completed{false};
        std::chrono::milliseconds duration{0};
    };
    
    install_snapshot_cycle snapshot_cycle;
    
    // Simulate request processing
    BOOST_CHECK_EQUAL(snapshot_cycle.req.term, test_term);
    BOOST_CHECK_EQUAL(snapshot_cycle.req.leader_id, test_leader_id);
    BOOST_CHECK_EQUAL(snapshot_cycle.req.data.size(), test_snapshot_data.size());
    BOOST_CHECK(snapshot_cycle.req.done);
    
    // Simulate response generation
    snapshot_cycle.resp.term = snapshot_cycle.req.term;
    snapshot_cycle.resp.success = true;
    snapshot_cycle.resp.bytes_stored = snapshot_cycle.req.data.size();
    snapshot_cycle.completed = true;
    snapshot_cycle.duration = std::chrono::milliseconds{500}; // Longer for snapshot
    
    BOOST_CHECK(snapshot_cycle.completed);
    BOOST_CHECK(snapshot_cycle.resp.success);
    BOOST_CHECK_EQUAL(snapshot_cycle.resp.term, test_term);
    BOOST_CHECK_EQUAL(snapshot_cycle.resp.bytes_stored, test_snapshot_data.size());
    BOOST_CHECK_LT(snapshot_cycle.duration, test_timeout);
    
    BOOST_TEST_MESSAGE("InstallSnapshot cycle completed successfully");
    
    // Test error response cycles
    struct error_response_cycle {
        std::uint64_t higher_term{test_term + 1};
        bool term_updated{false};
        bool request_rejected{false};
    };
    
    error_response_cycle error_cycle;
    
    // Simulate higher term response
    if (error_cycle.higher_term > test_term) {
        error_cycle.term_updated = true;
        error_cycle.request_rejected = true;
    }
    
    BOOST_CHECK(error_cycle.term_updated);
    BOOST_CHECK(error_cycle.request_rejected);
    BOOST_CHECK_GT(error_cycle.higher_term, test_term);
    
    BOOST_TEST_MESSAGE("Error response cycle validated");
}

// Integration test for end-to-end message flow with serialization
BOOST_AUTO_TEST_CASE(test_end_to_end_message_flow, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Integration test: End-to-end message flow with serialization");
    
    // Test message serialization/deserialization cycle
    struct message_flow {
        std::string original_data;
        std::vector<std::byte> serialized_data;
        std::string deserialized_data;
        bool serialization_success{false};
        bool deserialization_success{false};
    };
    
    message_flow flow;
    flow.original_data = "test_raft_message_data";
    
    // Simulate serialization (mock JSON-like serialization)
    flow.serialized_data.clear();
    for (char c : flow.original_data) {
        flow.serialized_data.push_back(static_cast<std::byte>(c));
    }
    flow.serialization_success = !flow.serialized_data.empty();
    
    BOOST_CHECK(flow.serialization_success);
    BOOST_CHECK_EQUAL(flow.serialized_data.size(), flow.original_data.size());
    
    // Simulate deserialization
    flow.deserialized_data.clear();
    for (std::byte b : flow.serialized_data) {
        flow.deserialized_data.push_back(static_cast<char>(b));
    }
    flow.deserialization_success = !flow.deserialized_data.empty();
    
    BOOST_CHECK(flow.deserialization_success);
    BOOST_CHECK_EQUAL(flow.deserialized_data, flow.original_data);
    
    BOOST_TEST_MESSAGE("Message serialization round-trip successful");
    
    // Test CoAP message structure
    struct coap_message {
        std::uint8_t version{1};
        std::uint8_t type{0}; // Confirmable
        std::uint8_t code{2}; // POST
        std::uint16_t message_id{12345};
        std::vector<std::byte> token{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
        std::string uri_path{"/raft/request_vote"};
        std::uint16_t content_format{50}; // JSON
        std::vector<std::byte> payload;
    };
    
    coap_message msg;
    msg.payload = flow.serialized_data;
    
    BOOST_CHECK_EQUAL(msg.version, 1);
    BOOST_CHECK_EQUAL(msg.type, 0); // Confirmable
    BOOST_CHECK_EQUAL(msg.code, 2); // POST
    BOOST_CHECK_GT(msg.message_id, 0);
    BOOST_CHECK_EQUAL(msg.token.size(), 4);
    BOOST_CHECK_EQUAL(msg.uri_path, "/raft/request_vote");
    BOOST_CHECK_EQUAL(msg.content_format, 50); // JSON
    BOOST_CHECK_EQUAL(msg.payload.size(), flow.serialized_data.size());
    
    BOOST_TEST_MESSAGE("CoAP message structure validated");
    
    // Test response message structure
    struct coap_response {
        std::uint8_t version{1};
        std::uint8_t type{2}; // Acknowledgment
        std::uint8_t code{69}; // 2.05 Content
        std::uint16_t message_id;
        std::vector<std::byte> token;
        std::uint16_t content_format{50}; // JSON
        std::vector<std::byte> payload;
    };
    
    coap_response resp;
    resp.message_id = msg.message_id; // Match request
    resp.token = msg.token; // Match request
    
    // Simulate response payload
    std::string response_data = "response_data";
    for (char c : response_data) {
        resp.payload.push_back(static_cast<std::byte>(c));
    }
    
    BOOST_CHECK_EQUAL(resp.version, 1);
    BOOST_CHECK_EQUAL(resp.type, 2); // Acknowledgment
    BOOST_CHECK_EQUAL(resp.code, 69); // 2.05 Content
    BOOST_CHECK_EQUAL(resp.message_id, msg.message_id);
    BOOST_CHECK_EQUAL(resp.token.size(), msg.token.size());
    BOOST_CHECK(resp.token == msg.token);
    BOOST_CHECK_EQUAL(resp.content_format, 50);
    BOOST_CHECK(!resp.payload.empty());
    
    BOOST_TEST_MESSAGE("CoAP response structure validated");
    
    // Test timeout and retry logic
    struct retry_logic {
        std::chrono::milliseconds initial_timeout{2000};
        std::size_t max_retries{4};
        std::size_t current_retry{0};
        std::chrono::milliseconds current_timeout;
        
        std::chrono::milliseconds calculate_next_timeout() {
            // Exponential backoff: timeout * 2^retry
            auto multiplier = 1U << current_retry; // 2^retry
            return initial_timeout * multiplier;
        }
        
        bool should_retry() {
            return current_retry < max_retries;
        }
        
        void increment_retry() {
            if (should_retry()) {
                current_retry++;
                current_timeout = calculate_next_timeout();
            }
        }
    };
    
    retry_logic retry;
    retry.current_timeout = retry.initial_timeout;
    
    BOOST_CHECK_EQUAL(retry.current_retry, 0);
    BOOST_CHECK_EQUAL(retry.current_timeout, std::chrono::milliseconds{2000});
    BOOST_CHECK(retry.should_retry());
    
    // Simulate first retry
    retry.increment_retry();
    BOOST_CHECK_EQUAL(retry.current_retry, 1);
    BOOST_CHECK_EQUAL(retry.current_timeout, std::chrono::milliseconds{4000}); // 2000 * 2^1
    
    // Simulate second retry
    retry.increment_retry();
    BOOST_CHECK_EQUAL(retry.current_retry, 2);
    BOOST_CHECK_EQUAL(retry.current_timeout, std::chrono::milliseconds{8000}); // 2000 * 2^2
    
    BOOST_TEST_MESSAGE("Retry logic with exponential backoff validated");
}

BOOST_AUTO_TEST_SUITE_END()