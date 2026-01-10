/**
 * Integration Test for Future Collection Operations
 * 
 * Tests future collection functionality with various scenarios including:
 * - Heartbeat collection with various response patterns
 * - Election vote collection with network failures
 * - Replication acknowledgment collection with slow followers
 * - Proper timeout and cancellation handling
 * 
 * Requirements: 2.1, 2.2, 2.3, 2.4, 2.5
 */

#define BOOST_TEST_MODULE RaftFutureCollectionIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/future_collector.hpp>
#include <raft/completion_exceptions.hpp>
#include <raft/types.hpp>
#include <raft/future.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <exception>
#include <string>
#include <random>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_future_collection_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    // Test constants
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::uint64_t test_term_2 = 2;
    constexpr std::uint64_t test_node_id_1 = 1;
    constexpr std::uint64_t test_node_id_2 = 2;
    constexpr std::uint64_t test_node_id_3 = 3;
    constexpr std::uint64_t test_node_id_4 = 4;
    constexpr std::uint64_t test_node_id_5 = 5;
    constexpr std::uint64_t test_log_index_1 = 1;
    constexpr std::uint64_t test_log_index_2 = 2;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr std::size_t cluster_size_3 = 3;
    constexpr std::size_t cluster_size_5 = 5;
    constexpr std::size_t majority_of_3 = 2;
    constexpr std::size_t majority_of_5 = 3;
    
    // Helper function to create successful append entries response
    auto create_successful_append_entries_response(std::uint64_t term) -> kythira::append_entries_response<> {
        return kythira::append_entries_response<>{
            ._term = term,
            ._success = true,
            ._conflict_index = std::nullopt,
            ._conflict_term = std::nullopt
        };
    }
    
    // Helper function to create failed append entries response
    auto create_failed_append_entries_response(std::uint64_t term, std::uint64_t conflict_index, std::uint64_t conflict_term) -> kythira::append_entries_response<> {
        return kythira::append_entries_response<>{
            ._term = term,
            ._success = false,
            ._conflict_index = conflict_index,
            ._conflict_term = conflict_term
        };
    }
    
    // Helper function to create successful vote response
    auto create_successful_vote_response(std::uint64_t term) -> kythira::request_vote_response<> {
        return kythira::request_vote_response<>{
            ._term = term,
            ._vote_granted = true
        };
    }
    
    // Helper function to create rejected vote response
    auto create_rejected_vote_response(std::uint64_t term) -> kythira::request_vote_response<> {
        return kythira::request_vote_response<>{
            ._term = term,
            ._vote_granted = false
        };
    }
    
    // Helper function to create delayed future
    template<typename T>
    auto create_delayed_future(T value, std::chrono::milliseconds delay) -> kythira::Future<T> {
        auto promise = std::make_shared<kythira::Promise<T>>();
        auto future = promise->getFuture();
        
        // Schedule the value to be set after delay
        std::thread([promise, value = std::move(value), delay]() mutable {
            std::this_thread::sleep_for(delay);
            promise->setValue(std::move(value));
        }).detach();
        
        return future;
    }
    
    // Helper function to create timeout future
    template<typename T>
    auto create_timeout_future(std::chrono::milliseconds delay) -> kythira::Future<T> {
        auto promise = std::make_shared<kythira::Promise<T>>();
        auto future = promise->getFuture();
        
        // Schedule timeout exception after delay
        std::thread([promise, delay]() {
            std::this_thread::sleep_for(delay);
            promise->setException(std::runtime_error("Operation timed out"));
        }).detach();
        
        return future;
    }
    
    // Helper function to create never-completing future
    template<typename T>
    auto create_never_completing_future() -> kythira::Future<T> {
        auto promise = std::make_shared<kythira::Promise<T>>();
        return promise->getFuture();
        // Promise is never fulfilled, so future never completes
    }
}

BOOST_AUTO_TEST_SUITE(future_collection_integration_tests, * boost::unit_test::timeout(120))

/**
 * Test: Heartbeat collection with all successful responses
 * 
 * Verifies that heartbeat collection works correctly when all followers
 * respond successfully within the timeout.
 * 
 * Requirements: 2.1
 */
