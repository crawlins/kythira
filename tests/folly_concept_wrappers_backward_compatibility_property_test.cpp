#define BOOST_TEST_MODULE folly_concept_wrappers_backward_compatibility_property_test
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
    constexpr const char* test_string_value = "test_value";
    constexpr int test_int_value = 42;
    constexpr const char* test_exception_message = "test exception";
}

/**
 * **Feature: folly-concept-wrappers, Property 10: Backward Compatibility and Interoperability**
 * 
 * This test validates that new wrapper classes maintain API compatibility with existing
 * Try and Future implementations, ensuring seamless integration with existing code.
 * 
 * **Validates: Requirements 10.1, 10.2, 10.3, 10.5**
 */
BOOST_AUTO_TEST_CASE(test_backward_compatibility_with_existing_try_and_future, * boost::unit_test::timeout(30)) {
    // Test that existing Try and Future classes continue to work as expected
    
    // Test existing Try<int> functionality
    {
        kythira::Try<int> try_with_value(test_int_value);
        BOOST_CHECK(try_with_value.hasValue());
        BOOST_CHECK(!try_with_value.hasException());
        BOOST_CHECK_EQUAL(try_with_value.value(), test_int_value);
    }
    
    // Test existing Try<void> functionality
    {
        kythira::Try<void> try_void;
        BOOST_CHECK(try_void.hasValue());
        BOOST_CHECK(!try_void.hasException());
        // void Try should not throw when accessing value
        BOOST_CHECK_NO_THROW(try_void.value());
    }
    
    // Test existing Try with exception
    {
        auto ex = std::make_exception_ptr(std::runtime_error(test_exception_message));
        kythira::Try<int> try_with_exception(ex);
        BOOST_CHECK(!try_with_exception.hasValue());
        BOOST_CHECK(try_with_exception.hasException());
        BOOST_CHECK(try_with_exception.exception() != nullptr);
    }
    
    // Test existing Future<int> functionality
    {
        kythira::Future<int> future_with_value(test_int_value);
        BOOST_CHECK(future_with_value.isReady());
        BOOST_CHECK_EQUAL(future_with_value.get(), test_int_value);
    }
    
    // Test existing Future<void> functionality
    {
        kythira::Future<void> future_void;
        BOOST_CHECK(future_void.isReady());
        BOOST_CHECK_NO_THROW(future_void.get());
    }
    
    // Test existing Future with exception
    {
        auto ex = std::make_exception_ptr(std::runtime_error(test_exception_message));
        kythira::Future<int> future_with_exception(ex);
        BOOST_CHECK(future_with_exception.isReady());
        BOOST_CHECK_THROW(future_with_exception.get(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(test_api_compatibility_with_existing_code, * boost::unit_test::timeout(30)) {
    // Test that new wrappers maintain the same API as existing implementations
    
    // Test Promise API compatibility
    {
        kythira::Promise<int> promise;
        BOOST_CHECK(!promise.isFulfilled());
        
        // setValue should work the same way
        promise.setValue(test_int_value);
        BOOST_CHECK(promise.isFulfilled());
        
        // getFuture should return a compatible Future
        auto future = promise.getFuture();
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_int_value);
    }
    
    // Test SemiPromise API compatibility
    {
        kythira::SemiPromise<std::string> semi_promise;
        BOOST_CHECK(!semi_promise.isFulfilled());
        
        // setValue should work the same way
        semi_promise.setValue(std::string(test_string_value));
        BOOST_CHECK(semi_promise.isFulfilled());
    }
    
    // Test Executor API compatibility
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        kythira::Executor executor(cpu_executor.get());
        BOOST_CHECK(executor.is_valid());
        
        // add should work the same way
        bool work_executed = false;
        executor.add([&work_executed]() { work_executed = true; });
        
        // Give some time for work to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        BOOST_CHECK(work_executed);
    }
    
    // Test KeepAlive API compatibility
    {
        auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
        kythira::Executor executor(cpu_executor.get());
        auto keep_alive = executor.get_keep_alive();
        
        BOOST_CHECK(keep_alive.is_valid());
        BOOST_CHECK(keep_alive.get() != nullptr);
    }
}

BOOST_AUTO_TEST_CASE(test_no_breaking_changes_to_existing_functionality, * boost::unit_test::timeout(30)) {
    // Test that existing functionality continues to work without breaking changes
    
    // Test that existing Future chaining still works
    {
        kythira::Future<int> future(test_int_value);
        
        auto chained = std::move(future).thenValue([](int value) {
            return value * 2;
        });
        
        BOOST_CHECK(chained.isReady());
        BOOST_CHECK_EQUAL(chained.get(), test_int_value * 2);
    }
    
    // Test that existing error handling still works
    {
        auto ex = std::make_exception_ptr(std::runtime_error(test_exception_message));
        kythira::Future<int> future(ex);
        
        auto handled = std::move(future).thenError([](std::exception_ptr ep) -> int {
            try {
                std::rethrow_exception(ep);
            } catch (const std::runtime_error& e) {
                return test_int_value; // Return default value on error
            }
            return 0;
        });
        
        BOOST_CHECK(handled.isReady());
        BOOST_CHECK_EQUAL(handled.get(), test_int_value);
    }
    
    // Test that existing void Future handling still works
    {
        kythira::Future<void> future;
        
        auto chained = std::move(future).thenValue([]() {
            return test_int_value;
        });
        
        BOOST_CHECK(chained.isReady());
        BOOST_CHECK_EQUAL(chained.get(), test_int_value);
    }
}

BOOST_AUTO_TEST_CASE(test_concept_compliance_maintained, * boost::unit_test::timeout(30)) {
    // Test that existing classes still satisfy their concepts
    
    // Test that Try still satisfies try_type concept
    static_assert(kythira::try_type<kythira::Try<int>, int>, 
                  "Try<int> must still satisfy try_type concept");
    static_assert(kythira::try_type<kythira::Try<void>, void>, 
                  "Try<void> must still satisfy try_type concept");
    
    // Test that Future still satisfies future concept
    static_assert(kythira::future<kythira::Future<int>, int>, 
                  "Future<int> must still satisfy future concept");
    static_assert(kythira::future<kythira::Future<void>, void>, 
                  "Future<void> must still satisfy future concept");
    
    // Test that Promise still satisfies promise concept
    static_assert(kythira::promise<kythira::Promise<int>, int>, 
                  "Promise<int> must still satisfy promise concept");
    static_assert(kythira::promise<kythira::Promise<void>, void>, 
                  "Promise<void> must still satisfy promise concept");
    
    // Test that SemiPromise still satisfies semi_promise concept
    static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>, 
                  "SemiPromise<int> must still satisfy semi_promise concept");
    static_assert(kythira::semi_promise<kythira::SemiPromise<void>, void>, 
                  "SemiPromise<void> must still satisfy semi_promise concept");
    
    // Test that Executor still satisfies executor concept
    static_assert(kythira::executor<kythira::Executor>, 
                  "Executor must still satisfy executor concept");
    
    // Test that KeepAlive still satisfies keep_alive concept
    static_assert(kythira::keep_alive<kythira::KeepAlive>, 
                  "KeepAlive must still satisfy keep_alive concept");
}

BOOST_AUTO_TEST_CASE(test_property_backward_compatibility_with_random_data, * boost::unit_test::timeout(60)) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> int_dist(-1000, 1000);
    std::uniform_int_distribution<std::size_t> size_dist(1, 10);
    
    for (std::size_t i = 0; i < test_iterations; ++i) {
        // Test Try backward compatibility with random values
        {
            int random_value = int_dist(gen);
            kythira::Try<int> try_value(random_value);
            
            // Should maintain same behavior as before
            BOOST_CHECK(try_value.hasValue());
            BOOST_CHECK(!try_value.hasException());
            BOOST_CHECK_EQUAL(try_value.value(), random_value);
        }
        
        // Test Future backward compatibility with random values
        {
            int random_value = int_dist(gen);
            kythira::Future<int> future_value(random_value);
            
            // Should maintain same behavior as before
            BOOST_CHECK(future_value.isReady());
            BOOST_CHECK_EQUAL(future_value.get(), random_value);
        }
        
        // Test Promise backward compatibility with random values
        {
            int random_value = int_dist(gen);
            kythira::Promise<int> promise;
            
            promise.setValue(random_value);
            BOOST_CHECK(promise.isFulfilled());
            
            auto future = promise.getFuture();
            BOOST_CHECK_EQUAL(future.get(), random_value);
        }
    }
}