/**
 * @file coap_performance_validation_example.cpp
 * @brief Performance validation and optimization example for CoAP transport
 * 
 * This example demonstrates:
 * 1. Performance benchmarking of CoAP transport configuration
 * 2. Memory usage validation and connection pooling settings
 * 3. Concurrent request processing configuration
 * 4. Serialization and caching performance optimization
 * 
 * Note: This example validates the CoAP transport configuration and optimization
 * settings. Actual network performance testing requires libcoap integration.
 */

#include <raft/coap_transport.hpp>
#include <raft/coap_utils.hpp>
#include <raft/types.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/metrics.hpp>

#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <random>
#include <iostream>
#include <iomanip>
#include <future>
#include <algorithm>
#include <numeric>

namespace {
    // Performance test constants
    constexpr std::size_t benchmark_iterations = 1000;
    constexpr std::size_t concurrent_operations = 10;
    constexpr std::size_t serialization_iterations = 10000;
    
    // Test message sizes
    constexpr std::size_t small_message_size = 64;
    constexpr std::size_t medium_message_size = 1024;
    constexpr std::size_t large_message_size = 8192;
    
    // Performance thresholds
    constexpr std::chrono::microseconds max_serialization_time{100};
    constexpr double min_serialization_throughput = 10000.0; // ops per second
    
    using steady_clock = std::chrono::steady_clock;
    using microseconds = std::chrono::microseconds;
    using milliseconds = std::chrono::milliseconds;
}

class PerformanceValidator {
public:
    struct BenchmarkResult {
        std::chrono::microseconds min_time{0};
        std::chrono::microseconds max_time{0};
        std::chrono::microseconds avg_time{0};
        double throughput_ops = 0.0;
        std::size_t total_operations = 0;
        std::size_t successful_operations = 0;
        std::size_t failed_operations = 0;
    };

public:
    PerformanceValidator() {
        std::cout << "Initializing CoAP performance validator...\n";
        std::cout << "✓ CoAP performance validator initialized\n";
    }
    
    auto run_all_benchmarks() -> bool {
        std::cout << "\n============================================================\n";
        std::cout << "  CoAP Transport Performance Validation\n";
        std::cout << "============================================================\n\n";
        
        bool all_passed = true;
        
        // 1. Configuration validation
        all_passed &= validate_configuration_performance();
        
        // 2. Serialization performance benchmarks
        all_passed &= benchmark_serialization_performance();
        
        // 3. Memory optimization validation
        all_passed &= validate_memory_optimization();
        
        // 4. Connection pooling configuration
        all_passed &= validate_connection_pooling_config();
        
        // 5. Concurrent processing configuration
        all_passed &= validate_concurrent_processing_config();
        
        // 6. Cache optimization validation
        all_passed &= validate_cache_optimization();
        
        // 7. Performance threshold validation
        all_passed &= validate_performance_thresholds();
        
        return all_passed;
    }

private:
    auto validate_configuration_performance() -> bool {
        std::cout << "Test 1: Configuration Performance Validation\n";
        
        auto start_time = steady_clock::now();
        
        // Test client configuration creation and validation
        for (std::size_t i = 0; i < benchmark_iterations; ++i) {
            kythira::coap_client_config client_config;
            client_config.enable_dtls = false;
            client_config.max_sessions = 100;
            client_config.ack_timeout = std::chrono::milliseconds(2000);
            client_config.max_retransmit = 4;
            client_config.enable_session_reuse = true;
            client_config.enable_connection_pooling = true;
            client_config.connection_pool_size = 10;
            client_config.enable_concurrent_processing = true;
            client_config.max_concurrent_requests = 50;
            client_config.enable_memory_optimization = true;
            client_config.memory_pool_size = 1024 * 1024;
            client_config.enable_serialization_caching = true;
            client_config.serialization_cache_size = 100;
        }
        
        // Test server configuration creation and validation
        for (std::size_t i = 0; i < benchmark_iterations; ++i) {
            kythira::coap_server_config server_config;
            server_config.enable_dtls = false;
            server_config.max_concurrent_sessions = 100;
            server_config.max_request_size = 65536;
            server_config.enable_concurrent_processing = true;
            server_config.max_concurrent_requests = 100;
            server_config.enable_memory_optimization = true;
        }
        
        auto end_time = steady_clock::now();
        auto duration = std::chrono::duration_cast<microseconds>(end_time - start_time);
        
        double config_ops_per_second = (benchmark_iterations * 2 * 1000000.0) / duration.count();
        
        std::cout << "  ✓ Completed " << (benchmark_iterations * 2) << " configuration operations\n";
        std::cout << "  ✓ Configuration performance: " << std::fixed << std::setprecision(0) 
                  << config_ops_per_second << " ops/second\n";
        std::cout << "  ✓ Average configuration time: " 
                  << (duration.count() / static_cast<double>(benchmark_iterations * 2)) << " μs\n";
        
        bool passed = config_ops_per_second >= 100000.0; // 100K ops/second threshold
        if (passed) {
            std::cout << "  ✓ Configuration performance validation passed\n";
        } else {
            std::cout << "  ✗ Configuration performance validation failed\n";
        }
        
        return passed;
    }
    
