#define BOOST_TEST_MODULE folly_concept_wrappers_interoperability_property_test
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/ExceptionWrapper.h>
#include <folly/Unit.h>
#include <folly/Executor.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "../include/raft/future.hpp"
#include "../include/concepts/future.hpp"

namespace {
    constexpr std::size_t test_iterations = 100;
    constexpr std::chrono::seconds test_timeout{30};
    constexpr const char* test_string_value = "interop_test";
    constexpr int test_int_value = 123;
    constexpr const char* test_exception_message = "interop exception";
}

/**
 * **Feature: folly-concept-wrappers, Property 10: Backward Compatibility and Interoperability**
 * 
 * This test validates seamless interoperability between different wrapper types,
 * ensuring they can be mixed and used together in the same code without issues.
 * 
 * **Validates: Requirements 10.2, 10.4**
 */
BOOST_AUTO_TEST_CASE(test_wrapper_type_interoperability, * boost::unit_test::timeout(30)) {
    // Test that different wrapper types can work together seamlessly
    
    // Test Promise -> Future interoperability
    {
        kythira::Promise<int> promise;
        auto future = promise.getFuture();
        
        // Promise and Future should work together
        promise.setValue(test_int_value);
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_int_value);
    }
    
    // Test SemiPromise -> Promise conversion (conceptual)
    {
        kythira::SemiPromise<std::string> semi_promise;
        semi_promise.setValue(std::string(test_string_value));
        BOOST_CHECK(semi_promise.isFulfilled());
        
        // SemiPromise should have same basic functionality as Promise
        // (they share the same underlying implementation)
    }
    
    // Test Executor -> KeepAlive interoperability
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        kythira::Executor executor(cpu_executor.get());
        auto keep_alive = executor.get_keep_alive();
        
        // Executor and KeepAlive should work together
        BOOST_CHECK(executor.is_valid());
        BOOST_CHECK(keep_alive.is_valid());
        BOOST_CHECK_EQUAL(executor.get(), keep_alive.get());
    }
}

BOOST_AUTO_TEST_CASE(test_mixed_wrapper_usage_in_same_code, * boost::unit_test::timeout(30)) {
    // Test using multiple wrapper types in the same code block
    
    auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(2);
    kythira::Executor executor(cpu_executor.get());
    auto keep_alive = executor.get_keep_alive();
    
    // Create promises and futures
    kythira::Promise<int> promise1;
    kythira::Promise<std::string> promise2;
    kythira::SemiPromise<double> semi_promise;
    
    auto future1 = promise1.getFuture();
    auto future2 = promise2.getFuture();
    
    // Use factory to create additional futures
    auto factory_future = kythira::FutureFactory::makeFuture(test_int_value);
    auto exceptional_future = kythira::FutureFactory::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error(test_exception_message)));
    
    // All should work together without issues
    promise1.setValue(test_int_value);
    promise2.setValue(std::string(test_string_value));
    semi_promise.setValue(3.14);
    
    BOOST_CHECK(promise1.isFulfilled());
    BOOST_CHECK(promise2.isFulfilled());
    BOOST_CHECK(semi_promise.isFulfilled());
    
    BOOST_CHECK_EQUAL(future1.get(), test_int_value);
    BOOST_CHECK_EQUAL(future2.get(), test_string_value);
    BOOST_CHECK_EQUAL(factory_future.get(), test_int_value);
    BOOST_CHECK_THROW(exceptional_future.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_conversion_between_wrapper_types, * boost::unit_test::timeout(30)) {
    // Test conversion utilities between wrapper types
    
    // Test folly::Try -> kythira::Try conversion
    {
        folly::Try<int> folly_try(test_int_value);
        kythira::Try<int> kythira_try(std::move(folly_try));
        
        BOOST_CHECK(kythira_try.hasValue());
        BOOST_CHECK_EQUAL(kythira_try.value(), test_int_value);
    }
    
    // Test folly::Future -> kythira::Future conversion
    {
        auto folly_future = folly::makeFuture(test_int_value);
        kythira::Future<int> kythira_future(std::move(folly_future));
        
        BOOST_CHECK(kythira_future.isReady());
        BOOST_CHECK_EQUAL(kythira_future.get(), test_int_value);
    }
    
    // Test folly::Promise -> kythira::Promise conversion
    {
        folly::Promise<int> folly_promise;
        auto folly_future = folly_promise.getFuture();
        
        kythira::Promise<int> kythira_promise(std::move(folly_promise));
        
        // Should be able to convert back and forth
        // Note: After moving folly_promise into kythira_promise, we can't check isFulfilled
        // on the original promise since it's been moved from
        BOOST_CHECK(!kythira_promise.isFulfilled());
    }
    
    // Test exception conversion between types
    {
        auto std_exception = std::make_exception_ptr(std::runtime_error(test_exception_message));
        auto folly_exception = kythira::detail::to_folly_exception_wrapper(std_exception);
        auto converted_back = kythira::detail::to_std_exception_ptr(folly_exception);
        
        BOOST_CHECK(converted_back != nullptr);
        
        // Both should represent the same exception
        try {
            std::rethrow_exception(converted_back);
        } catch (const std::runtime_error& e) {
            BOOST_CHECK_EQUAL(std::string(e.what()), test_exception_message);
        }
    }
}

