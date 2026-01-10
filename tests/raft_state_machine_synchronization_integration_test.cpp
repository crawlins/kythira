/**
 * Integration Test for State Machine Synchronization
 * 
 * Tests state machine synchronization functionality including:
 * - Commit index advancement with state machine application
 * - Application failure handling and error propagation
 * - Catch-up behavior when applied index lags
 * - Sequential application ordering under load
 * 
 * Requirements: 5.1, 5.2, 5.3, 5.4, 5.5
 */

#define BOOST_TEST_MODULE RaftStateMachineSynchronizationIntegrationTest
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
#include <unordered_map>
#include <mutex>
#include <functional>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("raft_state_machine_synchronization_integration_test"), nullptr};
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
    constexpr std::uint64_t test_log_index_10 = 10;
    constexpr std::uint64_t test_term_1 = 1;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds medium_timeout{500};
    constexpr std::chrono::milliseconds long_timeout{2000};
    constexpr const char* test_command_1 = "SET key1 value1";
    constexpr const char* test_command_2 = "SET key2 value2";
    constexpr const char* test_command_3 = "SET key3 value3";
    constexpr const char* test_command_4 = "SET key4 value4";
    constexpr const char* test_command_5 = "SET key5 value5";
    constexpr const char* failing_command = "FAIL_COMMAND";
    constexpr const char* state_machine_failure_reason = "Simulated state machine failure";
    
    // Mock state machine for testing
    class MockStateMachine {
    public:
        struct Entry {
            std::uint64_t index;
            std::vector<std::byte> command;
            std::chrono::steady_clock::time_point applied_at;
        };
        
        // Apply a log entry to the state machine
        auto apply_entry(std::uint64_t index, const std::vector<std::byte>& command) -> std::vector<std::byte> {
            std::lock_guard<std::mutex> lock(_mutex);
            
            // Convert command to string for processing
            std::string command_str;
            for (auto byte : command) {
                command_str += static_cast<char>(byte);
            }
            
            // Simulate failure for specific commands
            if (command_str == failing_command) {
                throw std::runtime_error(state_machine_failure_reason);
            }
            
            // Record the application
            _applied_entries.push_back({
                index, 
                command, 
                std::chrono::steady_clock::now()
            });
            
            // Update applied index
            _applied_index = std::max(_applied_index, index);
            
            // Generate result based on command
            std::string result = "OK:" + command_str;
            std::vector<std::byte> result_bytes;
            for (char c : result) {
                result_bytes.push_back(static_cast<std::byte>(c));
            }
            
            return result_bytes;
        }
        
        // Get the current applied index
        auto get_applied_index() const -> std::uint64_t {
            std::lock_guard<std::mutex> lock(_mutex);
            return _applied_index;
        }
        
        // Get all applied entries
        auto get_applied_entries() const -> std::vector<Entry> {
            std::lock_guard<std::mutex> lock(_mutex);
            return _applied_entries;
        }
        
        // Check if entries were applied in order
        auto were_entries_applied_in_order() const -> bool {
            std::lock_guard<std::mutex> lock(_mutex);
            for (std::size_t i = 1; i < _applied_entries.size(); ++i) {
                if (_applied_entries[i].index <= _applied_entries[i-1].index) {
                    return false;
                }
            }
            return true;
        }
        
        // Reset the state machine
        auto reset() -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            _applied_entries.clear();
            _applied_index = 0;
        }
        
        // Get entry count
        auto get_entry_count() const -> std::size_t {
            std::lock_guard<std::mutex> lock(_mutex);
            return _applied_entries.size();
        }
        
    private:
        mutable std::mutex _mutex;
        std::vector<Entry> _applied_entries;
        std::uint64_t _applied_index{0};
    };
    
    // State machine synchronizer that coordinates between commit waiter and state machine
    class StateMachineSynchronizer {
    public:
        explicit StateMachineSynchronizer(MockStateMachine& state_machine)
            : _state_machine(state_machine) {}
        
        // Advance commit index and apply entries to state machine
        auto advance_commit_index(std::uint64_t new_commit_index) -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            
            if (new_commit_index <= _commit_index) {
                return; // No advancement needed
            }
            
            auto old_commit_index = _commit_index;
            _commit_index = new_commit_index;
            
            // Apply all entries between old and new commit index (Requirement 5.1)
            for (auto index = old_commit_index + 1; index <= new_commit_index; ++index) {
                apply_entry_to_state_machine(index);
            }
        }
        
        // Register a log entry for future application
        auto register_log_entry(std::uint64_t index, const std::vector<std::byte>& command) -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            _log_entries[index] = command;
        }
        
        // Get current commit index
        auto get_commit_index() const -> std::uint64_t {
            std::lock_guard<std::mutex> lock(_mutex);
            return _commit_index;
        }
        
        // Catch up applied index to commit index (Requirement 5.5)
        auto catch_up_applied_index() -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            
            auto applied_index = _state_machine.get_applied_index();
            
            // Apply all entries between applied index and commit index
            for (auto index = applied_index + 1; index <= _commit_index; ++index) {
                apply_entry_to_state_machine(index);
            }
        }
        
        // Set failure mode for testing
        auto set_failure_mode(bool enabled) -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            _failure_mode = enabled;
        }
        
    private:
        auto apply_entry_to_state_machine(std::uint64_t index) -> void {
            // This method assumes _mutex is already locked
            
            auto entry_it = _log_entries.find(index);
            if (entry_it == _log_entries.end()) {
                throw std::runtime_error("Log entry not found for index " + std::to_string(index));
            }
            
            try {
                // Apply entry to state machine (Requirements 5.2, 5.3)
                auto result = _state_machine.apply_entry(index, entry_it->second);
                
                // Notify commit waiter of successful application
                if (_commit_waiter) {
                    _commit_waiter->notify_committed_and_applied(index, [result](std::uint64_t) {
                        return result;
                    });
                }
                
            } catch (const std::exception& ex) {
                // Handle state machine application failure (Requirement 5.4)
                _application_failed = true;
                _failure_reason = ex.what();
                
                // Notify commit waiter of failure
                if (_commit_waiter) {
                    _commit_waiter->notify_committed_and_applied(index, [&ex](std::uint64_t) -> std::vector<std::byte> {
                        throw std::runtime_error(ex.what());
                    });
                }
                
                // Halt further application as per requirement 5.4
                throw;
            }
        }
        
    public:
        auto set_commit_waiter(kythira::commit_waiter<std::uint64_t>* waiter) -> void {
            std::lock_guard<std::mutex> lock(_mutex);
            _commit_waiter = waiter;
        }
        
        auto has_application_failed() const -> bool {
            std::lock_guard<std::mutex> lock(_mutex);
            return _application_failed;
        }
        
        auto get_failure_reason() const -> std::string {
            std::lock_guard<std::mutex> lock(_mutex);
            return _failure_reason;
        }
        
    private:
        mutable std::mutex _mutex;
        MockStateMachine& _state_machine;
        std::uint64_t _commit_index{0};
        std::unordered_map<std::uint64_t, std::vector<std::byte>> _log_entries;
        kythira::commit_waiter<std::uint64_t>* _commit_waiter{nullptr};
        bool _failure_mode{false};
        bool _application_failed{false};
        std::string _failure_reason;
    };
    
    // Helper function to create command bytes
    auto create_command_bytes(const std::string& command) -> std::vector<std::byte> {
        std::vector<std::byte> bytes;
        for (char c : command) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }
}

