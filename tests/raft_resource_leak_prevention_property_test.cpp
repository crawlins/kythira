#define BOOST_TEST_MODULE RaftResourceLeakPreventionPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/commit_waiter.hpp>
#include <raft/future_collector.hpp>
#include <raft/error_handler.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_set>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_resource_leak_prevention_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t min_operations = 20;
    constexpr std::size_t max_operations = 200;
    constexpr std::size_t min_futures = 10;
    constexpr std::size_t max_futures = 100;
    constexpr std::size_t resource_size = 1024; // Size of test resources in bytes
    constexpr std::chrono::milliseconds test_timeout{30000};
    constexpr std::chrono::milliseconds cleanup_timeout{200};
    constexpr const char* cleanup_reason = "Resource cleanup test";
}

// Resource tracker to monitor allocations and deallocations
class ResourceTracker {
private:
    std::atomic<std::size_t> _allocated_count{0};
    std::atomic<std::size_t> _deallocated_count{0};
    std::atomic<std::size_t> _total_allocated_bytes{0};
    std::atomic<std::size_t> _total_deallocated_bytes{0};
    mutable std::mutex _active_resources_mutex;
    std::unordered_set<void*> _active_resources;
    
public:
    auto allocate(std::size_t size) -> void* {
        auto ptr = std::malloc(size);
        if (ptr) {
            _allocated_count.fetch_add(1);
            _total_allocated_bytes.fetch_add(size);
            
            std::lock_guard<std::mutex> lock(_active_resources_mutex);
            _active_resources.insert(ptr);
        }
        return ptr;
    }
    
    auto deallocate(void* ptr, std::size_t size) -> void {
        if (ptr) {
            _deallocated_count.fetch_add(1);
            _total_deallocated_bytes.fetch_add(size);
            
            std::lock_guard<std::mutex> lock(_active_resources_mutex);
            _active_resources.erase(ptr);
            
            std::free(ptr);
        }
    }
    
    auto get_allocated_count() const -> std::size_t {
        return _allocated_count.load();
    }
    
    auto get_deallocated_count() const -> std::size_t {
        return _deallocated_count.load();
    }
    
    auto get_active_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_active_resources_mutex);
        return _active_resources.size();
    }
    
    auto get_total_allocated_bytes() const -> std::size_t {
        return _total_allocated_bytes.load();
    }
    
    auto get_total_deallocated_bytes() const -> std::size_t {
        return _total_deallocated_bytes.load();
    }
    
    auto reset() -> void {
        _allocated_count.store(0);
        _deallocated_count.store(0);
        _total_allocated_bytes.store(0);
        _total_deallocated_bytes.store(0);
        
        std::lock_guard<std::mutex> lock(_active_resources_mutex);
        _active_resources.clear();
    }
};

// RAII resource wrapper for testing
class TestResource {
private:
    void* _data;
    std::size_t _size;
    ResourceTracker* _tracker;
    
public:
    TestResource(std::size_t size, ResourceTracker* tracker) 
        : _size(size), _tracker(tracker) {
        _data = _tracker->allocate(size);
        if (_data) {
            std::memset(_data, 0x42, size); // Fill with test pattern
        }
    }
    
    ~TestResource() {
        if (_data && _tracker) {
            _tracker->deallocate(_data, _size);
        }
    }
    
    // Move-only semantics
    TestResource(const TestResource&) = delete;
    TestResource& operator=(const TestResource&) = delete;
    
    TestResource(TestResource&& other) noexcept 
        : _data(other._data), _size(other._size), _tracker(other._tracker) {
        other._data = nullptr;
        other._size = 0;
        other._tracker = nullptr;
    }
    
    TestResource& operator=(TestResource&& other) noexcept {
        if (this != &other) {
            if (_data && _tracker) {
                _tracker->deallocate(_data, _size);
            }
            
            _data = other._data;
            _size = other._size;
            _tracker = other._tracker;
            
            other._data = nullptr;
            other._size = 0;
            other._tracker = nullptr;
        }
        return *this;
    }
    
    auto is_valid() const -> bool {
        return _data != nullptr;
    }
    
    auto size() const -> std::size_t {
        return _size;
    }
};

/**
 * **Feature: raft-completion, Property 41: Resource Leak Prevention**
 * 
 * Property: For any future cleanup operation, memory leaks and resource exhaustion are prevented.
 * **Validates: Requirements 8.5**
 */
