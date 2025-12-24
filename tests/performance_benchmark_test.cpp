#define BOOST_TEST_MODULE PerformanceBenchmarkTest
#include <boost/test/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <chrono>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <random>

/**
 * Performance benchmark test for future conversion
 * 
 * This test validates that the performance characteristics of kythira::Future
 * are reasonable and meet expected performance requirements.
 */
BOOST_AUTO_TEST_CASE(performance_benchmark_future_operations, * boost::unit_test::timeout(120)) {
    // Performance benchmarks for future operations
    
    BOOST_TEST_MESSAGE("Starting performance benchmarks for future operations");
    
    // Benchmark 1: Future creation and immediate resolution
    {
        constexpr int num_operations = 100000;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            kythira::Future<int> future(i);
            auto result = future.get();
            BOOST_CHECK_EQUAL(result, i);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time);
        
        double ops_per_second = (num_operations * 1000000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("Future creation/resolution: " << num_operations 
                          << " operations in " << duration.count() << "μs"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // Should be able to do at least 10,000 operations per second
        BOOST_CHECK(ops_per_second > 10000);
    }
    
    // Benchmark 2: Future with different data types
    {
        constexpr int num_operations = 10000;
        
        // Test with strings
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            std::string test_string = "test_string_" + std::to_string(i);
            kythira::Future<std::string> future(std::move(test_string));
            auto result = future.get();
            BOOST_CHECK(result.find("test_string_") == 0);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time);
        
        double ops_per_second = (num_operations * 1000000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("String future operations: " << num_operations 
                          << " operations in " << duration.count() << "μs"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // Should be able to do at least 1,000 string operations per second
        BOOST_CHECK(ops_per_second > 1000);
    }
    
    // Benchmark 3: Future with large objects
    {
        constexpr int num_operations = 1000;
        constexpr size_t vector_size = 10000;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            std::vector<int> large_vector(vector_size, i);
            kythira::Future<std::vector<int>> future(std::move(large_vector));
            auto result = future.get();
            BOOST_CHECK_EQUAL(result.size(), vector_size);
            BOOST_CHECK_EQUAL(result[0], i);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        
        double ops_per_second = (num_operations * 1000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("Large object future operations: " << num_operations 
                          << " operations in " << duration.count() << "ms"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // Should be able to do at least 100 large object operations per second
        BOOST_CHECK(ops_per_second > 100);
    }
    
    // Benchmark 4: Concurrent future operations
    {
        constexpr int num_threads = 4;
        constexpr int operations_per_thread = 10000;
        
        std::atomic<int> total_operations{0};
        std::vector<std::thread> threads;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < operations_per_thread; ++i) {
                    int value = t * operations_per_thread + i;
                    kythira::Future<int> future(value);
                    auto result = future.get();
                    BOOST_CHECK_EQUAL(result, value);
                    total_operations.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        
        int expected_operations = num_threads * operations_per_thread;
        BOOST_CHECK_EQUAL(total_operations.load(), expected_operations);
        
        double ops_per_second = (expected_operations * 1000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("Concurrent future operations: " << expected_operations 
                          << " operations across " << num_threads << " threads in " 
                          << duration.count() << "ms"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // Should be able to do at least 5,000 concurrent operations per second
        BOOST_CHECK(ops_per_second > 5000);
    }
    
    // Benchmark 5: Exception handling performance
    {
        constexpr int num_operations = 10000;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_operations; ++i) {
            auto exception_future = kythira::Future<int>(
                folly::exception_wrapper(std::runtime_error("test error"))
            );
            
            bool caught_exception = false;
            try {
                exception_future.get();
            } catch (const std::runtime_error&) {
                caught_exception = true;
            }
            
            BOOST_CHECK(caught_exception);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time);
        
        double ops_per_second = (num_operations * 1000000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("Exception handling: " << num_operations 
                          << " operations in " << duration.count() << "μs"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // Exception handling should still be reasonably fast
        BOOST_CHECK(ops_per_second > 1000);
    }
    
    // Benchmark 6: Memory allocation patterns
    {
        constexpr int num_operations = 1000;
        
        // Test memory usage with various object sizes
        std::vector<size_t> object_sizes = {1, 10, 100, 1000, 10000};
        
        for (size_t size : object_sizes) {
            auto start_time = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < num_operations; ++i) {
                std::vector<int> test_vector(size, i);
                kythira::Future<std::vector<int>> future(std::move(test_vector));
                auto result = future.get();
                BOOST_CHECK_EQUAL(result.size(), size);
            }
            
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);
            
            double ops_per_second = (num_operations * 1000000.0) / duration.count();
            
            BOOST_TEST_MESSAGE("Memory allocation (size " << size << "): " 
                              << num_operations << " operations in " 
                              << duration.count() << "μs"
                              << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        }
    }
    
    // Benchmark 7: Future concept method performance
    {
        constexpr int num_operations = 50000;
        
        // Test isReady() performance
        auto start_time = std::chrono::high_resolution_clock::now();
        
        kythira::Future<int> test_future(42);
        for (int i = 0; i < num_operations; ++i) {
            bool ready = test_future.isReady();
            BOOST_CHECK(ready);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time);
        
        double ops_per_second = (num_operations * 1000000.0) / duration.count();
        
        BOOST_TEST_MESSAGE("isReady() calls: " << num_operations 
                          << " operations in " << duration.count() << "μs"
                          << " (" << static_cast<int>(ops_per_second) << " ops/sec)");
        
        // isReady() should be very fast
        BOOST_CHECK(ops_per_second > 100000);
    }
    
    BOOST_TEST_MESSAGE("Performance benchmarks completed successfully");
}