BOOST_AUTO_TEST_CASE(heartbeat_collection_all_successful, * boost::unit_test::timeout(30)) {
    // Create futures for heartbeat responses from 4 followers (5-node cluster)
    std::vector<kythira::Future<kythira::append_entries_response<>>> heartbeat_futures;
    
    // All followers respond successfully
    for (std::size_t i = 0; i < 4; ++i) {
        auto response = create_successful_append_entries_response(test_term_1);
        heartbeat_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 10})
        );
    }
    
    // Collect majority (3 out of 4 followers + leader = majority of 5)
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(heartbeat_futures), medium_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because leader counts itself
    
    // Verify all responses are successful
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_1);
        BOOST_CHECK(response.success());
    }
}

/**
 * Test: Heartbeat collection with mixed responses
 * 
 * Verifies that heartbeat collection works correctly when some followers
 * respond successfully and others fail, but majority is achieved.
 * 
 * Requirements: 2.1
 */
BOOST_AUTO_TEST_CASE(heartbeat_collection_mixed_responses, * boost::unit_test::timeout(30)) {
    // Create futures for heartbeat responses from 4 followers (5-node cluster)
    std::vector<kythira::Future<kythira::append_entries_response<>>> heartbeat_futures;
    
    // 3 followers respond successfully (enough for majority)
    for (std::size_t i = 0; i < 3; ++i) {
        auto response = create_successful_append_entries_response(test_term_1);
        heartbeat_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 10})
        );
    }
    
    // 1 follower responds with failure
    auto failed_response = create_failed_append_entries_response(test_term_1, test_log_index_1, test_term_1);
    heartbeat_futures.push_back(
        create_delayed_future(std::move(failed_response), std::chrono::milliseconds{80})
    );
    
    // Collect majority
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(heartbeat_futures), medium_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses (including the failed one)
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because leader counts itself
    
    // Count successful and failed responses
    std::size_t successful_count = 0;
    std::size_t failed_count = 0;
    
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_1);
        if (response.success()) {
            successful_count++;
        } else {
            failed_count++;
        }
    }
    
    // Should have at least 3 successful responses
    BOOST_CHECK_GE(successful_count, 3);
    BOOST_CHECK_LE(failed_count, 1);
}

/**
 * Test: Heartbeat collection with timeout failures
 * 
 * Verifies that heartbeat collection handles timeout failures correctly
 * when some followers don't respond within the timeout.
 * 
 * Requirements: 2.1, 2.4
 */
BOOST_AUTO_TEST_CASE(heartbeat_collection_with_timeouts, * boost::unit_test::timeout(30)) {
    // Create futures for heartbeat responses from 4 followers (5-node cluster)
    std::vector<kythira::Future<kythira::append_entries_response<>>> heartbeat_futures;
    
    // 3 followers respond successfully within timeout
    for (std::size_t i = 0; i < 3; ++i) {
        auto response = create_successful_append_entries_response(test_term_1);
        heartbeat_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 10})
        );
    }
    
    // 1 follower times out (responds after timeout)
    heartbeat_futures.push_back(
        create_timeout_future<kythira::append_entries_response<>>(short_timeout + std::chrono::milliseconds{50})
    );
    
    // Collect majority with short timeout
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(heartbeat_futures), short_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses (3 successful, 1 timed out but majority achieved)
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because leader counts itself
    
    // All received responses should be successful (timed out ones are not included)
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_1);
        BOOST_CHECK(response.success());
    }
}

/**
 * Test: Election vote collection with successful majority
 * 
 * Verifies that election vote collection works correctly when a majority
 * of nodes grant votes to the candidate.
 * 
 * Requirements: 2.2
 */
