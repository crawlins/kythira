/**
 * @file executor_example.cpp
 * @brief Example demonstrating folly concept wrapper executor usage
 * 
 * This example shows how to:
 * 1. Create and use Executor wrappers
 * 2. Create and use KeepAlive wrappers
 * 3. Submit work to executors
 * 4. Handle executor lifetime and reference counting
 * 5. Use pointer-like access with KeepAlive
 */

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>

#include "../../include/raft/future.hpp"
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/init/Init.h>

namespace {
    constexpr const char* test_message = "Work executed successfully";
    constexpr std::chrono::milliseconds test_timeout{1000};
    constexpr std::size_t test_thread_count = 2;
}

class ExecutorExampleRunner {
public:
    auto run_all_scenarios() -> int {
        int failed_scenarios = 0;
        
        std::cout << "=== Folly Concept Wrapper Executor Examples ===\n\n";
        
        if (!test_executor_work_submission()) failed_scenarios++;
        if (!test_executor_inline_execution()) failed_scenarios++;
        if (!test_keep_alive_creation()) failed_scenarios++;
        if (!test_keep_alive_reference_counting()) failed_scenarios++;
        if (!test_executor_lifetime_management()) failed_scenarios++;
        
        std::cout << "\n=== Summary ===\n";
        if (failed_scenarios > 0) {
            std::cout << "❌ " << failed_scenarios << " scenario(s) failed\n";
            return 1;
        }
        
        std::cout << "✅ All scenarios passed!\n";
        return 0;
    }

private:
    auto test_executor_work_submission() -> bool {
        std::cout << "Test 1: Executor Work Submission\n";
        try {
            // Create a thread pool executor
            auto folly_executor = std::make_shared<folly::CPUThreadPoolExecutor>(test_thread_count);
            kythira::Executor executor(folly_executor.get());
            
            // Submit work and verify execution
            std::atomic<bool> work_executed{false};
            std::atomic<bool> work_completed{false};
            
            executor.add([&work_executed, &work_completed]() {
                work_executed = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                work_completed = true;
            });
            
            // Wait for work to start
            auto start_time = std::chrono::steady_clock::now();
            while (!work_executed && 
                   std::chrono::steady_clock::now() - start_time < test_timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            if (!work_executed) {
                std::cout << "  ❌ Work was not executed within timeout\n";
                return false;
            }
            
            // Wait for work to complete
            while (!work_completed && 
                   std::chrono::steady_clock::now() - start_time < test_timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            if (!work_completed) {
                std::cout << "  ❌ Work did not complete within timeout\n";
                return false;
            }
            
            std::cout << "  ✅ Executor work submission works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_executor_inline_execution() -> bool {
        std::cout << "Test 2: Executor Inline Execution\n";
        try {
            // Create an inline executor (executes immediately)
            folly::InlineExecutor folly_executor;
            kythira::Executor executor(&folly_executor);
            
            // Submit work that should execute immediately
            bool work_executed = false;
            
            executor.add([&work_executed]() {
                work_executed = true;
            });
            
            // With inline executor, work should be done immediately
            if (!work_executed) {
                std::cout << "  ❌ Inline executor did not execute work immediately\n";
                return false;
            }
            
            std::cout << "  ✅ Executor inline execution works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_keep_alive_creation() -> bool {
        std::cout << "Test 3: KeepAlive Creation and Access\n";
        try {
            // Create a thread pool executor and get KeepAlive
            auto folly_executor = std::make_shared<folly::CPUThreadPoolExecutor>(test_thread_count);
            auto folly_keep_alive = folly::Executor::getKeepAliveToken(folly_executor.get());
            
            kythira::KeepAlive keep_alive(std::move(folly_keep_alive));
            
            // Test pointer-like access
            auto* raw_executor = keep_alive.get();
            if (raw_executor == nullptr) {
                std::cout << "  ❌ KeepAlive get() returned null pointer\n";
                return false;
            }
            
            // Verify we can use the executor through KeepAlive
            std::atomic<bool> work_executed{false};
            
            raw_executor->add([&work_executed]() {
                work_executed = true;
            });
            
            // Wait for work execution
            auto start_time = std::chrono::steady_clock::now();
            while (!work_executed && 
                   std::chrono::steady_clock::now() - start_time < test_timeout) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            if (!work_executed) {
                std::cout << "  ❌ Work through KeepAlive was not executed\n";
                return false;
            }
            
            std::cout << "  ✅ KeepAlive creation and access works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_keep_alive_reference_counting() -> bool {
        std::cout << "Test 4: KeepAlive Reference Counting\n";
        try {
            // Create executor and KeepAlive
            auto folly_executor = std::make_shared<folly::CPUThreadPoolExecutor>(test_thread_count);
            auto folly_keep_alive = folly::Executor::getKeepAliveToken(folly_executor.get());
            
            kythira::KeepAlive keep_alive1(std::move(folly_keep_alive));
            
            // Test copy construction
            kythira::KeepAlive keep_alive2 = keep_alive1;
            
            // Both should point to the same executor
            if (keep_alive1.get() != keep_alive2.get()) {
                std::cout << "  ❌ Copied KeepAlive instances point to different executors\n";
                return false;
            }
            
            // Test move construction
            kythira::KeepAlive keep_alive3 = std::move(keep_alive2);
            
            // Original should still work, moved-to should work
            if (keep_alive1.get() == nullptr || keep_alive3.get() == nullptr) {
                std::cout << "  ❌ KeepAlive move construction failed\n";
                return false;
            }
            
            std::cout << "  ✅ KeepAlive reference counting works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_executor_lifetime_management() -> bool {
        std::cout << "Test 5: Executor Lifetime Management\n";
        try {
            // Test that executor wrapper handles null pointers gracefully
            // Note: The constructor throws for null, so we test the exception
            bool exception_thrown = false;
            try {
                kythira::Executor null_executor(nullptr);
            } catch (const std::invalid_argument&) {
                exception_thrown = true;
            }
            
            if (!exception_thrown) {
                std::cout << "  ❌ Executor should throw for null pointer\n";
                return false;
            }
            
            // Test that invalid executor throws when used
            kythira::Executor default_executor; // Default constructor creates invalid executor
            bool add_exception_thrown = false;
            try {
                default_executor.add([]() {
                    // This work should not execute
                });
            } catch (const std::runtime_error&) {
                add_exception_thrown = true;
            }
            
            if (!add_exception_thrown) {
                std::cout << "  ❌ Invalid executor should throw when adding work\n";
                return false;
            }
            
            std::cout << "  ✅ Executor lifetime management works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
};

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    ExecutorExampleRunner runner;
    return runner.run_all_scenarios();
}