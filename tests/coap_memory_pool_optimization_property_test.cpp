#define BOOST_TEST_MODULE CoAPMemoryPoolOptimizationPropertyTest
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/noop_metrics.hpp>

#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <vector>
#include <memory>

namespace {
    constexpr std::size_t test_pool_size = 1024 * 1024; // 1MB
    constexpr std::size_t test_small_pool_size = 4096; // 4KB
    constexpr std::size_t test_large_pool_size = 16 * 1024 * 1024; // 16MB
    constexpr std::size_t test_max_allocations = 1000;
    constexpr std::size_t test_min_allocations = 10;
    constexpr std::size_t test_max_allocation_size = 8192; // 8KB
    constexpr std::size_t test_min_allocation_size = 64; // 64 bytes
    constexpr const char* test_multicast_address = "224.0.1.200";
    constexpr std::uint16_t test_multicast_port = 5686;
}

using namespace kythira;

// Test types for CoAP transport
struct test_types {
    using future_type = folly::Future<std::vector<std::byte>>;
    using serializer_type = json_serializer;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

// Property test helper functions
namespace property_helpers {
    
    auto generate_random_pool_size() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_small_pool_size, test_large_pool_size);
        return dis(gen);
    }
    
    auto generate_random_allocation_size() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_allocation_size, test_max_allocation_size);
        return dis(gen);
    }
    
    auto generate_random_allocation_count() -> std::size_t {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<std::size_t> dis(test_min_allocations, test_max_allocations);
        return dis(gen);
    }
    
    auto create_test_client_with_memory_optimization(std::size_t pool_size) -> std::unique_ptr<coap_client<test_types>> {
        std::unordered_map<std::uint64_t, std::string> endpoints;
        coap_client_config config;
        config.enable_memory_optimization = true;
        config.memory_pool_size = pool_size;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        return std::make_unique<coap_client<test_types>>(
            std::move(endpoints),
            std::move(config),
            noop_metrics{}
        );
    }
    
    auto create_test_server_with_memory_optimization(std::size_t pool_size) -> std::unique_ptr<coap_server<test_types>> {
        coap_server_config config;
        config.enable_memory_optimization = true;
        config.memory_pool_size = pool_size;
        config.enable_multicast = true;
        config.multicast_address = test_multicast_address;
        config.multicast_port = test_multicast_port;
        
        return std::make_unique<coap_server<test_types>>(
            "0.0.0.0",
            test_multicast_port,
            std::move(config),
            noop_metrics{}
        );
    }
    
    auto create_test_memory_pool(std::size_t size) -> std::unique_ptr<memory_pool> {
        return std::make_unique<memory_pool>(size);
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool allocation and management
 * 
 * Property: For any memory pool with sufficient space, allocations should succeed
 * and return valid, aligned memory addresses.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_basic_allocation, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 100; ++iteration) {
        try {
            // Generate random test parameters
            auto pool_size = generate_random_pool_size();
            auto allocation_size = std::min(generate_random_allocation_size(), pool_size / 4);
            
            // Create memory pool
            auto pool = create_test_memory_pool(pool_size);
            
            // Property: Pool should be initially empty
            auto [current_usage, peak_usage, allocation_count, reset_count] = pool->get_usage_stats();
            BOOST_CHECK_EQUAL(current_usage, 0);
            BOOST_CHECK_EQUAL(peak_usage, 0);
            BOOST_CHECK_EQUAL(allocation_count, 0);
            BOOST_CHECK_EQUAL(reset_count, 0);
            
            // Property: Pool should have full available space initially
            BOOST_CHECK_EQUAL(pool->available_space(), pool_size);
            BOOST_CHECK_EQUAL(pool->get_utilization_percentage(), 0.0);
            BOOST_CHECK(!pool->is_exhausted());
            
            // Property: Allocation should succeed for reasonable sizes
            std::byte* ptr = pool->allocate(allocation_size);
            BOOST_REQUIRE(ptr != nullptr);
            
            // Property: Returned pointer should be properly aligned
            auto ptr_value = reinterpret_cast<std::uintptr_t>(ptr);
            BOOST_CHECK_EQUAL(ptr_value % 8, 0); // 8-byte alignment
            
            // Property: Pool usage should be updated correctly
            auto [usage_after, peak_after, alloc_count_after, reset_count_after] = pool->get_usage_stats();
            BOOST_CHECK_GT(usage_after, 0);
            BOOST_CHECK_GE(usage_after, allocation_size);
            BOOST_CHECK_EQUAL(peak_after, usage_after);
            BOOST_CHECK_EQUAL(alloc_count_after, 1);
            BOOST_CHECK_EQUAL(reset_count_after, 0);
            
            // Property: Available space should decrease
            BOOST_CHECK_LT(pool->available_space(), pool_size);
            BOOST_CHECK_GT(pool->get_utilization_percentage(), 0.0);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool exhaustion handling
 * 
 * Property: For any memory pool, when the pool is exhausted, allocations should
 * fail gracefully and return nullptr.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_exhaustion_handling, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate random test parameters
            auto pool_size = std::max(std::size_t{1024}, generate_random_pool_size() / 100); // Smaller pool for exhaustion test
            auto allocation_size = pool_size / 4; // Large allocation relative to pool
            
            // Create memory pool
            auto pool = create_test_memory_pool(pool_size);
            
            // Fill the pool with allocations
            std::vector<std::byte*> allocations;
            std::byte* ptr = nullptr;
            
            do {
                ptr = pool->allocate(allocation_size);
                if (ptr) {
                    allocations.push_back(ptr);
                }
            } while (ptr != nullptr);
            
            // Property: Pool should be exhausted or nearly exhausted
            BOOST_CHECK(pool->is_exhausted() || pool->get_utilization_percentage() > 75.0);
            
            // Property: Further allocations should fail
            std::byte* failed_ptr = pool->allocate(allocation_size);
            BOOST_CHECK(failed_ptr == nullptr);
            
            // Property: Small allocations should also fail when pool is exhausted
            std::byte* small_ptr = pool->allocate(64);
            if (pool->is_exhausted()) {
                BOOST_CHECK(small_ptr == nullptr);
            }
            
            // Property: Reset should restore pool to initial state
            pool->reset();
            
            auto [usage_after_reset, peak_after_reset, alloc_count_after_reset, reset_count_after_reset] = pool->get_usage_stats();
            BOOST_CHECK_EQUAL(usage_after_reset, 0);
            BOOST_CHECK_GT(peak_after_reset, 0); // Peak should be preserved
            BOOST_CHECK_GT(alloc_count_after_reset, 0); // Allocation count should be preserved
            BOOST_CHECK_EQUAL(reset_count_after_reset, 1);
            
            // Property: After reset, allocations should succeed again
            std::byte* ptr_after_reset = pool->allocate(allocation_size);
            BOOST_CHECK(ptr_after_reset != nullptr);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool concurrent access safety
 * 
 * Property: For any memory pool accessed concurrently, all operations should
 * be thread-safe and maintain consistency.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_concurrent_access, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 20; ++iteration) {
        try {
            // Generate random test parameters
            auto pool_size = generate_random_pool_size();
            auto thread_count = std::min(std::size_t{8}, std::max(std::size_t{2}, generate_random_allocation_count() / 100));
            auto allocations_per_thread = std::max(std::size_t{10}, generate_random_allocation_count() / thread_count);
            
            // Create memory pool
            auto pool = create_test_memory_pool(pool_size);
            
            // Launch concurrent allocation threads
            std::vector<std::thread> threads;
            std::vector<std::vector<std::byte*>> thread_allocations(thread_count);
            std::atomic<std::size_t> successful_allocations{0};
            std::atomic<std::size_t> failed_allocations{0};
            
            for (std::size_t t = 0; t < thread_count; ++t) {
                threads.emplace_back([&, t]() {
                    for (std::size_t i = 0; i < allocations_per_thread; ++i) {
                        auto allocation_size = generate_random_allocation_size();
                        std::byte* ptr = pool->allocate(allocation_size);
                        
                        if (ptr) {
                            thread_allocations[t].push_back(ptr);
                            successful_allocations.fetch_add(1);
                        } else {
                            failed_allocations.fetch_add(1);
                        }
                        
                        // Small delay to increase contention
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                });
            }
            
            // Wait for all threads to complete
            for (auto& thread : threads) {
                thread.join();
            }
            
            // Property: Total allocations should equal successful + failed
            auto total_attempts = thread_count * allocations_per_thread;
            BOOST_CHECK_EQUAL(successful_allocations.load() + failed_allocations.load(), total_attempts);
            
            // Property: All successful allocations should have valid pointers
            std::size_t total_successful = 0;
            for (const auto& thread_allocs : thread_allocations) {
                for (std::byte* ptr : thread_allocs) {
                    BOOST_CHECK(ptr != nullptr);
                    // Check alignment
                    auto ptr_value = reinterpret_cast<std::uintptr_t>(ptr);
                    BOOST_CHECK_EQUAL(ptr_value % 8, 0);
                }
                total_successful += thread_allocs.size();
            }
            
            BOOST_CHECK_EQUAL(total_successful, successful_allocations.load());
            
            // Property: Pool statistics should be consistent
            auto [final_usage, final_peak, final_alloc_count, final_reset_count] = pool->get_usage_stats();
            BOOST_CHECK_EQUAL(final_alloc_count, successful_allocations.load());
            BOOST_CHECK_GE(final_peak, final_usage);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool integration with CoAP client
 * 
 * Property: For any CoAP client with memory optimization enabled, the client
 * should use the memory pool for allocations and maintain pool statistics.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_coap_client_integration, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 30; ++iteration) {
        try {
            // Generate random test parameters
            auto pool_size = generate_random_pool_size();
            auto allocation_count = std::min(generate_random_allocation_count(), std::size_t{100});
            
            // Create CoAP client with memory optimization
            auto client = create_test_client_with_memory_optimization(pool_size);
            
            // Property: Client should support memory pool operations
            std::vector<std::byte*> allocations;
            std::size_t successful_allocations = 0;
            
            for (std::size_t i = 0; i < allocation_count; ++i) {
                auto allocation_size = generate_random_allocation_size();
                std::byte* ptr = client->allocate_from_pool(allocation_size);
                
                if (ptr) {
                    allocations.push_back(ptr);
                    successful_allocations++;
                    
                    // Property: Allocated memory should be properly aligned
                    auto ptr_value = reinterpret_cast<std::uintptr_t>(ptr);
                    BOOST_CHECK_EQUAL(ptr_value % 8, 0);
                } else {
                    // Allocation failed - this is acceptable when pool is exhausted
                    break;
                }
            }
            
            // Property: At least some allocations should succeed for reasonable pool sizes
            if (pool_size >= test_pool_size) {
                BOOST_CHECK_GT(successful_allocations, 0);
            }
            
            // Property: All allocated pointers should be unique
            std::sort(allocations.begin(), allocations.end());
            auto unique_end = std::unique(allocations.begin(), allocations.end());
            BOOST_CHECK_EQUAL(std::distance(allocations.begin(), unique_end), allocations.size());
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool performance characteristics
 * 
 * Property: For any memory pool, allocation performance should be consistent
 * and significantly faster than standard malloc/free operations.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_performance_characteristics, * boost::unit_test::timeout(120)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 10; ++iteration) {
        try {
            // Generate test parameters for performance testing
            auto pool_size = test_large_pool_size; // Use large pool for performance test
            auto allocation_count = std::min(generate_random_allocation_count(), std::size_t{1000});
            auto allocation_size = std::min(generate_random_allocation_size(), std::size_t{1024});
            
            // Create memory pool
            auto pool = create_test_memory_pool(pool_size);
            
            // Measure pool allocation performance
            auto pool_start = std::chrono::high_resolution_clock::now();
            std::vector<std::byte*> pool_allocations;
            
            for (std::size_t i = 0; i < allocation_count; ++i) {
                std::byte* ptr = pool->allocate(allocation_size);
                if (ptr) {
                    pool_allocations.push_back(ptr);
                } else {
                    break; // Pool exhausted
                }
            }
            
            auto pool_end = std::chrono::high_resolution_clock::now();
            auto pool_duration = std::chrono::duration_cast<std::chrono::microseconds>(pool_end - pool_start);
            
            // Measure standard allocation performance
            auto malloc_start = std::chrono::high_resolution_clock::now();
            std::vector<void*> malloc_allocations;
            
            for (std::size_t i = 0; i < pool_allocations.size(); ++i) {
                void* ptr = std::malloc(allocation_size);
                if (ptr) {
                    malloc_allocations.push_back(ptr);
                }
            }
            
            auto malloc_end = std::chrono::high_resolution_clock::now();
            auto malloc_duration = std::chrono::duration_cast<std::chrono::microseconds>(malloc_end - malloc_start);
            
            // Clean up malloc allocations
            for (void* ptr : malloc_allocations) {
                std::free(ptr);
            }
            
            // Property: Pool allocations should be successful
            BOOST_CHECK_GT(pool_allocations.size(), 0);
            BOOST_CHECK_EQUAL(pool_allocations.size(), malloc_allocations.size());
            
            // Property: Pool allocation should be faster than malloc (in most cases)
            // Note: This is not guaranteed in all environments, but generally expected
            if (pool_allocations.size() > 10) {
                auto pool_avg_time = pool_duration.count() / pool_allocations.size();
                auto malloc_avg_time = malloc_duration.count() / malloc_allocations.size();
                
                // Pool should be at least competitive with malloc
                // Allow some tolerance for measurement variance
                BOOST_CHECK_LE(pool_avg_time, malloc_avg_time * 2);
            }
            
            // Property: Pool utilization should be reasonable
            auto utilization = pool->get_utilization_percentage();
            BOOST_CHECK_GT(utilization, 0.0);
            BOOST_CHECK_LE(utilization, 100.0);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}

/**
 * Feature: coap-transport, Property 30: Memory pool statistics accuracy
 * 
 * Property: For any sequence of memory pool operations, the statistics should
 * accurately reflect the pool's usage and history.
 * 
 * Validates: Requirements 7.1
 */
BOOST_AUTO_TEST_CASE(property_memory_pool_statistics_accuracy, * boost::unit_test::timeout(60)) {
    using namespace property_helpers;
    
    // Property-based test with multiple iterations
    for (int iteration = 0; iteration < 50; ++iteration) {
        try {
            // Generate random test parameters
            auto pool_size = generate_random_pool_size();
            auto operation_count = std::min(generate_random_allocation_count(), std::size_t{200});
            
            // Create memory pool
            auto pool = create_test_memory_pool(pool_size);
            
            std::size_t expected_allocations = 0;
            std::size_t expected_resets = 0;
            std::size_t max_usage_seen = 0;
            
            for (std::size_t i = 0; i < operation_count; ++i) {
                if (i % 20 == 19) {
                    // Occasionally reset the pool
                    pool->reset();
                    expected_resets++;
                } else {
                    // Perform allocation
                    auto allocation_size = generate_random_allocation_size();
                    std::byte* ptr = pool->allocate(allocation_size);
                    
                    if (ptr) {
                        expected_allocations++;
                        auto [current_usage, peak_usage, alloc_count, reset_count] = pool->get_usage_stats();
                        if (current_usage > max_usage_seen) {
                            max_usage_seen = current_usage;
                        }
                    }
                }
            }
            
            // Property: Final statistics should match expected values
            auto [final_usage, final_peak, final_alloc_count, final_reset_count] = pool->get_usage_stats();
            
            BOOST_CHECK_EQUAL(final_alloc_count, expected_allocations);
            BOOST_CHECK_EQUAL(final_reset_count, expected_resets);
            BOOST_CHECK_GE(final_peak, max_usage_seen);
            
            // Property: Utilization percentage should be consistent
            auto utilization = pool->get_utilization_percentage();
            auto expected_utilization = (static_cast<double>(final_usage) / pool_size) * 100.0;
            BOOST_CHECK_CLOSE(utilization, expected_utilization, 0.1); // Within 0.1%
            
            // Property: Available space should be consistent
            auto available = pool->available_space();
            BOOST_CHECK_EQUAL(available, pool_size - final_usage);
            
            // Property: Exhaustion status should be consistent
            auto is_exhausted = pool->is_exhausted();
            BOOST_CHECK_EQUAL(is_exhausted, final_usage >= pool_size);
            
        } catch (const std::exception& e) {
            BOOST_FAIL("Property test iteration " + std::to_string(iteration) + " failed: " + e.what());
        }
    }
}