BOOST_AUTO_TEST_CASE(election_vote_collection_successful_majority, * boost::unit_test::timeout(30)) {
    // Create futures for vote responses from 4 other nodes (5-node cluster)
    std::vector<kythira::Future<kythira::request_vote_response<>>> vote_futures;
    
    // 3 nodes grant votes (enough for majority including candidate)
    for (std::size_t i = 0; i < 3; ++i) {
        auto response = create_successful_vote_response(test_term_2);
        vote_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 20})
        );
    }
    
    // 1 node rejects vote
    auto rejected_response = create_rejected_vote_response(test_term_2);
    vote_futures.push_back(
        create_delayed_future(std::move(rejected_response), std::chrono::milliseconds{100})
    );
    
    // Collect majority votes
    auto collection_future = kythira::raft_future_collector<kythira::request_vote_response<>>::collect_majority(
        std::move(vote_futures), medium_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because candidate votes for itself
    
    // Count granted and rejected votes
    std::size_t granted_count = 0;
    std::size_t rejected_count = 0;
    
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_2);
        if (response.vote_granted()) {
            granted_count++;
        } else {
            rejected_count++;
        }
    }
    
    // Should have at least 3 granted votes
    BOOST_CHECK_GE(granted_count, 3);
}

/**
 * Test: Election vote collection with network failures
 * 
 * Verifies that election vote collection handles network failures correctly
 * and can still succeed if enough nodes respond.
 * 
 * Requirements: 2.2, 2.4
 */
BOOST_AUTO_TEST_CASE(election_vote_collection_with_network_failures, * boost::unit_test::timeout(30)) {
    // Create futures for vote responses from 4 other nodes (5-node cluster)
    std::vector<kythira::Future<kythira::request_vote_response<>>> vote_futures;
    
    // 3 nodes grant votes successfully
    for (std::size_t i = 0; i < 3; ++i) {
        auto response = create_successful_vote_response(test_term_2);
        vote_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 20})
        );
    }
    
    // 1 node has network failure (timeout)
    vote_futures.push_back(
        create_timeout_future<kythira::request_vote_response<>>(short_timeout + std::chrono::milliseconds{50})
    );
    
    // Collect majority votes with short timeout
    auto collection_future = kythira::raft_future_collector<kythira::request_vote_response<>>::collect_majority(
        std::move(vote_futures), short_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses (3 successful, 1 failed due to network)
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because candidate votes for itself
    
    // All received responses should be vote grants (failed ones are not included)
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_2);
        BOOST_CHECK(response.vote_granted());
    }
}

/**
 * Test: Election vote collection failure due to insufficient votes
 * 
 * Verifies that election vote collection fails correctly when insufficient
 * nodes grant votes for a majority.
 * 
 * Requirements: 2.2, 2.4
 */
BOOST_AUTO_TEST_CASE(election_vote_collection_insufficient_votes, * boost::unit_test::timeout(30)) {
    // Create futures for vote responses from 4 other nodes (5-node cluster)
    std::vector<kythira::Future<kythira::request_vote_response<>>> vote_futures;
    
    // Only 1 node grants vote (not enough for majority even with candidate)
    auto granted_response = create_successful_vote_response(test_term_2);
    vote_futures.push_back(
        create_delayed_future(std::move(granted_response), std::chrono::milliseconds{50})
    );
    
    // 3 nodes reject votes
    for (std::size_t i = 0; i < 3; ++i) {
        auto response = create_rejected_vote_response(test_term_2);
        vote_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{60 + i * 10})
        );
    }
    
    // Collect majority votes
    auto collection_future = kythira::raft_future_collector<kythira::request_vote_response<>>::collect_majority(
        std::move(vote_futures), medium_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // We should get majority of responses (3 out of 4), but insufficient granted votes
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because candidate votes for itself
    
    // Count granted votes including candidate's self-vote
    std::size_t granted_count = 1; // Candidate votes for itself
    for (const auto& response : results) {
        if (response.vote_granted()) {
            granted_count++;
        }
    }
    
    // Should not have majority (need 3 out of 5, but only have 2: 1 granted + 1 candidate)
    BOOST_CHECK_LT(granted_count, majority_of_5);
}

/**
 * Test: Replication acknowledgment collection with slow followers
 * 
 * Verifies that replication acknowledgment collection works correctly
 * when some followers are slow to respond but majority is achieved.
 * 
 * Requirements: 2.3
 */