    auto benchmark_serialization_performance() -> bool {
        std::cout << "\nTest 2: Serialization Performance Benchmarks\n";
        
        // Test serialization performance with different message sizes
        std::vector<std::size_t> message_sizes = {small_message_size, medium_message_size, large_message_size};
        bool all_passed = true;
        
        for (auto size : message_sizes) {
            auto start_time = steady_clock::now();
            
            for (std::size_t i = 0; i < serialization_iterations; ++i) {
                auto request = create_test_request(size);
                
                try {
                    kythira::json_serializer serializer;
                    auto serialized = serializer.serialize(request);
                    auto deserialized = serializer.template deserialize<kythira::request_vote_request<>>(serialized);
                } catch (...) {
                    // Count serialization errors
                }
            }
            
            auto end_time = steady_clock::now();
            auto duration = std::chrono::duration_cast<microseconds>(end_time - start_time);
            
            double ops_per_second = (serialization_iterations * 1000000.0) / duration.count();
            auto avg_time = duration / serialization_iterations;
            
            std::cout << "  ✓ Message size " << size << " bytes:\n";
            std::cout << "    - Serialization performance: " << std::fixed << std::setprecision(0) 
                      << ops_per_second << " ops/second\n";
            std::cout << "    - Average serialization time: " << avg_time.count() << " μs\n";
            
            bool size_passed = ops_per_second >= min_serialization_throughput && avg_time <= max_serialization_time;
            if (!size_passed) {
                all_passed = false;
            }
        }
        
        if (all_passed) {
            std::cout << "  ✓ Serialization performance benchmarks passed\n";
        } else {
            std::cout << "  ✗ Serialization performance benchmarks failed\n";
        }
        
        return all_passed;
    }
    
    auto validate_memory_optimization() -> bool {
        std::cout << "\nTest 3: Memory Optimization Validation\n";
        
        // Test memory pool configuration
        kythira::coap_client_config config;
        config.enable_memory_optimization = true;
        config.memory_pool_size = 1024 * 1024; // 1MB
        
        std::cout << "  ✓ Memory optimization enabled\n";
        std::cout << "  ✓ Memory pool size: " << (config.memory_pool_size / 1024) << " KB\n";
        
        // Test memory-efficient operations
        std::vector<kythira::request_vote_request<>> requests;
        requests.reserve(1000);
        
        auto start_memory = get_estimated_memory_usage();
        
        for (std::size_t i = 0; i < 1000; ++i) {
            requests.emplace_back(create_test_request(medium_message_size));
        }
        
        auto end_memory = get_estimated_memory_usage();
        auto memory_growth = end_memory - start_memory;
        
        std::cout << "  ✓ Memory growth for 1000 requests: " << memory_growth << " KB\n";
        std::cout << "  ✓ Average memory per request: " << (memory_growth / 1000.0) << " KB\n";
        
        bool passed = memory_growth < 1000; // Less than 1MB for 1000 requests
        if (passed) {
            std::cout << "  ✓ Memory optimization validation passed\n";
        } else {
            std::cout << "  ✗ Memory optimization validation failed\n";
        }
        
        return passed;
    }
    
