/**
 * @file promise_example.cpp
 * @brief Example demonstrating folly concept wrapper promise usage
 * 
 * This example shows how to:
 * 1. Create and use SemiPromise wrappers
 * 2. Create and use Promise wrappers  
 * 3. Set values and exceptions on promises
 * 4. Retrieve futures from promises
 * 5. Handle void/Unit type conversions
 */

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <optional>

#include "../../include/raft/future.hpp"

namespace {
    constexpr const char* test_value = "Hello, Promise!";
    constexpr const char* test_error_message = "Test exception";
    constexpr int test_int_value = 42;
    constexpr std::chrono::milliseconds test_delay{100};
}

class PromiseExampleRunner {
public:
    auto run_all_scenarios() -> int {
        int failed_scenarios = 0;
        
        std::cout << "=== Folly Concept Wrapper Promise Examples ===\n\n";
        
        if (!test_semi_promise_value_setting()) failed_scenarios++;
        if (!test_semi_promise_exception_setting()) failed_scenarios++;
        if (!test_promise_future_retrieval()) failed_scenarios++;
        if (!test_promise_void_handling()) failed_scenarios++;
        if (!test_promise_lifecycle()) failed_scenarios++;
        
        std::cout << "\n=== Summary ===\n";
        if (failed_scenarios > 0) {
            std::cout << "❌ " << failed_scenarios << " scenario(s) failed\n";
            return 1;
        }
        
        std::cout << "✅ All scenarios passed!\n";
        return 0;
    }

private:
    auto test_semi_promise_value_setting() -> bool {
        std::cout << "Test 1: SemiPromise Value Setting\n";
        try {
            // Create a SemiPromise and set a value
            kythira::SemiPromise<std::string> semi_promise;
            
            // Verify initial state
            if (semi_promise.isFulfilled()) {
                std::cout << "  ❌ SemiPromise should not be fulfilled initially\n";
                return false;
            }
            
            // Set a value
            semi_promise.setValue(std::string(test_value));
            
            // Verify fulfilled state
            if (!semi_promise.isFulfilled()) {
                std::cout << "  ❌ SemiPromise should be fulfilled after setValue\n";
                return false;
            }
            
            std::cout << "  ✅ SemiPromise value setting works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_semi_promise_exception_setting() -> bool {
        std::cout << "Test 2: SemiPromise Exception Setting\n";
        try {
            // Create a SemiPromise and set an exception
            kythira::SemiPromise<int> semi_promise;
            
            // Set an exception
            auto exception = std::make_exception_ptr(std::runtime_error(test_error_message));
            semi_promise.setException(exception);
            
            // Verify fulfilled state
            if (!semi_promise.isFulfilled()) {
                std::cout << "  ❌ SemiPromise should be fulfilled after setException\n";
                return false;
            }
            
            std::cout << "  ✅ SemiPromise exception setting works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_promise_future_retrieval() -> bool {
        std::cout << "Test 3: Promise Future Retrieval\n";
        try {
            // Create a Promise and get its future
            kythira::Promise<int> promise;
            
            auto future = promise.getFuture();
            
            // Set a value on the promise
            promise.setValue(test_int_value);
            
            // Get the value from the future (this should be ready immediately)
            auto result = std::move(future).get();
            if (result != test_int_value) {
                std::cout << "  ❌ Future value mismatch: expected " << test_int_value 
                         << ", got " << result << "\n";
                return false;
            }
            
            std::cout << "  ✅ Promise future retrieval works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_promise_void_handling() -> bool {
        std::cout << "Test 4: Promise Void/Unit Handling\n";
        try {
            // Create a void Promise
            kythira::Promise<void> promise;
            
            auto future = promise.getFuture();
            
            // Set void value (Unit conversion)
            promise.setValue();
            
            // Get the void result
            std::move(future).get();
            
            std::cout << "  ✅ Promise void/Unit handling works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_promise_lifecycle() -> bool {
        std::cout << "Test 5: Promise Lifecycle Management\n";
        try {
            // Test promise-future relationship and lifecycle
            std::optional<kythira::Future<std::string>> future_opt;
            
            {
                kythira::Promise<std::string> promise;
                future_opt = promise.getFuture();
                
                // Promise goes out of scope, but future should still work
                promise.setValue(std::string(test_value));
            }
            
            // Future should still be valid and contain the value
            auto result = std::move(*future_opt).get();
            if (result != test_value) {
                std::cout << "  ❌ Future value mismatch after promise destruction\n";
                return false;
            }
            
            std::cout << "  ✅ Promise lifecycle management works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
};

int main() {
    PromiseExampleRunner runner;
    return runner.run_all_scenarios();
}