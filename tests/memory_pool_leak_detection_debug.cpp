#define BOOST_TEST_MODULE memory_pool_leak_detection_debug
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <thread>
#include <chrono>
#include <iostream>

using namespace kythira;

namespace {
    constexpr std::size_t test_pool_size = 64 * 1024;
    constexpr std::size_t test_block_size = 4096;
    constexpr auto short_leak_threshold = std::chrono::seconds{1};
    constexpr auto test_timeout_seconds = 30;
}

/**
 * Debug test to understand leak detection behavior
 */
BOOST_AUTO_TEST_CASE(debug_leak_detection, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    std::cout << "Pool created with leak detection enabled\n";
    std::cout << "Leak threshold: " << short_leak_threshold.count() << " seconds\n";
    std::cout << "Is leak detection enabled: " << pool.is_leak_detection_enabled() << "\n";
    
    // Allocate a single block
    void* ptr = pool.allocate(test_block_size / 2, "debug_allocation");
    BOOST_CHECK(ptr != nullptr);
    std::cout << "Allocated block at: " << ptr << "\n";
    
    // Check leaks immediately (should be 0)
    auto leaks_before = pool.detect_leaks();
    std::cout << "Leaks before threshold: " << leaks_before.size() << "\n";
    BOOST_CHECK_EQUAL(leaks_before.size(), 0);
    
    // Wait for threshold + buffer
    std::cout << "Waiting for leak threshold...\n";
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{500});
    
    // Check leaks after threshold (should be 1)
    std::cout << "Calling detect_leaks()...\n";
    auto leaks_after = pool.detect_leaks();
    std::cout << "Leaks after threshold: " << leaks_after.size() << "\n";
    std::cout << "Leak threshold setting: " << pool.get_leak_threshold().count() << " seconds\n";
    
    if (leaks_after.size() > 0) {
        for (const auto& leak : leaks_after) {
            std::cout << "Leak detected:\n";
            std::cout << "  Address: " << leak.address << "\n";
            std::cout << "  Size: " << leak.size << "\n";
            std::cout << "  Age: " << leak.age.count() << " seconds\n";
            std::cout << "  Context: " << leak.allocation_context << "\n";
            std::cout << "  Thread ID: " << leak.thread_id << "\n";
        }
    } else {
        std::cout << "ERROR: No leaks detected!\n";
        
        // Debug: Check pool metrics
        auto metrics = pool.get_metrics();
        std::cout << "Pool metrics:\n";
        std::cout << "  Total size: " << metrics.total_size << "\n";
        std::cout << "  Allocated size: " << metrics.allocated_size << "\n";
        std::cout << "  Free size: " << metrics.free_size << "\n";
        std::cout << "  Allocation count: " << metrics.allocation_count << "\n";
        std::cout << "  Deallocation count: " << metrics.deallocation_count << "\n";
    }
    
    BOOST_CHECK_EQUAL(leaks_after.size(), 1);
    
    // Clean up
    pool.deallocate(ptr);
}
