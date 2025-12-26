/**
 * @file concepts_usage_examples.cpp
 * @brief Comprehensive examples demonstrating the usage of enhanced C++20 concepts
 * 
 * This file provides practical examples of how to use the concepts defined in
 * include/concepts/future.hpp with both Folly types and custom implementations.
 */

#include <concepts/future.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

// Include Folly headers for examples
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/Try.h>
#include <folly/Unit.h>

// Include kythira wrappers
#include <raft/future.hpp>

namespace examples {

// Example constants
namespace {
    constexpr int example_value = 42;
    constexpr int example_multiplier = 2;
    constexpr std::chrono::milliseconds example_delay{100};
    constexpr std::chrono::seconds example_timeout{5};
    constexpr const char* example_message = "Hello, Concepts!";
    constexpr const char* example_error_message = "Example error occurred";
    constexpr std::size_t thread_pool_size = 4;
}

//=============================================================================
// Example 1: Basic try_type concept usage
//=============================================================================

/**
 * Generic function that works with any Try-like type
 */
template<kythira::try_type<int> TryType>
auto extract_value_safely(const TryType& try_obj, int default_value) -> int {
    if (try_obj.hasValue()) {
        return try_obj.value();
    } else {
        std::cerr << "Try contains exception, using default value\n";
        return default_value;
    }
}

auto demonstrate_try_concept() -> void {
    std::cout << "\n=== Try Concept Example ===\n";
    
    // Success case
    kythira::Try<int> success_try = kythira::Try<int>(example_value);
    auto result1 = extract_value_safely(success_try, 0);
    std::cout << "Success case result: " << result1 << "\n";
    
    // Error case
    kythira::Try<int> error_try = kythira::Try<int>(
        folly::exception_wrapper(std::runtime_error(example_error_message))
    );
    auto result2 = extract_value_safely(error_try, -1);
    std::cout << "Error case result: " << result2 << "\n";
}

//=============================================================================
// Example 2: Basic future concept usage
//=============================================================================

/**
 * Generic function that works with any Future-like type
 */
template<kythira::future<int> FutureType>
auto process_async_result(FutureType future) -> int {
    std::cout << "Processing future...\n";
    
    if (future.isReady()) {
        std::cout << "Future is ready, getting result immediately\n";
        return std::move(future).get();
    } else {
        std::cout << "Future not ready, adding continuation\n";
        return std::move(future)
            .thenValue([](int value) {
                std::cout << "Continuation executed with value: " << value << "\n";
                return value * example_multiplier;
            })
            .get();
    }
}

auto demonstrate_future_concept() -> void {
    std::cout << "\n=== Future Concept Example ===\n";
    
    // Ready future
    auto ready_future = kythira::Future<int>(example_value);
    auto result1 = process_async_result(std::move(ready_future));
    std::cout << "Ready future result: " << result1 << "\n";
    
    // Future with continuation
    folly::Promise<int> promise;
    auto folly_future = promise.getFuture();
    auto future = kythira::Future<int>(std::move(folly_future));
    
    // Fulfill promise in background thread
    std::thread([p = std::move(promise)]() mutable {
        std::this_thread::sleep_for(example_delay);
        p.setValue(example_value / 2);
    }).detach();
    
    auto result2 = process_async_result(std::move(future));
    std::cout << "Async future result: " << result2 << "\n";
}

//=============================================================================
// Example 3: Promise concepts usage
//=============================================================================

/**
 * Generic function that works with any Promise-like type
 */
template<kythira::promise<std::string> PromiseType>
auto create_greeting_future(PromiseType promise, const std::string& name) -> kythira::Future<std::string> {
    // Get future before fulfilling promise
    auto folly_future = promise.getFuture();
    auto future = kythira::Future<std::string>(std::move(folly_future));
    
    // Fulfill promise asynchronously
    std::thread([p = std::move(promise), name]() mutable {
        std::this_thread::sleep_for(example_delay);
        if (!p.isFulfilled()) {
            p.setValue("Hello, " + name + "!");
        }
    }).detach();
    
    return future;
}

/**
 * Generic function that works with semi-promise types
 */
template<kythira::semi_promise<int> SemiPromiseType>
auto fulfill_computation(SemiPromiseType& promise, int input) -> void {
    try {
        if (!promise.isFulfilled()) {
            int result = input * input; // Simple computation
            promise.setValue(result);
        }
    } catch (const std::exception& e) {
        if (!promise.isFulfilled()) {
            promise.setException(folly::exception_wrapper(e));
        }
    }
}

auto demonstrate_promise_concepts() -> void {
    std::cout << "\n=== Promise Concepts Example ===\n";
    
    // Promise concept example
    folly::Promise<std::string> greeting_promise;
    auto greeting_future = create_greeting_future(std::move(greeting_promise), "World");
    auto greeting = std::move(greeting_future).get();
    std::cout << "Greeting: " << greeting << "\n";
    
    // Semi-promise concept example
    folly::Promise<int> computation_promise;
    auto computation_folly_future = computation_promise.getFuture();
    auto computation_future = kythira::Future<int>(std::move(computation_folly_future));
    fulfill_computation(computation_promise, 7);
    auto computation_result = std::move(computation_future).get();
    std::cout << "Computation result: " << computation_result << "\n";
}

//=============================================================================
// Example 4: Executor concepts usage
//=============================================================================

/**
 * Generic function that works with any Executor-like type
 */
template<kythira::executor ExecutorType>
auto schedule_parallel_work(ExecutorType& executor, int num_tasks) -> void {
    std::cout << "Scheduling " << num_tasks << " parallel tasks\n";
    
    for (int i = 0; i < num_tasks; ++i) {
        executor.add([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::cout << "Task " << i << " completed on thread " 
                      << std::this_thread::get_id() << "\n";
        });
    }
}

/**
 * Generic function that works with KeepAlive types
 */
template<kythira::keep_alive KeepAliveType>
auto schedule_safe_work(KeepAliveType keep_alive, const std::string& work_name) -> void {
    std::cout << "Scheduling safe work: " << work_name << "\n";
    
    // The keep-alive ensures executor lifetime during work execution
    std::move(keep_alive).add([work_name](auto&&) {
        std::this_thread::sleep_for(example_delay);
        std::cout << "Safe work completed: " << work_name << "\n";
    });
}

auto demonstrate_executor_concepts() -> void {
    std::cout << "\n=== Executor Concepts Example ===\n";
    
    // Executor concept example
    folly::CPUThreadPoolExecutor thread_pool(thread_pool_size);
    schedule_parallel_work(thread_pool, 3);
    
    // Wait for tasks to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // KeepAlive concept example - use static method
    auto keep_alive = folly::Executor::getKeepAliveToken(&thread_pool);
    schedule_safe_work(std::move(keep_alive), "Critical Task");
    
    // Wait for safe work to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

//=============================================================================
// Example 5: Advanced concept combinations
//=============================================================================

/**
 * Complex example combining multiple concepts
 */
template<kythira::future<int> FutureType, 
         kythira::executor ExecutorType>
auto process_batch_async(std::vector<int> inputs, ExecutorType& executor) -> std::vector<int> {
    std::cout << "Processing batch of " << inputs.size() << " items\n";
    
    std::vector<kythira::Future<int>> futures;
    futures.reserve(inputs.size());
    
    // Create futures for each input
    for (int input : inputs) {
        folly::Promise<int> promise;
        auto folly_future = promise.getFuture();
        auto future = kythira::Future<int>(std::move(folly_future));
        
        // Schedule work on executor
        executor.add([p = std::move(promise), input]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            p.setValue(input * input);
        });
        
        futures.push_back(std::move(future));
    }
    
    // Collect all results (simplified - would use collectAll in real code)
    std::vector<int> results;
    results.reserve(futures.size());
    
    for (auto& future : futures) {
        results.push_back(std::move(future).get());
    }
    
    return results;
}

auto demonstrate_advanced_concepts() -> void {
    std::cout << "\n=== Advanced Concepts Example ===\n";
    
    // Batch processing example
    folly::CPUThreadPoolExecutor executor(thread_pool_size);
    std::vector<int> inputs = {1, 2, 3, 4, 5};
    auto results = process_batch_async<kythira::Future<int>>(inputs, executor);
    
    std::cout << "Batch results: ";
    for (int result : results) {
        std::cout << result << " ";
    }
    std::cout << "\n";
    
    // Simple future creation example
    std::cout << "Simple future creation demonstrated\n";
    auto simple_future = kythira::Future<int>(36);
    auto simple_result = std::move(simple_future).get();
    std::cout << "Simple future result: " << simple_result << "\n";
}

//=============================================================================
// Example 6: Concept validation and static assertions
//=============================================================================

auto demonstrate_concept_validation() -> void {
    std::cout << "\n=== Concept Validation Example ===\n";
    
    // Static assertions to verify Folly types satisfy concepts
    static_assert(kythira::try_type<kythira::Try<int>, int>);
    static_assert(kythira::try_type<kythira::Try<std::string>, std::string>);
    // Note: kythira::Try<void> is supported
    static_assert(kythira::try_type<kythira::Try<void>, void>);
    
    static_assert(kythira::future<kythira::Future<int>, int>);
    static_assert(kythira::future<kythira::Future<std::string>, std::string>);
    // Note: kythira::Future<void> is supported
    static_assert(kythira::future<kythira::Future<void>, void>);
    
    static_assert(kythira::semi_promise<folly::Promise<int>, int>);
    static_assert(kythira::promise<folly::Promise<int>, int>);
    
    static_assert(kythira::executor<folly::CPUThreadPoolExecutor>);
    
    using KeepAliveType = folly::Executor::KeepAlive<folly::CPUThreadPoolExecutor>;
    static_assert(kythira::keep_alive<KeepAliveType>);
    
    std::cout << "All static assertions passed!\n";
    std::cout << "Kythira wrapper types successfully satisfy the enhanced concepts.\n";
    std::cout << "Note: Kythira wrappers support void types properly.\n";
}

} // namespace examples

//=============================================================================
// Main function to run all examples
//=============================================================================

auto main() -> int {
    std::cout << "Enhanced C++20 Concepts Usage Examples\n";
    std::cout << "======================================\n";
    
    try {
        examples::demonstrate_try_concept();
        examples::demonstrate_future_concept();
        examples::demonstrate_promise_concepts();
        examples::demonstrate_executor_concepts();
        examples::demonstrate_advanced_concepts();
        examples::demonstrate_concept_validation();
        
        std::cout << "\n=== All Examples Completed Successfully ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Example failed with exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Example failed with unknown exception\n";
        return 1;
    }
}