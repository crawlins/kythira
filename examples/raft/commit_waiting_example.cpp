/**
 * Example: Commit Waiting in Raft
 * 
 * This example demonstrates:
 * 1. Client command submission with proper waiting (Requirements 1.1, 1.2)
 * 2. Timeout handling and error scenarios (Requirements 1.3)
 * 3. Leadership loss rejection (Requirements 1.4)
 * 4. Concurrent operations with ordering guarantees (Requirements 1.5)
 * 
 * This example shows how the Raft implementation ensures that client operations
 * wait for actual commit and state machine application before completing,
 * providing strong durability and consistency guarantees.
 */

#include <raft/commit_waiter.hpp>
#include <raft/completion_exceptions.hpp>
#include <raft/types.hpp>

#include <folly/init/Init.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <format>
#include <future>
#include <algorithm>

namespace {
    // Test configuration constants
    constexpr std::uint64_t initial_log_index = 1;
    constexpr std::uint64_t leader_term = 5;
    constexpr std::uint64_t new_term = 6;
    constexpr std::chrono::milliseconds short_timeout{100};
    constexpr std::chrono::milliseconds normal_timeout{1000};
    constexpr std::chrono::milliseconds long_timeout{5000};
    constexpr const char* test_command_1 = "SET key1=value1";
    constexpr const char* test_command_2 = "SET key2=value2";
    constexpr const char* test_command_3 = "SET key3=value3";
    constexpr const char* test_result_1 = "OK: key1=value1";
    constexpr const char* test_result_2 = "OK: key2=value2";
    constexpr const char* test_result_3 = "OK: key3=value3";
    constexpr std::size_t concurrent_operations_count = 10;
}

// Helper function to convert string to bytes
auto string_to_bytes(const std::string& str) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(str.size());
    for (char c : str) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

// Helper function to convert bytes to string
auto bytes_to_string(const std::vector<std::byte>& bytes) -> std::string {
    std::string str;
    str.reserve(bytes.size());
    for (auto byte : bytes) {
        str += static_cast<char>(byte);
    }
    return str;
}

// Mock state machine for testing
class mock_state_machine {
private:
    std::unordered_map<std::uint64_t, std::string> _applied_commands;
    std::atomic<std::uint64_t> _next_index{initial_log_index};
    
public:
    // Apply a command and return the result
    auto apply_command(std::uint64_t log_index, const std::string& command) -> std::vector<std::byte> {
        _applied_commands[log_index] = command;
        
        // Generate result based on command
        std::string result;
        if (command.starts_with("SET key1=")) {
            result = test_result_1;
        } else if (command.starts_with("SET key2=")) {
            result = test_result_2;
        } else if (command.starts_with("SET key3=")) {
            result = test_result_3;
        } else {
            result = std::format("OK: {}", command);
        }
        
        return string_to_bytes(result);
    }
    
    // Get the next available log index
    auto get_next_index() -> std::uint64_t {
        return _next_index.fetch_add(1);
    }
    
    // Check if a command was applied
    auto was_applied(std::uint64_t log_index) const -> bool {
        return _applied_commands.contains(log_index);
    }
    
    // Get applied command count
    auto get_applied_count() const -> std::size_t {
        return _applied_commands.size();
    }
};

