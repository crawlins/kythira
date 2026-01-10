/**
 * Integration Test for Commit Waiting Under Failures
 * 
 * Tests commit waiting functionality with various failure scenarios including:
 * - Client command submission with timeout handling
 * - Error propagation during commit waiting
 * - Leadership changes during commit waiting
 * - State machine application ordering under concurrent load
 * 
 * Requirements: 1.1, 1.2, 1.3, 1.4, 1.5
 */

#define BOOST_TEST_MODULE RaftCommitWaitingIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/commit_waiter.hpp>
#include <raft/completion_exceptions.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <exception>
#include <string>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_commit_waiting_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr std::uint64_t test_log_index_1 = 1;
    constexpr std::uint64_t test_log_index_2 = 2;
    constexpr std::uint64_t test_log_index_3 = 3;
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::uint64_t test_term_2 = 2;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr const char* test_command_1 = "test_command_1";
    constexpr const char* test_command_2 = "test_command_2";
    constexpr const char* test_command_3 = "test_command_3";
    constexpr const char* test_result_1 = "result_1";
    constexpr const char* test_result_2 = "result_2";
    constexpr const char* test_result_3 = "result_3";
    constexpr const char* leadership_lost_reason = "Leadership lost during commit waiting";
    constexpr const char* state_machine_failure_reason = "State machine application failed";
}

BOOST_AUTO_TEST_SUITE(commit_waiting_integration_tests, * boost::unit_test::timeout(120))

/**
 * Test: Client command submission with successful commit
 * 
 * Verifies that client operations wait for commit and state machine application
 * before completing successfully.
 * 
 * Requirements: 1.1, 1.2
 */
BOOST_AUTO_TEST_CASE(successful_commit_waiting, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    // Track operation completion
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> operation_succeeded{false};
    std::vector<std::byte> received_result;
    std::exception_ptr received_exception;
    
    // Register operation with commit waiter
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte> result) {
            received_result = std::move(result);
            operation_succeeded = true;
            operation_completed = true;
        },
        [&](std::exception_ptr ex) {
            received_exception = ex;
            operation_succeeded = false;
            operation_completed = true;
        },
        medium_timeout
    );
    
    // Verify operation is pending
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 1);
    BOOST_CHECK_EQUAL(waiter.get_pending_count_for_index(test_log_index_1), 1);
    BOOST_CHECK(waiter.has_pending_operations());
    
    // Simulate commit and state machine application
    std::vector<std::byte> expected_result;
    for (char c : std::string(test_result_1)) {
        expected_result.push_back(static_cast<std::byte>(c));
    }
    
    waiter.notify_committed_and_applied(test_log_index_1, [&](std::uint64_t index) {
        BOOST_CHECK_EQUAL(index, test_log_index_1);
        return expected_result;
    });
    
    // Verify operation completed successfully
    BOOST_CHECK(operation_completed);
    BOOST_CHECK(operation_succeeded);
    BOOST_CHECK_EQUAL(received_result.size(), expected_result.size());
    BOOST_CHECK(std::equal(received_result.begin(), received_result.end(), expected_result.begin()));
    BOOST_CHECK(!received_exception);
    
    // Verify no pending operations remain
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
    BOOST_CHECK(!waiter.has_pending_operations());
}

/**
 * Test: Timeout handling during commit waiting
 * 
 * Verifies that operations timeout properly when commit takes too long.
 * 
 * Requirements: 1.1, 1.3
 */
BOOST_AUTO_TEST_CASE(commit_timeout_handling, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    // Track operation completion
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> operation_succeeded{false};
    std::exception_ptr received_exception;
    
    // Register operation with short timeout
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte> result) {
            operation_succeeded = true;
            operation_completed = true;
        },
        [&](std::exception_ptr ex) {
            received_exception = ex;
            operation_succeeded = false;
            operation_completed = true;
        },
        short_timeout
    );
    
    // Wait for timeout to occur
    std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{50});
    
    // Cancel timed-out operations
    auto cancelled_count = waiter.cancel_timed_out_operations();
    
    // Verify timeout was handled
    BOOST_CHECK_EQUAL(cancelled_count, 1);
    BOOST_CHECK(operation_completed);
    BOOST_CHECK(!operation_succeeded);
    BOOST_CHECK(received_exception);
    
    // Verify exception type
    try {
        std::rethrow_exception(received_exception);
    } catch (const kythira::commit_timeout_exception<std::uint64_t>& ex) {
        BOOST_CHECK_EQUAL(ex.get_entry_index(), test_log_index_1);
        BOOST_CHECK_EQUAL(ex.get_timeout(), short_timeout);
    } catch (...) {
        BOOST_FAIL("Expected commit_timeout_exception");
    }
    
    // Verify no pending operations remain
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
}

