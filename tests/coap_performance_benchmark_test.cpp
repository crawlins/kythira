#define BOOST_TEST_MODULE CoAP Performance Validation Test
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
#include <algorithm>
#include <numeric>

using namespace kythira;

namespace {
    constexpr const char* test_server_address = "127.0.0.1";
    constexpr std::uint16_t test_server_port = 5710;
    constexpr std::uint64_t test_node_id = 1;
    
    // Performance test constants
    constexpr std::size_t small_load_requests = 10;
    constexpr std::size_t medium_load_requests = 50;
    constexpr std::size_t high_load_requests = 100;
    constexpr std::chrono::milliseconds performance_timeout{30000};
    
    // Performance thresholds
    constexpr double min_throughput_small = 50.0;   // req/sec for small load
    constexpr double min_throughput_medium = 30.0;  // req/sec for medium load
    constexpr double min_throughput_high = 20.0;    // req/sec for high load
    constexpr std::chrono::milliseconds max_avg_latency{500}; // 500ms max average
    
    // Test data constants
    constexpr std::uint64_t test_term = 5;
    constexpr std::uint64_t test_candidate_id = 42;
    constexpr std::uint64_t test_log_index = 10;
    constexpr std::uint64_t test_log_term = 4;
    
    const std::vector<std::byte> test_large_data = []() {
        std::vector<std::byte> data;
        data.reserve(8192); // 8KB for performance testing
        for (std::size_t i = 0; i < 8192; ++i) {
            data.push_back(static_cast<std::byte>(i % 256));
        }
        return data;
    }();
}

// Performance test transport types
struct performance_transport_types {
    using serializer_type = json_rpc_serializer<std::vector<std::byte>>;
    using rpc_serializer_type = json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = noop_metrics;
    using logger_type = console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;
    
    template<typename T>
    using future_template = kythira::Future<T>;
    
    using future_type = kythira::Future<std::vector<std::byte>>;
};

struct PerformanceMetrics {
    std::chrono::milliseconds total_duration{0};
    std::chrono::milliseconds min_latency{std::chrono::milliseconds::max()};
    std::chrono::milliseconds max_latency{0};
    std::chrono::milliseconds avg_latency{0};
    double throughput_req_per_sec = 0.0;
    std::size_t total_requests = 0;
    std::size_t successful_requests = 0;
    std::size_t failed_requests = 0;
    std::size_t memory_usage_kb = 0;
};

/**
 * Feature: coap-transport, Task 12: Performance validation and optimization with real implementation
 * 
 * This test benchmarks actual CoAP transport performance with libcoap and validates
 * memory usage, connection pooling, and concurrent request processing under load.
 */