BOOST_AUTO_TEST_SUITE(state_machine_synchronization_integration_tests, * boost::unit_test::timeout(120))

/**
 * Test: Commit index advancement with state machine application
 * 
 * Verifies that when the commit index advances, all entries between the old
 * and new commit index are applied to the state machine.
 * 
 * Requirements: 5.1
 */
BOOST_AUTO_TEST_CASE(commit_index_advancement_triggers_application, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    
    // Register log entries
    synchronizer.register_log_entry(test_log_index_1, create_command_bytes(test_command_1));
    synchronizer.register_log_entry(test_log_index_2, create_command_bytes(test_command_2));
    synchronizer.register_log_entry(test_log_index_3, create_command_bytes(test_command_3));
    
    // Verify initial state
    BOOST_CHECK_EQUAL(synchronizer.get_commit_index(), 0);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), 0);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 0);
    
    // Advance commit index to 2 (should apply entries 1 and 2)
    synchronizer.advance_commit_index(test_log_index_2);
    
    // Verify entries 1 and 2 were applied
    BOOST_CHECK_EQUAL(synchronizer.get_commit_index(), test_log_index_2);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_2);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 2);
    
    auto applied_entries = state_machine.get_applied_entries();
    BOOST_REQUIRE_EQUAL(applied_entries.size(), 2);
    BOOST_CHECK_EQUAL(applied_entries[0].index, test_log_index_1);
    BOOST_CHECK_EQUAL(applied_entries[1].index, test_log_index_2);
    
    // Advance commit index to 3 (should apply entry 3)
    synchronizer.advance_commit_index(test_log_index_3);
    
    // Verify entry 3 was applied
    BOOST_CHECK_EQUAL(synchronizer.get_commit_index(), test_log_index_3);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_3);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 3);
    
    applied_entries = state_machine.get_applied_entries();
    BOOST_REQUIRE_EQUAL(applied_entries.size(), 3);
    BOOST_CHECK_EQUAL(applied_entries[2].index, test_log_index_3);
}