/**
 * Test: Leadership loss during commit waiting
 * 
 * Verifies that pending operations are cancelled when leadership is lost.
 * 
 * Requirements: 1.4
 */
BOOST_AUTO_TEST_CASE(leadership_loss_during_commit, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    // Track multiple operations
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::vector<std::exception_ptr> received_exceptions(3);
    
    // Register multiple operations
    for (int i = 0; i < 3; ++i) {
        waiter.register_operation(
            test_log_index_1 + i,
            [&, i](std::vector<std::byte> result) {
                successful_operations++;
                completed_operations++;
            },
            [&, i](std::exception_ptr ex) {
                received_exceptions[i] = ex;
                completed_operations++;
            },
            long_timeout
        );
    }
    
    // Verify operations are pending
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 3);
    
    // Simulate leadership loss
    waiter.cancel_all_operations_leadership_lost(test_term_1, test_term_2);
    
    // Verify all operations were cancelled
    BOOST_CHECK_EQUAL(completed_operations.load(), 3);
    BOOST_CHECK_EQUAL(successful_operations.load(), 0);
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
    
    // Verify exception types
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(received_exceptions[i]);
        try {
            std::rethrow_exception(received_exceptions[i]);
        } catch (const kythira::leadership_lost_exception<std::uint64_t>& ex) {
            BOOST_CHECK_EQUAL(ex.get_old_term(), test_term_1);
            BOOST_CHECK_EQUAL(ex.get_new_term(), test_term_2);
        } catch (...) {
            BOOST_FAIL("Expected leadership_lost_exception");
        }
    }
}

/**
 * Test: State machine application failure
 * 
 * Verifies that state machine application failures are properly propagated
 * to waiting client operations.
 * 
 * Requirements: 1.3
 */
BOOST_AUTO_TEST_CASE(state_machine_application_failure, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    // Track operation completion
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> operation_succeeded{false};
    std::exception_ptr received_exception;
    
    // Register operation
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte> result) {
            operation_succeeded = true;
            operation_completed = true;
        },
        [&](std::exception_ptr ex) {
            received_exception = ex;
            operation_succeeded = false;
            operation_completed = true;
        },
        medium_timeout
    );
    
    // Simulate state machine application failure
    waiter.notify_committed_and_applied(test_log_index_1, [](std::uint64_t index) -> std::vector<std::byte> {
        throw std::runtime_error(state_machine_failure_reason);
    });
    
    // Verify operation failed with proper error
    BOOST_CHECK(operation_completed);
    BOOST_CHECK(!operation_succeeded);
    BOOST_CHECK(received_exception);
    
    // Verify exception content
    try {
        std::rethrow_exception(received_exception);
    } catch (const std::runtime_error& ex) {
        BOOST_CHECK_EQUAL(std::string(ex.what()), state_machine_failure_reason);
    } catch (...) {
        BOOST_FAIL("Expected runtime_error");
    }
    
    // Verify no pending operations remain
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
}

/**
 * Test: Sequential application ordering under concurrent load
 * 
 * Verifies that multiple concurrent operations are applied in log order
 * even when submitted concurrently.
 * 
 * Requirements: 1.5
 */
