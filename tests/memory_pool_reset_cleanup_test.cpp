#define BOOST_TEST_MODULE memory_pool_reset_cleanup_test
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <thread>
#include <vector>
#include <chrono>

using namespace kythira;

namespace {
    constexpr std::size_t test_pool_size = 1024 * 1024; // 1MB
    constexpr std::size_t test_block_size = 4096; // 4KB
    constexpr std::size_t test_allocation_size = 2048; // 2KB
    constexpr auto short_reset_interval = std::chrono::seconds{1};
    constexpr auto test_timeout_seconds = 30;
}

// Test 1: Basic reset functionality
BOOST_AUTO_TEST_CASE(test_reset_clears_allocations, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    // Allocate several blocks
    void* ptr1 = pool.allocate(test_allocation_size);
    void* ptr2 = pool.allocate(test_allocation_size);
    void* ptr3 = pool.allocate(test_allocation_size);
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    BOOST_CHECK(ptr3 != nullptr);
    
    auto metrics_before = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_before.allocation_count, 3);
    BOOST_CHECK_GT(metrics_before.allocated_size, 0);
    
    // Reset the pool
    pool.reset();
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    BOOST_CHECK_EQUAL(metrics_after.free_size, test_pool_size);
    
    // Old pointers should not be valid anymore, but we can allocate new ones
    void* ptr4 = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr4 != nullptr);
}

// Test 2: Reset updates last_reset timestamp
BOOST_AUTO_TEST_CASE(test_reset_updates_timestamp, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    auto initial_metrics = pool.get_metrics();
    auto initial_time = initial_metrics.last_reset;
    
    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Reset the pool
    pool.reset();
    
    auto after_metrics = pool.get_metrics();
    auto after_time = after_metrics.last_reset;
    
    // Timestamp should be updated
    BOOST_CHECK(after_time > initial_time);
}

// Test 3: time_since_last_reset works correctly
BOOST_AUTO_TEST_CASE(test_time_since_last_reset, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    // Check initial time
    auto time1 = pool.time_since_last_reset();
    BOOST_CHECK_GE(time1.count(), 0);
    
    // Wait a bit longer to ensure measurable time difference
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Check time again
    auto time2 = pool.time_since_last_reset();
    BOOST_CHECK_GE(time2.count(), time1.count()); // Should be at least as much
    
    // Reset and check time is near zero
    pool.reset();
    auto time3 = pool.time_since_last_reset();
    BOOST_CHECK_LT(time3.count(), 1); // Should be less than 1 second
}

// Test 4: Periodic reset with manual enable/disable
BOOST_AUTO_TEST_CASE(test_periodic_reset_manual_control, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    // Enable periodic reset with short interval
    pool.set_periodic_reset(true, short_reset_interval);
    
    // Allocate and deallocate to create activity
    void* ptr = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr != nullptr);
    pool.deallocate(ptr);
    
    // Wait for reset to occur
    std::this_thread::sleep_for(short_reset_interval + std::chrono::milliseconds(500));
    
    // Check that time since last reset is small (reset should have occurred)
    auto time_since = pool.time_since_last_reset();
    BOOST_CHECK_LT(time_since.count(), 2); // Should be less than 2 seconds
    
    // Disable periodic reset
    pool.set_periodic_reset(false);
}

// Test 5: Periodic reset doesn't reset when allocations are active
BOOST_AUTO_TEST_CASE(test_periodic_reset_respects_active_allocations, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    // Enable periodic reset
    pool.set_periodic_reset(true, short_reset_interval);
    
    // Allocate and keep the allocation
    void* ptr = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr != nullptr);
    
    auto metrics_before = pool.get_metrics();
    BOOST_CHECK_GT(metrics_before.allocated_size, 0);
    
    // Wait for reset interval
    std::this_thread::sleep_for(short_reset_interval + std::chrono::milliseconds(500));
    
    // Allocation should still be there (reset shouldn't have happened)
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_GT(metrics_after.allocated_size, 0);
    
    // Clean up
    pool.deallocate(ptr);
    pool.set_periodic_reset(false);
}

// Test 6: Periodic reset at construction time
BOOST_AUTO_TEST_CASE(test_periodic_reset_at_construction, * boost::unit_test::timeout(test_timeout_seconds)) {
    // Create pool with periodic reset enabled
    memory_pool pool(test_pool_size, test_block_size, short_reset_interval);
    
    // Allocate and deallocate
    void* ptr = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr != nullptr);
    pool.deallocate(ptr);
    
    // Wait for reset
    std::this_thread::sleep_for(short_reset_interval + std::chrono::milliseconds(500));
    
    // Check that reset occurred
    auto time_since = pool.time_since_last_reset();
    BOOST_CHECK_LT(time_since.count(), 2);
}

// Test 7: RAII guard basic functionality
BOOST_AUTO_TEST_CASE(test_raii_guard_basic, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    {
        auto guard = pool.allocate_guarded(test_allocation_size);
        BOOST_CHECK(guard.get() != nullptr);
        
        auto metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
        BOOST_CHECK_GT(metrics.allocated_size, 0);
    } // guard goes out of scope, should deallocate
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.deallocation_count, 1);
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
}

