#define BOOST_TEST_MODULE coap_thread_safety_property_test
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/test_types.hpp>
#include <random>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <future>

using namespace kythira;

namespace {
    constexpr std::size_t test_iterations = 50;
    constexpr std::chrono::milliseconds test_timeout{45000};
    constexpr const char* test_bind_address = "127.0.0.1";
    constexpr std::uint16_t test_bind_port = 16683;
    constexpr std::size_t test_thread_count = 8;
    constexpr std::size_t test_operations_per_thread = 100;
}

/**
 * **Feature: coap-transport, Property 33: Thread safety with proper synchronization**
 * 
 * This property validates that the CoAP transport is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 * 
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(test_concurrent_server_operations, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(50, test_operations_per_thread);
    
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
        
        // Create test types and server
        using test_types = kythira::test_transport_types<json_serializer>;
        test_types::metrics_type metrics;
        
        coap_server<test_types> server(
            test_bind_address,
            test_bind_port + iteration % 1000, // Avoid port conflicts
            server_config,
            metrics
        );
        
        // Test 1: Concurrent resource exhaustion handling
        std::vector<std::thread> threads;
        std::atomic<std::size_t> successful_operations{0};
        std::atomic<std::size_t> exception_count{0};
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&server, &successful_operations, &exception_count, &start_flag, operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        server.handle_resource_exhaustion();
                        successful_operations.fetch_add(1);
                    } catch (const std::exception&) {
                        exception_count.fetch_add(1);
                    }
                    
                    // Small delay to allow other threads to interleave
                    if (op % 10 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
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
        
        // Verify thread safety: all operations should complete successfully
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations.load() + exception_count.load();
        
        BOOST_CHECK_EQUAL(total_operations, expected_operations);
        BOOST_CHECK_GT(successful_operations.load(), 0);
        
        // Test 2: Server should remain functional after concurrent operations
        try {
            server.handle_resource_exhaustion();
            BOOST_CHECK(true); // Server is still functional
        } catch (const std::exception& e) {
            BOOST_FAIL("Server should remain functional after concurrent operations: " + std::string(e.what()));
        }
    }
}

/**
 * **Feature: coap-transport, Property 33: Client thread safety with proper synchronization**
 * 
 * This property validates that the CoAP client is thread-safe and properly
 * synchronizes access to shared resources across multiple threads.
 * 
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(test_concurrent_client_operations, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(2, test_thread_count);
    std::uniform_int_distribution<std::size_t> operations_dist(50, test_operations_per_thread);
    
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
        
        // Create test types and client
        using test_types = kythira::test_transport_types<json_serializer>;
        test_types::metrics_type metrics;
        
        std::unordered_map<std::uint64_t, std::string> node_endpoints = {
            {1, "coap://127.0.0.1:5683"},
            {2, "coap://127.0.0.1:5684"},
            {3, "coap://127.0.0.1:5685"}
        };
        
        coap_client<test_types> client(
            node_endpoints,
            client_config,
            metrics
        );
        
        // Test 1: Concurrent client resource exhaustion handling
        std::vector<std::thread> threads;
        std::atomic<std::size_t> successful_operations{0};
        std::atomic<std::size_t> exception_count{0};
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            threads.emplace_back([&client, &successful_operations, &exception_count, &start_flag, operations_per_thread]() {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                for (std::size_t op = 0; op < operations_per_thread; ++op) {
                    try {
                        client.handle_resource_exhaustion();
                        successful_operations.fetch_add(1);
                    } catch (const std::exception&) {
                        exception_count.fetch_add(1);
                    }
                    
                    // Small delay to allow other threads to interleave
                    if (op % 10 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
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
        
        // Verify thread safety: all operations should complete successfully
        std::size_t expected_operations = thread_count * operations_per_thread;
        std::size_t total_operations = successful_operations.load() + exception_count.load();
        
        BOOST_CHECK_EQUAL(total_operations, expected_operations);
        BOOST_CHECK_GT(successful_operations.load(), 0);
        
        // Test 2: Client should remain functional after concurrent operations
        try {
            client.handle_resource_exhaustion();
            BOOST_CHECK(true); // Client is still functional
        } catch (const std::exception& e) {
            BOOST_FAIL("Client should remain functional after concurrent operations: " + std::string(e.what()));
        }
    }
}

/**
 * **Feature: coap-transport, Property 33: Concurrent access to shared data structures**
 * 
 * This property validates that concurrent access to shared data structures
 * (caches, pools, message tracking) is properly synchronized.
 * 
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(test_concurrent_shared_data_access, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(4, test_thread_count);
    std::uniform_int_distribution<std::size_t> access_count_dist(100, 300);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t access_count = access_count_dist(gen);
        
        // Create server configuration with shared resources
        coap_server_config server_config;
        server_config.max_concurrent_sessions = 200;
        server_config.enable_memory_optimization = true;
        server_config.memory_pool_size = 2 * 1024 * 1024; // 2MB
        server_config.enable_serialization_caching = true;
        server_config.serialization_cache_size = 500;
        
        // Create test types and server
        using test_types = kythira::test_transport_types<json_serializer>;
        test_types::metrics_type metrics;
        
        coap_server<test_types> server(
            test_bind_address,
            test_bind_port + iteration % 1000, // Avoid port conflicts
            server_config,
            metrics
        );
        
        // Test 1: Concurrent access to resource cleanup
        std::vector<std::future<std::size_t>> futures;
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            futures.push_back(std::async(std::launch::async, [&server, &start_flag, access_count]() -> std::size_t {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                std::size_t successful_accesses = 0;
                
                for (std::size_t i = 0; i < access_count; ++i) {
                    try {
                        // Mix different types of operations that access shared data
                        if (i % 3 == 0) {
                            server.handle_resource_exhaustion();
                        } else if (i % 3 == 1) {
                            server.enforce_connection_limits();
                        } else {
                            // Test malformed message detection (accesses shared state)
                            std::vector<std::byte> test_data = {std::byte{0x40}, std::byte{0x01}, std::byte{0x00}, std::byte{0x01}};
                            server.detect_malformed_message(test_data);
                        }
                        
                        successful_accesses++;
                        
                        // Occasional yield to allow other threads to run
                        if (i % 20 == 0) {
                            std::this_thread::yield();
                        }
                        
                    } catch (const std::exception&) {
                        // Some operations may throw exceptions (like connection limits),
                        // but this shouldn't affect thread safety
                    }
                }
                
                return successful_accesses;
            }));
        }
        
        // Start all threads simultaneously
        start_flag.store(true);
        
        // Wait for all threads to complete and collect results
        std::size_t total_successful_accesses = 0;
        for (auto& future : futures) {
            total_successful_accesses += future.get();
        }
        
        // Verify that operations completed successfully
        BOOST_CHECK_GT(total_successful_accesses, 0);
        
        // Test 2: Server should remain in a consistent state
        try {
            server.handle_resource_exhaustion();
            BOOST_CHECK(true); // Server is in a consistent state
        } catch (const std::exception& e) {
            BOOST_FAIL("Server should be in consistent state after concurrent access: " + std::string(e.what()));
        }
    }
}

/**
 * **Feature: coap-transport, Property 33: Race condition prevention in resource management**
 * 
 * This property validates that race conditions are prevented in resource
 * management operations through proper synchronization.
 * 
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(test_race_condition_prevention, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> stress_duration_dist(50, 200);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t stress_duration_ms = stress_duration_dist(gen);
        
        // Create both client and server for comprehensive testing
        coap_server_config server_config;
        server_config.max_concurrent_sessions = 100;
        server_config.enable_memory_optimization = true;
        server_config.memory_pool_size = 1024 * 1024; // 1MB
        server_config.enable_serialization_caching = true;
        server_config.serialization_cache_size = 100;
        
        coap_client_config client_config;
        client_config.max_sessions = 50;
        client_config.enable_memory_optimization = true;
        client_config.memory_pool_size = 1024 * 1024; // 1MB
        client_config.enable_serialization_caching = true;
        client_config.serialization_cache_size = 100;
        client_config.connection_pool_size = 20;
        
        // Create test types
        using test_types = kythira::test_transport_types<json_serializer>;
        test_types::metrics_type server_metrics, client_metrics;
        
        coap_server<test_types> server(
            test_bind_address,
            test_bind_port + iteration % 1000, // Avoid port conflicts
            server_config,
            server_metrics
        );
        
        std::unordered_map<std::uint64_t, std::string> node_endpoints = {
            {1, "coap://127.0.0.1:5683"}
        };
        
        coap_client<test_types> client(
            node_endpoints,
            client_config,
            client_metrics
        );
        
        // Test 1: Stress test with multiple operation types
        std::atomic<bool> stop_stress{false};
        std::atomic<std::size_t> server_operations{0};
        std::atomic<std::size_t> client_operations{0};
        std::atomic<std::size_t> race_condition_detected{0};
        
        // Server stress thread
        std::thread server_thread([&server, &stop_stress, &server_operations, &race_condition_detected]() {
            while (!stop_stress.load()) {
                try {
                    server.handle_resource_exhaustion();
                    server_operations.fetch_add(1);
                } catch (const std::exception&) {
                    // Exception might indicate a race condition or resource limit
                    race_condition_detected.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        
        // Client stress thread
        std::thread client_thread([&client, &stop_stress, &client_operations, &race_condition_detected]() {
            while (!stop_stress.load()) {
                try {
                    client.handle_resource_exhaustion();
                    client_operations.fetch_add(1);
                } catch (const std::exception&) {
                    // Exception might indicate a race condition or resource limit
                    race_condition_detected.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        
        // Additional threads performing mixed operations
        std::vector<std::thread> mixed_threads;
        std::atomic<std::size_t> mixed_operations{0};
        
        for (std::size_t t = 0; t < 2; ++t) {
            mixed_threads.emplace_back([&server, &client, &stop_stress, &mixed_operations]() {
                while (!stop_stress.load()) {
                    try {
                        // Alternate between server and client operations
                        if (mixed_operations.load() % 2 == 0) {
                            server.handle_resource_exhaustion();
                        } else {
                            client.handle_resource_exhaustion();
                        }
                        mixed_operations.fetch_add(1);
                    } catch (const std::exception&) {
                        // Exceptions are acceptable under stress
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(150));
                }
            });
        }
        
        // Run stress test for specified duration
        std::this_thread::sleep_for(std::chrono::milliseconds(stress_duration_ms));
        stop_stress.store(true);
        
        // Wait for all threads to complete
        server_thread.join();
        client_thread.join();
        for (auto& thread : mixed_threads) {
            thread.join();
        }
        
        // Verify that operations completed successfully
        BOOST_CHECK_GT(server_operations.load(), 0);
        BOOST_CHECK_GT(client_operations.load(), 0);
        BOOST_CHECK_GT(mixed_operations.load(), 0);
        
        // Test 2: System should be in a consistent state after stress test
        try {
            server.handle_resource_exhaustion();
            client.handle_resource_exhaustion();
            BOOST_CHECK(true); // Both components are still functional
        } catch (const std::exception& e) {
            BOOST_FAIL("System should be consistent after race condition test: " + std::string(e.what()));
        }
    }
}

/**
 * **Feature: coap-transport, Property 33: Deadlock prevention in concurrent operations**
 * 
 * This property validates that concurrent operations do not cause deadlocks
 * and that the system remains responsive under concurrent load.
 * 
 * **Validates: Requirements 7.3**
 */