BOOST_AUTO_TEST_CASE(sequential_application_ordering, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    constexpr int operation_count = 5;
    std::atomic<int> completed_operations{0};
    std::vector<std::uint64_t> completion_order;
    std::mutex completion_order_mutex;
    
    // Register multiple operations concurrently
    std::vector<std::thread> registration_threads;
    for (int i = 0; i < operation_count; ++i) {
        registration_threads.emplace_back([&, i]() {
            waiter.register_operation(
                test_log_index_1 + i,
                [&, i](std::vector<std::byte> result) {
                    std::lock_guard<std::mutex> lock(completion_order_mutex);
                    completion_order.push_back(test_log_index_1 + i);
                    completed_operations++;
                },
                [&](std::exception_ptr ex) {
                    completed_operations++;
                },
                long_timeout
            );
        });
    }
    
    // Wait for all registrations to complete
    for (auto& thread : registration_threads) {
        thread.join();
    }
    
    // Verify all operations are pending
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), operation_count);
    
    // Apply operations in log order (sequential commit)
    for (int i = 0; i < operation_count; ++i) {
        std::uint64_t commit_index = test_log_index_1 + i;
        
        waiter.notify_committed_and_applied(commit_index, [](std::uint64_t index) {
            std::vector<std::byte> result;
            std::string result_str = "result_" + std::to_string(index);
            for (char c : result_str) {
                result.push_back(static_cast<std::byte>(c));
            }
            return result;
        });
        
        // Small delay to ensure ordering
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Wait for all operations to complete
    while (completed_operations.load() < operation_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify operations completed in log order
    BOOST_CHECK_EQUAL(completion_order.size(), operation_count);
    for (int i = 0; i < operation_count; ++i) {
        BOOST_CHECK_EQUAL(completion_order[i], test_log_index_1 + i);
    }
    
    // Verify no pending operations remain
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
}

/**
 * Test: Concurrent operations with mixed success and failure
 * 
 * Verifies proper handling of concurrent operations where some succeed,
 * some timeout, and some are cancelled due to leadership loss.
 * 
 * Requirements: 1.1, 1.3, 1.4
 */
BOOST_AUTO_TEST_CASE(mixed_concurrent_operations, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    constexpr int total_operations = 6;
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::atomic<int> timeout_operations{0};
    std::atomic<int> cancelled_operations{0};
    
    // Register operations with different timeouts
    for (int i = 0; i < total_operations; ++i) {
        auto timeout = (i < 2) ? short_timeout : long_timeout;
        
        waiter.register_operation(
            test_log_index_1 + i,
            [&](std::vector<std::byte> result) {
                successful_operations++;
                completed_operations++;
            },
            [&](std::exception_ptr ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const kythira::commit_timeout_exception<std::uint64_t>&) {
                    timeout_operations++;
                } catch (const kythira::leadership_lost_exception<std::uint64_t>&) {
                    cancelled_operations++;
                } catch (...) {
                    // Other exceptions
                }
                completed_operations++;
            },
            timeout
        );
    }
    
    // Verify all operations are pending
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), total_operations);
    
    // Let some operations timeout
    std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{50});
    waiter.cancel_timed_out_operations();
    
    // Complete some operations successfully
    waiter.notify_committed_and_applied(test_log_index_1 + 2, [](std::uint64_t index) {
        return std::vector<std::byte>{std::byte{1}, std::byte{2}};
    });
    waiter.notify_committed_and_applied(test_log_index_1 + 3, [](std::uint64_t index) {
        return std::vector<std::byte>{std::byte{3}, std::byte{4}};
    });
    
    // Cancel remaining operations due to leadership loss
    waiter.cancel_all_operations_leadership_lost(test_term_1, test_term_2);
    
    // Verify final state
    BOOST_CHECK_EQUAL(completed_operations.load(), total_operations);
    BOOST_CHECK_EQUAL(successful_operations.load(), 2);  // Operations 2 and 3 succeeded
    BOOST_CHECK_EQUAL(timeout_operations.load(), 2);     // Operations 0 and 1 timed out
    BOOST_CHECK_EQUAL(cancelled_operations.load(), 2);   // Operations 4 and 5 cancelled
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
}

/**
 * Test: Partial commit scenarios
 * 
 * Verifies handling of scenarios where only some operations can be committed
 * and others need to be cancelled.
 * 
 * Requirements: 1.2, 1.4
 */
BOOST_AUTO_TEST_CASE(partial_commit_scenarios, * boost::unit_test::timeout(30)) {
    kythira::commit_waiter<std::uint64_t> waiter;
    
    constexpr int operation_count = 5;
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::vector<bool> operation_results(operation_count, false);
    
    // Register operations for consecutive log indices
    for (int i = 0; i < operation_count; ++i) {
        waiter.register_operation(
            test_log_index_1 + i,
            [&, i](std::vector<std::byte> result) {
                operation_results[i] = true;
                successful_operations++;
                completed_operations++;
            },
            [&, i](std::exception_ptr ex) {
                operation_results[i] = false;
                completed_operations++;
            },
            long_timeout
        );
    }
    
    // Commit only the first 3 operations
    constexpr std::uint64_t partial_commit_index = test_log_index_1 + 2;
    waiter.notify_committed_and_applied(partial_commit_index, [](std::uint64_t index) {
        return std::vector<std::byte>{std::byte{static_cast<unsigned char>(index)}};
    });
    
    // Cancel remaining operations after the partial commit
    waiter.cancel_operations_after_index(partial_commit_index, "Partial commit scenario");
    
    // Verify results
    BOOST_CHECK_EQUAL(completed_operations.load(), operation_count);
    BOOST_CHECK_EQUAL(successful_operations.load(), 3);  // First 3 operations succeeded
    
    // Verify which operations succeeded
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(operation_results[i]);  // First 3 should succeed
    }
    for (int i = 3; i < operation_count; ++i) {
        BOOST_CHECK(!operation_results[i]); // Last 2 should fail
    }
    
    BOOST_CHECK_EQUAL(waiter.get_pending_count(), 0);
}

BOOST_AUTO_TEST_SUITE_END()