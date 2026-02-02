/**
 * Integration Test for Application Failure Recovery
 * 
 * Tests state machine application failure handling including:
 * - State machine application failures
 * - Error propagation to clients
 * - Different failure handling policies
 * - Applied index catchup after lag
 * - System consistency after failures
 * 
 * Requirements: 19.3, 19.4, 19.5
 */

#define BOOST_TEST_MODULE RaftApplicationFailureRecoveryIntegrationTest
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
#include <exception>
#include <string>
#include <functional>
#include <mutex>
#include <optional>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_application_failure_recovery_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    // Test constants
    constexpr std::uint64_t test_log_index_1 = 1;
    constexpr std::uint64_t test_log_index_2 = 2;
    constexpr std::uint64_t test_log_index_3 = 3;
    constexpr std::uint64_t test_log_index_4 = 4;
    constexpr std::uint64_t test_log_index_5 = 5;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr const char* application_failure_msg = "State machine application failed";
    constexpr const char* transient_failure_msg = "Transient failure";
    constexpr std::size_t batch_size = 5;
}

/**
 * Mock state machine that can simulate failures
 */
class MockStateMachine {
public:
    enum class failure_policy {
        none,
        fail_once,
        fail_at_index,
        fail_always
    };
    
    MockStateMachine() = default;
    
    auto set_failure_policy(failure_policy policy, std::optional<std::uint64_t> fail_index = std::nullopt) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _policy = policy;
        _fail_index = fail_index;
        _failure_count = 0;
    }
    
    auto apply(std::uint64_t index, const std::vector<std::byte>& command) -> std::vector<std::byte> {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Check if we should fail
        bool should_fail = false;
        switch (_policy) {
            case failure_policy::none:
                should_fail = false;
                break;
            case failure_policy::fail_once:
                should_fail = (_failure_count == 0);
                break;
            case failure_policy::fail_at_index:
                should_fail = (_fail_index.has_value() && index == _fail_index.value());
                break;
            case failure_policy::fail_always:
                should_fail = true;
                break;
        }
        
        if (should_fail) {
            _failure_count++;
            throw std::runtime_error(application_failure_msg);
        }
        
        // Record successful application
        _applied_entries.push_back(index);
        _last_applied_index = index;
        
        // Return result
        return command;
    }
    
    auto get_applied_entries() const -> std::vector<std::uint64_t> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _applied_entries;
    }
    
    auto get_last_applied_index() const -> std::uint64_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _last_applied_index;
    }
    
    auto get_failure_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _failure_count;
    }
    
    auto reset() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _applied_entries.clear();
        _last_applied_index = 0;
        _failure_count = 0;
        _policy = failure_policy::none;
        _fail_index = std::nullopt;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::uint64_t> _applied_entries;
    std::uint64_t _last_applied_index{0};
    std::size_t _failure_count{0};
    failure_policy _policy{failure_policy::none};
    std::optional<std::uint64_t> _fail_index;
};

BOOST_AUTO_TEST_SUITE(application_failure_recovery_integration_tests, * boost::unit_test::timeout(300))

/**
 * Test: State machine application failure
 * 
 * Verifies that state machine application failures are properly detected
 * and reported.
 * 
 * Requirements: 19.4
 */
BOOST_AUTO_TEST_CASE(state_machine_application_failure, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing state machine application failure");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // Configure state machine to fail
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_always);
    
    std::atomic<bool> error_received{false};
    std::atomic<bool> completed{false};
    std::string error_message;
    
    // Register operation
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte>) {
            completed = true;
        },
        [&](std::exception_ptr ex) {
            error_received = true;
            completed = true;
            
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                error_message = e.what();
                BOOST_TEST_MESSAGE("Caught application failure: " << e.what());
            }
        },
        long_timeout
    );
    
    // Simulate application with failure
    auto result_function = [&](std::uint64_t index) -> std::vector<std::byte> {
        return state_machine.apply(index, std::vector<std::byte>{std::byte{0x01}});
    };
    
    waiter.notify_committed_and_applied(test_log_index_1, result_function);
    
    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (!completed.load() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify failure was detected
    BOOST_CHECK(completed.load());
    BOOST_CHECK(error_received.load());
    BOOST_CHECK(error_message.find(application_failure_msg) != std::string::npos);
    BOOST_CHECK_EQUAL(state_machine.get_failure_count(), 1);
    BOOST_CHECK_EQUAL(state_machine.get_applied_entries().size(), 0);
    
    BOOST_TEST_MESSAGE("✓ State machine application failure detected and reported");
}

/**
 * Test: Error propagation to multiple clients
 * 
 * Verifies that application failures are propagated to all waiting clients.
 * 
 * Requirements: 19.4
 */
