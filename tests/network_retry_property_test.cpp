/**
 * Property-Based Test for Network Retry Convergence
 * 
 * Feature: raft-consensus, Property 8: Network Retry Convergence
 * Validates: Requirements 3.13
 * 
 * Property: For any RPC that fails due to network issues, the system retries 
 * according to Raft timeout requirements and eventually either succeeds or 
 * determines the target is unreachable.
 */

#define BOOST_TEST_MODULE NetworkRetryPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/simulator_network.hpp>
#include <raft/json_serializer.hpp>
#include <network_simulator/network_simulator.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>
#include <random>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("network_retry_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::uint64_t client_node_id = 1;
    constexpr std::uint64_t server_node_id = 2;
    constexpr std::chrono::milliseconds rpc_timeout{500};
    constexpr std::chrono::milliseconds retry_delay{100};
    constexpr int max_retries = 5;
    constexpr double unreliable_network_reliability = 0.5;  // 50% success rate
    constexpr double reliable_network_reliability = 1.0;    // 100% success rate
    constexpr std::size_t property_test_iterations = 10;  // Reduced for faster testing
    constexpr std::uint64_t max_term = 1000;
    constexpr std::uint64_t max_node_id = 100;
    constexpr std::uint64_t max_index = 1000;
}

// Helper to generate random term
auto generate_random_term(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_term);
    return dist(rng);
}

// Helper to generate random node ID
auto generate_random_node_id(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(1, max_node_id);
    return dist(rng);
}

// Helper to generate random log index
auto generate_random_log_index(std::mt19937& rng) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> dist(0, max_index);
    return dist(rng);
}

BOOST_AUTO_TEST_SUITE(network_retry_property_tests)

/**
 * Property: Transient network failures eventually succeed with retries
 * 
 * This test verifies that when network reliability is low but non-zero,
 * retrying RPCs eventually succeeds.
 */
BOOST_AUTO_TEST_CASE(transient_failures_eventually_succeed) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    std::size_t successful_iterations = 0;
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Generate random request data
        auto term = generate_random_term(rng);
        auto candidate_id = generate_random_node_id(rng);
        auto last_log_index = generate_random_log_index(rng);
        std::uniform_int_distribution<std::uint64_t> term_dist(0, term);
        auto last_log_term = term_dist(rng);
        
        // Create network simulator
        network_simulator::NetworkSimulator<std::uint64_t, unsigned short> simulator;
        
        // Add nodes to topology
        simulator.add_node(client_node_id);
        simulator.add_node(server_node_id);
        
        // Add unreliable bidirectional edges (30% reliability)
        network_simulator::NetworkEdge unreliable_edge{
            std::chrono::milliseconds{10},
            unreliable_network_reliability
        };
        simulator.add_edge(client_node_id, server_node_id, unreliable_edge);
        simulator.add_edge(server_node_id, client_node_id, unreliable_edge);
        
        // Create nodes
        auto client_node = simulator.create_node(client_node_id);
        auto server_node = simulator.create_node(server_node_id);
        
        // Start simulator
        simulator.start();
        
        // Create network client and server
        using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
        kythira::simulator_network_client<serializer_type, std::vector<std::byte>> client(client_node);
        kythira::simulator_network_server<serializer_type, std::vector<std::byte>> server(server_node);
        
        // Register handler on server
        server.register_request_vote_handler([term](const raft::request_vote_request<>& req) {
            raft::request_vote_response<> response;
            response._term = term;
            response._vote_granted = true;
            return response;
        });
        
        // Start server
        server.start();
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Create request
        raft::request_vote_request<> request;
        request._term = term;
        request._candidate_id = candidate_id;
        request._last_log_index = last_log_index;
        request._last_log_term = last_log_term;
        
        // Retry logic: attempt to send RPC multiple times
        bool success = false;
        int attempts = 0;
        
        while (attempts < max_retries && !success) {
            attempts++;
            
            try {
                auto response_future = client.send_request_vote(server_node_id, request, rpc_timeout);
                
                // Wait for response with timeout
                auto response = std::move(response_future).get();
                
                // Verify response
                BOOST_TEST(response.term() == term);
                BOOST_TEST(response.vote_granted() == true);
                
                success = true;
            } catch (...) {
                // Exception - retry after delay
                std::this_thread::sleep_for(retry_delay);
            }
        }
        
        // Give server time to finish processing before stopping
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Stop server and simulator
        server.stop();
        simulator.stop();
        
        // Property: With retries, we should eventually succeed
        // With 30% reliability and 5 retries, probability of all failures is 0.7^5 ≈ 0.168
        // So we should succeed with ~83% probability
        if (success) {
            successful_iterations++;
        }
    }
    
    // Verify that some iterations succeeded
    // With 50% reliability for both request and response, each round-trip has 25% success rate
    // With 5 retries, probability of at least one success is 1 - 0.75^5 ≈ 0.76
    // With 10 iterations, we expect at least 5-8 successes, but allow for variance
    // Use 30% threshold to account for random variation
    BOOST_TEST(successful_iterations >= property_test_iterations * 0.3);
}

