/**
 * Integration Test for Async Command Submission
 * 
 * Tests async command submission functionality including:
 * - Command submission with replication delays
 * - Concurrent command submissions
 * - Leadership changes during command processing
 * - Timeout handling for slow commits
 * - Proper ordering and linearizability
 * 
 * Requirements: 15.1, 15.2, 15.3, 15.4, 15.5
 */

#define BOOST_TEST_MODULE RaftAsyncCommandSubmissionIntegrationTest
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
#include <algorithm>
#include <mutex>
#include <condition_variable>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_async_command_submission_integration_test"), nullptr};
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
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::uint64_t test_term_2 = 2;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr std::chrono::milliseconds replication_delay{50};
    constexpr std::chrono::milliseconds slow_replication_delay{200};
    constexpr const char* test_command_1 = "command_1";
    constexpr const char* test_command_2 = "command_2";
    constexpr const char* test_command_3 = "command_3";
    constexpr const char* leadership_lost_msg = "Leadership lost during commit";
    constexpr const char* timeout_msg = "Operation timed out";
    constexpr std::size_t concurrent_command_count = 10;
}

/**
 * Mock replication simulator for testing async command submission
 */
class MockReplicationSimulator {
public:
    struct CommandRecord {
        std::uint64_t log_index;
        std::vector<std::byte> command;
        std::chrono::steady_clock::time_point submitted_at;
        std::chrono::steady_clock::time_point committed_at;
        std::chrono::steady_clock::time_point applied_at;
        bool committed{false};
        bool applied{false};
    };
    
    MockReplicationSimulator() = default;
    
    auto submit_command(
        std::uint64_t log_index,
        std::vector<std::byte> command,
        std::chrono::milliseconds replication_delay
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        CommandRecord record{
            .log_index = log_index,
            .command = std::move(command),
            .submitted_at = std::chrono::steady_clock::now(),
            .committed_at = {},
            .applied_at = {},
            .committed = false,
            .applied = false
        };
        
        _commands[log_index] = record;
        _replication_delays[log_index] = replication_delay;
    }
    