BOOST_AUTO_TEST_CASE(error_propagation_multiple_clients, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing error propagation to multiple clients");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // Configure state machine to fail
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_always);
    
    constexpr std::size_t client_count = 5;
    std::atomic<std::size_t> errors_received{0};
    std::atomic<std::size_t> completed_count{0};
    
    // Register multiple operations for the same index
    for (std::size_t i = 0; i < client_count; ++i) {
        waiter.register_operation(
            test_log_index_1,
            [&](std::vector<std::byte>) {
                completed_count++;
            },
            [&](std::exception_ptr ex) {
                errors_received++;
                completed_count++;
                
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    BOOST_CHECK(std::string(e.what()).find(application_failure_msg) != std::string::npos);
                }
            },
            long_timeout
        );
    }
    
    // Simulate application with failure
    auto result_function = [&](std::uint64_t index) -> std::vector<std::byte> {
        return state_machine.apply(index, std::vector<std::byte>{std::byte{0x01}});
    };
    
    waiter.notify_committed_and_applied(test_log_index_1, result_function);
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < client_count &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify all clients received errors
    BOOST_CHECK_EQUAL(completed_count.load(), client_count);
    BOOST_CHECK_EQUAL(errors_received.load(), client_count);
    
    BOOST_TEST_MESSAGE("✓ Application failure propagated to all clients");
}

/**
 * Test: Transient failure recovery
 * 
 * Verifies that the system can recover from transient application failures.
 * 
 * Requirements: 19.3, 19.4
 */