// Test scenario 1: Basic commit waiting - command submission with proper waiting
auto test_basic_commit_waiting() -> bool {
    std::cout << "Test 1: Basic Commit Waiting\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Submitting command and waiting for commit...\n";
        
        // Track operation completion
        bool operation_completed = false;
        std::vector<std::byte> operation_result;
        std::exception_ptr operation_error;
        
        // Get log index for the command
        auto log_index = state_machine.get_next_index();
        
        // Register operation with commit waiter
        commit_waiter.register_operation(
            log_index,
            [&operation_completed, &operation_result](std::vector<std::byte> result) {
                operation_completed = true;
                operation_result = std::move(result);
                std::cout << "    ✓ Command committed and applied\n";
            },
            [&operation_completed, &operation_error](std::exception_ptr error) {
                operation_completed = true;
                operation_error = error;
                std::cout << "    ✗ Command rejected\n";
            },
            normal_timeout
        );
        
        std::cout << std::format("  Registered command for log index {}\n", log_index);
        std::cout << std::format("  Pending operations: {}\n", commit_waiter.get_pending_count());
        
        // Simulate commit and state machine application
        commit_waiter.notify_committed_and_applied(log_index, [&state_machine](std::uint64_t index) {
            return state_machine.apply_command(index, test_command_1);
        });
        
        // Verify operation was completed successfully
        if (operation_completed && !operation_error) {
            auto result_str = bytes_to_string(operation_result);
            std::cout << std::format("  Command result: {}\n", result_str);
            
            if (result_str == test_result_1) {
                std::cout << "  ✓ Basic commit waiting completed successfully\n";
                return true;
            } else {
                std::cerr << std::format("  ✗ Failed: Unexpected result '{}'\n", result_str);
                return false;
            }
        } else {
            std::cerr << "  ✗ Failed: Operation not completed or completed with error\n";
            if (operation_error) {
                try {
                    std::rethrow_exception(operation_error);
                } catch (const std::exception& e) {
                    std::cerr << std::format("    Error: {}\n", e.what());
                }
            }
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 2: Application before future fulfillment
auto test_application_before_fulfillment() -> bool {
    std::cout << "\nTest 2: Application Before Future Fulfillment\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Testing that state machine application occurs before future fulfillment...\n";
        
        // Track operation completion and timing
        bool operation_completed = false;
        std::vector<std::byte> operation_result;
        std::chrono::steady_clock::time_point fulfillment_time;
        
        auto log_index = state_machine.get_next_index();
        
        // Register operation
        commit_waiter.register_operation(
            log_index,
            [&operation_completed, &operation_result, &fulfillment_time](std::vector<std::byte> result) {
                fulfillment_time = std::chrono::steady_clock::now();
                operation_completed = true;
                operation_result = std::move(result);
            },
            [&operation_completed](std::exception_ptr error) {
                operation_completed = true;
            },
            normal_timeout
        );
        
        // Record time before commit notification
        auto commit_start_time = std::chrono::steady_clock::now();
        
        // Simulate commit and application with timing verification
        commit_waiter.notify_committed_and_applied(log_index, [&state_machine, commit_start_time](std::uint64_t index) {
            // Simulate some processing time for state machine application
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            
            auto application_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                application_time - commit_start_time
            );
            
            std::cout << std::format("    State machine application completed after {}ms\n", elapsed.count());
            
            return state_machine.apply_command(index, test_command_2);
        });
        
        // Verify operation completed and state machine was applied
        if (operation_completed && state_machine.was_applied(log_index)) {
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                fulfillment_time - commit_start_time
            );
            
            std::cout << std::format("    Future fulfilled after {}ms\n", total_elapsed.count());
            std::cout << "  ✓ State machine application occurred before future fulfillment\n";
            return true;
        } else {
            std::cerr << "  ✗ Failed: Operation not completed or state machine not applied\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 3: Timeout handling and error scenarios
auto test_timeout_handling() -> bool {
    std::cout << "\nTest 3: Timeout Handling and Error Scenarios\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Testing timeout handling for uncommitted operations...\n";
        
        // Track operation completion
        bool operation_completed = false;
        std::exception_ptr operation_error;
        
        auto log_index = state_machine.get_next_index();
        
        // Register operation with short timeout
        commit_waiter.register_operation(
            log_index,
            [&operation_completed](std::vector<std::byte> result) {
                operation_completed = true;
                std::cout << "    Unexpected: Operation fulfilled\n";
            },
            [&operation_completed, &operation_error](std::exception_ptr error) {
                operation_completed = true;
                operation_error = error;
                std::cout << "    ✓ Operation timed out as expected\n";
            },
            short_timeout
        );
        
        std::cout << std::format("  Registered operation with {}ms timeout\n", short_timeout.count());
        
        // Wait for timeout to occur
        std::this_thread::sleep_for(short_timeout + std::chrono::milliseconds{50});
        
        // Check for timed-out operations
        auto cancelled_count = commit_waiter.cancel_timed_out_operations();
        
        std::cout << std::format("  Cancelled {} timed-out operations\n", cancelled_count);
        
        // Verify operation timed out correctly
        if (operation_completed && operation_error && cancelled_count > 0) {
            try {
                std::rethrow_exception(operation_error);
            } catch (const kythira::commit_timeout_exception<std::uint64_t>& e) {
                std::cout << std::format("    Timeout exception: {}\n", e.what());
                std::cout << std::format("    Entry index: {}\n", e.get_entry_index());
                std::cout << std::format("    Timeout duration: {}ms\n", e.get_timeout().count());
                
                if (e.get_entry_index() == log_index && e.get_timeout() == short_timeout) {
                    std::cout << "  ✓ Timeout handling working correctly\n";
                    return true;
                } else {
                    std::cerr << "  ✗ Failed: Incorrect timeout exception details\n";
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << std::format("  ✗ Failed: Unexpected exception type: {}\n", e.what());
                return false;
            }
        } else {
            std::cerr << "  ✗ Failed: Operation did not timeout correctly\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 4: Leadership loss rejection
auto test_leadership_loss_rejection() -> bool {
    std::cout << "\nTest 4: Leadership Loss Rejection\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Testing operation rejection due to leadership loss...\n";
        
        // Track multiple operations
        std::vector<bool> operations_completed(3, false);
        std::vector<std::exception_ptr> operation_errors(3);
        
        // Register multiple operations
        for (std::size_t i = 0; i < 3; ++i) {
            auto log_index = state_machine.get_next_index();
            
            commit_waiter.register_operation(
                log_index,
                [&operations_completed, i](std::vector<std::byte> result) {
                    operations_completed[i] = true;
                    std::cout << std::format("    Unexpected: Operation {} fulfilled\n", i);
                },
                [&operations_completed, &operation_errors, i](std::exception_ptr error) {
                    operations_completed[i] = true;
                    operation_errors[i] = error;
                    std::cout << std::format("    ✓ Operation {} rejected due to leadership loss\n", i);
                },
                long_timeout
            );
        }
        
        std::cout << std::format("  Registered {} operations\n", operations_completed.size());
        std::cout << std::format("  Pending operations: {}\n", commit_waiter.get_pending_count());
        
        // Simulate leadership loss
        commit_waiter.cancel_all_operations_leadership_lost(leader_term, new_term);
        
        std::cout << std::format("  Simulated leadership loss (term {} -> {})\n", leader_term, new_term);
        
        // Verify all operations were rejected
        bool all_completed = std::all_of(operations_completed.begin(), operations_completed.end(), 
                                       [](bool completed) { return completed; });
        
        bool all_have_errors = std::all_of(operation_errors.begin(), operation_errors.end(),
                                         [](const std::exception_ptr& error) { return error != nullptr; });
        
        if (all_completed && all_have_errors) {
            // Check the first error to verify it's the correct type
            try {
                std::rethrow_exception(operation_errors[0]);
            } catch (const kythira::leadership_lost_exception<std::uint64_t>& e) {
                std::cout << std::format("    Leadership loss exception: {}\n", e.what());
                std::cout << std::format("    Old term: {}, New term: {}\n", 
                         e.get_old_term(), e.get_new_term());
                
                if (e.get_old_term() == leader_term && e.get_new_term() == new_term) {
                    std::cout << "  ✓ Leadership loss rejection working correctly\n";
                    return true;
                } else {
                    std::cerr << "  ✗ Failed: Incorrect leadership loss exception details\n";
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << std::format("  ✗ Failed: Unexpected exception type: {}\n", e.what());
                return false;
            }
        } else {
            std::cerr << "  ✗ Failed: Not all operations were rejected correctly\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 5: Concurrent operations with ordering guarantees
auto test_concurrent_operations_ordering() -> bool {
    std::cout << "\nTest 5: Concurrent Operations with Ordering Guarantees\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Testing concurrent operations with log order preservation...\n";
        
        // Track completion order
        std::vector<std::uint64_t> completion_order;
        std::mutex completion_mutex;
        
        // Submit multiple concurrent operations
        std::vector<std::future<void>> operation_futures;
        std::vector<std::uint64_t> log_indices;
        
        for (std::size_t i = 0; i < concurrent_operations_count; ++i) {
            auto log_index = state_machine.get_next_index();
            log_indices.push_back(log_index);
            
            // Submit operation asynchronously
            auto future = std::async(std::launch::async, [&, log_index, i]() {
                commit_waiter.register_operation(
                    log_index,
                    [&completion_order, &completion_mutex, log_index](std::vector<std::byte> result) {
                        std::lock_guard<std::mutex> lock(completion_mutex);
                        completion_order.push_back(log_index);
                    },
                    [log_index](std::exception_ptr error) {
                        std::cout << std::format("    Operation {} rejected\n", log_index);
                    },
                    long_timeout
                );
            });
            
            operation_futures.push_back(std::move(future));
        }
        
        // Wait for all operations to be registered
        for (auto& future : operation_futures) {
            future.wait();
        }
        
        std::cout << std::format("  Submitted {} concurrent operations\n", concurrent_operations_count);
        std::cout << std::format("  Pending operations: {}\n", commit_waiter.get_pending_count());
        
        // Commit operations in log order (simulating sequential state machine application)
        std::sort(log_indices.begin(), log_indices.end());
        
        for (auto log_index : log_indices) {
            commit_waiter.notify_committed_and_applied(log_index, [&state_machine](std::uint64_t index) {
                auto command = std::format("SET concurrent_key_{}=value_{}", index, index);
                return state_machine.apply_command(index, command);
            });
            
            // Small delay to ensure ordering is preserved
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        
        // Wait a bit for all completions
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        
        // Verify completion order matches log order
        {
            std::lock_guard<std::mutex> lock(completion_mutex);
            
            std::cout << std::format("  Completed {} operations\n", completion_order.size());
            
            if (completion_order.size() == concurrent_operations_count) {
                // Check if completion order matches log order
                bool order_preserved = std::is_sorted(completion_order.begin(), completion_order.end());
                
                if (order_preserved) {
                    std::cout << "  ✓ Concurrent operations completed in log order\n";
                    std::cout << "    Completion order: ";
                    for (std::size_t i = 0; i < completion_order.size(); ++i) {
                        std::cout << completion_order[i];
                        if (i < completion_order.size() - 1) std::cout << ", ";
                    }
                    std::cout << "\n";
                    return true;
                } else {
                    std::cerr << "  ✗ Failed: Operations not completed in log order\n";
                    std::cout << "    Completion order: ";
                    for (std::size_t i = 0; i < completion_order.size(); ++i) {
                        std::cout << completion_order[i];
                        if (i < completion_order.size() - 1) std::cout << ", ";
                    }
                    std::cout << "\n";
                    return false;
                }
            } else {
                std::cerr << std::format("  ✗ Failed: Expected {} completions, got {}\n", 
                         concurrent_operations_count, completion_order.size());
                return false;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

// Test scenario 6: State machine application failure handling
auto test_state_machine_failure_handling() -> bool {
    std::cout << "\nTest 6: State Machine Application Failure Handling\n";
    
    try {
        kythira::commit_waiter<std::uint64_t> commit_waiter;
        mock_state_machine state_machine;
        
        std::cout << "  Testing error propagation when state machine application fails...\n";
        
        // Track operation completion
        bool operation_completed = false;
        std::exception_ptr operation_error;
        
        auto log_index = state_machine.get_next_index();
        
        // Register operation
        commit_waiter.register_operation(
            log_index,
            [&operation_completed](std::vector<std::byte> result) {
                operation_completed = true;
                std::cout << "    Unexpected: Operation fulfilled despite state machine failure\n";
            },
            [&operation_completed, &operation_error](std::exception_ptr error) {
                operation_completed = true;
                operation_error = error;
                std::cout << "    ✓ Operation rejected due to state machine failure\n";
            },
            normal_timeout
        );
        
        // Simulate commit with state machine failure
        commit_waiter.notify_committed_and_applied(log_index, [](std::uint64_t index) -> std::vector<std::byte> {
            // Simulate state machine application failure
            throw std::runtime_error("State machine application failed");
        });
        
        // Verify operation was rejected with the correct error
        if (operation_completed && operation_error) {
            try {
                std::rethrow_exception(operation_error);
            } catch (const std::runtime_error& e) {
                std::cout << std::format("    State machine error: {}\n", e.what());
                
                if (std::string(e.what()) == "State machine application failed") {
                    std::cout << "  ✓ State machine failure error propagation working correctly\n";
                    return true;
                } else {
                    std::cerr << "  ✗ Failed: Unexpected error message\n";
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << std::format("  ✗ Failed: Unexpected exception type: {}\n", e.what());
                return false;
            }
        } else {
            std::cerr << "  ✗ Failed: Operation not rejected or no error propagated\n";
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto main(int argc, char* argv[]) -> int {
    // Initialize Folly
    folly::Init init(&argc, &argv);
    
    std::cout << "========================================\n";
    std::cout << "  Commit Waiting Example\n";
    std::cout << "========================================\n\n";
    
    std::cout << "This example demonstrates commit waiting in Raft:\n";
    std::cout << "- Client command submission with proper waiting\n";
    std::cout << "- State machine application before future fulfillment\n";
    std::cout << "- Timeout handling and error scenarios\n";
    std::cout << "- Leadership loss rejection\n";
    std::cout << "- Concurrent operations with ordering guarantees\n";
    std::cout << "- State machine application failure handling\n\n";
    
    int failed_scenarios = 0;
    
    // Run all test scenarios
    if (!test_basic_commit_waiting()) failed_scenarios++;
    if (!test_application_before_fulfillment()) failed_scenarios++;
    if (!test_timeout_handling()) failed_scenarios++;
    if (!test_leadership_loss_rejection()) failed_scenarios++;
    if (!test_concurrent_operations_ordering()) failed_scenarios++;
    if (!test_state_machine_failure_handling()) failed_scenarios++;
    
    // Print summary
    std::cout << "\n========================================\n";
    if (failed_scenarios > 0) {
        std::cout << std::format("  {} scenario(s) failed\n", failed_scenarios);
        std::cout << "========================================\n";
        return 1;
    }
    
    std::cout << "  All scenarios passed!\n";
    std::cout << "  Commit waiting working correctly.\n";
    std::cout << "========================================\n";
    return 0;
}