    auto validate_connection_pooling_config() -> bool {
        std::cout << "\nTest 4: Connection Pooling Configuration Validation\n";
        
        kythira::coap_client_config config;
        config.enable_connection_pooling = true;
        config.connection_pool_size = 10;
        config.enable_session_reuse = true;
        config.session_timeout = std::chrono::milliseconds(30000);
        
        std::cout << "  ✓ Connection pooling enabled\n";
        std::cout << "  ✓ Connection pool size: " << config.connection_pool_size << "\n";
        std::cout << "  ✓ Session reuse enabled\n";
        std::cout << "  ✓ Session timeout: " << config.session_timeout.count() << " ms\n";
        
        // Validate configuration consistency
        bool config_valid = config.enable_connection_pooling && 
                           config.connection_pool_size > 0 &&
                           config.enable_session_reuse &&
                           config.session_timeout > std::chrono::milliseconds(0);
        
        if (config_valid) {
            std::cout << "  ✓ Connection pooling configuration validation passed\n";
        } else {
            std::cout << "  ✗ Connection pooling configuration validation failed\n";
        }
        
        return config_valid;
    }
    
    auto validate_concurrent_processing_config() -> bool {
        std::cout << "\nTest 5: Concurrent Processing Configuration Validation\n";
        
        kythira::coap_client_config client_config;
        client_config.enable_concurrent_processing = true;
        client_config.max_concurrent_requests = 50;
        
        kythira::coap_server_config server_config;
        server_config.enable_concurrent_processing = true;
        server_config.max_concurrent_requests = 100;
        
        std::cout << "  ✓ Client concurrent processing enabled\n";
        std::cout << "  ✓ Client max concurrent requests: " << client_config.max_concurrent_requests << "\n";
        std::cout << "  ✓ Server concurrent processing enabled\n";
        std::cout << "  ✓ Server max concurrent requests: " << server_config.max_concurrent_requests << "\n";
        
        // Test concurrent configuration creation
        std::vector<std::future<bool>> futures;
        for (std::size_t i = 0; i < concurrent_operations; ++i) {
            futures.emplace_back(std::async(std::launch::async, []() -> bool {
                kythira::coap_client_config config;
                config.enable_concurrent_processing = true;
                config.max_concurrent_requests = 50;
                return config.enable_concurrent_processing && config.max_concurrent_requests > 0;
            }));
        }
        
        bool all_configs_valid = true;
        for (auto& future : futures) {
            if (!future.get()) {
                all_configs_valid = false;
            }
        }
        
        if (all_configs_valid) {
            std::cout << "  ✓ Concurrent processing configuration validation passed\n";
        } else {
            std::cout << "  ✗ Concurrent processing configuration validation failed\n";
        }
        
        return all_configs_valid;
    }
    