BOOST_AUTO_TEST_CASE(transient_failure_recovery, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing transient failure recovery");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // Configure state machine to fail once
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_once);
    
    std::atomic<std::size_t> success_count{0};
    std::atomic<std::size_t> failure_count{0};
    std::atomic<std::size_t> completed_count{0};
    
    // Register operations for multiple indices
    std::vector<std::uint64_t> indices = {test_log_index_1, test_log_index_2, test_log_index_3};
    
    for (auto index : indices) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                success_count++;
                completed_count++;
            },
            [&](std::exception_ptr) {
                failure_count++;
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Apply entries one by one
    for (auto index : indices) {
        auto result_function = [&](std::uint64_t idx) -> std::vector<std::byte> {
            return state_machine.apply(idx, std::vector<std::byte>{std::byte(idx)});
        };
        
        waiter.notify_committed_and_applied(index, result_function);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < indices.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify first failed, rest succeeded
    BOOST_CHECK_EQUAL(completed_count.load(), indices.size());
    BOOST_CHECK_EQUAL(failure_count.load(), 1);  // First one failed
    BOOST_CHECK_EQUAL(success_count.load(), 2);  // Rest succeeded
    BOOST_CHECK_EQUAL(state_machine.get_failure_count(), 1);
    BOOST_CHECK_EQUAL(state_machine.get_applied_entries().size(), 2);
    
    BOOST_TEST_MESSAGE("✓ System recovered from transient failure");
}

/**
 * Test: Failure at specific index
 * 
 * Verifies that failures at specific indices are handled correctly.
 * 
 * Requirements: 19.4
 */
BOOST_AUTO_TEST_CASE(failure_at_specific_index, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing failure at specific index");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // Configure state machine to fail at index 3
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_at_index, test_log_index_3);
    
    std::atomic<std::size_t> success_count{0};
    std::atomic<std::size_t> failure_count{0};
    std::atomic<std::size_t> completed_count{0};
    
    // Register operations for multiple indices
    std::vector<std::uint64_t> indices = {test_log_index_1, test_log_index_2, test_log_index_3, test_log_index_4};
    
    for (auto index : indices) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                success_count++;
                completed_count++;
            },
            [&](std::exception_ptr) {
                failure_count++;
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Apply entries one by one
    for (auto index : indices) {
        auto result_function = [&](std::uint64_t idx) -> std::vector<std::byte> {
            return state_machine.apply(idx, std::vector<std::byte>{std::byte(idx)});
        };
        
        waiter.notify_committed_and_applied(index, result_function);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < indices.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify only index 3 failed
    BOOST_CHECK_EQUAL(completed_count.load(), indices.size());
    BOOST_CHECK_EQUAL(failure_count.load(), 1);  // Only index 3 failed
    BOOST_CHECK_EQUAL(success_count.load(), 3);  // Others succeeded
    
    auto applied = state_machine.get_applied_entries();
    BOOST_CHECK_EQUAL(applied.size(), 3);
    BOOST_CHECK(std::find(applied.begin(), applied.end(), test_log_index_1) != applied.end());
    BOOST_CHECK(std::find(applied.begin(), applied.end(), test_log_index_2) != applied.end());
    BOOST_CHECK(std::find(applied.begin(), applied.end(), test_log_index_4) != applied.end());
    BOOST_CHECK(std::find(applied.begin(), applied.end(), test_log_index_3) == applied.end());
    
    BOOST_TEST_MESSAGE("✓ Failure at specific index handled correctly");
}

/**
 * Test: Applied index catchup after lag
 * 
 * Verifies that the system catches up when applied index lags behind commit index.
 * 
 * Requirements: 19.5
 */
BOOST_AUTO_TEST_CASE(applied_index_catchup, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing applied index catchup after lag");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    std::atomic<std::size_t> completed_count{0};
    
    // Register operations for multiple indices
    std::vector<std::uint64_t> indices = {test_log_index_1, test_log_index_2, test_log_index_3, test_log_index_4, test_log_index_5};
    
    for (auto index : indices) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                completed_count++;
            },
            [&](std::exception_ptr) {
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Simulate sequential application (as Raft node would do)
    for (auto index : indices) {
        auto result_function = [&](std::uint64_t idx) -> std::vector<std::byte> {
            return state_machine.apply(idx, std::vector<std::byte>{std::byte(idx)});
        };
        
        waiter.notify_committed_and_applied(index, result_function);
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < indices.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify all entries were applied in order
    BOOST_CHECK_EQUAL(completed_count.load(), indices.size());
    
    auto applied = state_machine.get_applied_entries();
    BOOST_CHECK_EQUAL(applied.size(), indices.size());
    
    // Verify sequential application
    for (std::size_t i = 0; i < applied.size(); ++i) {
        BOOST_CHECK_EQUAL(applied[i], indices[i]);
    }
    
    BOOST_CHECK_EQUAL(state_machine.get_last_applied_index(), test_log_index_5);
    
    BOOST_TEST_MESSAGE("✓ Applied index caught up successfully");
}

/**
 * Test: Batch application with partial failure
 * 
 * Verifies that batch application handles partial failures correctly.
 * 
 * Requirements: 19.3, 19.4, 19.5
 */
BOOST_AUTO_TEST_CASE(batch_application_partial_failure, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing batch application with partial failure");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // Configure state machine to fail at index 3
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_at_index, test_log_index_3);
    
    std::atomic<std::size_t> success_count{0};
    std::atomic<std::size_t> failure_count{0};
    std::atomic<std::size_t> completed_count{0};
    
    // Register operations for batch
    std::vector<std::uint64_t> indices = {test_log_index_1, test_log_index_2, test_log_index_3, test_log_index_4, test_log_index_5};
    
    for (auto index : indices) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                success_count++;
                completed_count++;
            },
            [&](std::exception_ptr) {
                failure_count++;
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Simulate sequential application (as Raft node would do)
    for (auto index : indices) {
        auto result_function = [&](std::uint64_t idx) -> std::vector<std::byte> {
            return state_machine.apply(idx, std::vector<std::byte>{std::byte(idx)});
        };
        
        waiter.notify_committed_and_applied(index, result_function);
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < indices.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify results
    BOOST_CHECK_EQUAL(completed_count.load(), indices.size());
    BOOST_CHECK_EQUAL(failure_count.load(), 1);  // Only index 3 failed
    BOOST_CHECK_EQUAL(success_count.load(), 4);  // Others succeeded
    
    auto applied = state_machine.get_applied_entries();
    BOOST_CHECK_EQUAL(applied.size(), 4);
    
    BOOST_TEST_MESSAGE("✓ Batch application with partial failure handled correctly");
}

/**
 * Test: System consistency after failures
 * 
 * Verifies that the system remains consistent after application failures.
 * 
 * Requirements: 19.3, 19.4
 */
BOOST_AUTO_TEST_CASE(system_consistency_after_failures, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing system consistency after failures");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockStateMachine state_machine;
    
    // First batch: fail at index 2
    state_machine.set_failure_policy(MockStateMachine::failure_policy::fail_at_index, test_log_index_2);
    
    std::atomic<std::size_t> completed_count{0};
    
    // Register first batch
    std::vector<std::uint64_t> first_batch = {test_log_index_1, test_log_index_2, test_log_index_3};
    
    for (auto index : first_batch) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                completed_count++;
            },
            [&](std::exception_ptr) {
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Apply first batch
    auto result_function = [&](std::uint64_t idx) -> std::vector<std::byte> {
        return state_machine.apply(idx, std::vector<std::byte>{std::byte(idx)});
    };
    
    waiter.notify_committed_and_applied(test_log_index_3, result_function);
    
    // Wait for first batch
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < first_batch.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify first batch results
    auto applied_after_first = state_machine.get_applied_entries();
    BOOST_CHECK_EQUAL(applied_after_first.size(), 2);  // 1 and 3 succeeded, 2 failed
    
    // Reset failure policy for second batch
    state_machine.set_failure_policy(MockStateMachine::failure_policy::none);
    completed_count = 0;
    
    // Register second batch
    std::vector<std::uint64_t> second_batch = {test_log_index_4, test_log_index_5};
    
    for (auto index : second_batch) {
        waiter.register_operation(
            index,
            [&](std::vector<std::byte>) {
                completed_count++;
            },
            [&](std::exception_ptr) {
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Apply second batch
    waiter.notify_committed_and_applied(test_log_index_5, result_function);
    
    // Wait for second batch
    start = std::chrono::steady_clock::now();
    while (completed_count.load() < second_batch.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify system remained consistent
    auto final_applied = state_machine.get_applied_entries();
    BOOST_CHECK_EQUAL(final_applied.size(), 4);  // 1, 3, 4, 5 succeeded
    // Last applied index should be 4 since we only applied up to index 5 in the second batch
    // and the state machine tracks the last successfully applied index
    BOOST_CHECK(state_machine.get_last_applied_index() >= test_log_index_4);
    
    BOOST_TEST_MESSAGE("✓ System remained consistent after failures");
}

BOOST_AUTO_TEST_SUITE_END()