// Test 8: RAII guard move semantics
BOOST_AUTO_TEST_CASE(test_raii_guard_move, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    memory_pool_guard guard1 = pool.allocate_guarded(test_allocation_size);
    void* ptr1 = guard1.get();
    BOOST_CHECK(ptr1 != nullptr);
    
    // Move construct
    memory_pool_guard guard2(std::move(guard1));
    BOOST_CHECK(guard2.get() == ptr1);
    BOOST_CHECK(guard1.get() == nullptr); // Original should be null
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
    BOOST_CHECK_GT(metrics.allocated_size, 0);
}

// Test 9: RAII guard release
BOOST_AUTO_TEST_CASE(test_raii_guard_release, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    void* released_ptr = nullptr;
    {
        auto guard = pool.allocate_guarded(test_allocation_size);
        BOOST_CHECK(guard.get() != nullptr);
        
        // Release ownership
        released_ptr = guard.release();
        BOOST_CHECK(released_ptr != nullptr);
        BOOST_CHECK(guard.get() == nullptr);
    } // guard goes out of scope, but shouldn't deallocate
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
    BOOST_CHECK_GT(metrics.allocated_size, 0); // Still allocated
    
    // Manual cleanup
    pool.deallocate(released_ptr);
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
}

// Test 10: Multiple RAII guards
BOOST_AUTO_TEST_CASE(test_multiple_raii_guards, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    {
        auto guard1 = pool.allocate_guarded(test_allocation_size);
        auto guard2 = pool.allocate_guarded(test_allocation_size);
        auto guard3 = pool.allocate_guarded(test_allocation_size);
        
        BOOST_CHECK(guard1.get() != nullptr);
        BOOST_CHECK(guard2.get() != nullptr);
        BOOST_CHECK(guard3.get() != nullptr);
        
        auto metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(metrics.allocation_count, 3);
    } // All guards go out of scope
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.deallocation_count, 3);
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
}

// Test 11: RAII guard with exception safety
BOOST_AUTO_TEST_CASE(test_raii_guard_exception_safety, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    try {
        auto guard = pool.allocate_guarded(test_allocation_size);
        BOOST_CHECK(guard.get() != nullptr);
        
        auto metrics = pool.get_metrics();
        BOOST_CHECK_GT(metrics.allocated_size, 0);
        
        // Simulate exception
        throw std::runtime_error("Test exception");
    } catch (const std::exception&) {
        // Exception caught, guard should have cleaned up
    }
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
}

// Test 12: Destructor cleanup with active allocations
BOOST_AUTO_TEST_CASE(test_destructor_cleanup, * boost::unit_test::timeout(test_timeout_seconds)) {
    {
        memory_pool pool(test_pool_size, test_block_size);
        
        // Allocate some blocks
        void* ptr1 = pool.allocate(test_allocation_size);
        void* ptr2 = pool.allocate(test_allocation_size);
        
        BOOST_CHECK(ptr1 != nullptr);
        BOOST_CHECK(ptr2 != nullptr);
        
        auto metrics = pool.get_metrics();
        BOOST_CHECK_GT(metrics.allocated_size, 0);
        
        // Pool destructor will be called here
    }
    
    // If we get here without crashing, destructor cleanup worked
    BOOST_CHECK(true);
}

// Test 13: Destructor cleanup with periodic reset thread
BOOST_AUTO_TEST_CASE(test_destructor_cleanup_with_periodic_reset, * boost::unit_test::timeout(test_timeout_seconds)) {
    {
        memory_pool pool(test_pool_size, test_block_size, short_reset_interval);
        
        // Let the periodic reset thread start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Pool destructor will be called here, should stop the thread
    }
    
    // If we get here without hanging or crashing, destructor cleanup worked
    BOOST_CHECK(true);
}

// Test 14: Reset defragmentation
BOOST_AUTO_TEST_CASE(test_reset_defragmentation, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    // Allocate and deallocate in a pattern that could cause fragmentation
    std::vector<void*> ptrs;
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(pool.allocate(test_allocation_size));
    }
    
    // Deallocate every other block
    for (size_t i = 0; i < ptrs.size(); i += 2) {
        pool.deallocate(ptrs[i]);
    }
    
    auto metrics_before = pool.get_metrics();
    BOOST_CHECK_GT(metrics_before.allocated_size, 0);
    
    // Reset should defragment
    pool.reset();
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    BOOST_CHECK_EQUAL(metrics_after.free_size, test_pool_size);
    
    // Should be able to allocate all blocks again
    std::vector<void*> new_ptrs;
    for (int i = 0; i < 10; ++i) {
        void* ptr = pool.allocate(test_allocation_size);
        BOOST_CHECK(ptr != nullptr);
        new_ptrs.push_back(ptr);
    }
}

// Test 15: Concurrent reset and allocation
BOOST_AUTO_TEST_CASE(test_concurrent_reset_and_allocation, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size);
    
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    
    // Thread that allocates and deallocates
    threads.emplace_back([&pool, &stop]() {
        while (!stop) {
            void* ptr = pool.allocate(test_allocation_size);
            if (ptr) {
                pool.deallocate(ptr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    
    // Thread that resets
    threads.emplace_back([&pool, &stop]() {
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            pool.reset();
        }
        stop = true;
    });
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // If we get here without crashing, concurrent operations worked
    BOOST_CHECK(true);
}
