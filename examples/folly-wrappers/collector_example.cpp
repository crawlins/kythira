/**
 * @file collector_example.cpp
 * @brief Example demonstrating folly concept wrapper collection operations
 * 
 * This example shows how to:
 * 1. Use FutureCollector::collectAll to wait for all futures
 * 2. Use FutureCollector::collectAny to get first completed future
 * 3. Use FutureCollector::collectAnyWithoutException for first successful future
 * 4. Use FutureCollector::collectN for first N completed futures
 * 5. Handle timeout and cancellation scenarios
 */

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <stdexcept>

#include "../../include/raft/future.hpp"
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>

namespace {
    constexpr const char* test_value_prefix = "Result";
    constexpr const char* test_error_message = "Collector test exception";
    constexpr std::chrono::milliseconds short_delay{50};
    constexpr std::chrono::milliseconds medium_delay{100};
    constexpr std::chrono::milliseconds long_delay{200};
    constexpr std::size_t test_future_count = 5;
    constexpr std::size_t test_collect_n = 3;
}

class CollectorExampleRunner {
public:
    auto run_all_scenarios() -> int {
        int failed_scenarios = 0;
        
        std::cout << "=== Folly Concept Wrapper Collector Examples ===\n\n";
        
        if (!test_collect_all_success()) failed_scenarios++;
        if (!test_collect_any_first_completed()) failed_scenarios++;
        if (!test_collect_any_without_exception()) failed_scenarios++;
        if (!test_collect_n_futures()) failed_scenarios++;
        if (!test_collect_all_with_exception()) failed_scenarios++;
        
        std::cout << "\n=== Summary ===\n";
        if (failed_scenarios > 0) {
            std::cout << "❌ " << failed_scenarios << " scenario(s) failed\n";
            return 1;
        }
        
        std::cout << "✅ All scenarios passed!\n";
        return 0;
    }

private:
    auto create_delayed_future(int value, std::chrono::milliseconds delay) -> kythira::Future<int> {
        static auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(4);
        
        kythira::Promise<int> promise;
        auto future = promise.getFuture();
        
        // Schedule the promise fulfillment on the executor
        executor->add([promise = std::move(promise), value, delay]() mutable {
            std::this_thread::sleep_for(delay);
            promise.setValue(value);
        });
        
        return future;
    }
    
    auto create_exceptional_future(std::chrono::milliseconds delay) -> kythira::Future<int> {
        static auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(4);
        
        kythira::Promise<int> promise;
        auto future = promise.getFuture();
        
        // Schedule the promise exception on the executor
        executor->add([promise = std::move(promise), delay]() mutable {
            std::this_thread::sleep_for(delay);
            auto exception = std::make_exception_ptr(std::runtime_error(test_error_message));
            promise.setException(exception);
        });
        
        return future;
    }
    