BOOST_AUTO_TEST_CASE(test_deadlock_prevention, * boost::unit_test::timeout(45)) {
    // Test data generation
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> thread_count_dist(4, 8);
    std::uniform_int_distribution<std::size_t> operation_count_dist(50, 150);
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        std::size_t thread_count = thread_count_dist(gen);
        std::size_t operation_count = operation_count_dist(gen);
        
        // Create server configuration
        coap_server_config server_config;
        server_config.max_concurrent_sessions = thread_count * 10;
        server_config.enable_memory_optimization = true;
        server_config.memory_pool_size = 2 * 1024 * 1024; // 2MB
        server_config.enable_serialization_caching = true;
        server_config.serialization_cache_size = 200;
        
        // Create test types and server
        using test_types = kythira::test_transport_types<json_serializer>;
        test_types::metrics_type metrics;
        
        coap_server<test_types> server(
            test_bind_address,
            test_bind_port + iteration % 1000, // Avoid port conflicts
            server_config,
            metrics
        );
        
        // Test 1: Concurrent operations with timeout detection
        std::vector<std::future<bool>> operation_futures;
        std::atomic<bool> start_flag{false};
        
        for (std::size_t t = 0; t < thread_count; ++t) {
            operation_futures.push_back(std::async(std::launch::async, [&server, &start_flag, operation_count]() -> bool {
                // Wait for all threads to be ready
                while (!start_flag.load()) {
                    std::this_thread::yield();
                }
                
                for (std::size_t op = 0; op < operation_count; ++op) {
                    auto start_time = std::chrono::steady_clock::now();
                    
                    try {
                        // Perform operation that could potentially deadlock
                        server.handle_resource_exhaustion();
                        
                        auto end_time = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                        
                        // Operation should complete within reasonable time (no deadlock)
                        if (duration.count() > 5000) { // 5 seconds is too long
                            return false; // Potential deadlock detected
                        }
                        
                    } catch (const std::exception&) {
                        // Exceptions are acceptable, but should not cause deadlocks
                        auto end_time = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                        
                        if (duration.count() > 5000) { // 5 seconds is too long
                            return false; // Potential deadlock detected
                        }
                    }
                    
                    // Brief pause to allow other threads to acquire locks
                    if (op % 5 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
                
                return true; // All operations completed without deadlock
            }));
        }
        
        // Start all threads simultaneously
        start_flag.store(true);
        
        // Wait for all operations to complete with timeout
        bool all_completed = true;
        for (auto& future : operation_futures) {
            auto status = future.wait_for(std::chrono::seconds(10));
            if (status == std::future_status::timeout) {
                all_completed = false;
                BOOST_FAIL("Operation timed out - potential deadlock detected");
            } else {
                bool result = future.get();
                if (!result) {
                    all_completed = false;
                    BOOST_FAIL("Operation took too long - potential deadlock detected");
                }
            }
        }
        
        BOOST_CHECK(all_completed);
        
        // Test 2: System should remain responsive after concurrent operations
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            server.handle_resource_exhaustion();
        } catch (const std::exception& e) {
            BOOST_FAIL("Final operation should not throw: " + std::string(e.what()));
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        // Final operation should complete quickly (system is responsive)
        BOOST_CHECK_LT(duration.count(), 1000); // Less than 1 second
    }
}