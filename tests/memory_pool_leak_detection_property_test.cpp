#define BOOST_TEST_MODULE memory_pool_leak_detection_property_test
#include <boost/test/unit_test.hpp>
#include <raft/memory_pool.hpp>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <set>

using namespace kythira;

namespace {
    // Test constants
    constexpr std::size_t min_pool_size = 64 * 1024;      // 64KB
    constexpr std::size_t max_pool_size = 1024 * 1024;    // 1MB
    constexpr std::size_t min_block_size = 1024;          // 1KB
    constexpr std::size_t max_block_size = 8192;          // 8KB
    constexpr std::size_t num_property_iterations = 10;
    constexpr auto test_timeout_seconds = 120;
    constexpr auto short_leak_threshold = std::chrono::seconds{1};
    constexpr auto medium_leak_threshold = std::chrono::seconds{2};
    
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
        std::uniform_int_distribution<std::size_t> dist(1, std::min(max_blocks, std::size_t{50}));
        return dist(gen);
    }
    
    // Generate random allocation size
    auto random_allocation_size(std::size_t block_size) -> std::size_t {
        std::uniform_int_distribution<std::size_t> dist(1, block_size);
        return dist(gen);
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any allocation that exceeds the configured leak threshold,
 * the leak detection mechanism should identify it with accurate information
 * including address, size, age, and allocation context.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_threshold_accuracy, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        // Generate random pool configuration
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        // Ensure pool size is multiple of block size
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        // Create pool with leak detection enabled
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks with random contexts
        std::vector<void*> allocations;
        std::vector<std::string> contexts;
        
        for (std::size_t i = 0; i < alloc_count; ++i) {
            std::size_t alloc_size = random_allocation_size(block_size);
            std::string context = "property_test_" + std::to_string(i);
            void* ptr = pool.allocate(alloc_size, context);
            
            if (ptr) {
                allocations.push_back(ptr);
                contexts.push_back(context);
            }
        }
        
        // Wait for leak threshold to pass
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Detect leaks
        auto leaks = pool.detect_leaks();
        
        // Property: All allocations should be detected as leaks
        BOOST_CHECK_EQUAL(leaks.size(), allocations.size());
        
        // Property: Each leak should have accurate information
        for (const auto& leak : leaks) {
            // Address should be valid and in our allocations
            BOOST_CHECK(leak.address != nullptr);
            BOOST_CHECK(std::find(allocations.begin(), allocations.end(), leak.address) != allocations.end());
            
            // Size should be positive
            BOOST_CHECK_GT(leak.size, 0);
            BOOST_CHECK_LE(leak.size, block_size);
            
            // Age should be at least the threshold
            BOOST_CHECK_GE(leak.age, short_leak_threshold);
            
            // Context should be captured
            BOOST_CHECK(!leak.allocation_context.empty());
            
            // Thread ID should be captured
            BOOST_CHECK(!leak.thread_id.empty());
            BOOST_CHECK(leak.thread_id != "unknown");
        }
        
        // Clean up
        for (void* ptr : allocations) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any allocation pattern with mixed short-lived and long-lived
 * allocations, leak detection should only identify allocations that exceed
 * the threshold, not short-lived allocations.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_selective_identification, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, medium_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate long-lived blocks
        std::vector<void*> long_lived;
        for (std::size_t i = 0; i < alloc_count / 2; ++i) {
            void* ptr = pool.allocate(block_size / 2, "long_lived");
            if (ptr) {
                long_lived.push_back(ptr);
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(medium_leak_threshold + std::chrono::milliseconds{200});
        
        // Allocate short-lived blocks
        std::vector<void*> short_lived;
        for (std::size_t i = 0; i < alloc_count / 2; ++i) {
            void* ptr = pool.allocate(block_size / 2, "short_lived");
            if (ptr) {
                short_lived.push_back(ptr);
            }
        }
        
        // Detect leaks immediately
        auto leaks = pool.detect_leaks();
        
        // Property: Only long-lived allocations should be detected as leaks
        BOOST_CHECK_EQUAL(leaks.size(), long_lived.size());
        
        // Property: All detected leaks should be from long-lived allocations
        for (const auto& leak : leaks) {
            BOOST_CHECK(std::find(long_lived.begin(), long_lived.end(), leak.address) != long_lived.end());
            BOOST_CHECK(std::find(short_lived.begin(), short_lived.end(), leak.address) == short_lived.end());
            BOOST_CHECK(leak.allocation_context.find("long_lived") != std::string::npos);
        }
        
        // Clean up
        for (void* ptr : long_lived) {
            pool.deallocate(ptr);
        }
        for (void* ptr : short_lived) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any concurrent allocation pattern across multiple threads,
 * leak detection should accurately identify leaks from all threads with
 * correct thread ID attribution.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_multithreaded_accuracy, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 512 * 1024; // Fixed size for faster test
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        
        constexpr int num_threads = 4;
        std::vector<std::thread> threads;
        std::vector<std::vector<void*>> thread_allocations(num_threads);
        std::atomic<std::size_t> total_allocations{0};
        
        // Each thread allocates blocks
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                std::string context = "thread_" + std::to_string(t);
                for (int i = 0; i < 5; ++i) {
                    void* ptr = pool.allocate(block_size / 2, context);
                    if (ptr) {
                        thread_allocations[t].push_back(ptr);
                        total_allocations++;
                    }
                }
            });
        }
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Detect leaks
        auto leaks = pool.detect_leaks();
        
        // Property: All allocations from all threads should be detected
        BOOST_CHECK_EQUAL(leaks.size(), total_allocations.load());
        
        // Property: Each leak should have a valid thread ID
        for (const auto& leak : leaks) {
            BOOST_CHECK(!leak.thread_id.empty());
            BOOST_CHECK(leak.thread_id != "unknown");
            
            // Context should indicate which thread allocated it
            BOOST_CHECK(leak.allocation_context.find("thread_") != std::string::npos);
        }
        
        // Property: Leaks should be distributed across threads
        std::set<std::string> unique_thread_ids;
        for (const auto& leak : leaks) {
            unique_thread_ids.insert(leak.thread_id);
        }
        BOOST_CHECK_GT(unique_thread_ids.size(), 1); // At least 2 different threads
        
        // Clean up
        for (auto& thread_ptrs : thread_allocations) {
            for (void* ptr : thread_ptrs) {
                pool.deallocate(ptr);
            }
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any sequence of allocations and deallocations, leak detection
 * should correctly remove deallocated blocks from leak reports and only report
 * currently allocated blocks that exceed the threshold.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_deallocation_tracking, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2, "test_allocation");
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Detect leaks - should find all allocations
        auto leaks_before = pool.detect_leaks();
        BOOST_CHECK_EQUAL(leaks_before.size(), allocations.size());
        
        // Deallocate half the blocks
        std::size_t dealloc_count = allocations.size() / 2;
        for (std::size_t i = 0; i < dealloc_count; ++i) {
            pool.deallocate(allocations[i]);
            allocations[i] = nullptr;
        }
        
        // Detect leaks again - should only find remaining allocations
        auto leaks_after = pool.detect_leaks();
        
        // Property: Leak count should decrease by deallocation count
        BOOST_CHECK_EQUAL(leaks_after.size(), allocations.size() - dealloc_count);
        
        // Property: Deallocated addresses should not appear in leak report
        for (std::size_t i = 0; i < dealloc_count; ++i) {
            for (const auto& leak : leaks_after) {
                BOOST_CHECK(leak.address != allocations[i]);
            }
        }
        
        // Property: Remaining allocations should still be in leak report
        for (std::size_t i = dealloc_count; i < allocations.size(); ++i) {
            if (allocations[i] != nullptr) {
                bool found = false;
                for (const auto& leak : leaks_after) {
                    if (leak.address == allocations[i]) {
                        found = true;
                        break;
                    }
                }
                BOOST_CHECK(found);
            }
        }
        
        // Clean up remaining allocations
        for (std::size_t i = dealloc_count; i < allocations.size(); ++i) {
            if (allocations[i]) {
                pool.deallocate(allocations[i]);
            }
        }
        
        // Property: After all deallocations, no leaks should be detected
        auto leaks_final = pool.detect_leaks();
        BOOST_CHECK_EQUAL(leaks_final.size(), 0);
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any leak threshold configuration, leak detection should
 * accurately respect the threshold and only report allocations that have
 * exceeded the configured duration.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_threshold_configuration, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        // Generate random threshold between 1 and 3 seconds
        std::uniform_int_distribution<int> threshold_dist(1, 3);
        auto threshold = std::chrono::seconds{threshold_dist(gen)};
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2, "threshold_test");
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Wait less than threshold - should not detect leaks
        auto wait_time = threshold - std::chrono::milliseconds{500};
        if (wait_time.count() > 0) {
            std::this_thread::sleep_for(wait_time);
            auto leaks_before = pool.detect_leaks();
            
            // Property: No leaks should be detected before threshold
            BOOST_CHECK_EQUAL(leaks_before.size(), 0);
        }
        
        // Wait past threshold
        std::this_thread::sleep_for(std::chrono::seconds{1});
        auto leaks_after = pool.detect_leaks();
        
        // Property: All allocations should be detected after threshold
        BOOST_CHECK_EQUAL(leaks_after.size(), allocations.size());
        
        // Property: All detected leaks should have age >= threshold
        for (const auto& leak : leaks_after) {
            BOOST_CHECK_GE(leak.age, threshold);
        }
        
        // Clean up
        for (void* ptr : allocations) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any allocation pattern, enabling or disabling leak detection
 * should not affect the correctness of allocation/deallocation operations,
 * and leak detection should provide meaningful information in both modes.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_mode_independence, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Test with leak detection enabled
        memory_pool pool_enabled(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::vector<void*> allocs_enabled;
        
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool_enabled.allocate(block_size / 2, "enabled_test");
            if (ptr) {
                allocs_enabled.push_back(ptr);
            }
        }
        
        // Test with leak detection disabled
        memory_pool pool_disabled(pool_size, block_size, std::chrono::seconds{0}, false, short_leak_threshold);
        std::vector<void*> allocs_disabled;
        
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool_disabled.allocate(block_size / 2);
            if (ptr) {
                allocs_disabled.push_back(ptr);
            }
        }
        
        // Property: Both pools should allocate the same number of blocks
        BOOST_CHECK_EQUAL(allocs_enabled.size(), allocs_disabled.size());
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Detect leaks in both pools
        auto leaks_enabled = pool_enabled.detect_leaks();
        auto leaks_disabled = pool_disabled.detect_leaks();
        
        // Property: Both should detect the same number of leaks
        BOOST_CHECK_EQUAL(leaks_enabled.size(), allocs_enabled.size());
        BOOST_CHECK_EQUAL(leaks_disabled.size(), allocs_disabled.size());
        
        // Property: Enabled mode should provide detailed context
        for (const auto& leak : leaks_enabled) {
            BOOST_CHECK(leak.allocation_context.find("enabled_test") != std::string::npos);
            BOOST_CHECK(!leak.thread_id.empty());
            BOOST_CHECK(leak.thread_id != "unknown");
        }
        
        // Property: Disabled mode should provide basic information
        for (const auto& leak : leaks_disabled) {
            BOOST_CHECK(leak.address != nullptr);
            BOOST_CHECK_GT(leak.size, 0);
            BOOST_CHECK_GE(leak.age, short_leak_threshold);
            // Context should indicate leak detection is disabled
            BOOST_CHECK(leak.allocation_context.find("enable leak detection") != std::string::npos);
            BOOST_CHECK_EQUAL(leak.thread_id, "unknown");
        }
        
        // Property: Deallocation should work correctly in both modes
        for (void* ptr : allocs_enabled) {
            pool_enabled.deallocate(ptr);
        }
        for (void* ptr : allocs_disabled) {
            pool_disabled.deallocate(ptr);
        }
        
        // Property: After deallocation, no leaks should be detected in either mode
        auto final_leaks_enabled = pool_enabled.detect_leaks();
        auto final_leaks_disabled = pool_disabled.detect_leaks();
        
        BOOST_CHECK_EQUAL(final_leaks_enabled.size(), 0);
        BOOST_CHECK_EQUAL(final_leaks_disabled.size(), 0);
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any extended operation sequence with many allocation/deallocation
 * cycles, leak detection should prevent memory leaks by enabling early identification
 * and cleanup of long-lived allocations.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_prevention_through_detection, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024; // Fixed size for faster test
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        
        std::vector<void*> allocations;
        std::size_t leak_prevention_count = 0;
        
        // Simulate extended operation with periodic leak detection
        for (int cycle = 0; cycle < 10; ++cycle) {
            // Allocate some blocks
            std::size_t alloc_count = std::min(max_blocks / 4, std::size_t{10});
            for (std::size_t i = 0; i < alloc_count; ++i) {
                void* ptr = pool.allocate(block_size / 2, "cycle_" + std::to_string(cycle));
                if (ptr) {
                    allocations.push_back(ptr);
                }
            }
            
            // Wait for leak threshold
            std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
            
            // Detect and prevent leaks
            auto leaks = pool.detect_leaks();
            
            // Property: Leaks should be detected
            BOOST_CHECK_GT(leaks.size(), 0);
            
            // Prevent leaks by deallocating detected allocations
            for (const auto& leak : leaks) {
                pool.deallocate(leak.address);
                leak_prevention_count++;
                
                // Remove from allocations vector
                auto it = std::find(allocations.begin(), allocations.end(), leak.address);
                if (it != allocations.end()) {
                    *it = nullptr;
                }
            }
            
            // Property: After cleanup, no leaks should remain
            auto leaks_after_cleanup = pool.detect_leaks();
            BOOST_CHECK_EQUAL(leaks_after_cleanup.size(), 0);
        }
        
        // Property: Leak prevention should have cleaned up allocations
        BOOST_CHECK_GT(leak_prevention_count, 0);
        
        // Property: Pool should be in clean state
        auto final_metrics = pool.get_metrics();
        BOOST_CHECK_EQUAL(final_metrics.allocated_size, 0);
        
        // Clean up any remaining allocations
        for (void* ptr : allocations) {
            if (ptr) {
                pool.deallocate(ptr);
            }
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any allocation pattern with varying allocation sizes,
 * leak detection should accurately track and report the size of each
 * leaked allocation.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_size_accuracy, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks with varying sizes
        std::vector<std::pair<void*, std::size_t>> allocations; // ptr, requested_size
        
        for (std::size_t i = 0; i < alloc_count; ++i) {
            std::size_t requested_size = random_allocation_size(block_size);
            void* ptr = pool.allocate(requested_size, "size_test");
            
            if (ptr) {
                allocations.push_back({ptr, requested_size});
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Detect leaks
        auto leaks = pool.detect_leaks();
        
        // Property: All allocations should be detected
        BOOST_CHECK_EQUAL(leaks.size(), allocations.size());
        
        // Property: Each leak should report the correct size
        for (const auto& leak : leaks) {
            // Find the corresponding allocation
            auto it = std::find_if(allocations.begin(), allocations.end(),
                [&leak](const auto& alloc) { return alloc.first == leak.address; });
            
            BOOST_CHECK(it != allocations.end());
            
            if (it != allocations.end()) {
                // Size should match the requested size
                BOOST_CHECK_EQUAL(leak.size, it->second);
            }
        }
        
        // Clean up
        for (const auto& [ptr, size] : allocations) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any concurrent leak detection operations across multiple threads,
 * the leak detection mechanism should be thread-safe and provide consistent
 * results without data races or corruption.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_thread_safety, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 512 * 1024;
        std::size_t block_size = 4096;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        
        // Allocate some blocks that will become leaks
        std::vector<void*> allocations;
        for (int i = 0; i < 20; ++i) {
            void* ptr = pool.allocate(block_size / 2, "concurrent_test");
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Multiple threads detecting leaks concurrently
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> detection_count{0};
        std::atomic<bool> consistency_violation{false};
        std::vector<std::thread> threads;
        
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                while (!stop) {
                    auto leaks = pool.detect_leaks();
                    detection_count++;
                    
                    // Property: Leak count should be consistent
                    if (leaks.size() != allocations.size()) {
                        consistency_violation = true;
                    }
                    
                    // Property: Each leak should have valid information
                    for (const auto& leak : leaks) {
                        if (leak.address == nullptr ||
                            leak.size == 0 ||
                            leak.age < short_leak_threshold ||
                            leak.allocation_context.empty() ||
                            leak.thread_id.empty()) {
                            consistency_violation = true;
                        }
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
        }
        
        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Property: No consistency violations should occur
        BOOST_CHECK(!consistency_violation);
        
        // Property: Multiple detections should have occurred
        BOOST_CHECK_GT(detection_count.load(), 0);
        
        // Clean up
        for (void* ptr : allocations) {
            pool.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any allocation pattern, leak detection should have minimal
 * performance impact when disabled and acceptable overhead when enabled,
 * while maintaining correctness in both modes.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_performance_impact, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations / 10; ++iteration) {
        std::size_t pool_size = 256 * 1024;
        std::size_t block_size = 4096;
        constexpr int operations = 1000;
        
        // Measure performance without leak detection
        memory_pool pool_disabled(pool_size, block_size, std::chrono::seconds{0}, false);
        
        auto start_disabled = std::chrono::steady_clock::now();
        for (int i = 0; i < operations; ++i) {
            void* ptr = pool_disabled.allocate(block_size / 2);
            if (ptr) {
                pool_disabled.deallocate(ptr);
            }
        }
        auto end_disabled = std::chrono::steady_clock::now();
        auto duration_disabled = std::chrono::duration_cast<std::chrono::microseconds>(
            end_disabled - start_disabled);
        
        // Measure performance with leak detection
        memory_pool pool_enabled(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        
        auto start_enabled = std::chrono::steady_clock::now();
        for (int i = 0; i < operations; ++i) {
            void* ptr = pool_enabled.allocate(block_size / 2, "perf_test");
            if (ptr) {
                pool_enabled.deallocate(ptr);
            }
        }
        auto end_enabled = std::chrono::steady_clock::now();
        auto duration_enabled = std::chrono::duration_cast<std::chrono::microseconds>(
            end_enabled - start_enabled);
        
        // Property: Both modes should complete successfully
        BOOST_CHECK_GT(duration_disabled.count(), 0);
        BOOST_CHECK_GT(duration_enabled.count(), 0);
        
        // Property: Overhead should be reasonable (less than 10x)
        double overhead_ratio = static_cast<double>(duration_enabled.count()) / 
                               duration_disabled.count();
        BOOST_CHECK_LT(overhead_ratio, 10.0);
        
        // Property: Leak detection should still work correctly after performance test
        std::vector<void*> test_allocs;
        for (int i = 0; i < 5; ++i) {
            void* ptr = pool_enabled.allocate(block_size / 2, "final_test");
            if (ptr) {
                test_allocs.push_back(ptr);
            }
        }
        
        // Wait for leak threshold before detecting (need to exceed the threshold)
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{500});
        auto leaks = pool_enabled.detect_leaks();
        
        BOOST_CHECK_EQUAL(leaks.size(), test_allocs.size());
        
        // Clean up
        for (void* ptr : test_allocs) {
            pool_enabled.deallocate(ptr);
        }
    }
}

/**
 * **Feature: coap-transport, Property 40: Memory leak detection**
 * 
 * Property: For any pool reset operation, leak detection should correctly
 * handle the reset and not report false positives for allocations that
 * were cleared by the reset.
 * 
 * **Validates: Requirements 14.4**
 */
BOOST_AUTO_TEST_CASE(property_leak_detection_after_reset, * boost::unit_test::timeout(test_timeout_seconds)) {
    for (std::size_t iteration = 0; iteration < num_property_iterations; ++iteration) {
        std::size_t pool_size = random_pool_size();
        std::size_t block_size = random_block_size();
        
        pool_size = (pool_size / block_size) * block_size;
        if (pool_size == 0) continue;
        
        memory_pool pool(pool_size, block_size, std::chrono::seconds{0}, true, short_leak_threshold);
        std::size_t max_blocks = pool_size / block_size;
        std::size_t alloc_count = random_allocation_count(max_blocks);
        
        // Allocate blocks
        std::vector<void*> allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2, "pre_reset");
            if (ptr) {
                allocations.push_back(ptr);
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Verify leaks are detected before reset
        auto leaks_before = pool.detect_leaks();
        BOOST_CHECK_EQUAL(leaks_before.size(), allocations.size());
        
        // Reset the pool
        pool.reset();
        
        // Property: After reset, no leaks should be detected
        auto leaks_after_reset = pool.detect_leaks();
        BOOST_CHECK_EQUAL(leaks_after_reset.size(), 0);
        
        // Allocate new blocks after reset
        std::vector<void*> new_allocations;
        for (std::size_t i = 0; i < alloc_count; ++i) {
            void* ptr = pool.allocate(block_size / 2, "post_reset");
            if (ptr) {
                new_allocations.push_back(ptr);
            }
        }
        
        // Wait for leak threshold
        std::this_thread::sleep_for(short_leak_threshold + std::chrono::milliseconds{200});
        
        // Property: New allocations should be detected as leaks
        auto leaks_new = pool.detect_leaks();
        BOOST_CHECK_EQUAL(leaks_new.size(), new_allocations.size());
        
        // Property: All detected leaks should be from post-reset allocations
        for (const auto& leak : leaks_new) {
            BOOST_CHECK(leak.allocation_context.find("post_reset") != std::string::npos);
        }
        
        // Clean up
        for (void* ptr : new_allocations) {
            pool.deallocate(ptr);
        }
    }
}