    auto test_collect_all_success() -> bool {
        std::cout << "Test 1: FutureCollector collectAll (all success)\n";
        try {
            // Create multiple futures with different delays
            std::vector<kythira::Future<int>> futures;
            futures.push_back(create_delayed_future(1, short_delay));
            futures.push_back(create_delayed_future(2, medium_delay));
            futures.push_back(create_delayed_future(3, long_delay));
            
            // Collect all futures
            auto start_time = std::chrono::steady_clock::now();
            auto results = kythira::FutureCollector::collectAll(std::move(futures)).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Should wait for the longest delay
            if (elapsed < long_delay) {
                std::cout << "  ❌ collectAll returned too quickly (should wait for all)\n";
                return false;
            }
            
            // Verify all results
            if (results.size() != 3) {
                std::cout << "  ❌ collectAll result count mismatch: expected 3, got " 
                         << results.size() << "\n";
                return false;
            }
            
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (!results[i].hasValue() || results[i].value() != static_cast<int>(i + 1)) {
                    std::cout << "  ❌ collectAll result[" << i << "] mismatch: expected " 
                             << (i + 1) << ", got ";
                    if (results[i].hasValue()) {
                        std::cout << results[i].value();
                    } else {
                        std::cout << "exception";
                    }
                    std::cout << "\n";
                    return false;
                }
            }
            
            std::cout << "  ✅ FutureCollector collectAll (all success) works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_collect_any_first_completed() -> bool {
        std::cout << "Test 2: FutureCollector collectAny (first completed)\n";
        try {
            // Create futures with different delays (first should complete first)
            std::vector<kythira::Future<int>> futures;
            futures.push_back(create_delayed_future(100, short_delay));  // This should complete first
            futures.push_back(create_delayed_future(200, long_delay));
            futures.push_back(create_delayed_future(300, long_delay));
            
            // Collect any future
            auto start_time = std::chrono::steady_clock::now();
            auto result = kythira::FutureCollector::collectAny(std::move(futures)).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Should return quickly (after short_delay)
            if (elapsed > medium_delay) {
                std::cout << "  ❌ collectAny took too long (should return after first completion)\n";
                return false;
            }
            
            // Verify we got the first result
            if (!std::get<1>(result).hasValue() || std::get<1>(result).value() != 100) {
                std::cout << "  ❌ collectAny value mismatch: expected 100, got ";
                if (std::get<1>(result).hasValue()) {
                    std::cout << std::get<1>(result).value();
                } else {
                    std::cout << "exception";
                }
                std::cout << "\n";
                return false;
            }
            
            if (std::get<0>(result) != 0) {  // Should be index 0 (first future)
                std::cout << "  ❌ collectAny index mismatch: expected 0, got " 
                         << std::get<0>(result) << "\n";
                return false;
            }
            
            std::cout << "  ✅ FutureCollector collectAny (first completed) works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_collect_any_without_exception() -> bool {
        std::cout << "Test 3: FutureCollector collectAnyWithoutException\n";
        try {
            // Create futures: first fails, second succeeds
            std::vector<kythira::Future<int>> futures;
            futures.push_back(create_exceptional_future(short_delay));  // This fails first
            futures.push_back(create_delayed_future(42, medium_delay));  // This succeeds
            futures.push_back(create_delayed_future(99, long_delay));
            
            // Collect any successful future
            auto start_time = std::chrono::steady_clock::now();
            auto result = kythira::FutureCollector::collectAnyWithoutException(std::move(futures)).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Should wait for the first successful future (medium_delay)
            if (elapsed < short_delay) {
                std::cout << "  ❌ collectAnyWithoutException returned too quickly\n";
                return false;
            }
            
            // Verify we got the successful result
            if (std::get<1>(result) != 42) {
                std::cout << "  ❌ collectAnyWithoutException value mismatch: expected 42, got " 
                         << std::get<1>(result) << "\n";
                return false;
            }
            
            if (std::get<0>(result) != 1) {  // Should be index 1 (second future)
                std::cout << "  ❌ collectAnyWithoutException index mismatch: expected 1, got " 
                         << std::get<0>(result) << "\n";
                return false;
            }
            
            std::cout << "  ✅ FutureCollector collectAnyWithoutException works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_collect_n_futures() -> bool {
        std::cout << "Test 4: FutureCollector collectN\n";
        try {
            // Create multiple futures with different delays
            std::vector<kythira::Future<int>> futures;
            for (std::size_t i = 0; i < test_future_count; ++i) {
                auto delay = short_delay + std::chrono::milliseconds(i * 25);
                futures.push_back(create_delayed_future(static_cast<int>(i), delay));
            }
            
            // Collect first N futures
            auto start_time = std::chrono::steady_clock::now();
            auto results = kythira::FutureCollector::collectN(std::move(futures), test_collect_n).get();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            
            // Should return after the Nth future completes
            auto expected_delay = short_delay + std::chrono::milliseconds((test_collect_n - 1) * 25);
            if (elapsed < expected_delay) {
                std::cout << "  ❌ collectN returned too quickly\n";
                return false;
            }
            
            // Verify we got N results
            if (results.size() != test_collect_n) {
                std::cout << "  ❌ collectN result count mismatch: expected " 
                         << test_collect_n << ", got " << results.size() << "\n";
                return false;
            }
            
            // Results should be in completion order (0, 1, 2)
            for (std::size_t i = 0; i < test_collect_n; ++i) {
                if (!std::get<1>(results[i]).hasValue() || 
                    std::get<1>(results[i]).value() != static_cast<int>(i)) {
                    std::cout << "  ❌ collectN result[" << i << "] value mismatch\n";
                    return false;
                }
                if (std::get<0>(results[i]) != i) {
                    std::cout << "  ❌ collectN result[" << i << "] index mismatch\n";
                    return false;
                }
            }
            
            std::cout << "  ✅ FutureCollector collectN works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Exception: " << e.what() << "\n";
            return false;
        }
    }
    
    auto test_collect_all_with_exception() -> bool {
        std::cout << "Test 5: FutureCollector collectAll with Exception\n";
        try {
            // Create futures: some succeed, one fails
            std::vector<kythira::Future<int>> futures;
            futures.push_back(create_delayed_future(1, short_delay));
            futures.push_back(create_exceptional_future(medium_delay));  // This one fails
            futures.push_back(create_delayed_future(3, long_delay));
            
            // collectAll should propagate the exception
            bool exception_thrown = false;
            std::string exception_message;
            try {
                auto results = kythira::FutureCollector::collectAll(std::move(futures)).get();
                
                // Check if any result has an exception
                for (const auto& result : results) {
                    if (result.hasException()) {
                        exception_thrown = true;
                        auto ex_ptr = result.exception();
                        if (ex_ptr) {
                            try {
                                std::rethrow_exception(ex_ptr);
                            } catch (const std::runtime_error& e) {
                                exception_message = e.what();
                            }
                        }
                        break;
                    }
                }
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                exception_message = e.what();
            }
            
            if (!exception_thrown) {
                std::cout << "  ❌ collectAll should contain exception when any future fails\n";
                return false;
            }
            
            if (exception_message != test_error_message) {
                std::cout << "  ❌ Exception message mismatch in collectAll: expected '" 
                         << test_error_message << "', got '" << exception_message << "'\n";
                return false;
            }
            
            std::cout << "  ✅ FutureCollector collectAll with exception works correctly\n";
            return true;
        } catch (const std::exception& e) {
            std::cout << "  ❌ Unexpected exception: " << e.what() << "\n";
            return false;
        }
    }
};

int main(int argc, char* argv[]) {
    folly::Init init(&argc, &argv);
    CollectorExampleRunner runner;
    return runner.run_all_scenarios();
}