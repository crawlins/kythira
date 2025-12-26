#define BOOST_TEST_MODULE kythira_future_continuation_operations_property_test
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <thread>
#include <stdexcept>
#include <string>
#include <memory>

#include <folly/init/Init.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/GlobalExecutor.h>

#include "../include/raft/future.hpp"
#include "../include/concepts/future.hpp"

namespace {
    constexpr std::chrono::milliseconds short_delay{50};
    constexpr std::chrono::milliseconds medium_delay{100};
    constexpr std::chrono::milliseconds long_delay{200};
    constexpr std::chrono::milliseconds timeout_duration{300};
    constexpr int test_value = 42;
    constexpr const char* test_string = "test_value";
}

// Folly initialization fixture
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("kythira_future_continuation_operations_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

/**
 * **Feature: folly-concept-wrappers, Property 6: Continuation Operations**
 * 
 * Property: For any future and continuation operation, the operation should properly 
 * schedule, delay, or timeout the future while maintaining type safety and error propagation
 * 
 * **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**
 */
BOOST_AUTO_TEST_CASE(test_via_executor_scheduling, * boost::unit_test::timeout(60)) {
    // Test via() method for executor-based continuation scheduling
    
    // Create a thread pool executor
    auto thread_pool = std::make_unique<folly::CPUThreadPoolExecutor>(2);
    folly::Executor* executor = thread_pool.get();
    
    // Test with int future
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Schedule continuation on specific executor
        auto continued_future = std::move(future).via(executor);
        
        // Set value and verify it propagates through via
        promise.setValue(test_value);
        
        auto result = std::move(continued_future).get();
        BOOST_CHECK_EQUAL(result, test_value);
    }
    
    // Test with void future
    {
        auto promise = kythira::Promise<void>();
        auto future = promise.getFuture();
        
        // Schedule continuation on specific executor
        auto continued_future = std::move(future).via(executor);
        
        // Set value and verify it propagates through via
        promise.setValue();
        
        // Should complete without throwing
        std::move(continued_future).get();
        BOOST_CHECK(true); // If we get here, the test passed
    }
    
    // Test with string future
    {
        auto promise = kythira::Promise<std::string>();
        auto future = promise.getFuture();
        
        // Schedule continuation on specific executor
        auto continued_future = std::move(future).via(executor);
        
        // Set value and verify it propagates through via
        promise.setValue(std::string(test_string));
        
        auto result = std::move(continued_future).get();
        BOOST_CHECK_EQUAL(result, test_string);
    }
}

BOOST_AUTO_TEST_CASE(test_via_keepalive_scheduling, * boost::unit_test::timeout(60)) {
    // Test via() method with KeepAlive wrapper
    
    // Create a thread pool executor and get KeepAlive
    auto thread_pool = std::make_unique<folly::CPUThreadPoolExecutor>(2);
    auto keep_alive = kythira::KeepAlive(folly::getKeepAliveToken(thread_pool.get()));
    
    // Test with int future
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Schedule continuation using KeepAlive
        auto continued_future = std::move(future).via(keep_alive);
        
        // Set value and verify it propagates through via
        promise.setValue(test_value);
        
        auto result = std::move(continued_future).get();
        BOOST_CHECK_EQUAL(result, test_value);
    }
}

BOOST_AUTO_TEST_CASE(test_delay_execution, * boost::unit_test::timeout(60)) {
    // Test delay() method for time-based future delays
    
    // Test with int future
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Add delay to future
        auto delayed_future = std::move(future).delay(short_delay);
        
        // Set value immediately
        promise.setValue(test_value);
        
        // Measure time and verify delay
        auto start_time = std::chrono::steady_clock::now();
        auto result = std::move(delayed_future).get();
        auto end_time = std::chrono::steady_clock::now();
        
        BOOST_CHECK_EQUAL(result, test_value);
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        BOOST_CHECK_GE(elapsed.count(), short_delay.count() - 10); // Allow some tolerance
    }
    
    // Test with void future
    {
        auto promise = kythira::Promise<void>();
        auto future = promise.getFuture();
        
        // Add delay to future
        auto delayed_future = std::move(future).delay(short_delay);
        
        // Set value immediately
        promise.setValue();
        
        // Measure time and verify delay
        auto start_time = std::chrono::steady_clock::now();
        std::move(delayed_future).get();
        auto end_time = std::chrono::steady_clock::now();
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        BOOST_CHECK_GE(elapsed.count(), short_delay.count() - 10); // Allow some tolerance
    }
}

BOOST_AUTO_TEST_CASE(test_within_timeout_success, * boost::unit_test::timeout(60)) {
    // Test within() method for timeout behavior - success case
    
    // Test with int future that completes before timeout
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Add timeout constraint
        auto timeout_future = std::move(future).within(timeout_duration);
        
        // Set value quickly (before timeout)
        promise.setValue(test_value);
        
        auto result = std::move(timeout_future).get();
        BOOST_CHECK_EQUAL(result, test_value);
    }
    
    // Test with void future that completes before timeout
    {
        auto promise = kythira::Promise<void>();
        auto future = promise.getFuture();
        
        // Add timeout constraint
        auto timeout_future = std::move(future).within(timeout_duration);
        
        // Set value quickly (before timeout)
        promise.setValue();
        
        // Should complete without throwing
        std::move(timeout_future).get();
        BOOST_CHECK(true); // If we get here, the test passed
    }
}

