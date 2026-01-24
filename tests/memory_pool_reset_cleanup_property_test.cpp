#define BOOST_TEST_MODULE memory_pool_reset_cleanup_property_test
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>

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
    
    // Generate random reset interval
    auto random_reset_interval() -> std::chrono::seconds {
        std::uniform_int_distribution<int> dist(1, 5);
        return std::chrono::seconds{dist(gen)};
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any sequence of allocations and deallocations, reset() should:
 * 1. Clear all allocations
 * 2. Defragment the pool
 * 3. Reclaim all memory
 * 4. Update metrics correctly
 * 5. Allow subsequent allocations to succeed
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_reset_reclaims_all_memory, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        // Generate random pool configuration
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        // Ensure pool size is multiple of block size
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        
        // Calculate maximum number of blocks
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate random number of blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Verify allocations were made
        auto metrics_before = pool.get_metrics();
        BOOST_CHECK_GT(metrics_before.allocated_size, 0);
        BOOST_CHECK_EQUAL(metrics_before.allocation_count, allocations.size());
        
        // Reset the pool
        pool.reset();
        
        // Property: After reset, all memory should be reclaimed
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
        BOOST_CHECK_EQUAL(metrics_after.free_size, pool_size);
        
        // Property: Should be able to allocate all blocks again
        std::vector<void*> new_allocations;
        for (std::size_t i = 0; i < max_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            BOOST_CHECK(ptr != nullptr);
            if (ptr) {
                new_allocations.push_back(ptr);
            }
        }
        
        // Property: All blocks should be allocatable after reset
        BOOST_CHECK_EQUAL(new_allocations.size(), max_blocks);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any fragmentation pattern, reset() should defragment the pool
 * and restore it to a pristine state.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_reset_defragments_pool, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        
        // Create fragmentation by allocating all blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < max_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Deallocate random blocks to create fragmentation
        std::uniform_int_distribution<std::size_t> dist(0, allocations.size() - 1);
        std::size_t dealloc_count = allocations.size() / 2;
        
        for (std::size_t i = 0; i < dealloc_count; ++i) {
            std::size_t idx = dist(gen);
            if (allocations[idx] != nullptr) {
                pool.deallocate(allocations[idx]);
                allocations[idx] = nullptr;
            }
        }
        
        // Reset should defragment
        pool.reset();
        
        // Property: After reset, should be able to allocate contiguous blocks
        std::vector<void*> new_allocations;
        for (std::size_t i = 0; i < max_blocks; ++i) {
            void* ptr = pool.allocate(block_size / 2);
            BOOST_CHECK(ptr != nullptr);
            if (ptr) {
                new_allocations.push_back(ptr);
            }
        }
        
        // Property: All blocks should be available after defragmentation
        BOOST_CHECK_EQUAL(new_allocations.size(), max_blocks);
        
        // Property: Metrics should show zero fragmentation after reset
        auto metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics.allocated_size, pool_size);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any pool configuration, the destructor should properly clean up
 * all resources without leaking memory or hanging.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_destructor_cleanup, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        {
            memory_pool pool(pool_size, block_size);
            std::size_t max_blocks = pool_size / block_size;
            std::size_t alloc_count = random_allocation_count(max_blocks);
            
            // Allocate blocks without deallocating
            for (std::size_t i = 0; i < alloc_count; ++i) {
                void* ptr = pool.allocate(block_size / 2);
                (void)ptr; // Intentionally not deallocating
            }
            
            // Property: Destructor should handle active allocations
            // Pool destructor will be called here
        }
        
        // Property: If we reach here without crashing, destructor worked correctly
        BOOST_CHECK(true);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any pool with periodic reset enabled, the destructor should
 * properly stop the reset thread without hanging or crashing.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_destructor_stops_periodic_reset_thread, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        auto reset_interval = random_reset_interval();
        
        {
            memory_pool pool(pool_size, block_size, reset_interval);
            
            // Let the periodic reset thread start
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Property: Destructor should stop the thread cleanly
            // Pool destructor will be called here
        }
        
        // Property: If we reach here without hanging, destructor worked correctly
        BOOST_CHECK(true);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any periodic reset configuration, the reset mechanism should
 * only reset when there are no active allocations.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_periodic_reset_respects_active_allocations, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024; // Fixed size for faster test
        std::size_t block_size = 4096;
        
        auto reset_interval = std::chrono::seconds{1}; // Short interval for testing
        
        memory_pool pool(pool_size, block_size, reset_interval);
        
        // Allocate and keep allocation
        void* ptr = pool.allocate(block_size / 2);
        BOOST_CHECK(ptr != nullptr);
        
        auto metrics_before = pool.get_metrics();
        BOOST_CHECK_GT(metrics_before.allocated_size, 0);
        
        // Wait for reset interval
        std::this_thread::sleep_for(reset_interval + std::chrono::milliseconds(500));
        
        // Property: Allocation should still be there (reset shouldn't have happened)
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_GT(metrics_after.allocated_size, 0);
        
        // Clean up
        pool.deallocate(ptr);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any periodic reset configuration, the reset mechanism should
 * reset the pool when there are no active allocations.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_periodic_reset_resets_when_idle, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024;
        std::size_t block_size = 4096;
        
        auto reset_interval = std::chrono::seconds{1};
        
        memory_pool pool(pool_size, block_size, reset_interval);
        
        // Allocate and deallocate
        void* ptr = pool.allocate(block_size / 2);
        BOOST_CHECK(ptr != nullptr);
        pool.deallocate(ptr);
        
        // Wait for reset to occur
        std::this_thread::sleep_for(reset_interval + std::chrono::milliseconds(500));
        
        // Property: Reset should have occurred
        auto time_since = pool.time_since_last_reset();
        BOOST_CHECK_LT(time_since.count(), 2); // Should be less than 2 seconds
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any RAII guard, the destructor should automatically deallocate
 * the memory when the guard goes out of scope.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_raii_guard_automatic_cleanup, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        {
            std::vector<memory_pool_guard> guards;
            
            // Allocate with RAII guards
            for (std::size_t i = 0; i < alloc_count; ++i) {
                guards.push_back(pool.allocate_guarded(block_size / 2));
            }
            
            auto metrics_during = pool.get_metrics();
            BOOST_CHECK_EQUAL(metrics_during.allocation_count, alloc_count);
            BOOST_CHECK_GT(metrics_during.allocated_size, 0);
            
            // Property: Guards will automatically deallocate when going out of scope
        }
        
        // Property: All allocations should be deallocated
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
        BOOST_CHECK_EQUAL(metrics_after.deallocation_count, alloc_count);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any RAII guard, move semantics should transfer ownership
 * correctly without double-deallocation.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_raii_guard_move_semantics, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        
        {
            memory_pool_guard guard1 = pool.allocate_guarded(block_size / 2);
            void* ptr1 = guard1.get();
            BOOST_CHECK(ptr1 != nullptr);
            
            // Move construct
            memory_pool_guard guard2(std::move(guard1));
            
            // Property: Ownership should be transferred
            BOOST_CHECK(guard2.get() == ptr1);
            BOOST_CHECK(guard1.get() == nullptr);
            
            auto metrics = pool.get_metrics();
            BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
            
            // Property: Only one deallocation should occur when guard2 is destroyed
        }
        
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics_after.deallocation_count, 1);
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any RAII guard, release() should transfer ownership and
 * prevent automatic deallocation.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_raii_guard_release, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        
        void* released_ptr = nullptr;
        {
            auto guard = pool.allocate_guarded(block_size / 2);
            BOOST_CHECK(guard.get() != nullptr);
            
            // Release ownership
            released_ptr = guard.release();
            BOOST_CHECK(released_ptr != nullptr);
            BOOST_CHECK(guard.get() == nullptr);
            
            // Property: Guard should not deallocate when destroyed
        }
        
        // Property: Memory should still be allocated
        auto metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
        BOOST_CHECK_GT(metrics.allocated_size, 0);
        
        // Manual cleanup
        pool.deallocate(released_ptr);
        
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any exception during RAII guard lifetime, the guard should
 * still properly deallocate the memory (exception safety).
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_raii_guard_exception_safety, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        
        try {
            auto guard = pool.allocate_guarded(block_size / 2);
            BOOST_CHECK(guard.get() != nullptr);
            
            auto metrics = pool.get_metrics();
            BOOST_CHECK_GT(metrics.allocated_size, 0);
            
            // Simulate exception
            throw std::runtime_error("Test exception");
        } catch (const std::exception&) {
            // Property: Exception caught, guard should have cleaned up
        }
        
        // Property: Memory should be deallocated despite exception
        auto metrics_after = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any concurrent reset and allocation operations, the pool
 * should remain in a consistent state without crashes or data corruption.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_concurrent_reset_and_allocation, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 512 * 1024;
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size);
        
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> successful_allocations{0};
        std::atomic<std::size_t> reset_count{0};
        
        // Thread that allocates and deallocates
        std::thread alloc_thread([&]() {
            while (!stop) {
                void* ptr = pool.allocate(block_size / 2);
                if (ptr) {
                    successful_allocations++;
                    pool.deallocate(ptr);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        
        // Thread that resets
        std::thread reset_thread([&]() {
            for (int i = 0; i < 5; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                pool.reset();
                reset_count++;
            }
            stop = true;
        });
        
        alloc_thread.join();
        reset_thread.join();
        
        // Property: Operations should complete without crashes
        BOOST_CHECK_GT(successful_allocations.load(), 0);
        BOOST_CHECK_EQUAL(reset_count.load(), 5);
        
        // Property: Pool should be in consistent state
        auto metrics = pool.get_metrics();
        BOOST_CHECK_LE(metrics.allocated_size, pool_size);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any reset operation, the last_reset timestamp should be
 * updated correctly.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_reset_updates_timestamp, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size);
        
        auto initial_metrics = pool.get_metrics();
        auto initial_time = initial_metrics.last_reset;
        
        // Wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Reset the pool
        pool.reset();
        
        auto after_metrics = pool.get_metrics();
        auto after_time = after_metrics.last_reset;
        
        // Property: Timestamp should be updated
        BOOST_CHECK(after_time > initial_time);
        
        // Property: time_since_last_reset should be small
        auto time_since = pool.time_since_last_reset();
        BOOST_CHECK_LT(time_since.count(), 1);
    }
}

/**
 * **Feature: coap-transport, Property 38: Memory pool reset and cleanup**
 * 
 * Property: For any sequence of enable/disable periodic reset operations,
 * the pool should handle the transitions correctly without crashes.
 * 
 * **Validates: Requirements 14.2**
 */
BOOST_AUTO_TEST_CASE(property_periodic_reset_enable_disable, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024;
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size);
        
        // Enable periodic reset
        pool.set_periodic_reset(true, std::chrono::seconds{1});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Disable periodic reset
        pool.set_periodic_reset(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Enable again
        pool.set_periodic_reset(true, std::chrono::seconds{1});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Disable again
        pool.set_periodic_reset(false);
        
        // Property: Pool should handle enable/disable transitions correctly
        BOOST_CHECK(true);
    }
}
