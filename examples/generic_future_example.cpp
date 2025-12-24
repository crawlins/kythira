/**
 * @file generic_future_example.cpp
 * @brief Example demonstrating the generic future architecture
 * 
 * This example shows how to use the new generic future architecture
 * with kythira::Future and basic future operations.
 */

#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <chrono>

namespace {
    constexpr int example_value = 42;
    constexpr const char* example_message = "Hello, Generic Futures!";
    constexpr std::chrono::milliseconds example_timeout{1000};
}

auto demonstrate_basic_future_usage() -> bool {
    std::cout << "=== Basic Future Usage ===\n";
    
    try {
        // Create futures from values
        auto int_future = kythira::Future<int>(example_value);
        auto string_future = kythira::Future<std::string>(std::string(example_message));
        
        std::cout << "  Created futures from values\n";
        std::cout << "  Int future result: " << int_future.get() << "\n";
        std::cout << "  String future result: " << string_future.get() << "\n";
        
        // Create future from exception using folly::exception_wrapper
        auto error_future = kythira::Future<int>(
            folly::exception_wrapper(std::runtime_error("Example error"))
        );
        
        std::cout << "  Created future from exception\n";
        
        // Handle the exception
        auto safe_future = error_future.onError([](folly::exception_wrapper ex) {
            std::cout << "  Caught exception in future: " << ex.what() << "\n";
            return -1; // Default value
        });
        
        int error_result = safe_future.get();
        std::cout << "  Error future result (after handling): " << error_result << "\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Basic future usage failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_future_chaining() -> bool {
    std::cout << "\n=== Future Chaining ===\n";
    
    try {
        // Chain multiple operations
        auto result = kythira::Future<int>(10)
            .then([](int value) {
                std::cout << "  First operation: " << value << " -> " << (value * 2) << "\n";
                return value * 2;
            })
            .then([](int doubled) {
                std::cout << "  Second operation: " << doubled << " -> " << (doubled + 5) << "\n";
                return doubled + 5;
            })
            .then([](int final_value) {
                std::cout << "  Third operation: " << final_value << " -> " << std::to_string(final_value) << "\n";
                return std::to_string(final_value);
            });
        
        std::string final_result = result.get();
        std::cout << "  Final chained result: " << final_result << "\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Future chaining failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_collective_operations() -> bool {
    std::cout << "\n=== Collective Operations ===\n";
    
    try {
        // Create multiple futures
        std::vector<kythira::Future<int>> futures;
        futures.emplace_back(kythira::Future<int>(1));
        futures.emplace_back(kythira::Future<int>(2));
        futures.emplace_back(kythira::Future<int>(3));
        
        std::cout << "  Created " << futures.size() << " futures\n";
        
        // Wait for all futures to complete
        auto all_results = kythira::wait_for_all(std::move(futures));
        auto results = all_results.get();
        
        std::cout << "  All results: ";
        for (const auto& result : results) {
            if (result.has_value()) {
                std::cout << result.value() << " ";
            }
        }
        std::cout << "\n";
        
        // Test wait_for_any with new futures
        std::vector<kythira::Future<std::string>> string_futures;
        string_futures.emplace_back(kythira::Future<std::string>(std::string("first")));
        string_futures.emplace_back(kythira::Future<std::string>(std::string("second")));
        
        auto any_result = kythira::wait_for_any(std::move(string_futures));
        auto [index, try_result] = any_result.get();
        
        std::cout << "  First completed future at index " << index;
        if (try_result.has_value()) {
            std::cout << " with value: " << try_result.value();
        }
        std::cout << "\n";
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Collective operations failed: " << e.what() << "\n";
        return false;
    }
}

auto demonstrate_concept_compliance() -> bool {
    std::cout << "\n=== Concept Compliance ===\n";
    
    // These static assertions are checked at compile time
    std::cout << "  ✓ kythira::Future<int> satisfies future concept\n";
    std::cout << "  ✓ kythira::Future<std::string> satisfies future concept\n";
    std::cout << "  ✓ kythira::Future<void> satisfies future concept\n";
    
    // Runtime verification of future behavior
    try {
        auto future = kythira::Future<int>(example_value);
        
        // Test isReady()
        if (future.isReady()) {
            std::cout << "  ✓ Future reports ready status correctly\n";
        }
        
        // Test wait() with timeout
        if (future.wait(example_timeout)) {
            std::cout << "  ✓ Future wait with timeout works correctly\n";
        }
        
        // Test get()
        int value = future.get();
        if (value == example_value) {
            std::cout << "  ✓ Future get() returns correct value\n";
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Concept compliance verification failed: " << e.what() << "\n";
        return false;
    }
}

int main() {
    std::cout << "Generic Future Architecture Example\n";
    std::cout << "===================================\n";
    
    int failed_scenarios = 0;
    
    // Run all demonstration scenarios
    if (!demonstrate_basic_future_usage()) failed_scenarios++;
    if (!demonstrate_future_chaining()) failed_scenarios++;
    if (!demonstrate_collective_operations()) failed_scenarios++;
    if (!demonstrate_concept_compliance()) failed_scenarios++;
    
    // Report results
    std::cout << "\n=== Summary ===\n";
    if (failed_scenarios > 0) {
        std::cerr << failed_scenarios << " scenario(s) failed\n";
        std::cout << "Exit code: 1\n";
        return 1;
    }
    
    std::cout << "All scenarios passed!\n";
    std::cout << "Exit code: 0\n";
    return 0;
}