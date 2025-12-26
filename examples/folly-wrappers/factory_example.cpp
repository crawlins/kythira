/**
 * @file factory_example.cpp
 * @brief Example demonstrating folly concept wrapper factory operations
 * 
 * This example shows how to:
 * 1. Use FutureFactory to create futures from values
 * 2. Use FutureFactory to create exceptional futures
 * 3. Create ready futures with void/Unit handling
 * 4. Handle type deduction and conversion
 * 5. Work with different value types
 */

#include <iostream>
#include <string>
#include <chrono>
#include <stdexcept>
#include <vector>

#include "../../include/raft/future.hpp"

namespace {
    constexpr const char* test_string_value = "Factory created future";
    constexpr const char* test_error_message = "Factory created exception";
    constexpr int test_int_value = 123;
    constexpr double test_double_value = 3.14159;
}

class FactoryExampleRunner {
public:
    auto run_all_scenarios() -> int {
        int failed_scenarios = 0;
        
        std::cout << "=== Folly Concept Wrapper Factory Examples ===\n\n";
        
        if (!test_make_future_with_value()) failed_scenarios++;
        if (!test_make_exceptional_future()) failed_scenarios++;
        if (!test_make_ready_future_void()) failed_scenarios++;
        if (!test_factory_type_deduction()) failed_scenarios++;
        if (!test_factory_different_types()) failed_scenarios++;
        
        std::cout << "\n=== Summary ===\n";
        if (failed_scenarios > 0) {
            std::cout << "❌ " << failed_scenarios << " scenario(s) failed\n";
            return 1;
        }
        
        std::cout << "✅ All scenarios passed!\n";
        return 0;
    }

private:
    auto test_make_future_with_value() -> bool {
        std::cout << "Test 1: FutureFactory makeFuture with Value\n";
        try {
            // Create future from string value
            auto future_str = kythira::FutureFactory::makeFuture(std::string(test_string_value));
            
            // Future should be ready immediately
            if (!future_str.isReady()) {
                std::cout << "  ❌ Factory-created future should be ready immediately\n";
                return false;
            }
            
            // Get the value
            auto result = std::move(future_str).get();
            if (result != test_string_value) {
                std::cout << "  ❌ Factory future value mismatch: expected '" 
                         << test_string_value << "', got '" << result << "'\n";
                return false;
            }
            
            // Create future from integer value
            auto future_int = kythira::FutureFactory::makeFuture(test_int_value);
            auto int_result = std::move(future_int).get();
            if (int_result != test_int_value) {
                std::cout << "  ❌ Factory integer future value mismatch\n";
                return false;
            }
            
            std::cout << "  ✅ FutureFactory makeFuture with value works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_make_exceptional_future() -> bool {
        std::cout << "Test 2: FutureFactory makeExceptionalFuture\n";
        try {
            // Create exceptional future
            auto exception = std::make_exception_ptr(std::runtime_error(test_error_message));
            auto future = kythira::FutureFactory::makeExceptionalFuture<std::string>(exception);
            
            // Future should be ready immediately
            if (!future.isReady()) {
                std::cout << "  ❌ Exceptional future should be ready immediately\n";
                return false;
            }
            
            // Getting the value should throw
            bool exception_thrown = false;
            std::string exception_message;
            try {
                std::move(future).get();
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                exception_message = e.what();
            }
            
            if (!exception_thrown) {
                std::cout << "  ❌ Exceptional future should throw when getting value\n";
                return false;
            }
            
            if (exception_message != test_error_message) {
                std::cout << "  ❌ Exception message mismatch: expected '" 
                         << test_error_message << "', got '" << exception_message << "'\n";
                return false;
            }
            
            std::cout << "  ✅ FutureFactory makeExceptionalFuture works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Unexpected exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_make_ready_future_void() -> bool {
        std::cout << "Test 3: FutureFactory makeReadyFuture (void)\n";
        try {
            // Create ready void future
            auto future = kythira::FutureFactory::makeReadyFuture();
            
            // Future should be ready immediately
            if (!future.isReady()) {
                std::cout << "  ❌ Ready void future should be ready immediately\n";
                return false;
            }
            
            // Getting the void value should work without throwing
            std::move(future).get();
            
            std::cout << "  ✅ FutureFactory makeReadyFuture (void) works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_factory_type_deduction() -> bool {
        std::cout << "Test 4: Factory Type Deduction\n";
        try {
            // Test automatic type deduction
            auto future1 = kythira::FutureFactory::makeFuture(42);  // int
            auto future2 = kythira::FutureFactory::makeFuture(3.14);  // double
            auto future3 = kythira::FutureFactory::makeFuture(std::string("test"));  // string
            
            // All should be ready
            if (!future1.isReady() || !future2.isReady() || !future3.isReady()) {
                std::cout << "  ❌ Type-deduced futures should be ready immediately\n";
                return false;
            }
            
            // Verify values
            if (std::move(future1).get() != 42) {
                std::cout << "  ❌ Type-deduced int future value mismatch\n";
                return false;
            }
            
            if (std::abs(std::move(future2).get() - 3.14) > 0.001) {
                std::cout << "  ❌ Type-deduced double future value mismatch\n";
                return false;
            }
            
            if (std::move(future3).get() != "test") {
                std::cout << "  ❌ Type-deduced string future value mismatch\n";
                return false;
            }
            
            std::cout << "  ✅ Factory type deduction works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_factory_different_types() -> bool {
        std::cout << "Test 5: Factory with Different Types\n";
        try {
            // Test with various types
            std::vector<int> test_vector{1, 2, 3, 4, 5};
            auto future_vector = kythira::FutureFactory::makeFuture(test_vector);
            
            auto result_vector = std::move(future_vector).get();
            if (result_vector != test_vector) {
                std::cout << "  ❌ Vector future value mismatch\n";
                return false;
            }
            
            // Test with custom struct
            struct TestStruct {
                int value;
                std::string name;
                
                bool operator==(const TestStruct& other) const {
                    return value == other.value && name == other.name;
                }
            };
            
            TestStruct test_struct{test_int_value, test_string_value};
            auto future_struct = kythira::FutureFactory::makeFuture(test_struct);
            
            auto result_struct = std::move(future_struct).get();
            if (!(result_struct == test_struct)) {
                std::cout << "  ❌ Struct future value mismatch\n";
                return false;
            }
            
            std::cout << "  ✅ Factory with different types works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
};

int main() {
    FactoryExampleRunner runner;
    return runner.run_all_scenarios();
}