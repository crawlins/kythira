#define BOOST_TEST_MODULE memory_pool_size_monitoring_property_test
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>

using namespace kythira;

namespace {
    // Test constants
    constexpr std::size_t min_pool_size = 64 * 1024;      // 64KB
    constexpr std::size_t max_pool_size = 1024 * 1024;    // 1MB
    constexpr std::size_t min_block_size = 1024;          // 1KB
    constexpr std::size_t max_block_size = 8192;          // 8KB
    constexpr std::size_t num_property_iterations = 100;
    constexpr auto test_timeout_seconds = 120;
    
    // Random number generator
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Generate random pool size
    auto random_pool_size() -> std::size_t {
        std::uniform_int_distribution<std::size_t> dist(min_pool_size, max_pool_size);
        return dist(gen);
    }
    
    // Generate random block size
    auto random_block_size() -> std::size_t {
        std::uniform_int_distribution<std::size_t> dist(min_block_size, max_block_size);
        return dist(gen);
    }
    
    // Generate random allocation count
    auto random_allocation_count(std::size_t max_blocks) -> std::size_t {
        std::uniform_int_distribution<std::size_t> dist(1, std::min(max_blocks, std::size_t{100}));
        return dist(gen);
    }
    
    // Generate random allocation size
    auto random_allocation_size(std::size_t block_size) -> std::size_t {
        std::uniform_int_distribution<std::size_t> dist(1, block_size);
        return dist(gen);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any sequence of allocations and deallocations, the pool should
 * accurately track total_size, allocated_size, and free_size in real-time, with
 * the invariant: allocated_size + free_size = total_size
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_size_tracking_invariant, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        // Generate random pool configuration
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        // Ensure pool size is multiple of block size
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        // Generate random allocation pattern
        std::size_t alloc_count = random_allocation_count(max_blocks);
        std::vector<void*> allocations;
        
        // Perform random allocations and verify invariant at each step
        for (std::size_t i = 0; i < alloc_count; ++i) {
            std::size_t alloc_size = random_allocation_size(block_size);
            void* ptr = pool.allocate(alloc_size);
            
            if (ptr) {
                allocations.push_back(ptr);
                
                // Property: Invariant must hold after each allocation
                auto metrics = pool.get_metrics();
                BOOST_CHECK_EQUAL(metrics.total_size, pool_size);
                BOOST_CHECK_EQUAL(metrics.allocated_size + metrics.free_size, metrics.total_size);
                BOOST_CHECK_LE(metrics.allocated_size, pool_size);
                BOOST_CHECK_LE(metrics.free_size, pool_size);
            }
        }
        
        // Perform random deallocations and verify invariant
        std::uniform_int_distribution<std::size_t> dist(0, allocations.size() - 1);
        std::size_t dealloc_count = allocations.size() / 2;
        
        for (std::size_t i = 0; i < dealloc_count && !allocations.empty(); ++i) {
            std::size_t idx = dist(gen) % allocations.size();
            if (allocations[idx] != nullptr) {
                pool.deallocate(allocations[idx]);
                allocations[idx] = nullptr;
                
                // Property: Invariant must hold after each deallocation
                auto metrics = pool.get_metrics();
                BOOST_CHECK_EQUAL(metrics.total_size, pool_size);
                BOOST_CHECK_EQUAL(metrics.allocated_size + metrics.free_size, metrics.total_size);
            }
        }
        
        // Clean up remaining allocations
        for (void* ptr : allocations) {
            if (ptr) pool.deallocate(ptr);
        }
        
        // Property: After all deallocations, all memory should be free
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocated_size, 0);
        BOOST_CHECK_EQUAL(final_metrics.free_size, pool_size);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any allocation pattern, allocation_count and deallocation_count
 * should be monotonically increasing and accurately reflect the number of
 * operations performed.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_allocation_count_monotonic, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        std::vector<void*> allocations;
        std::size_t expected_alloc_count = 0;
        std::size_t expected_dealloc_count = 0;
        
        // Perform allocations
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
                expected_alloc_count++;
                
