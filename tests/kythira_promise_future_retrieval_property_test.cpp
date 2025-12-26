#define BOOST_TEST_MODULE KythiraPromiseFutureRetrievalPropertyTest
#include <boost/test/included/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>
#include <exception>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>

using namespace kythira;

// Test constants
namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test exception";
    constexpr double test_double = 3.14;
}

/**
 * **Feature: folly-concept-wrappers, Property 2: Promise Value and Exception Handling**
 * 
 * Property: For any promise wrapper and value/exception, setting the value or exception should properly convert types and make the associated future ready with the correct result
 * **Validates: Requirements 1.5**
 */
BOOST_AUTO_TEST_CASE(kythira_promise_future_retrieval_property_test, * boost::unit_test::timeout(90)) {
    // Test 1: getFuture returns properly wrapped Future instance for int type
    {
        Promise<int> promise;
        
        // Get future before setting value
        auto future = promise.getFuture();
        
        // Verify future is properly wrapped and has correct type
        static_assert(std::is_same_v<decltype(future), Future<int>>, 
                      "getFuture() should return Future<int>");
        
        // Future should not be ready initially
        BOOST_CHECK(!future.isReady());
        
        // Set value and verify future becomes ready with correct value
        promise.setValue(test_value);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_value);
    }
    
    // Test 2: getFuture returns properly wrapped Future instance for std::string type
    {
        Promise<std::string> promise;
        
        auto future = promise.getFuture();
        static_assert(std::is_same_v<decltype(future), Future<std::string>>, 
                      "getFuture() should return Future<std::string>");
        
        std::string test_str = "hello world";
        BOOST_CHECK(!future.isReady());
        
        promise.setValue(test_str);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_str);
    }
    
    // Test 3: getFuture returns properly wrapped Future instance for void type
    {
        Promise<void> promise;
        
        auto future = promise.getFuture();
        static_assert(std::is_same_v<decltype(future), Future<void>>, 
                      "getFuture() should return Future<void>");
        
        BOOST_CHECK(!future.isReady());
        
        promise.setValue(folly::Unit{});
        BOOST_CHECK(future.isReady());
        future.get(); // Should not throw
    }
    
    // Test 4: getSemiFuture returns properly wrapped Future instance for int type
    {
        Promise<int> promise;
        
        auto semi_future = promise.getSemiFuture();
        static_assert(std::is_same_v<decltype(semi_future), Future<int>>, 
                      "getSemiFuture() should return Future<int>");
        
        BOOST_CHECK(!semi_future.isReady());
        
        promise.setValue(test_value);
        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_EQUAL(semi_future.get(), test_value);
    }
    
    // Test 5: getSemiFuture returns properly wrapped Future instance for void type
    {
        Promise<void> promise;
        
        auto semi_future = promise.getSemiFuture();
        static_assert(std::is_same_v<decltype(semi_future), Future<void>>, 
                      "getSemiFuture() should return Future<void>");
        
        BOOST_CHECK(!semi_future.isReady());
        
        promise.setValue(folly::Unit{});
        BOOST_CHECK(semi_future.isReady());
        semi_future.get(); // Should not throw
    }
    
    // Test 6: Future retrieval with exception handling - getFuture
    {
        Promise<int> promise;
        
        auto future = promise.getFuture();
        BOOST_CHECK(!future.isReady());
        
        // Set exception and verify future becomes ready with exception
        auto ex = folly::exception_wrapper(std::runtime_error(test_string));
        promise.setException(ex);
        BOOST_CHECK(future.isReady());
        
        // Future should throw when getting value
        BOOST_CHECK_THROW(future.get(), std::runtime_error);
    }
    
    // Test 7: Future retrieval with exception handling - getSemiFuture
    {
        Promise<int> promise;
        
        auto semi_future = promise.getSemiFuture();
        BOOST_CHECK(!semi_future.isReady());
        
        // Set exception using std::exception_ptr
        try {
            throw std::runtime_error(test_string);
        } catch (...) {
            promise.setException(std::current_exception());
        }
        
        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_THROW(semi_future.get(), std::runtime_error);
    }
    
    // Test 8: Property-based testing - verify future retrieval works across multiple types and values
    for (int i = 0; i < 100; ++i) {
        int random_value = i * 7 + 13; // Simple pseudo-random generation
        
        // Test getFuture with various values
        {
            Promise<int> promise;
            auto future = promise.getFuture();
            
            // Verify type correctness
            static_assert(std::is_same_v<decltype(future), Future<int>>, 
                          "getFuture() should always return Future<int>");
            
            BOOST_CHECK(!future.isReady());
            promise.setValue(random_value);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(future.get(), random_value);
        }
        
        // Test getSemiFuture with various values
        {
            Promise<int> promise;
            auto semi_future = promise.getSemiFuture();
            
            // Verify type correctness
            static_assert(std::is_same_v<decltype(semi_future), Future<int>>, 
                          "getSemiFuture() should always return Future<int>");
            
            BOOST_CHECK(!semi_future.isReady());
            promise.setValue(random_value);
            BOOST_CHECK(semi_future.isReady());
            BOOST_CHECK_EQUAL(semi_future.get(), random_value);
        }
        
        // Test with string values
        {
            Promise<std::string> string_promise;
            std::string test_str = "test string " + std::to_string(i);
            
            auto future = string_promise.getFuture();
            static_assert(std::is_same_v<decltype(future), Future<std::string>>, 
                          "getFuture() should return Future<std::string>");
            
            string_promise.setValue(test_str);
            BOOST_CHECK_EQUAL(future.get(), test_str);
        }
        
        // Test with void promises
        {
            Promise<void> void_promise;
            
            auto void_future = void_promise.getFuture();
            static_assert(std::is_same_v<decltype(void_future), Future<void>>, 
                          "getFuture() should return Future<void>");
            
            void_promise.setValue(folly::Unit{});
            void_future.get(); // Should not throw
        }
        
        // Test exception propagation through future retrieval
        {
            Promise<int> exception_promise;
            auto future = exception_promise.getFuture();
            
            auto ex = folly::exception_wrapper(std::runtime_error("test exception " + std::to_string(i)));
            exception_promise.setException(ex);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_THROW(future.get(), std::runtime_error);
        }
    }
}