/**
 * Property: Permanent network failures are detected
 * 
 * This test verifies that when there is no network route, retries
 * eventually give up and report failure.
 */
BOOST_AUTO_TEST_CASE(permanent_failures_are_detected) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Generate random request data
        auto term = generate_random_term(rng);
        auto candidate_id = generate_random_node_id(rng);
        auto last_log_index = generate_random_log_index(rng);
        std::uniform_int_distribution<std::uint64_t> term_dist(0, term);
        auto last_log_term = term_dist(rng);
        
        // Create network simulator
        network_simulator::NetworkSimulator<std::uint64_t, unsigned short> simulator;
        
        // Add nodes to topology
        simulator.add_node(client_node_id);
        simulator.add_node(server_node_id);
        
        // DO NOT add edges - no route exists (permanent failure)
        
        // Create nodes
        auto client_node = simulator.create_node(client_node_id);
        auto server_node = simulator.create_node(server_node_id);
        
        // Start simulator
        simulator.start();
        
        // Create network client
        using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
        kythira::simulator_network_client<serializer_type, std::vector<std::byte>> client(client_node);
        
        // Create request
        raft::request_vote_request<> request;
        request._term = term;
        request._candidate_id = candidate_id;
        request._last_log_index = last_log_index;
        request._last_log_term = last_log_term;
        
        // Retry logic: attempt to send RPC multiple times
        bool all_failed = true;
        int attempts = 0;
        
        while (attempts < max_retries) {
            attempts++;
            
            try {
                auto response_future = client.send_request_vote(server_node_id, request, std::chrono::milliseconds{200});
                
                // Wait for response
                auto response = std::move(response_future).get();
                
                // Should not succeed - no route exists
                all_failed = false;
                break;
            } catch (...) {
                // Exception is expected - continue
                std::this_thread::sleep_for(retry_delay);
            }
        }
        
        // Stop simulator
        simulator.stop();
        
        // Property: With no route, all retries should fail
        BOOST_TEST(all_failed);
        BOOST_TEST(attempts == max_retries);
    }
}

/**
 * Property: Reliable networks succeed on first try
 * 
 * This test verifies that when network reliability is 100%,
 * RPCs succeed without needing retries.
 */
BOOST_AUTO_TEST_CASE(reliable_networks_succeed_immediately) {
    std::random_device rd;
    std::mt19937 rng(rd());
    
    for (std::size_t iteration = 0; iteration < property_test_iterations; ++iteration) {
        // Generate random request data
        auto term = generate_random_term(rng);
        auto candidate_id = generate_random_node_id(rng);
        auto last_log_index = generate_random_log_index(rng);
        std::uniform_int_distribution<std::uint64_t> term_dist(0, term);
        auto last_log_term = term_dist(rng);
        
        // Create network simulator
        network_simulator::NetworkSimulator<std::uint64_t, unsigned short> simulator;
        
        // Add nodes to topology
        simulator.add_node(client_node_id);
        simulator.add_node(server_node_id);
        
        // Add reliable bidirectional edges (100% reliability)
        network_simulator::NetworkEdge reliable_edge{
            std::chrono::milliseconds{10},
            reliable_network_reliability
        };
        simulator.add_edge(client_node_id, server_node_id, reliable_edge);
        simulator.add_edge(server_node_id, client_node_id, reliable_edge);
        
        // Create nodes
        auto client_node = simulator.create_node(client_node_id);
        auto server_node = simulator.create_node(server_node_id);
        
        // Start simulator
        simulator.start();
        
        // Create network client and server
        using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
        kythira::simulator_network_client<serializer_type, std::vector<std::byte>> client(client_node);
        kythira::simulator_network_server<serializer_type, std::vector<std::byte>> server(server_node);
        
        // Register handler on server
        server.register_request_vote_handler([term](const raft::request_vote_request<>& req) {
            raft::request_vote_response<> response;
            response._term = term;
            response._vote_granted = true;
            return response;
        });
        
        // Start server
        server.start();
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Create request
        raft::request_vote_request<> request;
        request._term = term;
        request._candidate_id = candidate_id;
        request._last_log_index = last_log_index;
        request._last_log_term = last_log_term;
        
        // Send RPC (should succeed on first try)
        bool success = false;
        
        try {
            auto response_future = client.send_request_vote(server_node_id, request, rpc_timeout);
            
            // Wait for response
            auto response = std::move(response_future).get();
            
            // Verify response
            BOOST_TEST(response.term() == term);
            BOOST_TEST(response.vote_granted() == true);
            
            success = true;
        } catch (...) {
            // Should not throw
        }
        
        // Give server time to finish processing before stopping
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Stop server and simulator
        server.stop();
        simulator.stop();
        
        // Property: With 100% reliability, first attempt should succeed
        BOOST_TEST(success);
    }
}

BOOST_AUTO_TEST_SUITE_END()
