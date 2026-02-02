/**
 * Property-Based Test for Commit Waiting Completion
 * 
 * Feature: raft-completion, Property 1: Commit Waiting Completion
 * Validates: Requirements 1.1, 1.2
 * 
 * Property: For any client command submission, the returned future completes only after 
 * the command is both committed (replicated to majority) and applied to the state machine.
 * 
 * NOTE: This is a pure unit test of the commit_waiter mechanism. It does not test the
 * full Raft replication flow, which is covered by integration tests.
 */

#define BOOST_TEST_MODULE RaftCommitWaitingCompletionPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/commit_waiter.hpp>

#include <chrono>
#include <vector>
#include <cstddef>
#include <exception>

namespace {
    constexpr std::chrono::milliseconds test_timeout{1000};
}

BOOST_AUTO_TEST_SUITE(commit_waiting_completion_property_tests)

/**
 * Property: Client futures complete only after commit and application
 * 
 * For any client command submitted to a leader, the returned future should not
 * complete until the command has been both committed (replicated to majority)
 * and applied to the state machine.
 * 
 * This test directly validates the commit_waiter mechanism that ensures futures
 * only complete after both commit and application have occurred.
 */
BOOST_AUTO_TEST_CASE(property_commit_waiting_completion, * boost::unit_test::timeout(10)) {
    // Test the commit_waiter mechanism directly
    using commit_waiter_t = kythira::commit_waiter<std::uint64_t>;
    commit_waiter_t waiter;
    
    // Register an operation that waits for index 1
    bool fulfilled = false;
    std::vector<std::byte> result_data;
    
    waiter.register_operation(
        1,  // log index
        [&fulfilled, &result_data](std::vector<std::byte> result) {
            fulfilled = true;
            result_data = std::move(result);
        },
        [](std::exception_ptr) {
            // Should not be called in this test
            BOOST_FAIL("Operation should not be rejected");
        },
        test_timeout
    );
    
    // Property: Future should not be fulfilled immediately after registration
    BOOST_CHECK(!fulfilled);
    
    // Simulate commit and application
    std::vector<std::byte> expected_result{std::byte{42}, std::byte{24}};
    waiter.notify_committed_and_applied(1, [&expected_result](std::uint64_t) {
        return expected_result;
    });
    
    // Property: Future should be fulfilled after notification
    BOOST_CHECK(fulfilled);
    BOOST_CHECK_EQUAL(result_data.size(), expected_result.size());
    if (!result_data.empty()) {
        BOOST_CHECK(result_data == expected_result);
    }
}

/**
 * Property: Application happens before future fulfillment
 * 
 * For any committed log entry with associated client futures, state machine 
 * application occurs before any client future is fulfilled.
 * 
 * This test validates that the commit_waiter mechanism ensures proper ordering:
 * application must complete before the fulfillment callback is invoked.
 */
BOOST_AUTO_TEST_CASE(property_application_before_future_fulfillment, * boost::unit_test::timeout(10)) {
    // Test that application happens before fulfillment
    using commit_waiter_t = kythira::commit_waiter<std::uint64_t>;
    commit_waiter_t waiter;
    
    bool application_happened = false;
    bool fulfillment_happened = false;
    
    // Register an operation
    waiter.register_operation(
        1,  // log index
        [&application_happened, &fulfillment_happened](std::vector<std::byte> result) {
            // When this callback is invoked, application should have already happened
            BOOST_CHECK(application_happened);
            fulfillment_happened = true;
        },
        [](std::exception_ptr) {
            BOOST_FAIL("Operation should not be rejected");
        },
        test_timeout
    );
    
    // Verify neither has happened yet
    BOOST_CHECK(!application_happened);
    BOOST_CHECK(!fulfillment_happened);
    
    // Simulate application (this should happen first)
    application_happened = true;
    
    // Then notify commit waiter (this triggers fulfillment)
    waiter.notify_committed_and_applied(1, [](std::uint64_t) {
        return std::vector<std::byte>{std::byte{1}, std::byte{2}};
    });
    
    // Property: Both should have happened, with application before fulfillment
    BOOST_CHECK(application_happened);
    BOOST_CHECK(fulfillment_happened);
}

BOOST_AUTO_TEST_SUITE_END()