                // Property: allocation_count should increase monotonically
                auto metrics = pool.get_metrics();
                BOOST_CHECK_EQUAL(metrics.allocation_count, expected_alloc_count);
                BOOST_CHECK_EQUAL(metrics.deallocation_count, expected_dealloc_count);
            }
        }
        
        // Perform deallocations
        for (void* ptr : allocations) {
            if (ptr) {
                pool.deallocate(ptr);
                expected_dealloc_count++;
                
                // Property: deallocation_count should increase monotonically
                auto metrics = pool.get_metrics();
                BOOST_CHECK_EQUAL(metrics.allocation_count, expected_alloc_count);
                BOOST_CHECK_EQUAL(metrics.deallocation_count, expected_dealloc_count);
            }
        }
        
        // Property: Final counts should match expected values
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocation_count, expected_alloc_count);
        BOOST_CHECK_EQUAL(final_metrics.deallocation_count, expected_dealloc_count);
        
        // Property: allocation_count >= deallocation_count always
        BOOST_CHECK_GE(final_metrics.allocation_count, final_metrics.deallocation_count);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any allocation pattern, peak_usage should track the maximum
 * allocated_size ever reached and should never decrease until reset.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_peak_usage_tracking, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        std::vector<void*> allocations;
        std::size_t observed_peak = 0;
        
        // Gradually increase allocations
        std::size_t alloc_count = std::min(max_blocks, std::size_t{50});
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
                
                auto metrics = pool.get_metrics();
                
                // Property: peak_usage should be >= current allocated_size
                BOOST_CHECK_GE(metrics.peak_usage, metrics.allocated_size);
                
                // Property: peak_usage should never decrease
                BOOST_CHECK_GE(metrics.peak_usage, observed_peak);
                
                observed_peak = metrics.peak_usage;
            }
        }
        
        // Deallocate some blocks
        std::size_t dealloc_count = allocations.size() / 2;
        for (std::size_t i = 0; i < dealloc_count; ++i) {
            if (allocations[i]) {
                pool.deallocate(allocations[i]);
                allocations[i] = nullptr;
                
                auto metrics = pool.get_metrics();
                
                // Property: peak_usage should remain at maximum even after deallocations
                BOOST_CHECK_EQUAL(metrics.peak_usage, observed_peak);
                BOOST_CHECK_GE(metrics.peak_usage, metrics.allocated_size);
            }
        }
        
        // Allocate more to potentially exceed previous peak
        for (std::size_t i = 0; i < dealloc_count && allocations.size() < max_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
                
                auto metrics = pool.get_metrics();
                
                // Property: peak_usage should update if we exceed previous peak
                if (metrics.allocated_size > observed_peak) {
                    BOOST_CHECK_EQUAL(metrics.peak_usage, metrics.allocated_size);
                    observed_peak = metrics.peak_usage;
                } else {
                    BOOST_CHECK_EQUAL(metrics.peak_usage, observed_peak);
                }
            }
        }
        
        // Clean up
        for (void* ptr : allocations) {
            if (ptr) pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any allocation pattern, fragmentation_ratio should accurately
 * reflect the percentage of free blocks and should be in the range [0, 100].
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_fragmentation_ratio_calculation, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t total_blocks = pool_size / block_size;
        
        // Property: Initially, fragmentation should be 100% (all free)
        auto initial_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(initial_metrics.fragmentation_ratio, 100);
        
        // Allocate all blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < total_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Property: When fully allocated, fragmentation should be 0%
        auto full_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(full_metrics.fragmentation_ratio, 0);
        
        // Deallocate half the blocks
        std::size_t dealloc_count = allocations.size() / 2;
        for (std::size_t i = 0; i < dealloc_count; ++i) {
            pool.deallocate(allocations[i]);
            allocations[i] = nullptr;
        }
        
        // Property: Fragmentation should be approximately 50%
        auto half_metrics = pool.get_metrics();
        BOOST_CHECK_GE(half_metrics.fragmentation_ratio, 0);
        BOOST_CHECK_LE(half_metrics.fragmentation_ratio, 100);
        
        // Calculate expected fragmentation
        std::size_t used_blocks = allocations.size() - dealloc_count;
        std::size_t expected_frag = static_cast<std::size_t>(
            (1.0 - static_cast<double>(used_blocks) / total_blocks) * 100);
        
        // Allow small rounding differences (within 5%)
        std::size_t tolerance = 5;
        BOOST_CHECK_GE(half_metrics.fragmentation_ratio, expected_frag > tolerance ? expected_frag - tolerance : 0);
        BOOST_CHECK_LE(half_metrics.fragmentation_ratio, expected_frag + tolerance);
        
        // Clean up remaining allocations
        for (void* ptr : allocations) {
            if (ptr) pool.deallocate(ptr);
        }
        
        // Property: After all deallocations, fragmentation should be 100%
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.fragmentation_ratio, 100);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any concurrent allocation and deallocation operations, metrics
 * should remain consistent and accurate without data races.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_concurrent_metrics_consistency, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 512 * 1024; // Fixed size for faster test
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size);
        
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> total_allocations{0};
        std::atomic<std::size_t> total_deallocations{0};
        std::vector<std::thread> threads;
        
        // Multiple threads performing allocations and deallocations
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                std::vector<void*> local_ptrs;
                while (!stop) {
                    // Allocate
                    void* ptr = pool.allocate(block_size / 2);
                    if (ptr) {
                        local_ptrs.push_back(ptr);
                        total_allocations++;
                    }
                    
                    // Deallocate some
                    if (local_ptrs.size() > 10) {
                        pool.deallocate(local_ptrs.front());
                        local_ptrs.erase(local_ptrs.begin());
                        total_deallocations++;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                
                // Clean up
                for (void* p : local_ptrs) {
                    pool.deallocate(p);
                    total_deallocations++;
                }
            });
        }
        
        // Thread that continuously checks metrics consistency
        std::atomic<bool> consistency_violation{false};
        threads.emplace_back([&]() {
            while (!stop) {
                auto metrics = pool.get_metrics();
                
                // Property: Invariants must always hold
                if (metrics.allocated_size + metrics.free_size != metrics.total_size) {
                    consistency_violation = true;
                }
                if (metrics.allocated_size > metrics.total_size) {
                    consistency_violation = true;
                }
                if (metrics.free_size > metrics.total_size) {
                    consistency_violation = true;
                }
                if (metrics.peak_usage < metrics.allocated_size) {
                    consistency_violation = true;
                }
                if (metrics.allocation_count < metrics.deallocation_count) {
                    consistency_violation = true;
                }
                if (metrics.fragmentation_ratio > 100) {
                    consistency_violation = true;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        
        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Property: No consistency violations should occur
        BOOST_CHECK(!consistency_violation);
        
        // Property: Final metrics should be consistent
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocated_size + final_metrics.free_size, 
                         final_metrics.total_size);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any pool configuration, metrics should provide accurate
 * capacity planning information through utilization percentage and peak usage.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_capacity_planning_metrics, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        // Simulate varying load pattern
        std::vector<void*> allocations;
        std::size_t target_blocks = max_blocks / 2;
        
        // Ramp up allocations
        for (std::size_t i = 0; i < target_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
                
                auto metrics = pool.get_metrics();
                double utilization = pool.get_utilization_percentage();
                
                // Property: Utilization should match allocated_size / total_size
                double expected_util = (static_cast<double>(metrics.allocated_size) / pool_size) * 100.0;
                BOOST_CHECK_CLOSE(utilization, expected_util, 0.1);
                
                // Property: Utilization should be in valid range
                BOOST_CHECK_GE(utilization, 0.0);
                BOOST_CHECK_LE(utilization, 100.0);
                
                // Property: Peak usage should track maximum
                BOOST_CHECK_GE(metrics.peak_usage, metrics.allocated_size);
            }
        }
        
        // Property: At target load, utilization should be approximately 50%
        double mid_utilization = pool.get_utilization_percentage();
        BOOST_CHECK_GT(mid_utilization, 40.0);
        BOOST_CHECK_LT(mid_utilization, 60.0);
        
        // Deallocate half
        for (std::size_t i = 0; i < allocations.size() / 2; ++i) {
            pool.deallocate(allocations[i]);
            allocations[i] = nullptr;
        }
        
        // Property: Utilization should decrease
        double reduced_utilization = pool.get_utilization_percentage();
        BOOST_CHECK_LT(reduced_utilization, mid_utilization);
        
        // Property: Peak usage should remain at maximum
        auto metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics.peak_usage, target_blocks * block_size);
        
        // Clean up
        for (void* ptr : allocations) {
            if (ptr) pool.deallocate(ptr);
        }
        
        // Property: After cleanup, utilization should be 0%
        double final_utilization = pool.get_utilization_percentage();
        BOOST_CHECK_EQUAL(final_utilization, 0.0);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any interleaved allocation and deallocation pattern, metrics
 * should accurately reflect the current state at every point in time.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_real_time_metrics_accuracy, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        std::vector<void*> allocations;
        std::size_t expected_alloc_count = 0;
        std::size_t expected_dealloc_count = 0;
        std::size_t expected_allocated_size = 0;
        std::size_t expected_peak = 0;
        
        // Perform interleaved operations
        std::uniform_int_distribution<int> op_dist(0, 1);
        std::size_t operations = std::min(max_blocks * 2, std::size_t{200});
        
        for (std::size_t i = 0; i < operations; ++i) {
            bool should_allocate = op_dist(gen) == 0 || allocations.empty();
            
            if (should_allocate && allocations.size() < max_blocks) {
                // Allocate
                void* ptr = pool.allocate(block_size / 2);
                if (ptr) {
                    allocations.push_back(ptr);
                    expected_alloc_count++;
                    expected_allocated_size += block_size;
                    expected_peak = std::max(expected_peak, expected_allocated_size);
                }
            } else if (!allocations.empty()) {
                // Deallocate
                std::uniform_int_distribution<std::size_t> idx_dist(0, allocations.size() - 1);
                std::size_t idx = idx_dist(gen);
                
                if (allocations[idx] != nullptr) {
                    pool.deallocate(allocations[idx]);
                    allocations[idx] = nullptr;
                    expected_dealloc_count++;
                    expected_allocated_size -= block_size;
                }
            }
            
            // Property: Metrics should match expected values at every step
            auto metrics = pool.get_metrics();
            BOOST_CHECK_EQUAL(metrics.allocation_count, expected_alloc_count);
            BOOST_CHECK_EQUAL(metrics.deallocation_count, expected_dealloc_count);
            BOOST_CHECK_EQUAL(metrics.allocated_size, expected_allocated_size);
            BOOST_CHECK_EQUAL(metrics.free_size, pool_size - expected_allocated_size);
            BOOST_CHECK_EQUAL(metrics.peak_usage, expected_peak);
        }
        
        // Clean up
        for (void* ptr : allocations) {
            if (ptr) pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any pool reset operation, metrics should be updated correctly
 * while preserving cumulative counters (allocation_count, deallocation_count).
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_metrics_after_reset, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        auto metrics_before = pool.get_metrics();
        BOOST_CHECK_GT(metrics_before.allocated_size, 0);
        BOOST_CHECK_GT(metrics_before.allocation_count, 0);
        
        // Reset the pool
        pool.reset();
        
        auto metrics_after = pool.get_metrics();
        
        // Property: Size metrics should be reset
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
        BOOST_CHECK_EQUAL(metrics_after.free_size, pool_size);
        
        // Property: total_size should remain constant
        BOOST_CHECK_EQUAL(metrics_after.total_size, pool_size);
        
        // Property: Fragmentation should be 100% (all free)
        BOOST_CHECK_EQUAL(metrics_after.fragmentation_ratio, 100);
        
        // Property: Should be able to allocate all blocks after reset
        std::vector<void*> new_allocations;
        for (std::size_t i = 0; i < max_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                new_allocations.push_back(ptr);
            }
        }
        
        BOOST_CHECK_EQUAL(new_allocations.size(), max_blocks);
        
        // Clean up
        for (void* ptr : new_allocations) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any pool exhaustion scenario, metrics should accurately
 * reflect the exhausted state and provide correct capacity information.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_metrics_at_exhaustion, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t total_blocks = pool_size / block_size;
        
        // Allocate all blocks to exhaust the pool
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < total_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Property: Pool should be exhausted
        BOOST_CHECK(pool.is_exhausted());
        
        auto metrics = pool.get_metrics();
        
        // Property: At exhaustion, allocated_size should equal total_size
        BOOST_CHECK_EQUAL(metrics.allocated_size, pool_size);
        
        // Property: At exhaustion, free_size should be 0
        BOOST_CHECK_EQUAL(metrics.free_size, 0);
        
        // Property: At exhaustion, fragmentation should be 0%
        BOOST_CHECK_EQUAL(metrics.fragmentation_ratio, 0);
        
        // Property: At exhaustion, utilization should be 100%
        double utilization = pool.get_utilization_percentage();
        BOOST_CHECK_EQUAL(utilization, 100.0);
        
        // Property: At exhaustion, available_space should be 0
        BOOST_CHECK_EQUAL(pool.available_space(), 0);
        
        // Property: At exhaustion, free_block_count should be 0
        BOOST_CHECK_EQUAL(pool.free_block_count(), 0);
        
        // Property: At exhaustion, allocated_block_count should equal total blocks
        BOOST_CHECK_EQUAL(pool.allocated_block_count(), total_blocks);
        
        // Property: Further allocations should fail
        void* ptr = pool.allocate(block_size / 2);
        BOOST_CHECK(ptr == nullptr);
        
        // Deallocate one block
        pool.deallocate(allocations[0]);
        allocations[0] = nullptr;
        
        // Property: After deallocation, pool should no longer be exhausted
        BOOST_CHECK(!pool.is_exhausted());
        
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_GT(metrics_after.free_size, 0);
        BOOST_CHECK_LT(metrics_after.allocated_size, pool_size);
        
        // Clean up
        for (void* p : allocations) {
            if (p) pool.deallocate(p);
        }
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any varying block sizes, metrics should accurately track
 * memory usage based on actual block allocation (not requested size).
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_metrics_with_varying_sizes, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        std::vector<void*> allocations;
        std::size_t expected_allocated = 0;
        
        // Allocate with varying requested sizes (all use same block size)
        std::size_t alloc_count = random_allocation_count(max_blocks);
        for (std::size_t i = 0; i < alloc_count; ++i) {
            std::size_t requested_size = random_allocation_size(block_size);
            void* ptr = pool.allocate(requested_size);
            
            if (ptr) {
                allocations.push_back(ptr);
                expected_allocated += block_size; // Each allocation uses one block
                
                auto metrics = pool.get_metrics();
                
                // Property: allocated_size should reflect block size, not requested size
                BOOST_CHECK_EQUAL(metrics.allocated_size, expected_allocated);
                
                // Property: Invariant should hold
                BOOST_CHECK_EQUAL(metrics.allocated_size + metrics.free_size, pool_size);
            }
        }
        
        // Property: Number of allocated blocks should match allocation count
        BOOST_CHECK_EQUAL(pool.allocated_block_count(), allocations.size());
        
        // Clean up
        for (void* ptr : allocations) {
            pool.deallocate(ptr);
        }
        
        // Property: After cleanup, all metrics should return to initial state
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocated_size, 0);
        BOOST_CHECK_EQUAL(final_metrics.free_size, pool_size);
        BOOST_CHECK_EQUAL(pool.allocated_block_count(), 0);
        BOOST_CHECK_EQUAL(pool.free_block_count(), max_blocks);
    }
}