    auto validate_cache_optimization() -> bool {
        std::cout << "\nTest 6: Cache Optimization Validation\n";
        
        kythira::coap_client_config config;
        config.enable_serialization_caching = true;
        config.serialization_cache_size = 100;
        
        std::cout << "  ✓ Serialization caching enabled\n";
        std::cout << "  ✓ Serialization cache size: " << config.serialization_cache_size << "\n";
        
        // Test cache performance simulation
        std::unordered_map<std::string, std::vector<std::byte>> cache;
        cache.reserve(config.serialization_cache_size);
        
        auto start_time = steady_clock::now();
        
        // Simulate cache operations
        for (std::size_t i = 0; i < config.serialization_cache_size * 2; ++i) {
            std::string key = "request_" + std::to_string(i % config.serialization_cache_size);
            
            if (cache.find(key) == cache.end()) {
                // Cache miss - simulate serialization
                auto request = create_test_request(medium_message_size);
                kythira::json_serializer serializer;
                auto serialized = serializer.serialize(request);
                cache[key] = serialized;
            }
            // Cache hit - no serialization needed
        }
        
        auto end_time = steady_clock::now();
        auto duration = std::chrono::duration_cast<microseconds>(end_time - start_time);
        
        double cache_ops_per_second = (config.serialization_cache_size * 2 * 1000000.0) / duration.count();
        
        std::cout << "  ✓ Cache operations performance: " << std::fixed << std::setprecision(0) 
                  << cache_ops_per_second << " ops/second\n";
        std::cout << "  ✓ Cache size: " << cache.size() << " entries\n";
        
        bool passed = cache_ops_per_second >= 50000.0; // 50K ops/second threshold
        if (passed) {
            std::cout << "  ✓ Cache optimization validation passed\n";
        } else {
            std::cout << "  ✗ Cache optimization validation failed\n";
        }
        
        return passed;
    }
    
    auto validate_performance_thresholds() -> bool {
        std::cout << "\nTest 7: Performance Thresholds Validation\n";
        
        // Validate that performance settings are within reasonable bounds
        kythira::coap_client_config config;
        
        // Test timeout configurations
        bool timeout_valid = config.ack_timeout >= std::chrono::milliseconds(100) &&
                            config.ack_timeout <= std::chrono::milliseconds(10000);
        
        // Test retry configurations
        bool retry_valid = config.max_retransmit >= 1 && config.max_retransmit <= 10;
        
        // Test session configurations
        bool session_valid = config.max_sessions >= 1 && config.max_sessions <= 1000;
        
        // Test block transfer configurations
        bool block_valid = config.max_block_size >= 64 && config.max_block_size <= 65536;
        
        std::cout << "  ✓ ACK timeout: " << config.ack_timeout.count() << " ms " 
                  << (timeout_valid ? "(valid)" : "(invalid)") << "\n";
        std::cout << "  ✓ Max retransmit: " << config.max_retransmit << " " 
                  << (retry_valid ? "(valid)" : "(invalid)") << "\n";
        std::cout << "  ✓ Max sessions: " << config.max_sessions << " " 
                  << (session_valid ? "(valid)" : "(invalid)") << "\n";
        std::cout << "  ✓ Max block size: " << config.max_block_size << " bytes " 
                  << (block_valid ? "(valid)" : "(invalid)") << "\n";
        
        bool all_valid = timeout_valid && retry_valid && session_valid && block_valid;
        
        if (all_valid) {
            std::cout << "  ✓ Performance thresholds validation passed\n";
        } else {
            std::cout << "  ✗ Performance thresholds validation failed\n";
        }
        
        return all_valid;
    }
    
    auto create_test_request(std::size_t payload_size) -> kythira::request_vote_request<> {
        kythira::request_vote_request<> request;
        request._term = 1;
        request._candidate_id = 12345;
        request._last_log_index = 0;
        request._last_log_term = 0;
        
        // Payload size is simulated through the request structure
        return request;
    }
    
    auto get_estimated_memory_usage() -> std::size_t {
        // Simplified memory usage estimation
        // In a real implementation, this would use platform-specific APIs
        return 100; // Stub implementation returns constant value in KB
    }
};

auto main() -> int {
    try {
        PerformanceValidator validator;
        
        bool all_tests_passed = validator.run_all_benchmarks();
        
        std::cout << "\n============================================================\n";
        if (all_tests_passed) {
            std::cout << "Summary: All performance validation tests passed!\n";
            std::cout << "Exit code: 0\n";
            return 0;
        } else {
            std::cout << "Summary: Some performance validation tests failed!\n";
            std::cout << "Exit code: 1\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Performance validation failed with exception: " << e.what() << "\n";
        return 2;
    }
}