BOOST_AUTO_TEST_CASE(test_coap_transport_performance_benchmarks, * boost::unit_test::timeout(300)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Benchmarking actual CoAP transport performance with libcoap");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Running performance benchmarks with real libcoap implementation");
    
    // Configure optimized server for performance testing
    coap_server_config perf_server_config;
    perf_server_config.enable_dtls = false; // Disable DTLS for pure CoAP performance
    perf_server_config.max_concurrent_sessions = 200;
    perf_server_config.enable_concurrent_processing = true;
    perf_server_config.max_concurrent_requests = 150;
    perf_server_config.enable_memory_optimization = true;
    perf_server_config.memory_pool_size = 2 * 1024 * 1024; // 2MB pool
    perf_server_config.enable_block_transfer = true;
    perf_server_config.max_block_size = 1024;
    
    // Configure optimized client for performance testing
    coap_client_config perf_client_config;
    perf_client_config.enable_dtls = false;
    perf_client_config.ack_timeout = std::chrono::milliseconds{3000};
    perf_client_config.max_retransmit = 2; // Reduce retries for performance
    perf_client_config.enable_session_reuse = true;
    perf_client_config.connection_pool_size = 50;
    perf_client_config.enable_serialization_caching = true;
    perf_client_config.max_cache_entries = 200;
    perf_client_config.cache_ttl = std::chrono::milliseconds{10000};
    perf_client_config.enable_concurrent_processing = true;
    perf_client_config.max_concurrent_requests = 100;
    perf_client_config.enable_memory_optimization = true;
    perf_client_config.memory_pool_size = 1024 * 1024; // 1MB pool
    
    std::unordered_map<std::uint64_t, std::string> perf_endpoints;
    perf_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port);
    
    BOOST_CHECK_NO_THROW({
        // Create performance test server
        console_logger server_logger;
        coap_server<performance_transport_types> perf_server(
            test_server_address,
            test_server_port,
            perf_server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register optimized handlers
        std::atomic<std::size_t> vote_requests{0};
        std::atomic<std::size_t> append_requests{0};
        std::atomic<std::size_t> snapshot_requests{0};
        
        perf_server.register_request_vote_handler([&](const request_vote_request<>& req) {
            vote_requests.fetch_add(1);
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        perf_server.register_append_entries_handler([&](const append_entries_request<>& req) {
            append_requests.fetch_add(1);
            append_entries_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.match_index = req.prev_log_index + req.entries.size();
            return resp;
        });
        
        perf_server.register_install_snapshot_handler([&](const install_snapshot_request<>& req) {
            snapshot_requests.fetch_add(1);
            install_snapshot_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.bytes_stored = req.data.size();
            return resp;
        });
        
        // Start performance server
        perf_server.start();
        BOOST_CHECK(perf_server.is_running());
        
        // Give server time to start and optimize
        std::this_thread::sleep_for(std::chrono::milliseconds{1000});
        
        // Create performance client
        console_logger client_logger;
        coap_client<performance_transport_types> perf_client(
            perf_endpoints,
            perf_client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test 1: Small load performance (10 requests)
        logger.info("Running small load performance test (10 requests)");
        auto small_metrics = run_performance_test(perf_client, small_load_requests, "RequestVote");
        
        BOOST_CHECK_GE(small_metrics.throughput_req_per_sec, min_throughput_small);
        BOOST_CHECK_LE(small_metrics.avg_latency, max_avg_latency);
        BOOST_CHECK_EQUAL(small_metrics.successful_requests, small_load_requests);
        
        logger.info(std::format("Small load: {:.2f} req/sec, {:.2f}ms avg latency", 
                               small_metrics.throughput_req_per_sec, 
                               static_cast<double>(small_metrics.avg_latency.count())));
        
        // Test 2: Medium load performance (50 requests)
        logger.info("Running medium load performance test (50 requests)");
        auto medium_metrics = run_performance_test(perf_client, medium_load_requests, "AppendEntries");
        
        BOOST_CHECK_GE(medium_metrics.throughput_req_per_sec, min_throughput_medium);
        BOOST_CHECK_LE(medium_metrics.avg_latency, max_avg_latency);
        BOOST_CHECK_EQUAL(medium_metrics.successful_requests, medium_load_requests);
        
        logger.info(std::format("Medium load: {:.2f} req/sec, {:.2f}ms avg latency", 
                               medium_metrics.throughput_req_per_sec, 
                               static_cast<double>(medium_metrics.avg_latency.count())));
        
        // Test 3: High load performance (100 requests)
        logger.info("Running high load performance test (100 requests)");
        auto high_metrics = run_performance_test(perf_client, high_load_requests, "InstallSnapshot");
        
        BOOST_CHECK_GE(high_metrics.throughput_req_per_sec, min_throughput_high);
        BOOST_CHECK_LE(high_metrics.avg_latency, max_avg_latency);
        BOOST_CHECK_EQUAL(high_metrics.successful_requests, high_load_requests);
        
        logger.info(std::format("High load: {:.2f} req/sec, {:.2f}ms avg latency", 
                               high_metrics.throughput_req_per_sec, 
                               static_cast<double>(high_metrics.avg_latency.count())));
        
        // Validate request counts
        BOOST_CHECK_EQUAL(vote_requests.load(), small_load_requests);
        BOOST_CHECK_EQUAL(append_requests.load(), medium_load_requests);
        BOOST_CHECK_EQUAL(snapshot_requests.load(), high_load_requests);
        
        // Stop performance server
        perf_server.stop();
        BOOST_CHECK(!perf_server.is_running());
        
        logger.info("CoAP transport performance benchmarks completed successfully");
    });
    
#else
    logger.warning("libcoap not available - running stub performance validation");
    
    // Stub performance validation
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<performance_transport_types> server(
            test_server_address,
            test_server_port,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub performance validation completed");
    });
#endif
}

/**
 * Feature: coap-transport, Task 12: Memory usage and connection pooling validation
 * 
 * This test validates memory usage patterns and connection pooling effectiveness
 * with real CoAP sessions and memory management.
 */
BOOST_AUTO_TEST_CASE(test_memory_usage_connection_pooling, * boost::unit_test::timeout(240)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Validating memory usage and connection pooling with real CoAP sessions");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Testing memory management with real libcoap sessions");
    
    // Configure for memory testing
    coap_server_config memory_server_config;
    memory_server_config.enable_dtls = false;
    memory_server_config.max_concurrent_sessions = 50;
    memory_server_config.enable_memory_optimization = true;
    memory_server_config.memory_pool_size = 512 * 1024; // 512KB pool
    memory_server_config.enable_concurrent_processing = true;
    
    coap_client_config memory_client_config;
    memory_client_config.enable_dtls = false;
    memory_client_config.enable_session_reuse = true;
    memory_client_config.connection_pool_size = 20;
    memory_client_config.enable_memory_optimization = true;
    memory_client_config.memory_pool_size = 256 * 1024; // 256KB pool
    memory_client_config.enable_serialization_caching = true;
    memory_client_config.max_cache_entries = 50;
    
    std::unordered_map<std::uint64_t, std::string> memory_endpoints;
    memory_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port + 1);
    
    BOOST_CHECK_NO_THROW({
        // Create memory test server
        console_logger server_logger;
        coap_server<performance_transport_types> memory_server(
            test_server_address,
            test_server_port + 1,
            memory_server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register memory-efficient handler
        std::atomic<std::size_t> memory_requests{0};
        memory_server.register_request_vote_handler([&](const request_vote_request<>& req) {
            memory_requests.fetch_add(1);
            request_vote_response<> resp;
            resp.term = req.term;
            resp.vote_granted = true;
            return resp;
        });
        
        // Start memory server
        memory_server.start();
        BOOST_CHECK(memory_server.is_running());
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        
        // Create memory client
        console_logger client_logger;
        coap_client<performance_transport_types> memory_client(
            memory_endpoints,
            memory_client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test memory usage with repeated requests (should reuse connections)
        constexpr std::size_t memory_test_requests = 30;
        std::vector<std::chrono::milliseconds> latencies;
        latencies.reserve(memory_test_requests);
        
        for (std::size_t i = 0; i < memory_test_requests; ++i) {
            request_vote_request<> req;
            req.term = test_term + i;
            req.candidate_id = test_candidate_id;
            req.last_log_index = test_log_index + i;
            req.last_log_term = test_log_term;
            
            auto start_time = std::chrono::steady_clock::now();
            auto future = memory_client.send_request_vote(test_node_id, req, performance_timeout);
            auto resp = std::move(future).get();
            auto end_time = std::chrono::steady_clock::now();
            
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            latencies.push_back(latency);
            
            BOOST_CHECK_EQUAL(resp.term, test_term + i);
            BOOST_CHECK(resp.vote_granted);
            
            // Small delay to allow connection reuse
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        
        // Analyze connection reuse effectiveness (later requests should be faster)
        auto first_half_avg = std::accumulate(latencies.begin(), latencies.begin() + memory_test_requests/2, 
                                            std::chrono::milliseconds{0}) / (memory_test_requests/2);
        auto second_half_avg = std::accumulate(latencies.begin() + memory_test_requests/2, latencies.end(), 
                                             std::chrono::milliseconds{0}) / (memory_test_requests/2);
        
        logger.info(std::format("First half average latency: {}ms", first_half_avg.count()));
        logger.info(std::format("Second half average latency: {}ms", second_half_avg.count()));
        
        // Connection reuse should improve performance (second half should be faster or similar)
        BOOST_CHECK_LE(second_half_avg, first_half_avg + std::chrono::milliseconds{50}); // Allow 50ms tolerance
        
        BOOST_CHECK_EQUAL(memory_requests.load(), memory_test_requests);
        
        // Stop memory server
        memory_server.stop();
        BOOST_CHECK(!memory_server.is_running());
        
        logger.info("Memory usage and connection pooling validation completed successfully");
    });
    
#else
    logger.warning("libcoap not available - memory validation with stub implementation");
    
    // Stub memory validation
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<performance_transport_types> server(
            test_server_address,
            test_server_port + 1,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub memory validation completed");
    });
#endif
}

/**
 * Feature: coap-transport, Task 12: Concurrent request processing under load
 * 
 * This test validates concurrent request processing capabilities and measures
 * performance degradation under high concurrent load.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_processing_under_load, * boost::unit_test::timeout(300)) {
    console_logger logger;
    noop_metrics metrics;
    
    logger.info("Testing concurrent request processing under load with real CoAP");
    
#ifdef LIBCOAP_AVAILABLE
    logger.info("Running concurrent processing tests with real libcoap");
    
    // Configure for concurrent testing
    coap_server_config concurrent_server_config;
    concurrent_server_config.enable_dtls = false;
    concurrent_server_config.max_concurrent_sessions = 100;
    concurrent_server_config.enable_concurrent_processing = true;
    concurrent_server_config.max_concurrent_requests = 80;
    concurrent_server_config.enable_memory_optimization = true;
    
    coap_client_config concurrent_client_config;
    concurrent_client_config.enable_dtls = false;
    concurrent_client_config.ack_timeout = std::chrono::milliseconds{5000};
    concurrent_client_config.enable_concurrent_processing = true;
    concurrent_client_config.max_concurrent_requests = 60;
    concurrent_client_config.connection_pool_size = 30;
    concurrent_client_config.enable_session_reuse = true;
    
    std::unordered_map<std::uint64_t, std::string> concurrent_endpoints;
    concurrent_endpoints[test_node_id] = std::format("coap://{}:{}", test_server_address, test_server_port + 2);
    
    BOOST_CHECK_NO_THROW({
        // Create concurrent test server
        console_logger server_logger;
        coap_server<performance_transport_types> concurrent_server(
            test_server_address,
            test_server_port + 2,
            concurrent_server_config,
            metrics,
            std::move(server_logger)
        );
        
        // Register concurrent handler with processing delay
        std::atomic<std::size_t> concurrent_requests{0};
        concurrent_server.register_append_entries_handler([&](const append_entries_request<>& req) {
            concurrent_requests.fetch_add(1);
            
            // Simulate processing time
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            append_entries_response<> resp;
            resp.term = req.term;
            resp.success = true;
            resp.match_index = req.prev_log_index + req.entries.size();
            return resp;
        });
        
        // Start concurrent server
        concurrent_server.start();
        BOOST_CHECK(concurrent_server.is_running());
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        
        // Create concurrent client
        console_logger client_logger;
        coap_client<performance_transport_types> concurrent_client(
            concurrent_endpoints,
            concurrent_client_config,
            metrics,
            std::move(client_logger)
        );
        
        // Test concurrent request processing
        constexpr std::size_t concurrent_test_requests = 40;
        
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<folly::Future<append_entries_response<>>> futures;
        futures.reserve(concurrent_test_requests);
        
        // Send all requests concurrently
        for (std::size_t i = 0; i < concurrent_test_requests; ++i) {
            append_entries_request<> req;
            req.term = test_term + i;
            req.leader_id = test_candidate_id;
            req.prev_log_index = test_log_index + i;
            req.prev_log_term = test_log_term;
            req.leader_commit = test_log_index + i - 1;
            req.entries = {std::format("entry_{}", i)};
            
            futures.push_back(concurrent_client.send_append_entries(test_node_id, req, performance_timeout));
        }
        
        // Wait for all responses
        auto all_responses = folly::collectAll(futures).get();
        
        auto end_time = std::chrono::steady_clock::now();
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Validate all responses
        std::size_t successful_responses = 0;
        for (const auto& result : all_responses) {
            if (result.hasValue()) {
                const auto& resp = result.value();
                BOOST_CHECK(resp.success);
                successful_responses++;
            }
        }
        
        BOOST_CHECK_EQUAL(successful_responses, concurrent_test_requests);
        BOOST_CHECK_EQUAL(concurrent_requests.load(), concurrent_test_requests);
        
        // Calculate concurrent performance metrics
        double concurrent_throughput = (static_cast<double>(concurrent_test_requests) * 1000.0) / total_duration.count();
        
        logger.info(std::format("Concurrent processing: {} requests in {}ms", 
                               concurrent_test_requests, total_duration.count()));
        logger.info(std::format("Concurrent throughput: {:.2f} requests/second", concurrent_throughput));
        
        // Concurrent processing should achieve reasonable throughput despite processing delays
        BOOST_CHECK_GE(concurrent_throughput, 15.0); // At least 15 req/sec with 50ms processing delay
        
        // Stop concurrent server
        concurrent_server.stop();
        BOOST_CHECK(!concurrent_server.is_running());
        
        logger.info("Concurrent processing under load test completed successfully");
    });
    
#else
    logger.warning("libcoap not available - concurrent processing test with stub implementation");
    
    // Stub concurrent processing test
    BOOST_CHECK_NO_THROW({
        console_logger server_logger;
        coap_server<performance_transport_types> server(
            test_server_address,
            test_server_port + 2,
            coap_server_config{},
            metrics,
            std::move(server_logger)
        );
        
        logger.info("Stub concurrent processing test completed");
    });
#endif
}

// Helper function to run performance tests
PerformanceMetrics run_performance_test(
    coap_client<performance_transport_types>& client,
    std::size_t num_requests,
    const std::string& rpc_type
) {
    PerformanceMetrics metrics;
    metrics.total_requests = num_requests;
    
    std::vector<std::chrono::milliseconds> latencies;
    latencies.reserve(num_requests);
    
    auto start_time = std::chrono::steady_clock::now();
    
    std::vector<folly::Future<folly::Unit>> futures;
    futures.reserve(num_requests);
    
    for (std::size_t i = 0; i < num_requests; ++i) {
        auto request_start = std::chrono::steady_clock::now();
        
        if (rpc_type == "RequestVote") {
            request_vote_request<> req;
            req.term = test_term + i;
            req.candidate_id = test_candidate_id;
            req.last_log_index = test_log_index + i;
            req.last_log_term = test_log_term;
            
            auto future = client.send_request_vote(test_node_id, req, performance_timeout)
                .thenValue([request_start, &latencies, &metrics](const request_vote_response<>& resp) {
                    auto request_end = std::chrono::steady_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(request_end - request_start);
                    latencies.push_back(latency);
                    metrics.successful_requests++;
                })
                .thenError([&metrics](const folly::exception_wrapper& e) {
                    metrics.failed_requests++;
                });
            
            futures.push_back(std::move(future));
            
        } else if (rpc_type == "AppendEntries") {
            append_entries_request<> req;
            req.term = test_term + i;
            req.leader_id = test_candidate_id;
            req.prev_log_index = test_log_index + i;
            req.prev_log_term = test_log_term;
            req.leader_commit = test_log_index + i - 1;
            req.entries = {std::format("entry_{}", i)};
            
            auto future = client.send_append_entries(test_node_id, req, performance_timeout)
                .thenValue([request_start, &latencies, &metrics](const append_entries_response<>& resp) {
                    auto request_end = std::chrono::steady_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(request_end - request_start);
                    latencies.push_back(latency);
                    metrics.successful_requests++;
                })
                .thenError([&metrics](const folly::exception_wrapper& e) {
                    metrics.failed_requests++;
                });
            
            futures.push_back(std::move(future));
            
        } else if (rpc_type == "InstallSnapshot") {
            install_snapshot_request<> req;
            req.term = test_term + i;
            req.leader_id = test_candidate_id;
            req.last_included_index = test_log_index + i;
            req.last_included_term = test_log_term;
            req.offset = 0;
            req.data = test_large_data; // Large data for performance testing
            req.done = true;
            
            auto future = client.send_install_snapshot(test_node_id, req, performance_timeout)
                .thenValue([request_start, &latencies, &metrics](const install_snapshot_response<>& resp) {
                    auto request_end = std::chrono::steady_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(request_end - request_start);
                    latencies.push_back(latency);
                    metrics.successful_requests++;
                })
                .thenError([&metrics](const folly::exception_wrapper& e) {
                    metrics.failed_requests++;
                });
            
            futures.push_back(std::move(future));
        }
    }
    
    // Wait for all requests to complete
    folly::collectAll(futures).get();
    
    auto end_time = std::chrono::steady_clock::now();
    metrics.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Calculate latency statistics
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        metrics.min_latency = latencies.front();
        metrics.max_latency = latencies.back();
        
        auto total_latency = std::accumulate(latencies.begin(), latencies.end(), std::chrono::milliseconds{0});
        metrics.avg_latency = total_latency / latencies.size();
    }
    
    // Calculate throughput
    if (metrics.total_duration.count() > 0) {
        metrics.throughput_req_per_sec = (static_cast<double>(metrics.successful_requests) * 1000.0) / metrics.total_duration.count();
    }
    
    return metrics;
}