/**
 * **Feature: coap-transport, Property 39: Memory pool size monitoring**
 * 
 * Property: For any long-running allocation pattern, metrics should remain
 * accurate and consistent over extended periods without drift or overflow.
 * 
 * **Validates: Requirements 14.3**
 */
BOOST_AUTO_TEST_CASE(property_metrics_long_term_stability, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024; // Fixed size for faster test
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        std::vector<void*> allocations;
        allocations.reserve(max_blocks);
        
        // Perform many allocation/deallocation cycles
        std::size_t cycles = 1000;
        std::size_t total_allocs = 0;
        std::size_t total_deallocs = 0;
        
        for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
            // Allocate a few blocks
            for (int i = 0; i < 5 && allocations.size() < max_blocks; ++i) {
                void* ptr = pool.allocate(block_size / 2);
                if (ptr) {
                    allocations.push_back(ptr);
                    total_allocs++;
                }
            }
            
            // Deallocate a few blocks
            for (int i = 0; i < 3 && !allocations.empty(); ++i) {
                pool.deallocate(allocations.back());
                allocations.pop_back();
                total_deallocs++;
            }
            
            // Property: Metrics should remain consistent
            auto metrics = pool.get_metrics();
            BOOST_CHECK_EQUAL(metrics.total_size, pool_size);
            BOOST_CHECK_EQUAL(metrics.allocated_size + metrics.free_size, pool_size);
            BOOST_CHECK_EQUAL(metrics.allocation_count, total_allocs);
            BOOST_CHECK_EQUAL(metrics.deallocation_count, total_deallocs);
            BOOST_CHECK_LE(metrics.fragmentation_ratio, 100);
            BOOST_CHECK_GE(metrics.peak_usage, metrics.allocated_size);
        }
        
        // Property: After many cycles, metrics should still be accurate
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocation_count, total_allocs);
        BOOST_CHECK_EQUAL(final_metrics.deallocation_count, total_deallocs);
        BOOST_CHECK_EQUAL(final_metrics.allocated_size, allocations.size() * block_size);
        
        // Clean up
        for (void* ptr : allocations) {
            pool.deallocate(ptr);
        }
    }
}