BOOST_AUTO_TEST_CASE(test_concept_constrained_template_compatibility, * boost::unit_test::timeout(30)) {
    // Test that wrappers work with concept-constrained template functions
    
    // Simple template function that requires future concept
    auto process_int_future = [](kythira::Future<int> future) -> int {
        return future.get();
    };
    
    // Simple template function that requires promise concept
    auto fulfill_int_promise = [](kythira::Promise<int>& promise, int value) -> void {
        promise.setValue(value);
    };
    
    // Simple template function that requires executor concept
    auto submit_work_to_executor = [](kythira::Executor& executor, std::function<void()> work) -> void {
        executor.add(std::move(work));
    };
    
    // Test with our wrapper types
    {
        kythira::Future<int> future(test_int_value);
        int result = process_int_future(std::move(future));
        BOOST_CHECK_EQUAL(result, test_int_value);
    }
    
    {
        kythira::Promise<int> promise;
        fulfill_int_promise(promise, test_int_value);
        BOOST_CHECK(promise.isFulfilled());
    }
    
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        kythira::Executor executor(cpu_executor.get());
        
        bool work_done = false;
        submit_work_to_executor(executor, [&work_done]() { work_done = true; });
        
        // Give some time for work to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        BOOST_CHECK(work_done);
    }
}

BOOST_AUTO_TEST_CASE(test_collection_operations_interoperability, * boost::unit_test::timeout(30)) {
    // Test that collection operations work with mixed wrapper types
    
    // Create futures using different methods
    std::vector<kythira::Future<int>> futures;
    
    // Future from value
    futures.push_back(kythira::Future<int>(test_int_value));
    
    // Future from factory
    futures.push_back(kythira::FutureFactory::makeFuture(test_int_value + 1));
    
    // Future from promise
    kythira::Promise<int> promise;
    futures.push_back(promise.getFuture());
    promise.setValue(test_int_value + 2);
    
    // Test collectAll with mixed futures
    auto all_results = kythira::FutureCollector::collectAll(std::move(futures));
    BOOST_CHECK(all_results.isReady());
    
    auto results = all_results.get();
    BOOST_CHECK_EQUAL(results.size(), 3);
    
    for (const auto& result : results) {
        BOOST_CHECK(result.hasValue());
        BOOST_CHECK(!result.hasException());
    }
    
    BOOST_CHECK_EQUAL(results[0].value(), test_int_value);
    BOOST_CHECK_EQUAL(results[1].value(), test_int_value + 1);
    BOOST_CHECK_EQUAL(results[2].value(), test_int_value + 2);
}

BOOST_AUTO_TEST_CASE(test_property_interoperability_with_random_data, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> int_dist(-1000, 1000);
    std::uniform_int_distribution<std::size_t> size_dist(2, 5);
    
    auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(2);
    
    for (std::size_t i = 0; i < test_iterations; ++i) {
        // Test interoperability with random values
        int random_value1 = int_dist(gen);
        int random_value2 = int_dist(gen);
        
        // Create mixed wrapper types
        kythira::Promise<int> promise;
        kythira::SemiPromise<int> semi_promise;
        kythira::Executor executor(cpu_executor.get());
        auto keep_alive = executor.get_keep_alive();
        
        // Test that they work together
        auto future = promise.getFuture();
        promise.setValue(random_value1);
        semi_promise.setValue(random_value2);
        
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(semi_promise.isFulfilled());
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), random_value1);
        
        // Test executor interoperability
        BOOST_CHECK(executor.is_valid());
        BOOST_CHECK(keep_alive.is_valid());
        BOOST_CHECK_EQUAL(executor.get(), keep_alive.get());
        
        // Test factory interoperability
        auto factory_future = kythira::FutureFactory::makeFuture(random_value1);
        BOOST_CHECK(factory_future.isReady());
        BOOST_CHECK_EQUAL(factory_future.get(), random_value1);
    }
}

BOOST_AUTO_TEST_CASE(test_void_type_interoperability, * boost::unit_test::timeout(30)) {
    // Test interoperability specifically with void types (which use folly::Unit internally)
    
    // Test void Promise -> void Future
    {
        kythira::Promise<void> void_promise;
        auto void_future = void_promise.getFuture();
        
        void_promise.setValue();
        BOOST_CHECK(void_promise.isFulfilled());
        BOOST_CHECK(void_future.isReady());
        BOOST_CHECK_NO_THROW(void_future.get());
    }
    
    // Test void SemiPromise
    {
        kythira::SemiPromise<void> void_semi_promise;
        void_semi_promise.setValue();
        BOOST_CHECK(void_semi_promise.isFulfilled());
    }
    
    // Test void Future chaining
    {
        kythira::Future<void> void_future;
        auto chained = std::move(void_future).thenValue([]() {
            return test_int_value;
        });
        
        BOOST_CHECK(chained.isReady());
        BOOST_CHECK_EQUAL(chained.get(), test_int_value);
    }
    
    // Test void collection operations
    {
        std::vector<kythira::Future<void>> void_futures;
        void_futures.push_back(kythira::Future<void>());
        void_futures.push_back(kythira::FutureFactory::makeFuture());
        
        auto all_results = kythira::FutureCollector::collectAll(std::move(void_futures));
        BOOST_CHECK(all_results.isReady());
        
        auto results = all_results.get();
        BOOST_CHECK_EQUAL(results.size(), 2);
        
        for (const auto& result : results) {
            BOOST_CHECK(result.hasValue());
            BOOST_CHECK(!result.hasException());
        }
    }
}