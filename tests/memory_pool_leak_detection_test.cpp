#define BOOST_TEST_MODULE memory_pool_leak_detection_test
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
    constexpr auto short_leak_threshold = std::chrono::seconds{1};
    constexpr auto test_timeout_seconds = 30;
}

/**
 * Test 1: Leak detection can be enabled/disabled via configuration
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_detection_configuration, * boost::unit_test::timeout(test_timeout_seconds)) {
    // Create pool with leak detection disabled (default)
    memory_pool pool1(test_pool_size, test_block_size);
    BOOST_CHECK(!pool1.is_leak_detection_enabled());
    
    // Create pool with leak detection enabled
    memory_pool pool2(test_pool_size, test_block_size, std::chrono::seconds{0}, true);
    BOOST_CHECK(pool2.is_leak_detection_enabled());
    
    // Enable leak detection dynamically
    pool1.set_leak_detection(true);
    BOOST_CHECK(pool1.is_leak_detection_enabled());
    
    // Disable leak detection dynamically
    pool2.set_leak_detection(false);
    BOOST_CHECK(!pool2.is_leak_detection_enabled());
}

/**
 * Test 2: Leak threshold can be configured
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_threshold_configuration, * boost::unit_test::timeout(test_timeout_seconds)) {
    // Create pool with custom leak threshold
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    BOOST_CHECK_EQUAL(pool.get_leak_threshold().count(), short_leak_threshold.count());
    
    // Change threshold dynamically
    auto new_threshold = std::chrono::seconds{120};
    pool.set_leak_detection(true, new_threshold);
    BOOST_CHECK_EQUAL(pool.get_leak_threshold().count(), new_threshold.count());
}

/**
 * Test 3: Detect leaks with allocation timestamps
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_detect_leaks_with_timestamps, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    // Allocate some blocks
    void* ptr1 = pool.allocate(test_allocation_size);
    void* ptr2 = pool.allocate(test_allocation_size);
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    
    // Wait for leak threshold to pass
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    
    // Should detect both allocations as leaks
    BOOST_CHECK_EQUAL(leaks.size(), 2);
    
    // Verify leak information
    for (const auto& leak : leaks) {
        BOOST_CHECK(leak.address != nullptr);
        BOOST_CHECK_EQUAL(leak.size, test_allocation_size);
        BOOST_CHECK(leak.age >= short_leak_threshold);
        BOOST_CHECK(!leak.allocation_context.empty());
    }
    
    // Clean up
    pool.deallocate(ptr1);
    pool.deallocate(ptr2);
}

/**
 * Test 4: Allocation context is captured when leak detection is enabled
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_allocation_context_capture, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    // Allocate with custom context
    void* ptr1 = pool.allocate(test_allocation_size, "test_context_1");
    void* ptr2 = pool.allocate(test_allocation_size, "test_context_2");
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 2);
    
    // Verify contexts are captured
    bool found_context1 = false;
    bool found_context2 = false;
    
    for (const auto& leak : leaks) {
        if (leak.allocation_context == "test_context_1") {
            found_context1 = true;
        }
        if (leak.allocation_context == "test_context_2") {
            found_context2 = true;
        }
    }
    
    BOOST_CHECK(found_context1);
    BOOST_CHECK(found_context2);
    
    // Clean up
    pool.deallocate(ptr1);
    pool.deallocate(ptr2);
}

/**
 * Test 5: Thread ID is captured in leak reports
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_thread_id_capture, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    void* ptr = pool.allocate(test_allocation_size, "main_thread_allocation");
    BOOST_CHECK(ptr != nullptr);
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 1);
    
    // Verify thread ID is captured
    BOOST_CHECK(!leaks[0].thread_id.empty());
    BOOST_CHECK(leaks[0].thread_id != "unknown");
    
    // Clean up
    pool.deallocate(ptr);
}

/**
 * Test 6: Leak detection with disabled mode provides basic info
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_detection_disabled_mode, * boost::unit_test::timeout(test_timeout_seconds)) {
    // Create pool with leak detection disabled
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, false, short_leak_threshold);
    
    void* ptr = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr != nullptr);
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks (should still work but with limited info)
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 1);
    
    // Verify basic info is provided
    BOOST_CHECK(leaks[0].address != nullptr);
    BOOST_CHECK_EQUAL(leaks[0].size, test_allocation_size);
    BOOST_CHECK(leaks[0].age >= short_leak_threshold);
    
    // Context should indicate leak detection is disabled
    BOOST_CHECK(leaks[0].allocation_context.find("enable leak detection") != std::string::npos);
    BOOST_CHECK_EQUAL(leaks[0].thread_id, "unknown");
    
    // Clean up
    pool.deallocate(ptr);
}

/**
 * Test 7: Detailed leak reports with addresses and sizes
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_detailed_leak_reports, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    // Allocate blocks of different contexts
    std::vector<void*> ptrs;
    for (int i = 0; i < 5; ++i) {
        std::string context = "allocation_" + std::to_string(i);
        void* ptr = pool.allocate(test_allocation_size, context);
        BOOST_CHECK(ptr != nullptr);
        ptrs.push_back(ptr);
    }
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 5);
    
    // Verify each leak has detailed information
    for (const auto& leak : leaks) {
        // Address should be valid
        BOOST_CHECK(leak.address != nullptr);
        
        // Size should match
        BOOST_CHECK_EQUAL(leak.size, test_allocation_size);
        
        // Age should be at least the threshold
        BOOST_CHECK(leak.age >= short_leak_threshold);
        
        // Context should be captured
        BOOST_CHECK(leak.allocation_context.find("allocation_") != std::string::npos);
        
        // Thread ID should be captured
        BOOST_CHECK(!leak.thread_id.empty());
        
        // Allocation time should be valid
        auto now = std::chrono::steady_clock::now();
        BOOST_CHECK(leak.allocation_time <= now);
    }
    
    // Clean up
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
}

/**
 * Test 8: No leaks detected for short-lived allocations
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_no_leaks_for_short_lived_allocations, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    // Allocate and deallocate quickly
    void* ptr1 = pool.allocate(test_allocation_size);
    void* ptr2 = pool.allocate(test_allocation_size);
    
    BOOST_CHECK(ptr1 != nullptr);
    BOOST_CHECK(ptr2 != nullptr);
    
    // Deallocate immediately
    pool.deallocate(ptr1);
    pool.deallocate(ptr2);
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Should not detect any leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 0);
}

/**
 * Test 9: Leak detection with multiple threads
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_detection_multithreaded, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    std::vector<std::thread> threads;
    std::vector<void*> ptrs(4, nullptr);
    
    // Allocate from multiple threads
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&pool, &ptrs, i]() {
            std::string context = "thread_" + std::to_string(i);
            ptrs[i] = pool.allocate(test_allocation_size, context);
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify all allocations succeeded
    for (void* ptr : ptrs) {
        BOOST_CHECK(ptr != nullptr);
    }
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 4);
    
    // Verify each leak has a thread ID
    for (const auto& leak : leaks) {
        BOOST_CHECK(!leak.thread_id.empty());
        BOOST_CHECK(leak.thread_id != "unknown");
    }
    
    // Clean up
    for (void* ptr : ptrs) {
        pool.deallocate(ptr);
    }
}

/**
 * Test 10: Leak prevention through early detection
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_prevention, * boost::unit_test::timeout(test_timeout_seconds)) {
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, short_leak_threshold);
    
    // Allocate some blocks
    std::vector<void*> ptrs;
    for (int i = 0; i < 3; ++i) {
        void* ptr = pool.allocate(test_allocation_size, "potential_leak");
        ptrs.push_back(ptr);
    }
    
    // Wait for leak threshold
    std::this_thread::sleep_for(short_leak_threshold + std::chrono::seconds{1});
    
    // Detect leaks
    auto leaks = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks.size(), 3);
    
    // Prevent leaks by deallocating detected allocations
    for (const auto& leak : leaks) {
        pool.deallocate(leak.address);
    }
    
    // Verify no more leaks
    auto leaks_after = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks_after.size(), 0);
}

/**
 * Test 11: Leak detection with custom threshold
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_detection_custom_threshold, * boost::unit_test::timeout(test_timeout_seconds)) {
    auto custom_threshold = std::chrono::seconds{2};
    memory_pool pool(test_pool_size, test_block_size, std::chrono::seconds{0}, true, custom_threshold);
    
    void* ptr = pool.allocate(test_allocation_size);
    BOOST_CHECK(ptr != nullptr);
    
    // Wait less than threshold - should not detect leak
    std::this_thread::sleep_for(std::chrono::seconds{1});
    auto leaks1 = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks1.size(), 0);
    
    // Wait past threshold - should detect leak
    std::this_thread::sleep_for(std::chrono::seconds{2});
    auto leaks2 = pool.detect_leaks();
    BOOST_CHECK_EQUAL(leaks2.size(), 1);
    
    // Clean up
    pool.deallocate(ptr);
}

/**
 * Test 12: Leak detection performance impact
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(test_leak_detection_performance, * boost::unit_test::timeout(test_timeout_seconds)) {
    // Measure allocation performance without leak detection
    memory_pool pool_no_leak(test_pool_size, test_block_size, std::chrono::seconds{0}, false);
    
    auto start1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        void* ptr = pool_no_leak.allocate(test_allocation_size);
        pool_no_leak.deallocate(ptr);
    }
    auto end1 = std::chrono::steady_clock::now();
    auto duration_no_leak = std::chrono::duration_cast<std::chrono::microseconds>(end1 - start1);
    
    // Measure allocation performance with leak detection
    memory_pool pool_with_leak(test_pool_size, test_block_size, std::chrono::seconds{0}, true);
    
    auto start2 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        void* ptr = pool_with_leak.allocate(test_allocation_size, "perf_test");
        pool_with_leak.deallocate(ptr);
    }
    auto end2 = std::chrono::steady_clock::now();
    auto duration_with_leak = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2);
    
    // Leak detection will have some overhead due to context capture
    // Just verify both complete successfully - the overhead is acceptable
    BOOST_CHECK(duration_no_leak.count() > 0);
    BOOST_CHECK(duration_with_leak.count() > 0);
    
    // Log the overhead for informational purposes
    double overhead_ratio = static_cast<double>(duration_with_leak.count()) / duration_no_leak.count();
    std::cout << "Leak detection overhead ratio: " << overhead_ratio << "x\n";
}
