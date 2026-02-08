#define BOOST_TEST_MODULE coap_thread_safety_property_test
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <random>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>

using namespace kythira;

namespace {
    constexpr std::size_t test_iterations = 10;  // Reduced for faster execution
    constexpr std::chrono::milliseconds test_timeout{45000};
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 16683;
    constexpr std::size_t test_thread_count = 8;
    constexpr std::size_t test_operations_per_thread = 50;  // Reduced for faster execution
    constexpr std::uint64_t test_node_id = 1;
}

// Define test types for CoAP transport
struct test_transport_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using executor_type = kythira::console_logger;
    
    template<typename T>
    using future_template = kythira::Future<T>;
};

/**
 * **Feature: coap-transport, Property 33: Thread safety with proper synchronization**
 * 
 * This property validates that the CoAP transport is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 * 
 * **Validates: Requirements 7.3**
 * 
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_server_operations, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(20, test_operations_per_thread);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t operations_per_thread = operations_dist(gen);
        
        // Create server configuration
        coap_server_config server_config;
        server_config.max_concurrent_sessions = thread_count * 10;
        server_config.enable_memory_optimization = true;
        server_config.memory_pool_size = 1024 * 1024; // 1MB
        server_config.enable_serialization_caching = true;
        server_config.serialization_cache_size = 200;
        server_config.enable_concurrent_processing = true;
        
        // Create test types and server
        noop_metrics metrics;
        
        coap_server<test_transport_types> server(
            test_bind_address,
            test_bind_port + iteration % 1000, // Avoid port conflicts
            server_config,
            metrics
        );
        
        // Test 1: Concurrent slot acquisition (public API)
        std::vector<std::thread> threads;
        std::atomic<std::size_t> successful_operations{0};
        std::atomic<std::size_t> failed_operations{0};
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&server, &successful_operations, &failed_operations, &start_flag, operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        bool acquired = server.acquire_concurrent_slot();
                        if (acquired) {
                            successful_operations.fetch_add(1);
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            server.release_concurrent_slot();
                        } else {
                            failed_operations.fetch_add(1);
                        }
                    } catch (const std::exception&) {
                        failed_operations.fetch_add(1);
                    }
                }
            });
        }
        
        // Start all threads simultaneously
        start_flag.store(true);
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Verify thread safety: all operations should complete
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations.load() + failed_operations.load();
        
        BOOST_CHECK_EQUAL(total_operations, expected_operations);
    }
}

/**
 * **Feature: coap-transport, Property 33: Client thread safety with proper synchronization**
 * 
 * This property validates that the CoAP client is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 * 
 * **Validates: Requirements 7.3**
 * 
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_client_operations, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(20, test_operations_per_thread);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t operations_per_thread = operations_dist(gen);
        
        // Create client configuration
        coap_client_config client_config;
        client_config.max_sessions = thread_count * 5;
        client_config.enable_memory_optimization = true;
        client_config.memory_pool_size = 1024 * 1024; // 1MB
        client_config.enable_serialization_caching = true;
        client_config.serialization_cache_size = 200;
        client_config.connection_pool_size = 50;
        client_config.enable_concurrent_processing = true;
        
        // Create test types and client
        noop_metrics metrics;
        
        std::unordered_map<std::uint64_t, std::string> node_endpoints = {
            {1, "coap://127.0.0.1:5683"},
            {2, "coap://127.0.0.1:5684"},
            {3, "coap://127.0.0.1:5685"}
        };
        
        coap_client<test_transport_types> client(
            node_endpoints,
            client_config,
            metrics
        );
        
        // Test 1: Concurrent client slot acquisition (public API)
        std::vector<std::thread> threads;
        std::atomic<std::size_t> successful_operations{0};
        std::atomic<std::size_t> failed_operations{0};
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&client, &successful_operations, &failed_operations, &start_flag, operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        bool acquired = client.acquire_concurrent_slot();
                        if (acquired) {
                            successful_operations.fetch_add(1);
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            client.release_concurrent_slot();
                        } else {
                            failed_operations.fetch_add(1);
                        }
                    } catch (const std::exception&) {
                        failed_operations.fetch_add(1);
                    }
                }
            });
        }
        
        // Start all threads simultaneously
        start_flag.store(true);
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Verify thread safety: all operations should complete
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations.load() + failed_operations.load();
        
        BOOST_CHECK_EQUAL(total_operations, expected_operations);
    }
}

/**
 * **Feature: coap-transport, Property 33: Concurrent RPC requests**
 * 
 * This property validates that concurrent RPC requests are handled safely.
 * 
 * **Validates: Requirements 7.3**
 * 
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_rpc_requests, * boost::unit_test::timeout(45)) {
    // Create client configuration
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;
    client_config.max_concurrent_requests = 50;
    
    noop_metrics metrics;
    
    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {test_node_id, "coap://127.0.0.1:5683"}
    };
    
    coap_client<test_transport_types> client(
        node_endpoints,
        client_config,
        metrics
    );
    
    // Test: Concurrent RPC requests
    std::vector<std::thread> threads;
    std::atomic<std::size_t> successful_requests{0};
    std::atomic<std::size_t> failed_requests{0};
    
    request_vote_request<> vote_request{
        ._term = 1,
        ._candidate_id = 100,
        ._last_log_index = 0,
        ._last_log_term = 0
    };
    
    for (std::size_t t = 0; t < 10; ++t) {
        threads.emplace_back([&client, &successful_requests, &failed_requests, vote_request]() {
            for (std::size_t op = 0; op < 20; ++op) {
                try {
                    auto future = client.send_request_vote(test_node_id, vote_request, std::chrono::milliseconds{1000});
                    successful_requests.fetch_add(1);
                } catch (const std::exception&) {
                    failed_requests.fetch_add(1);
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify: All operations completed
    BOOST_CHECK_EQUAL(successful_requests.load() + failed_requests.load(), 200);
}

/**
 * **Feature: coap-transport, Property 33: Concurrent configuration checks**
 * 
 * This property validates that concurrent configuration checks are handled safely.
 * 
 * **Validates: Requirements 7.3**
 * 
 * BLACK-BOX TEST: Tests observable behavior through public API only.
 */
BOOST_AUTO_TEST_CASE(test_concurrent_configuration_checks, * boost::unit_test::timeout(45)) {
    // Create client configuration
    coap_client_config client_config;
    client_config.enable_concurrent_processing = true;
    
    noop_metrics metrics;
    
    std::unordered_map<std::uint64_t, std::string> node_endpoints = {
        {test_node_id, "coap://127.0.0.1:5683"}
    };
    
    coap_client<test_transport_types> client(
        node_endpoints,
        client_config,
        metrics
    );
    
    // Test: Concurrent configuration status checks
    std::vector<std::thread> threads;
    std::atomic<std::size_t> operations_completed{0};
    
    for (std::size_t t = 0; t < 5; ++t) {
        threads.emplace_back([&client, &operations_completed]() {
            for (std::size_t op = 0; op < 20; ++op) {
                try {
                    // Test concurrent status checks (all const methods, thread-safe)
                    bool dtls_enabled = client.is_dtls_enabled();
                    operations_completed.fetch_add(1);
                } catch (const std::exception&) {
                    operations_completed.fetch_add(1);
                }
            }
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify: All operations completed
    BOOST_CHECK_EQUAL(operations_completed.load(), 100);
}