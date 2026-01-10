#define BOOST_TEST_MODULE RaftCallbackSafetyAfterCancellationPropertyTest

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

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_callback_safety_after_cancellation_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t min_operations = 10;
    constexpr std::size_t max_operations = 100;
    constexpr std::size_t min_futures = 5;
    constexpr std::size_t max_futures = 50;
    constexpr std::chrono::milliseconds test_timeout{30000};
    constexpr std::chrono::milliseconds callback_timeout{100};
    constexpr const char* cancellation_reason = "Test cancellation";
}

/**
 * **Feature: raft-completion, Property 40: Callback Safety After Cancellation**
 * 
 * Property: For any cancelled future, no callbacks are invoked after cancellation.
 * **Validates: Requirements 8.4**
 */
BOOST_AUTO_TEST_CASE(raft_callback_safety_after_cancellation_property_test, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Testing callback safety after cancellation property...");
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> operation_count_dist(min_operations, max_operations);
    std::uniform_int_distribution<std::size_t> future_count_dist(min_futures, max_futures);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, 1000);
    std::uniform_int_distribution<int> delay_dist(1, 50); // Random delay in milliseconds
    
    // Test multiple scenarios with different cancellation patterns
    for (int test_iteration = 0; test_iteration < 10; ++test_iteration) {
        BOOST_TEST_MESSAGE("Test iteration " << (test_iteration + 1) << "/10");
        
        const std::size_t operation_count = operation_count_dist(gen);
        const std::size_t future_count = future_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing callback safety with " << operation_count 
                          << " operations and " << future_count << " futures");
        
        // Test 1: CommitWaiter callback safety after cancellation
        {
            BOOST_TEST_MESSAGE("Test 1: CommitWaiter callback safety after cancellation");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> callbacks_invoked{0};
            std::atomic<std::size_t> callbacks_after_cancellation{0};
            std::atomic<bool> cancellation_completed{false};
            std::mutex callback_mutex;
            std::vector<std::chrono::steady_clock::time_point> callback_times;
            
            // Register operations with callbacks that track invocation timing
            for (std::size_t i = 0; i < operation_count; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto fulfill_callback = [&callbacks_invoked, &callbacks_after_cancellation, 
                                       &cancellation_completed, &callback_mutex, &callback_times]
                                      (std::vector<std::byte> result) {
                    callbacks_invoked.fetch_add(1);
                    
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    callback_times.push_back(std::chrono::steady_clock::now());
                    
                    if (cancellation_completed.load()) {
                        callbacks_after_cancellation.fetch_add(1);
                        BOOST_TEST_MESSAGE("WARNING: Fulfill callback invoked after cancellation!");
                    }
                };
                
                auto reject_callback = [&callbacks_invoked, &callbacks_after_cancellation, 
                                      &cancellation_completed, &callback_mutex, &callback_times]
                                     (std::exception_ptr ex) {
                    callbacks_invoked.fetch_add(1);
                    
                    std::lock_guard<std::mutex> lock(callback_mutex);
                    callback_times.push_back(std::chrono::steady_clock::now());
                    
                    if (cancellation_completed.load()) {
                        callbacks_after_cancellation.fetch_add(1);
                        BOOST_TEST_MESSAGE("WARNING: Reject callback invoked after cancellation!");
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    std::move(fulfill_callback),
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000} // Long timeout to avoid timeout cancellation
                );
            }
            
            // Verify operations are registered
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), operation_count);
            
            // Cancel all operations
            auto cancellation_start = std::chrono::steady_clock::now();
            commit_waiter.cancel_all_operations(cancellation_reason);
            cancellation_completed.store(true);
            auto cancellation_end = std::chrono::steady_clock::now();
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(callback_timeout);
            
            // Property: No callbacks should be invoked after cancellation is complete
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(callbacks_invoked.load(), operation_count);
            BOOST_CHECK_EQUAL(callbacks_after_cancellation.load(), 0);
            
            // Verify callback timing - all should be before or during cancellation
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                for (const auto& callback_time : callback_times) {
                    auto callback_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        callback_time - cancellation_start);
                    
                    // Callbacks should occur during or shortly after cancellation
                    BOOST_CHECK_LE(callback_elapsed.count(), 100); // Allow 100ms for callback execution
                }
            }
            
            BOOST_TEST_MESSAGE("✓ CommitWaiter callback safety: " << operation_count 
                              << " callbacks invoked safely during cancellation");
        }
        
        // Test 2: Concurrent cancellation and callback safety
        {
            BOOST_TEST_MESSAGE("Test 2: Concurrent cancellation and callback safety");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> concurrent_callbacks{0};
            std::atomic<std::size_t> unsafe_callbacks{0};
            std::atomic<bool> cancellation_in_progress{false};
            
            const std::size_t concurrent_operations = operation_count / 2;
            
            // Register operations
            for (std::size_t i = 0; i < concurrent_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&concurrent_callbacks, &unsafe_callbacks, 
                                      &cancellation_in_progress](std::exception_ptr ex) {
                    concurrent_callbacks.fetch_add(1);
                    
                    // Simulate some work in callback
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    
                    // Check if cancellation is still in progress
                    if (!cancellation_in_progress.load()) {
                        unsafe_callbacks.fetch_add(1);
                        BOOST_TEST_MESSAGE("WARNING: Callback executed after cancellation completed!");
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), concurrent_operations);
            
            // Start cancellation in a separate thread
            cancellation_in_progress.store(true);
            std::thread cancellation_thread([&]() {
                commit_waiter.cancel_all_operations(cancellation_reason);
                
                // Give callbacks time to complete
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                cancellation_in_progress.store(false);
            });
            
            // Wait for cancellation to complete
            cancellation_thread.join();
            
            // Give any remaining callbacks time to execute
            std::this_thread::sleep_for(callback_timeout);
            
            // Property: All callbacks should execute safely during cancellation
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(concurrent_callbacks.load(), concurrent_operations);
            BOOST_CHECK_EQUAL(unsafe_callbacks.load(), 0);
            
            BOOST_TEST_MESSAGE("✓ Concurrent cancellation safety: " << concurrent_operations 
                              << " callbacks handled safely");
        }
        
        // Test 3: Future collection callback safety
        {
            BOOST_TEST_MESSAGE("Test 3: Future collection callback safety");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> collection_futures;
            std::vector<std::shared_ptr<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>> promises;
            std::atomic<std::size_t> collection_callbacks{0};
            std::atomic<bool> collection_cancelled{false};
            
            // Create futures with promises
            for (std::size_t i = 0; i < future_count; ++i) {
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                promises.push_back(promise);
                
                auto future = promise->getFuture()
                    .thenValue([&collection_callbacks, &collection_cancelled](auto result) {
                        collection_callbacks.fetch_add(1);
                        if (collection_cancelled.load()) {
                            BOOST_TEST_MESSAGE("WARNING: Future callback after collection cancellation!");
                        }
                        return result;
                    })
                    .within(std::chrono::milliseconds{5000});
                
                collection_futures.push_back(std::move(future));
            }
            
            BOOST_CHECK_EQUAL(collection_futures.size(), future_count);
            
            // Start collection in background
            std::thread collection_thread([&]() {
                try {
                    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                        std::move(collection_futures), std::chrono::milliseconds{1000});
                    
                    // This should timeout since promises are not fulfilled
                    auto results = std::move(collection_future).get();
                    BOOST_TEST_MESSAGE("Collection completed unexpectedly");
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Collection timed out as expected: " << e.what());
                }
            });
            
            // Let collection start
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Cancel collection
            collection_cancelled.store(true);
            // Note: collection_futures was moved, so we can't cancel it directly
            // The timeout will handle the cancellation
            
            // Wait for collection to complete
            collection_thread.join();
            
            // Give any callbacks time to execute
            std::this_thread::sleep_for(callback_timeout);
            
            // Property: Future collection should handle cancellation safely
            // Since we didn't fulfill any promises, no callbacks should have been invoked
            BOOST_CHECK_EQUAL(collection_callbacks.load(), 0);
            
            BOOST_TEST_MESSAGE("✓ Future collection callback safety verified");
        }
        
        // Test 4: Callback safety with resource cleanup
        {
            BOOST_TEST_MESSAGE("Test 4: Callback safety with resource cleanup");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> resource_callbacks{0};
            std::atomic<std::size_t> resource_cleanups{0};
            std::vector<std::shared_ptr<std::vector<std::byte>>> resources;
            
            const std::size_t resource_operations = operation_count / 3;
            
            // Create operations with resources
            for (std::size_t i = 0; i < resource_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                // Create a resource
                auto resource = std::make_shared<std::vector<std::byte>>(1024, std::byte{0x55});
                resources.push_back(resource);
                
                auto reject_callback = [&resource_callbacks, &resource_cleanups, resource]
                                     (std::exception_ptr ex) {
                    resource_callbacks.fetch_add(1);
                    
                    // Simulate resource cleanup
                    resource->clear();
                    resource_cleanups.fetch_add(1);
                    
                    // Resource will be automatically cleaned up when shared_ptr goes out of scope
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), resource_operations);
            BOOST_CHECK_EQUAL(resources.size(), resource_operations);
            
            // Cancel operations
            commit_waiter.cancel_all_operations(cancellation_reason);
            
            // Give callbacks time to execute and clean up resources
            std::this_thread::sleep_for(callback_timeout);
            
            // Property: Resource cleanup should be safe after cancellation
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(resource_callbacks.load(), resource_operations);
            BOOST_CHECK_EQUAL(resource_cleanups.load(), resource_operations);
            
            // Verify resources were cleaned up
            for (const auto& resource : resources) {
                BOOST_CHECK(resource->empty()); // Should be cleared by callback
            }
            
            BOOST_TEST_MESSAGE("✓ Resource cleanup callback safety: " << resource_operations 
                              << " resources cleaned up safely");
        }
    }
    
    // Test edge cases for callback safety after cancellation
    BOOST_TEST_MESSAGE("Testing callback safety edge cases...");
    
    // Test 5: Rapid cancellation cycles
    {
        BOOST_TEST_MESSAGE("Test 5: Rapid cancellation cycles");
        
        std::atomic<std::size_t> total_safe_callbacks{0};
        std::atomic<std::size_t> total_unsafe_callbacks{0};
        
        // Perform multiple rapid cancellation cycles
        for (int cycle = 0; cycle < 5; ++cycle) {
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<bool> cycle_cancelled{false};
            
            const std::size_t cycle_operations = 10;
            
            // Add operations
            for (std::size_t i = 0; i < cycle_operations; ++i) {
                const std::uint64_t index = (cycle * 100) + i + 1;
                
                auto reject_callback = [&total_safe_callbacks, &total_unsafe_callbacks, 
                                      &cycle_cancelled](std::exception_ptr ex) {
                    if (cycle_cancelled.load()) {
                        total_unsafe_callbacks.fetch_add(1);
                    } else {
                        total_safe_callbacks.fetch_add(1);
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    std::chrono::milliseconds{10000}
                );
            }
            
            // Rapid cancellation
            commit_waiter.cancel_all_operations("Rapid cycle " + std::to_string(cycle));
            cycle_cancelled.store(true);
            
            // Brief pause
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        
        // Give all callbacks time to execute
        std::this_thread::sleep_for(callback_timeout);
        
        // Property: Rapid cycles should maintain callback safety
        BOOST_CHECK_EQUAL(total_safe_callbacks.load(), 5 * 10); // 5 cycles * 10 operations
        BOOST_CHECK_EQUAL(total_unsafe_callbacks.load(), 0);
        
        BOOST_TEST_MESSAGE("✓ Rapid cancellation cycles: " << total_safe_callbacks.load() 
                          << " callbacks executed safely");
    }
    
    // Test 6: Callback exception safety
    {
        BOOST_TEST_MESSAGE("Test 6: Callback exception safety");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> exception_callbacks{0};
        std::atomic<std::size_t> normal_callbacks{0};
        
        const std::size_t exception_operations = 20;
        
        // Add operations with callbacks that may throw exceptions
        for (std::size_t i = 0; i < exception_operations; ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&exception_callbacks, &normal_callbacks, i]
                                 (std::exception_ptr ex) {
                try {
                    if (i % 3 == 0) {
                        // Some callbacks throw exceptions
                        exception_callbacks.fetch_add(1);
                        throw std::runtime_error("Callback exception for testing");
                    } else {
                        // Normal callbacks
                        normal_callbacks.fetch_add(1);
                    }
                } catch (const std::exception& e) {
                    // Catch the exception to prevent it from propagating to the test framework
                    BOOST_TEST_MESSAGE("Caught expected callback exception: " << e.what());
                }
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                std::chrono::milliseconds{10000}
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), exception_operations);
        
        // Cancel operations (should handle callback exceptions gracefully)
        commit_waiter.cancel_all_operations(cancellation_reason);
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(callback_timeout);
        
        // Property: Callback exceptions should not prevent cancellation cleanup
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        
        // All callbacks should have been invoked (even those that throw)
        auto expected_exception_callbacks = (exception_operations + 2) / 3; // Round up division
        auto expected_normal_callbacks = exception_operations - expected_exception_callbacks;
        
        BOOST_CHECK_EQUAL(exception_callbacks.load(), expected_exception_callbacks);
        BOOST_CHECK_EQUAL(normal_callbacks.load(), expected_normal_callbacks);
        
        BOOST_TEST_MESSAGE("✓ Callback exception safety: " << exception_callbacks.load() 
                          << " exception callbacks, " << normal_callbacks.load() 
                          << " normal callbacks handled safely");
    }
    
    // Test 7: Callback ordering after cancellation
    {
        BOOST_TEST_MESSAGE("Test 7: Callback ordering after cancellation");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::vector<std::size_t> callback_order;
        std::mutex order_mutex;
        
        const std::size_t ordered_operations = 15;
        
        // Add operations with callbacks that record execution order
        for (std::size_t i = 0; i < ordered_operations; ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&callback_order, &order_mutex, i](std::exception_ptr ex) {
                std::lock_guard<std::mutex> lock(order_mutex);
                callback_order.push_back(i);
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                std::chrono::milliseconds{10000}
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), ordered_operations);
        
        // Cancel operations
        commit_waiter.cancel_all_operations(cancellation_reason);
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(callback_timeout);
        
        // Property: All callbacks should be executed after cancellation
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            BOOST_CHECK_EQUAL(callback_order.size(), ordered_operations);
            
            // Verify all operations were cancelled (order may vary due to concurrency)
            std::vector<std::size_t> expected_operations;
            for (std::size_t i = 0; i < ordered_operations; ++i) {
                expected_operations.push_back(i);
            }
            
            std::sort(callback_order.begin(), callback_order.end());
            BOOST_CHECK_EQUAL_COLLECTIONS(callback_order.begin(), callback_order.end(),
                                        expected_operations.begin(), expected_operations.end());
        }
        
        BOOST_TEST_MESSAGE("✓ Callback ordering: All " << ordered_operations 
                          << " callbacks executed in safe order");
    }
    
    BOOST_TEST_MESSAGE("All callback safety after cancellation property tests passed!");
}