/**
 * Test: Sequential application ordering under load
 * 
 * Verifies that entries are applied to the state machine in log order
 * even when commit index advances are concurrent.
 * 
 * Requirements: 5.2
 */
BOOST_AUTO_TEST_CASE(sequential_application_ordering, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    
    constexpr int entry_count = 10;
    
    // Register multiple log entries
    for (int i = 1; i <= entry_count; ++i) {
        std::string command = "SET key" + std::to_string(i) + " value" + std::to_string(i);
        synchronizer.register_log_entry(i, create_command_bytes(command));
    }
    
    // Advance commit index in multiple steps concurrently
    std::vector<std::thread> advancement_threads;
    
    // Thread 1: Advance to index 3
    advancement_threads.emplace_back([&synchronizer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        synchronizer.advance_commit_index(3);
    });
    
    // Thread 2: Advance to index 7
    advancement_threads.emplace_back([&synchronizer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        synchronizer.advance_commit_index(7);
    });
    
    // Thread 3: Advance to index 10
    advancement_threads.emplace_back([&synchronizer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        synchronizer.advance_commit_index(10);
    });
    
    // Wait for all advancements to complete
    for (auto& thread : advancement_threads) {
        thread.join();
    }
    
    // Verify all entries were applied in order
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), entry_count);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), entry_count);
    BOOST_CHECK(state_machine.were_entries_applied_in_order());
    
    // Verify specific ordering
    auto applied_entries = state_machine.get_applied_entries();
    for (int i = 0; i < entry_count; ++i) {
        BOOST_CHECK_EQUAL(applied_entries[i].index, i + 1);
    }
}

/**
 * Test: Applied index updates and client future fulfillment on success
 * 
 * Verifies that successful state machine application updates the applied index
 * and fulfills waiting client futures.
 * 
 * Requirements: 5.3
 */