BOOST_AUTO_TEST_CASE(replication_acknowledgment_collection_slow_followers, * boost::unit_test::timeout(30)) {
    // Create futures for replication responses from 4 followers (5-node cluster)
    std::vector<kythira::Future<kythira::append_entries_response<>>> replication_futures;
    
    // 2 followers respond quickly
    for (std::size_t i = 0; i < 2; ++i) {
        auto response = create_successful_append_entries_response(test_term_1);
        replication_futures.push_back(
            create_delayed_future(std::move(response), std::chrono::milliseconds{50 + i * 10})
        );
    }
    
    // 1 follower responds slowly but within timeout
    auto slow_response = create_successful_append_entries_response(test_term_1);
    replication_futures.push_back(
        create_delayed_future(std::move(slow_response), std::chrono::milliseconds{400})
    );
    
    // 1 follower is very slow (will timeout)
    replication_futures.push_back(
        create_delayed_future(
            create_successful_append_entries_response(test_term_1), 
            medium_timeout + std::chrono::milliseconds{100}
        )
    );
    
    // Collect majority acknowledgments
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(replication_futures), medium_timeout);
    
    // Wait for collection to complete
    auto results = std::move(collection_future).get();
    
    // Verify we got majority responses (3 out of 4 followers responded, plus leader = majority)
    BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because leader counts itself
    
    // All received responses should be successful
    for (const auto& response : results) {
        BOOST_CHECK_EQUAL(response.term(), test_term_1);
        BOOST_CHECK(response.success());
    }
}

/**
 * Test: Future collection cancellation cleanup
 * 
 * Verifies that future collection operations can be cancelled and
 * resources are properly cleaned up.
 * 
 * Requirements: 2.5
 */
BOOST_AUTO_TEST_CASE(future_collection_cancellation_cleanup, * boost::unit_test::timeout(30)) {
    // Create futures that will never complete
    std::vector<kythira::Future<kythira::append_entries_response<>>> never_completing_futures;
    
    for (std::size_t i = 0; i < 4; ++i) {
        never_completing_futures.push_back(
            create_never_completing_future<kythira::append_entries_response<>>()
        );
    }
    
    // Start collection with long timeout
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(never_completing_futures), long_timeout);
    
    // Cancel the collection by letting it go out of scope and timing out with short wait
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // Wait for a short time, then let the future be destroyed
        auto results = std::move(collection_future).within(short_timeout).get();
        BOOST_FAIL("Collection should have timed out");
    } catch (const std::exception&) {
        // Expected timeout exception
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        
        // Verify timeout occurred within expected timeframe
        BOOST_CHECK_LE(elapsed, short_timeout + std::chrono::milliseconds{100});
    }
    
    // Test passes if we reach here without hanging or crashing
    BOOST_CHECK(true);
}

/**
 * Test: Concurrent future collections
 * 
 * Verifies that multiple future collection operations can run concurrently
 * without interfering with each other.
 * 
 * Requirements: 2.1, 2.2, 2.3
 */
BOOST_AUTO_TEST_CASE(concurrent_future_collections, * boost::unit_test::timeout(30)) {
    constexpr std::size_t concurrent_collections = 3;
    std::vector<std::future<std::vector<kythira::append_entries_response<>>>> collection_results;
    
    // Start multiple concurrent collections
    for (std::size_t collection_id = 0; collection_id < concurrent_collections; ++collection_id) {
        // Create futures for each collection
        std::vector<kythira::Future<kythira::append_entries_response<>>> futures;
        
        for (std::size_t i = 0; i < 4; ++i) {
            auto response = create_successful_append_entries_response(test_term_1);
            futures.push_back(
                create_delayed_future(
                    std::move(response), 
                    std::chrono::milliseconds{50 + collection_id * 10 + i * 5}
                )
            );
        }
        
        // Start collection and store the std::future
        auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
            std::move(futures), medium_timeout);
        
        // Convert to std::future for concurrent waiting
        collection_results.push_back(
            std::async(std::launch::async, [collection_future = std::move(collection_future)]() mutable {
                return std::move(collection_future).get();
            })
        );
    }
    
    // Wait for all collections to complete
    for (auto& result_future : collection_results) {
        auto results = result_future.get();
        
        // Verify each collection got majority responses
        BOOST_CHECK_GE(results.size(), majority_of_5 - 1); // -1 because leader counts itself
        
        // Verify all responses are successful
        for (const auto& response : results) {
            BOOST_CHECK_EQUAL(response.term(), test_term_1);
            BOOST_CHECK(response.success());
        }
    }
}