BOOST_AUTO_TEST_CASE(test_within_timeout_failure, * boost::unit_test::timeout(60)) {
    // Test within() method for timeout behavior - timeout case
    
    // Test with int future that times out
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Add short timeout constraint
        auto timeout_future = std::move(future).within(short_delay);
        
        // Don't set value - let it timeout
        
        // Should throw timeout exception
        BOOST_CHECK_THROW(std::move(timeout_future).get(), std::exception);
    }
}

BOOST_AUTO_TEST_CASE(test_ensure_cleanup_success, * boost::unit_test::timeout(60)) {
    // Test ensure() method for cleanup functionality - success case
    
    bool cleanup_called = false;
    
    // Test with int future that succeeds
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Add cleanup function
        auto ensured_future = std::move(future).ensure([&cleanup_called]() {
            cleanup_called = true;
        });
        
        // Set value
        promise.setValue(test_value);
        
        auto result = std::move(ensured_future).get();
        BOOST_CHECK_EQUAL(result, test_value);
        BOOST_CHECK(cleanup_called);
    }
}

BOOST_AUTO_TEST_CASE(test_ensure_cleanup_failure, * boost::unit_test::timeout(60)) {
    // Test ensure() method for cleanup functionality - failure case
    
    bool cleanup_called = false;
    
    // Test with int future that fails
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Add cleanup function
        auto ensured_future = std::move(future).ensure([&cleanup_called]() {
            cleanup_called = true;
        });
        
        // Set exception
        promise.setException(std::make_exception_ptr(std::runtime_error("test error")));
        
        // Should throw the exception but still call cleanup
        BOOST_CHECK_THROW(std::move(ensured_future).get(), std::runtime_error);
        BOOST_CHECK(cleanup_called);
    }
}

BOOST_AUTO_TEST_CASE(test_chained_continuation_operations, * boost::unit_test::timeout(60)) {
    // Test chaining multiple continuation operations together
    
    // Create a thread pool executor
    auto thread_pool = std::make_unique<folly::CPUThreadPoolExecutor>(2);
    folly::Executor* executor = thread_pool.get();
    
    bool cleanup_called = false;
    
    // Test chaining via, delay, within, and ensure
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        // Chain multiple continuation operations
        auto chained_future = std::move(future)
            .via(executor)
            .delay(short_delay)
            .within(timeout_duration)
            .ensure([&cleanup_called]() {
                cleanup_called = true;
            });
        
        // Set value
        promise.setValue(test_value);
        
        auto result = std::move(chained_future).get();
        BOOST_CHECK_EQUAL(result, test_value);
        BOOST_CHECK(cleanup_called);
    }
}

BOOST_AUTO_TEST_CASE(test_continuation_type_safety, * boost::unit_test::timeout(60)) {
    // Test that continuation operations maintain proper type safety
    
    // Test type preservation through continuation operations
    {
        auto promise = kythira::Promise<std::string>();
        auto future = promise.getFuture();
        
        // Apply continuation operations and verify type is preserved
        auto continued_future = std::move(future)
            .delay(short_delay)
            .within(timeout_duration);
        
        promise.setValue(std::string(test_string));
        
        auto result = std::move(continued_future).get();
        BOOST_CHECK_EQUAL(result, test_string);
        
        // Verify result is still std::string
        static_assert(std::is_same_v<decltype(result), std::string>);
    }
}

BOOST_AUTO_TEST_CASE(test_continuation_error_propagation, * boost::unit_test::timeout(60)) {
    // Test that continuation operations properly propagate errors
    
    // Test error propagation through via
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        auto thread_pool = std::make_unique<folly::CPUThreadPoolExecutor>(2);
        auto continued_future = std::move(future).via(thread_pool.get());
        
        // Set exception
        promise.setException(std::make_exception_ptr(std::runtime_error("test error")));
        
        // Should propagate the exception
        BOOST_CHECK_THROW(std::move(continued_future).get(), std::runtime_error);
    }
    
    // Test error propagation through delay
    {
        auto promise = kythira::Promise<int>();
        auto future = promise.getFuture();
        
        auto delayed_future = std::move(future).delay(short_delay);
        
        // Set exception
        promise.setException(std::make_exception_ptr(std::runtime_error("test error")));
        
        // Should propagate the exception
        BOOST_CHECK_THROW(std::move(delayed_future).get(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(test_void_future_unit_conversion, * boost::unit_test::timeout(60)) {
    // Test that continuation operations properly handle void/Unit conversion
    
    // Test void future with continuation operations
    {
        auto promise = kythira::Promise<void>();
        auto future = promise.getFuture();
        
        auto thread_pool = std::make_unique<folly::CPUThreadPoolExecutor>(2);
        
        // Apply continuation operations to void future
        auto continued_future = std::move(future)
            .via(thread_pool.get())
            .delay(short_delay)
            .within(timeout_duration);
        
        promise.setValue();
        
        // Should complete without throwing
        std::move(continued_future).get();
        BOOST_CHECK(true); // If we get here, the test passed
    }
}