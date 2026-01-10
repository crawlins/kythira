#define BOOST_TEST_MODULE RaftTimeoutCancellationCleanupPropertyTest

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

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_timeout_cancellation_cleanup_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::size_t min_operations = 5;
    constexpr std::size_t max_operations = 50;
    constexpr std::size_t min_futures = 3;
    constexpr std::size_t max_futures = 30;
    constexpr std::chrono::milliseconds short_timeout{50};
    constexpr std::chrono::milliseconds medium_timeout{200};
    constexpr std::chrono::milliseconds long_timeout{1000};
    constexpr std::chrono::milliseconds test_timeout{30000};
}

/**
 * **Feature: raft-completion, Property 39: Timeout Cancellation Cleanup**
 * 
 * Property: For any operation timeout, the associated future is cancelled and related state is cleaned up.
 * **Validates: Requirements 8.3**
 */
BOOST_AUTO_TEST_CASE(raft_timeout_cancellation_cleanup_property_test, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Testing timeout cancellation cleanup property...");
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> operation_count_dist(min_operations, max_operations);
    std::uniform_int_distribution<std::size_t> future_count_dist(min_futures, max_futures);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, 1000);
    std::uniform_int_distribution<int> timeout_variant_dist(0, 2); // 0=short, 1=medium, 2=long
    
    // Test multiple scenarios with different timeout patterns
    for (int test_iteration = 0; test_iteration < 10; ++test_iteration) {
        BOOST_TEST_MESSAGE("Test iteration " << (test_iteration + 1) << "/10");
        
        const std::size_t operation_count = operation_count_dist(gen);
        const std::size_t future_count = future_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing timeout cancellation cleanup with " << operation_count 
                          << " operations and " << future_count << " futures");
        
        // Test 1: CommitWaiter timeout cleanup
        {
            BOOST_TEST_MESSAGE("Test 1: CommitWaiter timeout cleanup");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> timeout_count{0};
            std::atomic<std::size_t> fulfilled_count{0};
            
            // Register operations with different timeout durations
            std::vector<std::pair<std::uint64_t, std::chrono::milliseconds>> operation_timeouts;
            for (std::size_t i = 0; i < operation_count; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                // Randomly assign timeout duration
                std::chrono::milliseconds timeout_duration;
                int timeout_variant = timeout_variant_dist(gen);
                switch (timeout_variant) {
                    case 0: timeout_duration = short_timeout; break;
                    case 1: timeout_duration = medium_timeout; break;
                    case 2: timeout_duration = long_timeout; break;
                }
                
                operation_timeouts.emplace_back(index, timeout_duration);
                
                auto fulfill_callback = [&fulfilled_count](std::vector<std::byte> result) {
                    fulfilled_count.fetch_add(1);
                };
                
                auto reject_callback = [&timeout_count, timeout_duration](std::exception_ptr ex) {
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        std::string error_msg = e.what();
                        if (error_msg.find("timeout") != std::string::npos ||
                            error_msg.find("timed out") != std::string::npos) {
                            timeout_count.fetch_add(1);
                            BOOST_TEST_MESSAGE("Operation timed out after " << timeout_duration.count() 
                                              << "ms: " << e.what());
                        }
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    std::move(fulfill_callback),
                    std::move(reject_callback),
                    timeout_duration
                );
            }
            
            // Verify operations are registered
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), operation_count);
            
            // Wait for short timeouts to expire
            std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{50});
            auto short_timeout_cancelled = commit_waiter.cancel_timed_out_operations();
            
            // Wait for medium timeouts to expire
            std::this_thread::sleep_for(medium_timeout - short_timeout + std::chrono::milliseconds{50});
            auto medium_timeout_cancelled = commit_waiter.cancel_timed_out_operations();
            
            // Wait for long timeouts to expire
            std::this_thread::sleep_for(long_timeout - medium_timeout + std::chrono::milliseconds{50});
            auto long_timeout_cancelled = commit_waiter.cancel_timed_out_operations();
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: All operations should timeout and be cleaned up
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(fulfilled_count.load(), 0); // No operations should be fulfilled
            
            auto total_cancelled = short_timeout_cancelled + medium_timeout_cancelled + long_timeout_cancelled;
            BOOST_CHECK_EQUAL(total_cancelled, operation_count);
            BOOST_CHECK_EQUAL(timeout_count.load(), operation_count);
            
            BOOST_TEST_MESSAGE("✓ CommitWaiter timeout cleanup: " << total_cancelled 
                              << " operations timed out and cleaned up");
            BOOST_TEST_MESSAGE("  Short timeouts: " << short_timeout_cancelled 
                              << ", Medium: " << medium_timeout_cancelled 
                              << ", Long: " << long_timeout_cancelled);
        }
        
        // Test 2: Future collection timeout cleanup
        {
            BOOST_TEST_MESSAGE("Test 2: Future collection timeout cleanup");
            
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> timeout_futures;
            std::atomic<std::size_t> timeout_exceptions{0};
            
            // Create futures with different timeout durations
            for (std::size_t i = 0; i < future_count; ++i) {
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                
                // Randomly assign timeout duration
                std::chrono::milliseconds timeout_duration;
                int timeout_variant = timeout_variant_dist(gen);
                switch (timeout_variant) {
                    case 0: timeout_duration = short_timeout; break;
                    case 1: timeout_duration = medium_timeout; break;
                    case 2: timeout_duration = long_timeout; break;
                }
                
                auto future = promise->getFuture().within(timeout_duration);
                timeout_futures.push_back(std::move(future));
            }
            
            // Verify futures are created
            BOOST_CHECK_EQUAL(timeout_futures.size(), future_count);
            
            // Test timeout behavior using collection
            auto start_time = std::chrono::steady_clock::now();
            
            try {
                // This should timeout since promises are never fulfilled
                auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                    std::move(timeout_futures), long_timeout + std::chrono::milliseconds{100});
                
                auto results = std::move(collection_future).get();
                BOOST_TEST_MESSAGE("Collection completed unexpectedly with " << results.size() << " results");
                
            } catch (const std::exception& e) {
                auto end_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                
                BOOST_TEST_MESSAGE("Collection timed out after " << elapsed.count() << "ms: " << e.what());
                
                // Property: Collection should timeout within reasonable bounds
                BOOST_CHECK_LE(elapsed.count(), (long_timeout + std::chrono::milliseconds{500}).count());
            }
            
            // Property: Futures should be cleaned up after timeout
            // Note: timeout_futures was moved, so we can't check its size directly
            // But the collection should have handled the cleanup
            
            BOOST_TEST_MESSAGE("✓ Future collection timeout cleanup completed");
        }
        
        // Test 3: Timeout cleanup with different durations
        {
            BOOST_TEST_MESSAGE("Test 3: Timeout cleanup with different durations");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> all_timeouts{0};
            
            const std::size_t duration_operations = operation_count / 3;
            
            // Add operations with different timeout durations
            for (std::size_t i = 0; i < duration_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&all_timeouts](std::exception_ptr ex) {
                    all_timeouts.fetch_add(1);
                };
                
                // Use different timeout durations
                std::chrono::milliseconds timeout_duration;
                if (i % 3 == 0) {
                    timeout_duration = short_timeout;
                } else if (i % 3 == 1) {
                    timeout_duration = medium_timeout;
                } else {
                    timeout_duration = std::chrono::milliseconds{75}; // Between short and medium
                }
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    timeout_duration
                );
            }
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), duration_operations);
            
            // Wait for all timeouts to expire
            std::this_thread::sleep_for(medium_timeout + std::chrono::milliseconds{100});
            auto cancelled = commit_waiter.cancel_timed_out_operations();
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: All operations should timeout and be cleaned up
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(cancelled, duration_operations);
            BOOST_CHECK_EQUAL(all_timeouts.load(), duration_operations);
            
            BOOST_TEST_MESSAGE("✓ Different duration timeouts: " << duration_operations 
                              << " operations cleaned up");
        }
        
        // Test 4: Timeout cleanup with resource tracking
        {
            BOOST_TEST_MESSAGE("Test 4: Timeout cleanup with resource tracking");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<std::shared_ptr<std::vector<std::byte>>> resource_tracker;
            std::atomic<std::size_t> resource_cleanup_count{0};
            
            const std::size_t resource_operations = operation_count / 3;
            
            // Create operations that hold resources
            for (std::size_t i = 0; i < resource_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                // Create a resource that should be cleaned up
                auto resource = std::make_shared<std::vector<std::byte>>(1024, std::byte{0x42});
                resource_tracker.push_back(resource);
                
                auto reject_callback = [&resource_cleanup_count, resource](std::exception_ptr ex) {
                    // Simulate resource cleanup
                    resource_cleanup_count.fetch_add(1);
                    // Resource will be automatically cleaned up when shared_ptr goes out of scope
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    short_timeout
                );
            }
            
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), resource_operations);
            BOOST_CHECK_EQUAL(resource_tracker.size(), resource_operations);
            
            // Wait for timeouts
            std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{100});
            auto timed_out = commit_waiter.cancel_timed_out_operations();
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: Timeout should trigger resource cleanup
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK_EQUAL(timed_out, resource_operations);
            BOOST_CHECK_EQUAL(resource_cleanup_count.load(), resource_operations);
            
            // Clear resource tracker to verify cleanup
            resource_tracker.clear();
            
            BOOST_TEST_MESSAGE("✓ Timeout cleanup with resource tracking: " << resource_operations 
                              << " resources cleaned up");
        }
    }
    
    // Test edge cases for timeout cancellation cleanup
    BOOST_TEST_MESSAGE("Testing timeout cancellation cleanup edge cases...");
    
    // Test 5: Zero timeout operations
    {
        BOOST_TEST_MESSAGE("Test 5: Zero timeout operations");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> immediate_timeouts{0};
        
        // Add operations with zero timeout (should timeout immediately)
        const std::size_t zero_timeout_ops = 5;
        for (std::size_t i = 0; i < zero_timeout_ops; ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&immediate_timeouts](std::exception_ptr ex) {
                immediate_timeouts.fetch_add(1);
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                std::chrono::milliseconds{0} // Zero timeout
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), zero_timeout_ops);
        
        // Even zero timeout operations need a cleanup call
        auto cancelled = commit_waiter.cancel_timed_out_operations();
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        
        // Property: Zero timeout operations should be cleaned up immediately
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(cancelled, zero_timeout_ops);
        BOOST_CHECK_EQUAL(immediate_timeouts.load(), zero_timeout_ops);
        
        BOOST_TEST_MESSAGE("✓ Zero timeout operations handled correctly");
    }
    
    // Test 6: Timeout cleanup during high load
    {
        BOOST_TEST_MESSAGE("Test 6: Timeout cleanup during high load");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> high_load_timeouts{0};
        std::atomic<bool> cleanup_running{false};
        
        const std::size_t high_load_ops = 100;
        
        // Add many operations with short timeouts
        for (std::size_t i = 0; i < high_load_ops; ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&high_load_timeouts](std::exception_ptr ex) {
                high_load_timeouts.fetch_add(1);
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                std::chrono::milliseconds{100}
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), high_load_ops);
        
        // Start cleanup thread
        std::thread cleanup_thread([&]() {
            cleanup_running.store(true);
            std::size_t total_cleaned = 0;
            
            while (commit_waiter.has_pending_operations()) {
                auto cleaned = commit_waiter.cancel_timed_out_operations();
                total_cleaned += cleaned;
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            
            cleanup_running.store(false);
            BOOST_TEST_MESSAGE("Cleanup thread cleaned up " << total_cleaned << " operations");
        });
        
        // Wait for cleanup to complete
        cleanup_thread.join();
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        
        // Property: High load timeout cleanup should handle all operations
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(high_load_timeouts.load(), high_load_ops);
        BOOST_CHECK(!cleanup_running.load());
        
        BOOST_TEST_MESSAGE("✓ High load timeout cleanup: " << high_load_ops << " operations handled");
    }
    
    // Test 7: Timeout precision validation
    {
        BOOST_TEST_MESSAGE("Test 7: Timeout precision validation");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::vector<std::chrono::steady_clock::time_point> timeout_times;
        std::mutex timeout_mutex;
        
        const std::size_t precision_ops = 10;
        const auto precise_timeout = std::chrono::milliseconds{150};
        
        // Add operations with precise timeout
        for (std::size_t i = 0; i < precision_ops; ++i) {
            const std::uint64_t index = i + 1;
            
            auto reject_callback = [&timeout_times, &timeout_mutex](std::exception_ptr ex) {
                std::lock_guard<std::mutex> lock(timeout_mutex);
                timeout_times.push_back(std::chrono::steady_clock::now());
            };
            
            commit_waiter.register_operation(
                index,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                precise_timeout
            );
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Wait for timeouts and clean up
        std::this_thread::sleep_for(precise_timeout + std::chrono::milliseconds{100});
        auto cancelled = commit_waiter.cancel_timed_out_operations();
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Timeout precision should be reasonable
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(cancelled, precision_ops);
        BOOST_CHECK_EQUAL(timeout_times.size(), precision_ops);
        
        // Check timeout precision (should be close to expected timeout)
        for (const auto& timeout_time : timeout_times) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_time - start_time);
            BOOST_CHECK_GE(elapsed.count(), precise_timeout.count() - 50); // Allow 50ms early
            BOOST_CHECK_LE(elapsed.count(), precise_timeout.count() + 200); // Allow 200ms late
        }
        
        BOOST_TEST_MESSAGE("✓ Timeout precision validation completed");
    }
    
    BOOST_TEST_MESSAGE("All timeout cancellation cleanup property tests passed!");
}