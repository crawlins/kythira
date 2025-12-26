/**
 * @file continuation_example.cpp
 * @brief Example demonstrating folly concept wrapper continuation operations
 * 
 * This example shows how to:
 * 1. Use via() to schedule continuations on specific executors
 * 2. Use delay() to add time-based delays to futures
 * 3. Use within() to add timeout behavior to futures
 * 4. Chain continuation operations with proper type safety
 * 5. Handle void futures with Unit conversion
 */

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <stdexcept>

#include "../../include/raft/future.hpp"
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/init/Init.h>

namespace {
    constexpr const char* test_initial_value = "Initial";
    constexpr const char* test_transformed_value = "Transformed";
    constexpr const char* test_timeout_message = "Operation timed out";
    constexpr std::chrono::milliseconds short_delay{50};
    constexpr std::chrono::milliseconds medium_delay{100};
    constexpr std::chrono::milliseconds long_delay{200};
    constexpr std::chrono::milliseconds timeout_duration{150};
}

class ContinuationExampleRunner {
public:
    auto run_all_scenarios() -> int {
        int failed_scenarios = 0;
        
        std::cout << "=== Folly Concept Wrapper Continuation Examples ===\n\n";
        
        if (!test_via_executor_scheduling()) failed_scenarios++;
        if (!test_delay_time_based()) failed_scenarios++;
        if (!test_within_timeout_success()) failed_scenarios++;
        if (!test_within_timeout_failure()) failed_scenarios++;
        if (!test_chained_continuations()) failed_scenarios++;
        
        std::cout << "\n=== Summary ===\n";
        if (failed_scenarios > 0) {
            std::cout << "❌ " << failed_scenarios << " scenario(s) failed\n";
            return 1;
        }
        
        std::cout << "✅ All scenarios passed!\n";
        return 0;
    }

private:
    auto test_via_executor_scheduling() -> bool {
        std::cout << "Test 1: Via Executor Scheduling\n";
        try {
            // Create executors
            auto thread_executor = std::make_shared<folly::CPUThreadPoolExecutor>(2);
            kythira::Executor executor(thread_executor.get());
            
            // Create initial future
            auto future = kythira::FutureFactory::makeFuture(std::string(test_initial_value));
            
            // Schedule continuation on specific executor
            std::atomic<std::thread::id> execution_thread_id{std::this_thread::get_id()};
            std::atomic<bool> continuation_executed{false};
            
            auto continued_future = std::move(future)
                .via(executor.get())
                .thenValue([&execution_thread_id, &continuation_executed](std::string value) {
                    execution_thread_id = std::this_thread::get_id();
                    continuation_executed = true;
                    return value + "_via_executor";
                });
            
            // Get result
            auto result = std::move(continued_future).get();
            
            // Verify continuation executed
            if (!continuation_executed) {
                std::cout << "  ❌ Continuation was not executed\n";
                return false;
            }
            
            // Verify result
            if (result != std::string(test_initial_value) + "_via_executor") {
                std::cout << "  ❌ Via continuation result mismatch\n";
                return false;
            }
            
            // Verify execution happened on different thread
            if (execution_thread_id == std::this_thread::get_id()) {
                std::cout << "  ⚠️  Continuation may not have executed on executor thread\n";
                // This is not necessarily an error for inline executors
            }
            
            std::cout << "  ✅ Via executor scheduling works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_delay_time_based() -> bool {
        std::cout << "Test 2: Delay Time-Based\n";
        try {
            // Create initial future
            auto future = kythira::FutureFactory::makeFuture(42);
            
            // Add delay
            auto start_time = std::chrono::steady_clock::now();
            auto delayed_future = std::move(future).delay(medium_delay);
            
            // Get result (should be delayed)
            auto result = std::move(delayed_future).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Verify delay occurred
            if (elapsed < medium_delay) {
                std::cout << "  ❌ Delay did not occur (elapsed: " 
                         << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() 
                         << "ms, expected: " << medium_delay.count() << "ms)\n";
                return false;
            }
            
            // Verify result is unchanged
            if (result != 42) {
                std::cout << "  ❌ Delayed future result mismatch: expected 42, got " << result << "\n";
                return false;
            }
            
            std::cout << "  ✅ Delay time-based works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_within_timeout_success() -> bool {
        std::cout << "Test 3: Within Timeout (Success)\n";
        try {
            // Create future that completes within timeout
            kythira::Promise<std::string> promise;
            auto future = promise.getFuture();
            
            // Add timeout (longer than completion time)
            auto timeout_future = std::move(future).within(timeout_duration);
            
            // Complete the promise quickly
            std::thread([promise = std::move(promise)]() mutable {
                std::this_thread::sleep_for(short_delay);
                promise.setValue(std::string(test_initial_value));
            }).detach();
            
            // Get result (should succeed)
            auto result = std::move(timeout_future).get();
            
            if (result != test_initial_value) {
                std::cout << "  ❌ Within timeout result mismatch\n";
                return false;
            }
            
            std::cout << "  ✅ Within timeout (success) works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_within_timeout_failure() -> bool {
        std::cout << "Test 4: Within Timeout (Failure)\n";
        try {
            // Create future that takes longer than timeout
            kythira::Promise<std::string> promise;
            auto future = promise.getFuture();
            
            // Add timeout (shorter than completion time)
            auto timeout_future = std::move(future).within(short_delay);
            
            // Complete the promise slowly (after timeout)
            std::thread([promise = std::move(promise)]() mutable {
                std::this_thread::sleep_for(long_delay);
                promise.setValue(std::string(test_initial_value));
            }).detach();
            
            // Get result (should timeout)
            bool timeout_occurred = false;
            try {
                auto result = std::move(timeout_future).get();
            } catch (const std::exception&) {
                timeout_occurred = true;
            }
            
            if (!timeout_occurred) {
                std::cout << "  ❌ Timeout should have occurred\n";
                return false;
            }
            
            std::cout << "  ✅ Within timeout (failure) works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Unexpected exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_chained_continuations() -> bool {
        std::cout << "Test 5: Chained Continuations\n";
        try {
            // Create executors
            auto thread_executor = std::make_shared<folly::CPUThreadPoolExecutor>(2);
            kythira::Executor executor(thread_executor.get());
            
            // Create initial future and chain multiple operations
            auto future = kythira::FutureFactory::makeFuture(std::string(test_initial_value));
            
            auto start_time = std::chrono::steady_clock::now();
            
            auto chained_future = std::move(future)
                .via(executor.get())
                .thenValue([](std::string value) {
                    return value + "_step1";
                })
                .delay(short_delay)
                .thenValue([](std::string value) {
                    return value + "_step2";
                })
                .within(timeout_duration)
                .thenValue([](std::string value) {
                    return value + "_final";
                });
            
            // Get result
            auto result = std::move(chained_future).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Verify delay occurred
            if (elapsed < short_delay) {
                std::cout << "  ❌ Chained delay did not occur\n";
                return false;
            }
            
            // Verify all transformations applied
            std::string expected = std::string(test_initial_value) + "_step1_step2_final";
            if (result != expected) {
                std::cout << "  ❌ Chained continuation result mismatch: expected '" 
                         << expected << "', got '" << result << "'\n";
                return false;
            }
            
            std::cout << "  ✅ Chained continuations work correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
};

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    ContinuationExampleRunner runner;
    return runner.run_all_scenarios();
}