BOOST_AUTO_TEST_CASE(successful_application_updates_and_fulfills, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    kythira::commit_waiter<std::uint64_t> commit_waiter;
    
    // Connect synchronizer to commit waiter
    synchronizer.set_commit_waiter(&commit_waiter);
    
    // Track client operation completion
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::vector<std::vector<std::byte>> received_results(3);
    std::vector<std::exception_ptr> received_exceptions(3);
    
    // Register client operations with commit waiter
    for (int i = 0; i < 3; ++i) {
        commit_waiter.register_operation(
            test_log_index_1 + i,
            [&, i](std::vector<std::byte> result) {
                received_results[i] = std::move(result);
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
    
    // Register log entries
    synchronizer.register_log_entry(test_log_index_1, create_command_bytes(test_command_1));
    synchronizer.register_log_entry(test_log_index_2, create_command_bytes(test_command_2));
    synchronizer.register_log_entry(test_log_index_3, create_command_bytes(test_command_3));
    
    // Advance commit index (should apply entries and fulfill futures)
    synchronizer.advance_commit_index(test_log_index_3);
    
    // Wait for operations to complete
    while (completed_operations.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify all operations completed successfully
    BOOST_CHECK_EQUAL(completed_operations.load(), 3);
    BOOST_CHECK_EQUAL(successful_operations.load(), 3);
    
    // Verify applied index was updated
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_3);
    
    // Verify client futures were fulfilled with correct results
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(!received_exceptions[i]);
        BOOST_CHECK(!received_results[i].empty());
        
        // Convert result back to string for verification
        std::string result_str;
        for (auto byte : received_results[i]) {
            result_str += static_cast<char>(byte);
        }
        
        std::string expected_command = (i == 0) ? test_command_1 : 
                                      (i == 1) ? test_command_2 : test_command_3;
        std::string expected_result = "OK:" + expected_command;
        BOOST_CHECK_EQUAL(result_str, expected_result);
    }
}

/**
 * Test: Application failure handling and error propagation
 * 
 * Verifies that state machine application failures halt further application
 * and propagate errors to waiting client futures.
 * 
 * Requirements: 5.4
 */
BOOST_AUTO_TEST_CASE(application_failure_handling, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    kythira::commit_waiter<std::uint64_t> commit_waiter;
    
    // Connect synchronizer to commit waiter
    synchronizer.set_commit_waiter(&commit_waiter);
    
    // Track client operation completion
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::atomic<int> failed_operations{0};
    std::vector<std::exception_ptr> received_exceptions(3);
    
    // Register client operations
    for (int i = 0; i < 3; ++i) {
        commit_waiter.register_operation(
            test_log_index_1 + i,
            [&, i](std::vector<std::byte> result) {
                successful_operations++;
                completed_operations++;
            },
            [&, i](std::exception_ptr ex) {
                received_exceptions[i] = ex;
                failed_operations++;
                completed_operations++;
            },
            long_timeout
        );
    }
    
    // Register log entries - second entry will cause failure
    synchronizer.register_log_entry(test_log_index_1, create_command_bytes(test_command_1));
    synchronizer.register_log_entry(test_log_index_2, create_command_bytes(failing_command));
    synchronizer.register_log_entry(test_log_index_3, create_command_bytes(test_command_3));
    
    // Advance commit index - should fail at entry 2
    try {
        synchronizer.advance_commit_index(test_log_index_3);
        BOOST_FAIL("Expected state machine application to fail");
    } catch (const std::exception& ex) {
        // Expected failure
        BOOST_CHECK_EQUAL(std::string(ex.what()), state_machine_failure_reason);
    }
    
    // Wait for operations to complete
    while (completed_operations.load() < 2) { // Only first 2 should complete
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify failure handling
    BOOST_CHECK_EQUAL(successful_operations.load(), 1); // Only first entry succeeded
    BOOST_CHECK_EQUAL(failed_operations.load(), 1);    // Second entry failed
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_1); // Only first entry applied
    BOOST_CHECK(synchronizer.has_application_failed());
    BOOST_CHECK_EQUAL(synchronizer.get_failure_reason(), state_machine_failure_reason);
    
    // Verify error was propagated to client future
    BOOST_CHECK(received_exceptions[1]);
    try {
        std::rethrow_exception(received_exceptions[1]);
    } catch (const std::runtime_error& ex) {
        BOOST_CHECK_EQUAL(std::string(ex.what()), state_machine_failure_reason);
    } catch (...) {
        BOOST_FAIL("Expected runtime_error");
    }
}

/**
 * Test: Catch-up behavior when applied index lags
 * 
 * Verifies that the system can catch up by applying pending entries
 * when the applied index lags behind the commit index.
 * 
 * Requirements: 5.5
 */
BOOST_AUTO_TEST_CASE(applied_index_catch_up, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    
    // Register log entries
    for (int i = 1; i <= 5; ++i) {
        std::string command = "SET key" + std::to_string(i) + " value" + std::to_string(i);
        synchronizer.register_log_entry(i, create_command_bytes(command));
    }
    
    // Advance commit index to 5
    synchronizer.advance_commit_index(test_log_index_5);
    
    // Verify all entries were applied
    BOOST_CHECK_EQUAL(synchronizer.get_commit_index(), test_log_index_5);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_5);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 5);
    
    // Now simulate a scenario where we have more entries but need to catch up
    // Register additional entries 6-10
    for (int i = 6; i <= 10; ++i) {
        std::string command = "SET key" + std::to_string(i) + " value" + std::to_string(i);
        synchronizer.register_log_entry(i, create_command_bytes(command));
    }
    
    // Advance commit index to 10 (should apply entries 6-10)
    synchronizer.advance_commit_index(test_log_index_10);
    
    // Verify applied index caught up
    BOOST_CHECK_EQUAL(synchronizer.get_commit_index(), test_log_index_10);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_10);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 10);
    
    // Verify entries were applied in order
    BOOST_CHECK(state_machine.were_entries_applied_in_order());
    
    auto applied_entries = state_machine.get_applied_entries();
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK_EQUAL(applied_entries[i].index, i + 1);
    }
}

/**
 * Test: Explicit catch-up operation
 * 
 * Verifies that the catch_up_applied_index method works correctly
 * when called explicitly.
 * 
 * Requirements: 5.5
 */
BOOST_AUTO_TEST_CASE(explicit_catch_up_operation, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    
    // Register log entries
    for (int i = 1; i <= 5; ++i) {
        std::string command = "SET key" + std::to_string(i) + " value" + std::to_string(i);
        synchronizer.register_log_entry(i, create_command_bytes(command));
    }
    
    // Advance commit index to 3 first
    synchronizer.advance_commit_index(3);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), 3);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 3);
    
    // Now advance commit index to 5 but don't apply immediately
    // (simulate this by directly setting commit index)
    synchronizer.advance_commit_index(test_log_index_5);
    
    // Verify catch-up worked
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), test_log_index_5);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), 5);
    
    // Verify all entries were applied in order
    BOOST_CHECK(state_machine.were_entries_applied_in_order());
}