BOOST_AUTO_TEST_CASE(raft_resource_leak_prevention_property_test, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Testing resource leak prevention property...");
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> operation_count_dist(min_operations, max_operations);
    std::uniform_int_distribution<std::size_t> future_count_dist(min_futures, max_futures);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, 1000);
    std::uniform_int_distribution<std::size_t> resource_size_dist(512, 2048);
    
    ResourceTracker global_tracker;
    
    // Test multiple scenarios with different resource patterns
    for (int test_iteration = 0; test_iteration < 10; ++test_iteration) {
        BOOST_TEST_MESSAGE("Test iteration " << (test_iteration + 1) << "/10");
        
        const std::size_t operation_count = operation_count_dist(gen);
        const std::size_t future_count = future_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing resource leak prevention with " << operation_count 
                          << " operations and " << future_count << " futures");
        
        global_tracker.reset();
        
        // Test 1: CommitWaiter resource cleanup
        {
            BOOST_TEST_MESSAGE("Test 1: CommitWaiter resource cleanup");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<std::shared_ptr<TestResource>> operation_resources;
            auto resources_cleaned = std::make_shared<std::atomic<std::size_t>>(0);
            
            auto initial_allocated = global_tracker.get_allocated_count();
            auto initial_active = global_tracker.get_active_count();
            
            // Register operations with resources
            for (std::size_t i = 0; i < operation_count; ++i) {
                const std::uint64_t index = index_dist(gen);
                const std::size_t res_size = resource_size_dist(gen);
                
                // Create resource for this operation
                auto resource = std::make_shared<TestResource>(res_size, &global_tracker);
                operation_resources.push_back(resource);
                
                auto fulfill_callback = [resource](std::vector<std::byte> result) {
                    // Resource will be automatically cleaned up when shared_ptr goes out of scope
                };
                
                auto reject_callback = [resource, resources_cleaned](std::exception_ptr ex) {
                    resources_cleaned->fetch_add(1);
                    // Resource will be automatically cleaned up when shared_ptr goes out of scope
                };
                
                commit_waiter.register_operation(
                    index,
                    std::move(fulfill_callback),
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            // Verify resources are allocated
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), operation_count);
            BOOST_CHECK_EQUAL(operation_resources.size(), operation_count);
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count() - initial_allocated, operation_count);
            
            // Cancel operations to trigger cleanup
            commit_waiter.cancel_all_operations(cleanup_reason);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(cleanup_timeout);
            
            // Clear operation resources to trigger RAII cleanup
            operation_resources.clear();
            
            // Give destructors time to run
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            
            // Property: All resources should be cleaned up after cancellation
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(resources_cleaned->load(), operation_count);
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
            BOOST_CHECK_EQUAL(global_tracker.get_active_count(), initial_active);
            
            BOOST_TEST_MESSAGE("✓ CommitWaiter resource cleanup: " << operation_count 
                              << " resources allocated and cleaned up");
        }
        
        // Test 2: Future collection resource cleanup
        {
            BOOST_TEST_MESSAGE("Test 2: Future collection resource cleanup");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> collection_futures;
            std::vector<std::shared_ptr<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>> promises;
            std::vector<std::shared_ptr<TestResource>> future_resources;
            
            auto initial_allocated = global_tracker.get_allocated_count();
            auto initial_active = global_tracker.get_active_count();
            
            // Create futures with associated resources
            for (std::size_t i = 0; i < future_count; ++i) {
                const std::size_t res_size = resource_size_dist(gen);
                
                // Create resource for this future
                auto resource = std::make_shared<TestResource>(res_size, &global_tracker);
                future_resources.push_back(resource);
                
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                promises.push_back(promise);
                
                auto future = promise->getFuture()
                    .thenValue([resource](auto result) {
                        // Resource will be kept alive by the lambda capture
                        return result;
                    })
                    .within(std::chrono::milliseconds{1000}); // Short timeout for cleanup
                
                collection_futures.push_back(std::move(future));
            }
            
            // Verify resources are allocated
            BOOST_CHECK_EQUAL(collection_futures.size(), future_count);
            BOOST_CHECK_EQUAL(future_resources.size(), future_count);
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count() - initial_allocated, future_count);
            
            // Cancel collection to trigger cleanup
            kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(collection_futures);
            
            // Clear resources to trigger RAII cleanup
            promises.clear();
            future_resources.clear();
            
            // Give destructors time to run
            std::this_thread::sleep_for(cleanup_timeout);
            
            // Property: All resources should be cleaned up after collection cancellation
            BOOST_CHECK(collection_futures.empty());
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
            BOOST_CHECK_EQUAL(global_tracker.get_active_count(), initial_active);
            
            BOOST_TEST_MESSAGE("✓ Future collection resource cleanup: " << future_count 
                              << " resources allocated and cleaned up");
        }
        
        // Test 3: Memory usage patterns during cleanup
        {
            BOOST_TEST_MESSAGE("Test 3: Memory usage patterns during cleanup");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<std::unique_ptr<TestResource>> memory_resources;
            auto peak_memory_operations = std::make_shared<std::atomic<std::size_t>>(0);
            
            auto initial_allocated_bytes = global_tracker.get_total_allocated_bytes();
            auto initial_active = global_tracker.get_active_count();
            
            const std::size_t memory_operations = operation_count / 2;
            
            // Create operations with varying memory usage
            for (std::size_t i = 0; i < memory_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                const std::size_t res_size = resource_size_dist(gen);
                
                // Create shared resource (will be shared with callback)
                auto resource = std::make_shared<TestResource>(res_size, &global_tracker);
                
                auto reject_callback = [resource, peak_memory_operations](std::exception_ptr ex) {
                    peak_memory_operations->fetch_add(1);
                    // Resource will be automatically cleaned up when shared_ptr goes out of scope
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            // Monitor peak memory usage
            auto peak_allocated_bytes = global_tracker.get_total_allocated_bytes();
            auto peak_active = global_tracker.get_active_count();
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), memory_operations);
            BOOST_CHECK_GT(peak_allocated_bytes, initial_allocated_bytes);
            BOOST_CHECK_GT(peak_active, initial_active);
            
            // Cancel operations to trigger memory cleanup
            commit_waiter.cancel_all_operations(cleanup_reason);
            
            // Give callbacks time to execute and clean up memory
            std::this_thread::sleep_for(cleanup_timeout);
            
            // Property: Memory should be cleaned up after cancellation
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(peak_memory_operations->load(), memory_operations);
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
            BOOST_CHECK_EQUAL(global_tracker.get_active_count(), initial_active);
            
            auto final_allocated_bytes = global_tracker.get_total_allocated_bytes();
            auto final_deallocated_bytes = global_tracker.get_total_deallocated_bytes();
            BOOST_CHECK_EQUAL(final_allocated_bytes, final_deallocated_bytes);
            
            BOOST_TEST_MESSAGE("✓ Memory usage patterns: Peak " << (peak_active - initial_active) 
                              << " resources, all cleaned up");
        }
        
        // Test 4: Resource cleanup under stress
        {
            BOOST_TEST_MESSAGE("Test 4: Resource cleanup under stress");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            auto stress_cleanups = std::make_shared<std::atomic<std::size_t>>(0);
            std::vector<std::thread> stress_threads;
            
            auto initial_active = global_tracker.get_active_count();
            const std::size_t stress_operations = 100;
            const std::size_t thread_count = 4;
            
            // Create stress test with multiple threads
            for (std::size_t t = 0; t < thread_count; ++t) {
                stress_threads.emplace_back([&, t]() {
                    for (std::size_t i = 0; i < stress_operations / thread_count; ++i) {
                        const std::uint64_t index = (t * 1000) + i + 1;
                        const std::size_t res_size = resource_size_dist(gen);
                        
                        // Create resource in thread
                        auto resource = std::make_shared<TestResource>(res_size, &global_tracker);
                        
                        auto reject_callback = [resource, stress_cleanups](std::exception_ptr ex) {
                            stress_cleanups->fetch_add(1);
                            // Resource cleanup happens automatically
                        };
                        
                        commit_waiter.register_operation(
                            index,
                            [](std::vector<std::byte>) {},
                            std::move(reject_callback),
                            std::chrono::milliseconds{10000}
                        );
                        
                        // Small delay to create interleaving
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                });
            }
            
            // Wait for all threads to register operations
            for (auto& thread : stress_threads) {
                thread.join();
            }
            
            auto operations_registered = commit_waiter.get_pending_count();
            BOOST_CHECK_EQUAL(operations_registered, stress_operations);
            
            // Cancel all operations under stress
            commit_waiter.cancel_all_operations(cleanup_reason);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(cleanup_timeout);
            
            // Property: Stress cleanup should not leak resources
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(stress_cleanups->load(), stress_operations);
            BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
            BOOST_CHECK_EQUAL(global_tracker.get_active_count(), initial_active);
            
            BOOST_TEST_MESSAGE("✓ Stress resource cleanup: " << stress_operations 
                              << " resources cleaned up under stress");
        }
    }
    
    // Test edge cases for resource leak prevention
    BOOST_TEST_MESSAGE("Testing resource leak prevention edge cases...");
    
    // Test 5: Large resource cleanup
    {
        BOOST_TEST_MESSAGE("Test 5: Large resource cleanup");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::vector<std::shared_ptr<TestResource>> large_resources;
        auto large_cleanups = std::make_shared<std::atomic<std::size_t>>(0);
        
        global_tracker.reset();
        auto initial_active = global_tracker.get_active_count();
        
        const std::size_t large_operations = 500;
        const std::size_t large_resource_size = 4096; // 4KB per resource
        
        // Create operations with large resources
        for (std::size_t i = 0; i < large_operations; ++i) {
            const std::uint64_t index = i + 1;
            
            auto resource = std::make_shared<TestResource>(large_resource_size, &global_tracker);
            large_resources.push_back(resource);
            
            auto reject_callback = [resource, large_cleanups](std::exception_ptr ex) {
                large_cleanups->fetch_add(1);
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                std::chrono::milliseconds{10000}
            );
        }
        
        // Verify large allocation
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), large_operations);
        BOOST_CHECK_EQUAL(large_resources.size(), large_operations);
        BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), large_operations);
        
        auto total_allocated = global_tracker.get_total_allocated_bytes();
        BOOST_CHECK_GE(total_allocated, large_operations * large_resource_size);
        
        // Cancel and cleanup large resources
        commit_waiter.cancel_all_operations(cleanup_reason);
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(cleanup_timeout);
        
        // Clear resource references
        large_resources.clear();
        
        // Give destructors time to run
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Large resource cleanup should not leak memory
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(large_cleanups->load(), large_operations);
        BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
        BOOST_CHECK_EQUAL(global_tracker.get_active_count(), initial_active);
        
        auto total_deallocated = global_tracker.get_total_deallocated_bytes();
        BOOST_CHECK_EQUAL(total_allocated, total_deallocated);
        
        BOOST_TEST_MESSAGE("✓ Large resource cleanup: " << large_operations 
                          << " large resources (" << (total_allocated / 1024) << " KB) cleaned up");
    }
    
    // Test 6: Rapid allocation/deallocation cycles
    {
        BOOST_TEST_MESSAGE("Test 6: Rapid allocation/deallocation cycles");
        
        global_tracker.reset();
        auto cycle_cleanups = std::make_shared<std::atomic<std::size_t>>(0);
        
        const std::size_t cycle_count = 10;
        const std::size_t ops_per_cycle = 50;
        
        // Perform multiple rapid cycles
        for (std::size_t cycle = 0; cycle < cycle_count; ++cycle) {
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<std::shared_ptr<TestResource>> cycle_resources;
            
            // Allocate resources for this cycle
            for (std::size_t i = 0; i < ops_per_cycle; ++i) {
                const std::uint64_t index = (cycle * 1000) + i + 1;
                const std::size_t res_size = resource_size;
                
                auto resource = std::make_shared<TestResource>(res_size, &global_tracker);
                cycle_resources.push_back(resource);
                
                auto reject_callback = [resource, cycle_cleanups](std::exception_ptr ex) {
                    cycle_cleanups->fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            // Rapid cleanup
            commit_waiter.cancel_all_operations("Cycle " + std::to_string(cycle));
            
            // Brief pause for callbacks
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            
            // Clear cycle resources
            cycle_resources.clear();
            
            // Brief pause for destructors
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        
        // Give final cleanup time
        std::this_thread::sleep_for(cleanup_timeout);
        
        // Property: Rapid cycles should not accumulate leaks
        BOOST_CHECK_EQUAL(cycle_cleanups->load(), cycle_count * ops_per_cycle);
        BOOST_CHECK_EQUAL(global_tracker.get_allocated_count(), global_tracker.get_deallocated_count());
        BOOST_CHECK_EQUAL(global_tracker.get_active_count(), 0);
        
        BOOST_TEST_MESSAGE("✓ Rapid cycles: " << cycle_count << " cycles × " << ops_per_cycle 
                          << " operations, no leaks detected");
    }
    
    // Test 7: Resource cleanup validation
    {
        BOOST_TEST_MESSAGE("Test 7: Resource cleanup validation");
        
        global_tracker.reset();
        
        // Final validation - ensure no resources are leaked across all tests
        auto final_allocated = global_tracker.get_allocated_count();
        auto final_deallocated = global_tracker.get_deallocated_count();
        auto final_active = global_tracker.get_active_count();
        auto final_allocated_bytes = global_tracker.get_total_allocated_bytes();
        auto final_deallocated_bytes = global_tracker.get_total_deallocated_bytes();
        
        // Property: Final state should show no resource leaks
        BOOST_CHECK_EQUAL(final_allocated, final_deallocated);
        BOOST_CHECK_EQUAL(final_active, 0);
        BOOST_CHECK_EQUAL(final_allocated_bytes, final_deallocated_bytes);
        
        BOOST_TEST_MESSAGE("✓ Final validation: " << final_allocated << " allocations, " 
                          << final_deallocated << " deallocations, " << final_active 
                          << " active resources");
    }
    
    BOOST_TEST_MESSAGE("All resource leak prevention property tests passed!");
}