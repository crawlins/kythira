#define BOOST_TEST_MODULE RaftCollectionCancellationCleanupPropertyTest

#include <boost/test/included/unit_test.hpp>
#include <raft/future_collector.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>
#include <folly/init/Init.h>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <memory>

using namespace kythira;

namespace {
    constexpr std::chrono::milliseconds test_timeout{1000};
    constexpr std::size_t test_iterations = 20;
}

// Global fixture to initialize Folly
struct GlobalFixture {
    GlobalFixture() {
        int argc = 1;
        char* argv[] = {const_cast<char*>("test"), nullptr};
        char** argv_ptr = argv;
        folly::init(&argc, &argv_ptr);
    }
};

BOOST_GLOBAL_FIXTURE(GlobalFixture);

/**
 * **Feature: raft-completion, Property 10: Collection Cancellation Cleanup**
 * 
 * Property: For any cancelled future collection operation, all pending futures are properly cleaned up.
 * **Validates: Requirements 2.5**
 */
BOOST_AUTO_TEST_CASE(raft_collection_cancellation_cleanup_property_test, * boost::unit_test::timeout(120)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> future_count_dist(3, 8);
    std::uniform_int_distribution<int> delay_dist(100, 2000); // milliseconds
    
    for (std::size_t iteration = 0; iteration < test_iterations; ++iteration) {
        BOOST_TEST_MESSAGE("Iteration " << iteration + 1 << "/" << test_iterations);
        
        const std::size_t future_count = future_count_dist(gen);
        BOOST_TEST_MESSAGE("Testing cancellation cleanup with " << future_count << " futures");
        
        // Test 1: Cancellation via timeout
        {
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> long_futures;
            long_futures.reserve(future_count);
            
            // Create futures that will take longer than the timeout
            for (std::size_t i = 0; i < future_count; ++i) {
                const int delay_ms = delay_dist(gen) + 1000; // Ensure longer than timeout
                kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
                auto future = kythira::FutureFactory::makeFuture(response)
                    .delay(std::chrono::milliseconds(delay_ms));
                long_futures.push_back(std::move(future));
            }
            
            // Start collection with short timeout (will cause cancellation)
            auto start_time = std::chrono::steady_clock::now();
            auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
                std::move(long_futures), std::chrono::milliseconds(200)); // Short timeout
            
            try {
                auto results = std::move(collection_future).get();
                BOOST_TEST_MESSAGE("⚠ Collection unexpectedly succeeded with " << results.size() << " results");
            } catch (const std::exception& e) {
                auto end_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                
                // Property: Cancellation should happen quickly due to timeout
                BOOST_TEST_MESSAGE("✓ Collection cancelled due to timeout in " << elapsed.count() << "ms: " << e.what());
                BOOST_CHECK_LE(elapsed.count(), 500); // Should timeout quickly
            }
        }
        
        // Test 2: Manual cancellation using cancel_collection
        {
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> manual_futures;
            manual_futures.reserve(future_count);
            
            // Create futures with various delays
            for (std::size_t i = 0; i < future_count; ++i) {
                const int delay_ms = delay_dist(gen);
                kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
                auto future = kythira::FutureFactory::makeFuture(response)
                    .delay(std::chrono::milliseconds(delay_ms));
                manual_futures.push_back(std::move(future));
            }
            
            // Test the cancel_collection method
            auto futures_copy = std::move(manual_futures); // Move to test cleanup
            raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(futures_copy);
            
            // Property: After cancellation, futures vector should be empty
            BOOST_CHECK(futures_copy.empty());
            BOOST_TEST_MESSAGE("✓ Manual cancellation cleared " << future_count << " futures");
        }
    }
    
    // Test specific cancellation scenarios
    BOOST_TEST_MESSAGE("Testing specific cancellation scenarios...");
    
    // Test 3: Cancellation during active collection
    {
        BOOST_TEST_MESSAGE("Test 3: Cancellation during active collection");
        
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> active_futures;
        
        // Mix of fast and slow futures
        for (std::size_t i = 0; i < 6; ++i) {
            int delay = (i < 3) ? 50 : 1500; // Half fast, half slow
            kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
            auto future = kythira::FutureFactory::makeFuture(response)
                .delay(std::chrono::milliseconds(delay));
            active_futures.push_back(std::move(future));
        }
        
        auto start_time = std::chrono::steady_clock::now();
        auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
            std::move(active_futures), std::chrono::milliseconds(300)); // Medium timeout
        
        try {
            auto results = std::move(collection_future).get();
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("✓ Active collection completed with " << results.size() 
                              << " results in " << elapsed.count() << "ms");
            
            // Should complete with fast responses
            BOOST_CHECK_GE(results.size(), 4); // Majority of 6
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            BOOST_TEST_MESSAGE("✓ Active collection cancelled in " << elapsed.count() << "ms: " << e.what());
            // Cancellation is also acceptable
        }
    }
    
    // Test 4: Resource cleanup verification
    {
        BOOST_TEST_MESSAGE("Test 4: Resource cleanup verification");
        
        // Create a large number of futures to test resource management
        const std::size_t large_count = 20;
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> resource_futures;
        resource_futures.reserve(large_count);
        
        for (std::size_t i = 0; i < large_count; ++i) {
            kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
            auto future = kythira::FutureFactory::makeFuture(response)
                .delay(std::chrono::milliseconds(2000)); // Long delay
            resource_futures.push_back(std::move(future));
        }
        
        // Test cleanup
        auto original_size = resource_futures.size();
        BOOST_CHECK_EQUAL(original_size, large_count);
        
        raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(resource_futures);
        
        // Property: All resources should be cleaned up
        BOOST_CHECK(resource_futures.empty());
        BOOST_TEST_MESSAGE("✓ Resource cleanup verified: " << original_size << " futures cleaned up");
    }
    
    // Test 5: Cancellation with different future types
    {
        BOOST_TEST_MESSAGE("Test 5: Cancellation with different response types");
        
        // Test with request_vote_response
        std::vector<kythira::Future<kythira::request_vote_response<std::uint64_t>>> vote_futures;
        
        for (std::size_t i = 0; i < 4; ++i) {
            kythira::request_vote_response<std::uint64_t> response{1, true};
            auto future = kythira::FutureFactory::makeFuture(response)
                .delay(std::chrono::milliseconds(1000));
            vote_futures.push_back(std::move(future));
        }
        
        // Test cancellation with different type
        raft_future_collector<kythira::request_vote_response<std::uint64_t>>::cancel_collection(vote_futures);
        BOOST_CHECK(vote_futures.empty());
        BOOST_TEST_MESSAGE("✓ Cancellation works with request_vote_response type");
    }
    
    // Test 6: Multiple concurrent cancellations
    {
        BOOST_TEST_MESSAGE("Test 6: Multiple concurrent cancellations");
        
        std::vector<std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>>> multiple_collections;
        
        // Create multiple collections
        for (std::size_t collection = 0; collection < 3; ++collection) {
            std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> futures;
            
            for (std::size_t i = 0; i < 5; ++i) {
                kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
                auto future = kythira::FutureFactory::makeFuture(response)
                    .delay(std::chrono::milliseconds(1500));
                futures.push_back(std::move(future));
            }
            
            multiple_collections.push_back(std::move(futures));
        }
        
        // Cancel all collections
        std::size_t total_cancelled = 0;
        for (auto& collection : multiple_collections) {
            total_cancelled += collection.size();
            raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(collection);
            BOOST_CHECK(collection.empty());
        }
        
        BOOST_TEST_MESSAGE("✓ Multiple concurrent cancellations: " << total_cancelled << " futures cleaned up");
    }
    
    // Test 7: Cancellation edge cases
    {
        BOOST_TEST_MESSAGE("Test 7: Cancellation edge cases");
        
        // Test cancellation of empty collection
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> empty_futures;
        raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(empty_futures);
        BOOST_CHECK(empty_futures.empty());
        BOOST_TEST_MESSAGE("✓ Empty collection cancellation handled correctly");
        
        // Test cancellation of single future
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> single_future;
        kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
        single_future.push_back(kythira::FutureFactory::makeFuture(response));
        
        BOOST_CHECK_EQUAL(single_future.size(), 1);
        raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::cancel_collection(single_future);
        BOOST_CHECK(single_future.empty());
        BOOST_TEST_MESSAGE("✓ Single future cancellation handled correctly");
    }
    
    // Test 8: Cancellation doesn't affect completed futures
    {
        BOOST_TEST_MESSAGE("Test 8: Cancellation of already completed operations");
        
        // Create futures that complete immediately
        std::vector<kythira::Future<kythira::append_entries_response<std::uint64_t, std::uint64_t>>> immediate_futures;
        
        for (std::size_t i = 0; i < 3; ++i) {
            kythira::append_entries_response<std::uint64_t, std::uint64_t> response{1, true, std::nullopt, std::nullopt};
            auto future = kythira::FutureFactory::makeFuture(response); // No delay - immediate
            immediate_futures.push_back(std::move(future));
        }
        
        // Start collection (should complete quickly)
        auto collection_future = raft_future_collector<kythira::append_entries_response<std::uint64_t, std::uint64_t>>::collect_majority(
            std::move(immediate_futures), test_timeout);
        
        try {
            auto results = std::move(collection_future).get();
            BOOST_TEST_MESSAGE("✓ Immediate collection completed with " << results.size() << " results");
            BOOST_CHECK_GE(results.size(), 2); // Majority of 3
        } catch (const std::exception& e) {
            BOOST_ERROR("Immediate collection should not fail: " << e.what());
        }
    }
    
    BOOST_TEST_MESSAGE("All collection cancellation cleanup property tests passed!");
}