    auto simulate_replication(std::uint64_t log_index) -> bool {
        std::this_thread::sleep_for(_replication_delays[log_index]);
        
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _commands.find(log_index);
        if (it != _commands.end() && !it->second.committed) {
            it->second.committed = true;
            it->second.committed_at = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    auto simulate_application(std::uint64_t log_index) -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _commands.find(log_index);
        if (it != _commands.end() && it->second.committed && !it->second.applied) {
            it->second.applied = true;
            it->second.applied_at = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    auto get_command_record(std::uint64_t log_index) const -> std::optional<CommandRecord> {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _commands.find(log_index);
        return (it != _commands.end()) ? std::optional<CommandRecord>{it->second} : std::nullopt;
    }
    
    auto reset() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _commands.clear();
        _replication_delays.clear();
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::uint64_t, CommandRecord> _commands;
    std::unordered_map<std::uint64_t, std::chrono::milliseconds> _replication_delays;
};

BOOST_AUTO_TEST_SUITE(async_command_submission_integration_tests, * boost::unit_test::timeout(300))

/**
 * Test: Command submission with replication delays
 * 
 * Verifies that commands wait for replication to complete before
 * the future is fulfilled, even with delays.
 * 
 * Requirements: 15.1, 15.2
 */
BOOST_AUTO_TEST_CASE(command_submission_with_replication_delays, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing command submission with replication delays");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    // Submit command with replication delay
    std::vector<std::byte> command{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    simulator.submit_command(test_log_index_1, command, replication_delay);
    
    // Track completion
    std::atomic<bool> future_completed{false};
    std::atomic<bool> future_succeeded{false};
    std::vector<std::byte> result;
    
    // Register operation
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte> res) {
            result = std::move(res);
            future_succeeded = true;
            future_completed = true;
        },
        [&](std::exception_ptr) {
            future_succeeded = false;
            future_completed = true;
        },
        long_timeout
    );
    
    // Verify future doesn't complete immediately
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    BOOST_CHECK(!future_completed.load());
    
    // Simulate replication in background
    auto replication_thread = std::thread([&simulator]() {
        simulator.simulate_replication(test_log_index_1);
    });
    
    // Wait for replication
    replication_thread.join();
    
    // Verify still not completed (waiting for application)
    BOOST_CHECK(!future_completed.load());
    
    // Simulate application
    simulator.simulate_application(test_log_index_1);
    
    // Notify commit waiter (using simple overload)
    waiter.notify_committed_and_applied(test_log_index_1);
    
    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (!future_completed.load() && 
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify completion
    BOOST_CHECK(future_completed.load());
    BOOST_CHECK(future_succeeded.load());
    
    // Verify timing
    auto record = simulator.get_command_record(test_log_index_1);
    BOOST_REQUIRE(record.has_value());
    BOOST_CHECK(record->committed);
    BOOST_CHECK(record->applied);
    
    auto commit_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        record->committed_at - record->submitted_at);
    BOOST_TEST_MESSAGE("Commit latency: " << commit_latency.count() << "ms");
    BOOST_CHECK_GE(commit_latency.count(), replication_delay.count());
    
    BOOST_TEST_MESSAGE("✓ Command submission with replication delays works correctly");
}

/**
 * Test: Concurrent command submissions
 * 
 * Verifies that multiple commands can be submitted concurrently and
 * are applied in the correct log order.
 * 
 * Requirements: 15.5
 */
BOOST_AUTO_TEST_CASE(concurrent_command_submissions, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing concurrent command submissions");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    // Track completion order
    std::mutex order_mutex;
    std::vector<std::uint64_t> completion_order;
    std::atomic<std::size_t> completed_count{0};
    
    // Submit multiple commands concurrently
    std::vector<std::thread> submission_threads;
    for (std::size_t i = 0; i < concurrent_command_count; ++i) {
        submission_threads.emplace_back([&, i]() {
            std::uint64_t log_index = test_log_index_1 + i;
            std::vector<std::byte> command{std::byte(i)};
            
            simulator.submit_command(log_index, command, replication_delay);
            
            waiter.register_operation(
                log_index,
                [&, log_index](std::vector<std::byte>) {
                    std::lock_guard<std::mutex> lock(order_mutex);
                    completion_order.push_back(log_index);
                    completed_count++;
                },
                [&](std::exception_ptr) {
                    completed_count++;
                },
                long_timeout
            );
        });
    }
    
    // Wait for all submissions
    for (auto& thread : submission_threads) {
        thread.join();
    }
    
    // Simulate replication and application in log order
    for (std::size_t i = 0; i < concurrent_command_count; ++i) {
        std::uint64_t log_index = test_log_index_1 + i;
        
        // Replicate
        simulator.simulate_replication(log_index);
        
        // Apply
        simulator.simulate_application(log_index);
        
        // Notify
        waiter.notify_committed_and_applied(log_index);
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < concurrent_command_count &&
           std::chrono::steady_clock::now() - start < long_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify all completed
    BOOST_CHECK_EQUAL(completed_count.load(), concurrent_command_count);
    
    // Verify completion order matches log order
    BOOST_CHECK_EQUAL(completion_order.size(), concurrent_command_count);
    for (std::size_t i = 0; i < completion_order.size(); ++i) {
        BOOST_CHECK_EQUAL(completion_order[i], test_log_index_1 + i);
    }
    
    BOOST_TEST_MESSAGE("✓ Concurrent command submissions maintain log order");
}

/**
 * Test: Leadership changes during command processing
 * 
 * Verifies that commands are properly rejected when leadership is lost
 * before commit.
 * 
 * Requirements: 15.4
 */
BOOST_AUTO_TEST_CASE(leadership_changes_during_processing, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing leadership changes during command processing");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    // Submit multiple commands
    std::vector<std::uint64_t> log_indices = {test_log_index_1, test_log_index_2, test_log_index_3};
    std::atomic<std::size_t> succeeded_count{0};
    std::atomic<std::size_t> failed_count{0};
    std::atomic<std::size_t> completed_count{0};
    
    for (auto log_index : log_indices) {
        std::vector<std::byte> command{std::byte(log_index)};
        simulator.submit_command(log_index, command, replication_delay);
        
        waiter.register_operation(
            log_index,
            [&](std::vector<std::byte>) {
                succeeded_count++;
                completed_count++;
            },
            [&](std::exception_ptr ex) {
                failed_count++;
                completed_count++;
                
                // Verify it's a leadership lost exception
                try {
                    std::rethrow_exception(ex);
                } catch (const kythira::leadership_lost_exception<std::uint64_t>& leadership_ex) {
                    BOOST_TEST_MESSAGE("Caught expected leadership_lost_exception: " << leadership_ex.what());
                } catch (const std::exception& e) {
                    BOOST_TEST_MESSAGE("Caught unexpected exception: " << e.what());
                    BOOST_FAIL("Expected leadership_lost_exception");
                } catch (...) {
                    BOOST_TEST_MESSAGE("Caught unknown exception");
                    BOOST_FAIL("Expected leadership_lost_exception");
                }
            },
            long_timeout
        );
    }
    
    // Simulate first command succeeding
    simulator.simulate_replication(test_log_index_1);
    simulator.simulate_application(test_log_index_1);
    waiter.notify_committed_and_applied(test_log_index_1);
    
    // Wait for first command
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    // Simulate leadership loss before other commands commit
    waiter.cancel_all_operations_leadership_lost(test_term_1, test_term_2);
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < log_indices.size() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify results
    BOOST_CHECK_EQUAL(completed_count.load(), log_indices.size());
    BOOST_CHECK_EQUAL(succeeded_count.load(), 1);  // Only first command succeeded
    BOOST_CHECK_EQUAL(failed_count.load(), 2);     // Other two failed due to leadership loss
    
    BOOST_TEST_MESSAGE("✓ Leadership changes properly reject pending commands");
}

/**
 * Test: Timeout handling for slow commits
 * 
 * Verifies that operations timeout appropriately when commits take too long.
 * 
 * Requirements: 15.1, 15.4
 */
BOOST_AUTO_TEST_CASE(timeout_handling_slow_commits, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing timeout handling for slow commits");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    // Submit command with very slow replication
    std::vector<std::byte> command{std::byte{0xFF}};
    simulator.submit_command(test_log_index_1, command, slow_replication_delay);
    
    std::atomic<bool> timed_out{false};
    std::atomic<bool> completed{false};
    
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte>) {
            completed = true;
        },
        [&](std::exception_ptr ex) {
            completed = true;
            
            // Verify it's a timeout exception
            try {
                std::rethrow_exception(ex);
            } catch (const kythira::commit_timeout_exception<std::uint64_t>& timeout_ex) {
                timed_out = true;
                BOOST_TEST_MESSAGE("Caught expected commit_timeout_exception: " << timeout_ex.what());
            } catch (const std::exception& other_ex) {
                BOOST_TEST_MESSAGE("Caught exception: " << other_ex.what());
            }
        },
        short_timeout  // Short timeout to trigger timeout
    );
    
    // Start slow replication in background
    auto replication_thread = std::thread([&simulator]() {
        simulator.simulate_replication(test_log_index_1);
    });
    
    // Trigger timeout check
    std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{50});
    waiter.cancel_timed_out_operations();
    
    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (!completed.load() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    replication_thread.join();
    
    // Verify timeout occurred
    BOOST_CHECK(completed.load());
    BOOST_CHECK(timed_out.load());
    
    BOOST_TEST_MESSAGE("✓ Timeout handling works correctly for slow commits");
}

/**
 * Test: Proper ordering and linearizability
 * 
 * Verifies that commands are applied in strict log order even when
 * they complete at different times.
 * 
 * Requirements: 15.5
 */
BOOST_AUTO_TEST_CASE(proper_ordering_linearizability, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing proper ordering and linearizability");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    // Submit commands with varying replication delays
    std::vector<std::pair<std::uint64_t, std::chrono::milliseconds>> commands = {
        {test_log_index_1, std::chrono::milliseconds{100}},
        {test_log_index_2, std::chrono::milliseconds{50}},   // Faster
        {test_log_index_3, std::chrono::milliseconds{150}},
        {test_log_index_4, std::chrono::milliseconds{25}},   // Fastest
        {test_log_index_5, std::chrono::milliseconds{75}}
    };
    
    std::mutex order_mutex;
    std::vector<std::uint64_t> application_order;
    std::atomic<std::size_t> completed_count{0};
    
    // Submit all commands
    for (const auto& [log_index, delay] : commands) {
        std::vector<std::byte> command{std::byte(log_index)};
        simulator.submit_command(log_index, command, delay);
        
        waiter.register_operation(
            log_index,
            [&, log_index](std::vector<std::byte>) {
                std::lock_guard<std::mutex> lock(order_mutex);
                application_order.push_back(log_index);
                completed_count++;
            },
            [&](std::exception_ptr) {
                completed_count++;
            },
            long_timeout
        );
    }
    
    // Simulate replication with different delays (out of order completion)
    std::vector<std::thread> replication_threads;
    for (const auto& [log_index, delay] : commands) {
        replication_threads.emplace_back([&simulator, log_index]() {
            simulator.simulate_replication(log_index);
        });
    }
    
    // Wait for all replications
    for (auto& thread : replication_threads) {
        thread.join();
    }
    
    // Apply in strict log order (even though replication completed out of order)
    for (const auto& [log_index, delay] : commands) {
        simulator.simulate_application(log_index);
        waiter.notify_committed_and_applied(log_index);
    }
    
    // Wait for all completions
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < commands.size() &&
           std::chrono::steady_clock::now() - start < long_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify all completed
    BOOST_CHECK_EQUAL(completed_count.load(), commands.size());
    
    // Verify application order is strictly sequential (log order)
    BOOST_CHECK_EQUAL(application_order.size(), commands.size());
    for (std::size_t i = 0; i < application_order.size(); ++i) {
        BOOST_CHECK_EQUAL(application_order[i], test_log_index_1 + i);
        BOOST_TEST_MESSAGE("Application order[" << i << "] = " << application_order[i]);
    }
    
    BOOST_TEST_MESSAGE("✓ Commands applied in strict log order (linearizability maintained)");
}

/**
 * Test: State machine application before future fulfillment
 * 
 * Verifies that futures are only fulfilled after state machine application
 * completes successfully.
 * 
 * Requirements: 15.2
 */
BOOST_AUTO_TEST_CASE(application_before_future_fulfillment, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing state machine application before future fulfillment");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    MockReplicationSimulator simulator;
    
    std::vector<std::byte> command{std::byte{0xAA}};
    simulator.submit_command(test_log_index_1, command, replication_delay);
    
    std::atomic<bool> future_fulfilled{false};
    std::atomic<bool> application_completed{false};
    
    waiter.register_operation(
        test_log_index_1,
        [&](std::vector<std::byte>) {
            // Verify application happened before future fulfillment
            BOOST_CHECK(application_completed.load());
            future_fulfilled = true;
        },
        [&](std::exception_ptr) {
            future_fulfilled = true;
        },
        long_timeout
    );
    
    // Simulate replication
    simulator.simulate_replication(test_log_index_1);
    
    // Verify future not fulfilled yet (waiting for application)
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    BOOST_CHECK(!future_fulfilled.load());
    
    // Simulate application
    simulator.simulate_application(test_log_index_1);
    application_completed = true;
    
    // Now notify (which should fulfill the future)
    waiter.notify_committed_and_applied(test_log_index_1);
    
    // Wait for future fulfillment
    auto start = std::chrono::steady_clock::now();
    while (!future_fulfilled.load() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    BOOST_CHECK(future_fulfilled.load());
    BOOST_TEST_MESSAGE("✓ Future fulfilled only after state machine application");
}

/**
 * Test: Error propagation on application failure
 * 
 * Verifies that state machine application failures are properly
 * propagated to client futures.
 * 
 * Requirements: 15.3
 */
BOOST_AUTO_TEST_CASE(error_propagation_application_failure, * boost::unit_test::timeout(60)) {
    BOOST_TEST_MESSAGE("Testing error propagation on application failure");
    
    kythira::commit_waiter<std::uint64_t> waiter;
    
    std::atomic<bool> error_received{false};
    std::atomic<bool> completed{false};
    std::string error_message;
    
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
    
    // Simulate application failure by using a result function that throws
    auto failing_result_function = [](std::uint64_t) -> std::vector<std::byte> {
        throw std::runtime_error("State machine application failed");
    };
    
    waiter.notify_committed_and_applied(test_log_index_1, failing_result_function);
    
    // Wait for completion
    auto start = std::chrono::steady_clock::now();
    while (!completed.load() &&
           std::chrono::steady_clock::now() - start < medium_timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    BOOST_CHECK(completed.load());
    BOOST_CHECK(error_received.load());
    BOOST_CHECK(error_message.find("application failed") != std::string::npos);
    
    BOOST_TEST_MESSAGE("✓ Application failures properly propagated to futures");
}

BOOST_AUTO_TEST_SUITE_END()