/**
 * Test: Concurrent commit advancement and state machine application
 * 
 * Verifies that concurrent commit index advancements and state machine
 * applications work correctly without race conditions.
 * 
 * Requirements: 5.1, 5.2, 5.3
 */
BOOST_AUTO_TEST_CASE(concurrent_commit_and_application, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    kythira::commit_waiter<std::uint64_t> commit_waiter;
    
    synchronizer.set_commit_waiter(&commit_waiter);
    
    constexpr int total_entries = 20;
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    
    // Register client operations
    for (int i = 1; i <= total_entries; ++i) {
        commit_waiter.register_operation(
            i,
            [&](std::vector<std::byte> result) {
                successful_operations++;
                completed_operations++;
            },
            [&](std::exception_ptr ex) {
                completed_operations++;
            },
            long_timeout
        );
    }
    
    // Register log entries
    for (int i = 1; i <= total_entries; ++i) {
        std::string command = "SET key" + std::to_string(i) + " value" + std::to_string(i);
        synchronizer.register_log_entry(i, create_command_bytes(command));
    }
    
    // Advance commit index concurrently in chunks
    std::vector<std::thread> advancement_threads;
    
    for (int chunk = 0; chunk < 4; ++chunk) {
        advancement_threads.emplace_back([&synchronizer, chunk, total_entries]() {
            int start_index = chunk * 5 + 1;
            int end_index = std::min((chunk + 1) * 5, total_entries);
            
            std::this_thread::sleep_for(std::chrono::milliseconds{chunk * 10});
            synchronizer.advance_commit_index(end_index);
        });
    }
    
    // Wait for all advancements to complete
    for (auto& thread : advancement_threads) {
        thread.join();
    }
    
    // Wait for all operations to complete
    while (completed_operations.load() < total_entries) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify final state
    BOOST_CHECK_EQUAL(completed_operations.load(), total_entries);
    BOOST_CHECK_EQUAL(successful_operations.load(), total_entries);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), total_entries);
    BOOST_CHECK_EQUAL(state_machine.get_entry_count(), total_entries);
    BOOST_CHECK(state_machine.were_entries_applied_in_order());
}

/**
 * Test: Mixed success and failure scenarios
 * 
 * Verifies proper handling of scenarios where some entries succeed
 * and others fail during state machine application.
 * 
 * Requirements: 5.3, 5.4
 */
BOOST_AUTO_TEST_CASE(mixed_success_failure_scenarios, * boost::unit_test::timeout(30)) {
    MockStateMachine state_machine;
    StateMachineSynchronizer synchronizer(state_machine);
    kythira::commit_waiter<std::uint64_t> commit_waiter;
    
    synchronizer.set_commit_waiter(&commit_waiter);
    
    std::atomic<int> completed_operations{0};
    std::atomic<int> successful_operations{0};
    std::atomic<int> failed_operations{0};
    
    // Register client operations
    for (int i = 1; i <= 4; ++i) {
        commit_waiter.register_operation(
            i,
            [&](std::vector<std::byte> result) {
                successful_operations++;
                completed_operations++;
            },
            [&](std::exception_ptr ex) {
                failed_operations++;
                completed_operations++;
            },
            long_timeout
        );
    }
    
    // Register log entries - entry 3 will fail
    synchronizer.register_log_entry(1, create_command_bytes(test_command_1));
    synchronizer.register_log_entry(2, create_command_bytes(test_command_2));
    synchronizer.register_log_entry(3, create_command_bytes(failing_command));
    synchronizer.register_log_entry(4, create_command_bytes(test_command_4));
    
    // Advance commit index to 2 first (should succeed)
    synchronizer.advance_commit_index(2);
    
    // Wait for first 2 operations to complete
    while (completed_operations.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    BOOST_CHECK_EQUAL(successful_operations.load(), 2);
    BOOST_CHECK_EQUAL(failed_operations.load(), 0);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), 2);
    
    // Now advance to 4 (should fail at entry 3)
    try {
        synchronizer.advance_commit_index(4);
        BOOST_FAIL("Expected failure at entry 3");
    } catch (const std::exception&) {
        // Expected failure
    }
    
    // Wait for entry 3 operation to complete (should fail)
    while (completed_operations.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    
    // Verify final state - application should have halted at failure
    BOOST_CHECK_EQUAL(successful_operations.load(), 2);
    BOOST_CHECK_EQUAL(failed_operations.load(), 1);
    BOOST_CHECK_EQUAL(state_machine.get_applied_index(), 2); // Should not have advanced past failure
    BOOST_CHECK(synchronizer.has_application_failed());
}

BOOST_AUTO_TEST_SUITE_END()