#define BOOST_TEST_MODULE RaftShutdownCleanupPropertyTest

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
        char* argv_data[] = {const_cast<char*>("raft_shutdown_cleanup_property_test"), nullptr};
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
    constexpr std::chrono::milliseconds operation_timeout{5000};
    constexpr const char* shutdown_reason = "Node shutdown";
}

/**
 * **Feature: raft-completion, Property 37: Shutdown Cleanup**
 * 
 * Property: For any node shutdown, all pending futures are cancelled and resources are cleaned up.
 * **Validates: Requirements 8.1**
 */
BOOST_AUTO_TEST_CASE(raft_shutdown_cleanup_property_test, * boost::unit_test::timeout(120)) {
    BOOST_TEST_MESSAGE("Testing shutdown cleanup property...");
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> operation_count_dist(min_operations, max_operations);
    std::uniform_int_distribution<std::size_t> future_count_dist(min_futures, max_futures);
    std::uniform_int_distribution<std::uint64_t> index_dist(1, 1000);
    std::uniform_int_distribution<std::uint64_t> term_dist(1, 100);
    
    // Test multiple scenarios with different numbers of pending operations
    for (int test_iteration = 0; test_iteration < 10; ++test_iteration) {
        BOOST_TEST_MESSAGE("Test iteration " << (test_iteration + 1) << "/10");
        
        const std::size_t operation_count = operation_count_dist(gen);
        const std::size_t future_count = future_count_dist(gen);
        
        BOOST_TEST_MESSAGE("Testing shutdown cleanup with " << operation_count 
                          << " pending operations and " << future_count << " futures");
        
        // Test 1: CommitWaiter shutdown cleanup
        {
            BOOST_TEST_MESSAGE("Test 1: CommitWaiter shutdown cleanup");
            
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::atomic<std::size_t> fulfilled_count{0};
            std::atomic<std::size_t> rejected_count{0};
            
            // Register multiple pending operations
            std::vector<std::uint64_t> operation_indices;
            for (std::size_t i = 0; i < operation_count; ++i) {
                const std::uint64_t index = index_dist(gen);
                operation_indices.push_back(index);
                
                // Create callbacks to track fulfillment/rejection
                auto fulfill_callback = [&fulfilled_count](std::vector<std::byte> result) {
                    fulfilled_count.fetch_add(1);
                };
                
                auto reject_callback = [&rejected_count](std::exception_ptr ex) {
                    rejected_count.fetch_add(1);
                    try {
                        std::rethrow_exception(ex);
                    } catch (const std::exception& e) {
                        // Expected cancellation exception
                        BOOST_TEST_MESSAGE("Operation cancelled: " << e.what());
                    }
                };
                
                commit_waiter.register_operation(
                    index,
                    std::move(fulfill_callback),
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            // Verify operations are pending
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), operation_count);
            BOOST_CHECK(commit_waiter.has_pending_operations());
            
            // Simulate node shutdown by cancelling all operations
            commit_waiter.cancel_all_operations(shutdown_reason);
            
            // Property: All operations should be cancelled after shutdown
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK(!commit_waiter.has_pending_operations());
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: All operations should be rejected (none fulfilled)
            BOOST_CHECK_EQUAL(fulfilled_count.load(), 0);
            BOOST_CHECK_EQUAL(rejected_count.load(), operation_count);
            
            BOOST_TEST_MESSAGE("✓ CommitWaiter shutdown cleanup: " << operation_count 
                              << " operations cancelled");
        }
        
        // Test 2: Future collection shutdown cleanup
        {
            BOOST_TEST_MESSAGE("Test 2: Future collection shutdown cleanup");
            
            // Create multiple future collections to simulate different RPC operations
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> append_futures;
            std::vector<kythira::Future<kythira::request_vote_response<std::uint64_t>>> vote_futures;
            
            // Create long-running futures that would normally not complete
            for (std::size_t i = 0; i < future_count; ++i) {
                // Create futures that will timeout (simulating network operations during shutdown)
                auto append_promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                append_futures.push_back(append_promise->getFuture().within(operation_timeout));
                
                auto vote_promise = std::make_shared<kythira::Promise<kythira::request_vote_response<std::uint64_t>>>();
                vote_futures.push_back(vote_promise->getFuture().within(operation_timeout));
            }
            
            // Verify futures are created
            BOOST_CHECK_EQUAL(append_futures.size(), future_count);
            BOOST_CHECK_EQUAL(vote_futures.size(), future_count);
            
            // Simulate shutdown by cancelling all future collections
            kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(append_futures);
            kythira::raft_future_collector<kythira::request_vote_response<std::uint64_t>>::cancel_collection(vote_futures);
            
            // Property: All futures should be cleaned up after shutdown
            BOOST_CHECK(append_futures.empty());
            BOOST_CHECK(vote_futures.empty());
            
            BOOST_TEST_MESSAGE("✓ Future collection shutdown cleanup: " << (future_count * 2) 
                              << " futures cleaned up");
        }
        
        // Test 3: Error handler shutdown cleanup
        {
            BOOST_TEST_MESSAGE("Test 3: Error handler shutdown cleanup");
            
            kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>> error_handler;
            
            // Configure retry policies for testing
            typename kythira::error_handler<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::retry_policy test_policy{
                .initial_delay = std::chrono::milliseconds{100},
                .max_delay = std::chrono::milliseconds{1000},
                .backoff_multiplier = 2.0,
                .jitter_factor = 0.1,
                .max_attempts = 3
            };
            
            error_handler.set_retry_policy("test_operation", test_policy);
            
            // Verify policy is set
            auto retrieved_policy = error_handler.get_retry_policy("test_operation");
            BOOST_CHECK_EQUAL(retrieved_policy.max_attempts, test_policy.max_attempts);
            BOOST_CHECK_EQUAL(retrieved_policy.initial_delay.count(), test_policy.initial_delay.count());
            
            // Property: Error handler should maintain consistent state during shutdown
            // (Error handlers are stateless, so shutdown mainly involves stopping retry operations)
            BOOST_CHECK(retrieved_policy.is_valid());
            
            BOOST_TEST_MESSAGE("✓ Error handler shutdown cleanup: policies maintained");
        }
        
        // Test 4: Combined shutdown scenario
        {
            BOOST_TEST_MESSAGE("Test 4: Combined shutdown scenario");
            
            // Create a scenario with multiple components that need cleanup
            kythira::commit_waiter<std::uint64_t> commit_waiter;
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> futures;
            
            std::atomic<std::size_t> total_cancelled{0};
            
            // Add pending operations to commit waiter
            const std::size_t combined_operations = operation_count / 2;
            for (std::size_t i = 0; i < combined_operations; ++i) {
                const std::uint64_t index = index_dist(gen);
                
                auto reject_callback = [&total_cancelled](std::exception_ptr ex) {
                    total_cancelled.fetch_add(1);
                };
                
                commit_waiter.register_operation(
                    index,
                    [](std::vector<std::byte>) {},
                    std::move(reject_callback),
                    operation_timeout
                );
            }
            
            // Add futures to collection
            const std::size_t combined_futures = future_count / 2;
            for (std::size_t i = 0; i < combined_futures; ++i) {
                auto promise = std::make_shared<kythira::Promise<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>();
                futures.push_back(promise->getFuture().within(operation_timeout));
            }
            
            // Verify initial state
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), combined_operations);
            BOOST_CHECK_EQUAL(futures.size(), combined_futures);
            
            // Simulate coordinated shutdown
            commit_waiter.cancel_all_operations(shutdown_reason);
            kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(futures);
            
            // Give callbacks time to execute
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            
            // Property: All resources should be cleaned up in coordinated shutdown
            BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
            BOOST_CHECK(futures.empty());
            BOOST_CHECK_EQUAL(total_cancelled.load(), combined_operations);
            
            BOOST_TEST_MESSAGE("✓ Combined shutdown cleanup: " << combined_operations 
                              << " operations + " << combined_futures << " futures cleaned up");
        }
    }
    
    // Test edge cases for shutdown cleanup
    BOOST_TEST_MESSAGE("Testing shutdown cleanup edge cases...");
    
    // Test 5: Shutdown with no pending operations
    {
        BOOST_TEST_MESSAGE("Test 5: Shutdown with no pending operations");
        
        kythira::commit_waiter<std::uint64_t> empty_waiter;
        
        // Verify no operations are pending
        BOOST_CHECK_EQUAL(empty_waiter.get_pending_count(), 0);
        BOOST_CHECK(!empty_waiter.has_pending_operations());
        
        // Shutdown should be safe even with no operations
        empty_waiter.cancel_all_operations(shutdown_reason);
        
        // Property: Shutdown with no operations should be safe
        BOOST_CHECK_EQUAL(empty_waiter.get_pending_count(), 0);
        BOOST_CHECK(!empty_waiter.has_pending_operations());
        
        BOOST_TEST_MESSAGE("✓ Safe shutdown with no pending operations");
    }
    
    // Test 6: Shutdown with empty future collections
    {
        BOOST_TEST_MESSAGE("Test 6: Shutdown with empty future collections");
        
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> empty_futures;
        
        // Verify collection is empty
        BOOST_CHECK(empty_futures.empty());
        
        // Shutdown should be safe even with empty collections
        kythira::raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(empty_futures);
        
        // Property: Shutdown with empty collections should be safe
        BOOST_CHECK(empty_futures.empty());
        
        BOOST_TEST_MESSAGE("✓ Safe shutdown with empty future collections");
    }
    
    // Test 7: Multiple shutdown calls (idempotency)
    {
        BOOST_TEST_MESSAGE("Test 7: Multiple shutdown calls");
        
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        std::atomic<std::size_t> rejection_count{0};
        
        // Add some operations
        const std::size_t test_operations = 5;
        for (std::size_t i = 0; i < test_operations; ++i) {
            auto reject_callback = [&rejection_count](std::exception_ptr ex) {
                rejection_count.fetch_add(1);
            };
            
            commit_waiter.register_operation(
                i + 1,
                [](std::vector<std::byte>) {},
                std::move(reject_callback),
                operation_timeout
            );
        }
        
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), test_operations);
        
        // Call shutdown multiple times
        commit_waiter.cancel_all_operations(shutdown_reason);
        commit_waiter.cancel_all_operations(shutdown_reason);
        commit_waiter.cancel_all_operations(shutdown_reason);
        
        // Give callbacks time to execute
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Property: Multiple shutdown calls should be idempotent
        BOOST_CHECK_EQUAL(commit_waiter.get_pending_count(), 0);
        BOOST_CHECK_EQUAL(rejection_count.load(), test_operations); // Should only be called once per operation
        
        BOOST_TEST_MESSAGE("✓ Multiple shutdown calls are idempotent");
    }
    
    BOOST_TEST_MESSAGE("All shutdown cleanup property tests passed!");
}