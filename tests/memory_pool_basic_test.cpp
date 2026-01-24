#define BOOST_TEST_MODULE memory_pool_basic_test
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <thread>
#include <vector>

using namespace kythira;

BOOST_AUTO_TEST_CASE(test_memory_pool_construction, * boost::unit_test::timeout(30)) {
    // Test basic construction
    memory_pool pool(1024 * 1024, 4096); // 1MB pool with 4KB blocks
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.total_size, 1024 * 1024);
    BOOST_CHECK_EQUAL(metrics.free_size, 1024 * 1024);
    BOOST_CHECK_EQUAL(metrics.allocated_size, 0);
    BOOST_CHECK_EQUAL(metrics.allocation_count, 0);
    BOOST_CHECK_EQUAL(metrics.deallocation_count, 0);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_allocation, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096); // 1MB pool with 4KB blocks
    
    // Allocate a block
    void* ptr1 = pool.allocate(2048);
    BOOST_CHECK(ptr1 != nullptr);
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.allocation_count, 1);
    BOOST_CHECK_EQUAL(metrics.allocated_size, 4096); // One block allocated
    BOOST_CHECK_EQUAL(metrics.free_size, 1024 * 1024 - 4096);
    
    // Allocate another block
    void* ptr2 = pool.allocate(3000);
    BOOST_CHECK(ptr2 != nullptr);
    BOOST_CHECK(ptr1 != ptr2);
    
    metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.allocation_count, 2);
    BOOST_CHECK_EQUAL(metrics.allocated_size, 8192); // Two blocks allocated
}

BOOST_AUTO_TEST_CASE(test_memory_pool_deallocation, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    // Allocate and deallocate
    void* ptr = pool.allocate(2048);
    BOOST_CHECK(ptr != nullptr);
    
    auto metrics_before = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_before.allocation_count, 1);
    BOOST_CHECK_EQUAL(metrics_before.allocated_size, 4096);
    
    pool.deallocate(ptr);
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.deallocation_count, 1);
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    BOOST_CHECK_EQUAL(metrics_after.free_size, 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_reset, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    // Allocate several blocks
    void* ptr1 = pool.allocate(2048);
    void* ptr2 = pool.allocate(3000);
    void* ptr3 = pool.allocate(1024);
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    BOOST_CHECK(ptr3 != nullptr);
    
    auto metrics_before = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_before.allocation_count, 3);
    BOOST_CHECK_EQUAL(metrics_before.allocated_size, 12288); // Three blocks
    
    // Reset the pool
    pool.reset();
    
    auto metrics_after = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics_after.allocated_size, 0);
    BOOST_CHECK_EQUAL(metrics_after.free_size, 1024 * 1024);
    
    // Should be able to allocate again
    void* ptr4 = pool.allocate(2048);
    BOOST_CHECK(ptr4 != nullptr);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_exhaustion, * boost::unit_test::timeout(30)) {
    memory_pool pool(16384, 4096); // Small pool: 4 blocks
    
    // Allocate all blocks
    void* ptr1 = pool.allocate(2048);
    void* ptr2 = pool.allocate(2048);
    void* ptr3 = pool.allocate(2048);
    void* ptr4 = pool.allocate(2048);
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    BOOST_CHECK(ptr3 != nullptr);
    BOOST_CHECK(ptr4 != nullptr);
    
    // Pool should be exhausted
    BOOST_CHECK(pool.is_exhausted());
    
    // Next allocation should fail
    void* ptr5 = pool.allocate(2048);
    BOOST_CHECK(ptr5 == nullptr);
    
    // Deallocate one block
    pool.deallocate(ptr1);
    
    // Should be able to allocate again
    BOOST_CHECK(!pool.is_exhausted());
    void* ptr6 = pool.allocate(2048);
    BOOST_CHECK(ptr6 != nullptr);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_thread_safety, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    std::vector<std::thread> threads;
    std::vector<void*> allocations(10, nullptr);
    
    // Allocate from multiple threads
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&pool, &allocations, i]() {
            allocations[i] = pool.allocate(2048);
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Check all allocations succeeded
    for (void* ptr : allocations) {
        BOOST_CHECK(ptr != nullptr);
    }
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.allocation_count, 10);
    
    // Deallocate from multiple threads
    threads.clear();
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&pool, &allocations, i]() {
            pool.deallocate(allocations[i]);
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.deallocation_count, 10);
    BOOST_CHECK_EQUAL(metrics.allocated_size, 0);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_metrics, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    // Allocate and track peak usage
    void* ptr1 = pool.allocate(2048);
    void* ptr2 = pool.allocate(3000);
    void* ptr3 = pool.allocate(1024);
    
    auto metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.peak_usage, 12288); // Three blocks
    
    // Deallocate one
    pool.deallocate(ptr2);
    
    metrics = pool.get_metrics();
    BOOST_CHECK_EQUAL(metrics.peak_usage, 12288); // Peak should remain
    BOOST_CHECK_EQUAL(metrics.allocated_size, 8192); // Two blocks remaining
    
    // Check utilization
    double utilization = pool.get_utilization_percentage();
    BOOST_CHECK(utilization > 0.0 && utilization < 100.0);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_leak_detection, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    // Allocate some blocks
    void* ptr1 = pool.allocate(2048);
    void* ptr2 = pool.allocate(3000);
    
    // Wait a bit to simulate long-lived allocations
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Detect leaks (threshold is 60 seconds, so these won't be detected as leaks yet)
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 0);
    
    // Clean up
    pool.deallocate(ptr1);
    pool.deallocate(ptr2);
}

BOOST_AUTO_TEST_CASE(test_memory_pool_block_size_limit, * boost::unit_test::timeout(30)) {
    memory_pool pool(1024 * 1024, 4096);
    
    // Try to allocate more than block size
    void* ptr = pool.allocate(8192); // Larger than block size
    BOOST_CHECK(ptr == nullptr); // Should fail
    
    // Allocate within block size
    void* ptr2 = pool.allocate(4096);
    BOOST_CHECK(ptr2 != nullptr); // Should succeed
    
    pool.deallocate(ptr2);
}