/**
 * Test that retrieved futures satisfy the future concept
 */
BOOST_AUTO_TEST_CASE(retrieved_future_concept_compliance_test, * boost::unit_test::timeout(30)) {
    // Test that futures returned by getFuture satisfy future concept
    {
        Promise<int> promise;
        auto future = promise.getFuture();
        
        promise.setValue(test_value);
        BOOST_CHECK_EQUAL(future.get(), test_value);
    }
    
    // Test that futures returned by getSemiFuture satisfy future concept
    {
        Promise<std::string> promise;
        auto semi_future = promise.getSemiFuture();
        
        std::string test_str = "test";
        promise.setValue(test_str);
        BOOST_CHECK_EQUAL(semi_future.get(), test_str);
    }
    
    // Test with void type
    {
        Promise<void> promise;
        auto future = promise.getFuture();
        
        promise.setValue(folly::Unit{});
        future.get(); // Should not throw
    }
}

/**
 * Test future retrieval behavior and lifecycle management
 */
BOOST_AUTO_TEST_CASE(future_retrieval_lifecycle_test, * boost::unit_test::timeout(30)) {
    // Test that promise-future relationship is properly maintained
    {
        Promise<int> promise;
        
        // Get future before fulfilling promise
        auto future = promise.getFuture();
        BOOST_CHECK(!promise.isFulfilled());
        BOOST_CHECK(!future.isReady());
        
        // Fulfill promise and verify future becomes ready
        promise.setValue(test_value);
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(future.isReady());
        
        // Verify value propagation
        BOOST_CHECK_EQUAL(future.get(), test_value);
    }
    
    // Test that getSemiFuture works independently of getFuture
    {
        Promise<int> promise;
        
        auto semi_future = promise.getSemiFuture();
        BOOST_CHECK(!semi_future.isReady());
        
        promise.setValue(test_value);
        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_EQUAL(semi_future.get(), test_value);
    }
    
    // Test proper resource cleanup when promise goes out of scope
    {
        Promise<int> promise;
        auto future = promise.getFuture();
        promise.setValue(test_value);
        
        // Future should still be valid and contain the value
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_value);
    }
}

/**
 * Test move semantics with future retrieval
 */
BOOST_AUTO_TEST_CASE(future_retrieval_move_semantics_test, * boost::unit_test::timeout(30)) {
    // Test that promise can be moved after getting future
    {
        Promise<int> promise1;
        auto future = promise1.getFuture();
        
        // Move promise
        Promise<int> promise2 = std::move(promise1);
        
        // Original future should still work with moved promise
        promise2.setValue(test_value);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(future.get(), test_value);
    }
    
    // Test with movable values
    {
        Promise<std::string> promise;
        auto future = promise.getFuture();
        
        std::string movable_string = "movable test string";
        promise.setValue(std::move(movable_string));
        
        auto result = future.get();
        BOOST_CHECK_EQUAL(result, "movable test string");
    }
}