/**
 * Test: Collection with all futures timing out
 * 
 * Verifies that future collection handles the case where all futures
 * time out and no majority can be achieved.
 * 
 * Requirements: 2.4
 */
BOOST_AUTO_TEST_CASE(collection_all_futures_timeout, * boost::unit_test::timeout(30)) {
    // Create futures that all timeout
    std::vector<kythira::Future<kythira::append_entries_response<>>> timeout_futures;
    
    for (std::size_t i = 0; i < 4; ++i) {
        timeout_futures.push_back(
            create_timeout_future<kythira::append_entries_response<>>(
                short_timeout + std::chrono::milliseconds{50}
            )
        );
    }
    
    // Collect majority (should fail due to all timeouts)
    auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_majority(
        std::move(timeout_futures), short_timeout);
    
    // Wait for collection to fail
    try {
        auto results = std::move(collection_future).get();
        BOOST_FAIL("Collection should have failed due to all timeouts");
    } catch (const kythira::future_collection_exception& ex) {
        // Expected exception due to insufficient responses
        BOOST_CHECK(true);
    } catch (const std::exception& ex) {
        // Other timeout-related exceptions are also acceptable
        BOOST_CHECK(true);
    }
}

/**
 * Test: Collection strategy variations
 * 
 * Verifies that different collection strategies (all, majority, any, count)
 * work correctly with various response patterns.
 * 
 * Requirements: 2.1, 2.2, 2.3
 */
BOOST_AUTO_TEST_CASE(collection_strategy_variations, * boost::unit_test::timeout(30)) {
    using CollectionStrategy = kythira::raft_future_collector<kythira::append_entries_response<>>::collection_strategy;
    
    // Test "any" strategy - should return as soon as first future completes
    {
        std::vector<kythira::Future<kythira::append_entries_response<>>> futures;
        
        // First future completes quickly
        auto quick_response = create_successful_append_entries_response(test_term_1);
        futures.push_back(create_delayed_future(std::move(quick_response), std::chrono::milliseconds{50}));
        
        // Other futures complete slowly
        for (std::size_t i = 0; i < 3; ++i) {
            auto slow_response = create_successful_append_entries_response(test_term_1);
            futures.push_back(create_delayed_future(std::move(slow_response), std::chrono::milliseconds{300 + i * 100}));
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_with_strategy(
            std::move(futures), CollectionStrategy::any, medium_timeout);
        
        auto results = std::move(collection_future).get();
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        
        // Should complete quickly (within ~100ms) and return 1 result
        BOOST_CHECK_LE(elapsed, std::chrono::milliseconds{150});
        BOOST_CHECK_EQUAL(results.size(), 1);
        BOOST_CHECK(results[0].success());
    }
    
    // Test "count" strategy - should return when specific count is reached
    {
        std::vector<kythira::Future<kythira::append_entries_response<>>> futures;
        
        // 2 futures complete quickly
        for (std::size_t i = 0; i < 2; ++i) {
            auto quick_response = create_successful_append_entries_response(test_term_1);
            futures.push_back(create_delayed_future(std::move(quick_response), std::chrono::milliseconds{50 + i * 10}));
        }
        
        // 2 futures complete slowly
        for (std::size_t i = 0; i < 2; ++i) {
            auto slow_response = create_successful_append_entries_response(test_term_1);
            futures.push_back(create_delayed_future(std::move(slow_response), std::chrono::milliseconds{300 + i * 100}));
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        // Collect exactly 2 responses
        auto collection_future = kythira::raft_future_collector<kythira::append_entries_response<>>::collect_with_strategy(
            std::move(futures), CollectionStrategy::count, medium_timeout, 2);
        
        auto results = std::move(collection_future).get();
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        
        // Should complete quickly (within ~100ms) and return 2 results
        BOOST_CHECK_LE(elapsed, std::chrono::milliseconds{150});
        BOOST_CHECK_EQUAL(results.size(), 2);
        
        for (const auto& response : results) {
            BOOST_CHECK(response.success());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()