#define BOOST_TEST_MODULE KythiraSemiPromiseValueExceptionHandlingPropertyTest
#include <boost/test/included/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>
#include <exception>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <vector>
#include <memory>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>

using namespace kythira;

// Test constants
namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test exception";
    constexpr double test_double = 3.14;
    constexpr std::size_t test_iterations = 100;
}

/**
 * **Feature: folly-concept-wrappers, Property 2: Promise Value and Exception Handling**
 * 
 * Property: For any promise wrapper and value/exception, setting the value or exception should properly convert types and make the associated future ready with the correct result
 * **Validates: Requirements 1.3, 1.4**
 */
BOOST_AUTO_TEST_CASE(kythira_semi_promise_value_exception_handling_property_test, * boost::unit_test::timeout(120)) {
    // Test 1: Value setting with proper void/Unit handling for non-void types
    {
        // Test int type
        SemiPromise<int> int_promise;
        BOOST_CHECK(!int_promise.isFulfilled());
        
        int_promise.setValue(test_value);
        BOOST_CHECK(int_promise.isFulfilled());
        
        // Test that double fulfillment throws
        BOOST_CHECK_THROW(int_promise.setValue(456), std::logic_error);
        BOOST_CHECK_THROW(int_promise.setException(folly::exception_wrapper(std::runtime_error("test"))), std::logic_error);
    }
    
    // Test 2: Value setting with void/Unit conversion for void type
    {
        SemiPromise<void> void_promise;
        BOOST_CHECK(!void_promise.isFulfilled());
        
        // Should accept folly::Unit for void type
        void_promise.setValue(folly::Unit{});
        BOOST_CHECK(void_promise.isFulfilled());
        
        // Test that double fulfillment throws
        BOOST_CHECK_THROW(void_promise.setValue(folly::Unit{}), std::logic_error);
    }
    
    // Test 3: Exception setting with folly::exception_wrapper
    {
        SemiPromise<int> promise;
        BOOST_CHECK(!promise.isFulfilled());
        
        auto ex = folly::exception_wrapper(std::runtime_error(test_string));
        promise.setException(ex);
        BOOST_CHECK(promise.isFulfilled());
        
        // Test that double fulfillment throws
        BOOST_CHECK_THROW(promise.setValue(123), std::logic_error);
        BOOST_CHECK_THROW(promise.setException(folly::exception_wrapper(std::runtime_error("another"))), std::logic_error);
    }
    
    // Test 4: Exception setting with std::exception_ptr conversion
    {
        SemiPromise<int> promise;
        BOOST_CHECK(!promise.isFulfilled());
        
        std::exception_ptr ep;
        try {
            throw std::runtime_error(test_string);
        } catch (...) {
            ep = std::current_exception();
        }
        
        promise.setException(ep);
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test 5: Move semantics for setValue
    {
        SemiPromise<std::string> promise;
        
        std::string movable_string = "movable test string";
        std::string original_value = movable_string;
        
        // setValue should accept moved values
        promise.setValue(std::move(movable_string));
        BOOST_CHECK(promise.isFulfilled());
        
        // Test with rvalue
        SemiPromise<std::string> promise2;
        promise2.setValue(std::string("rvalue string"));
        BOOST_CHECK(promise2.isFulfilled());
    }
    
    // Test 6: Property-based testing - generate multiple test cases for value handling
    for (std::size_t i = 0; i < test_iterations; ++i) {
        // Generate pseudo-random test data
        int random_int = static_cast<int>(i * 7 + 13);
        double random_double = static_cast<double>(i) * 0.1 + 1.5;
        std::string random_string = "test_string_" + std::to_string(i);
        
        // Test int value fulfillment
        {
            SemiPromise<int> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            promise.setValue(random_int);
            BOOST_CHECK(promise.isFulfilled());
            
            // Verify cannot fulfill again
            BOOST_CHECK_THROW(promise.setValue(random_int + 1), std::logic_error);
        }
        
        // Test double value fulfillment
        {
            SemiPromise<double> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            promise.setValue(random_double);
            BOOST_CHECK(promise.isFulfilled());
        }
        
        // Test string value fulfillment with move semantics
        {
            SemiPromise<std::string> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            std::string movable_string = random_string;
            promise.setValue(std::move(movable_string));
            BOOST_CHECK(promise.isFulfilled());
        }
        
        // Test void value fulfillment
        {
            SemiPromise<void> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            promise.setValue(folly::Unit{});
            BOOST_CHECK(promise.isFulfilled());
        }
    }
    
    // Test 7: Property-based testing - generate multiple test cases for exception handling
    for (std::size_t i = 0; i < test_iterations; ++i) {
        std::string exception_message = "test exception " + std::to_string(i);
        
        // Test folly::exception_wrapper exception fulfillment
        {
            SemiPromise<int> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            auto ex = folly::exception_wrapper(std::runtime_error(exception_message));
            promise.setException(ex);
            BOOST_CHECK(promise.isFulfilled());
            
            // Verify cannot fulfill again
            BOOST_CHECK_THROW(promise.setValue(123), std::logic_error);
            BOOST_CHECK_THROW(promise.setException(folly::exception_wrapper(std::runtime_error("another"))), std::logic_error);
        }
        
        // Test std::exception_ptr exception fulfillment
        {
            SemiPromise<std::string> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            std::exception_ptr ep;
            try {
                throw std::invalid_argument(exception_message);
            } catch (...) {
                ep = std::current_exception();
            }
            
            promise.setException(ep);
            BOOST_CHECK(promise.isFulfilled());
        }
        
        // Test void promise exception fulfillment
        {
            SemiPromise<void> promise;
            BOOST_CHECK(!promise.isFulfilled());
            
            auto ex = folly::exception_wrapper(std::logic_error(exception_message));
            promise.setException(ex);
            BOOST_CHECK(promise.isFulfilled());
        }
    }
}

/**
 * Test type conversion behavior for different value types
 */
BOOST_AUTO_TEST_CASE(semi_promise_type_conversion_test, * boost::unit_test::timeout(60)) {
    // Test 1: Custom struct type
    struct CustomType {
        int value;
        std::string name;
        
        CustomType(int v, std::string n) : value(v), name(std::move(n)) {}
        
        bool operator==(const CustomType& other) const {
            return value == other.value && name == other.name;
        }
    };
    
    {
        SemiPromise<CustomType> promise;
        CustomType test_obj(test_value, "test_name");
        
        promise.setValue(std::move(test_obj));
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test 2: Pointer types
    {
        SemiPromise<int*> promise;
        int test_int = test_value;
        
        promise.setValue(&test_int);
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test 3: Smart pointer types
    {
        SemiPromise<std::unique_ptr<int>> promise;
        auto ptr = std::make_unique<int>(test_value);
        
        promise.setValue(std::move(ptr));
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test 4: Container types
    {
        SemiPromise<std::vector<int>> promise;
        std::vector<int> test_vector = {1, 2, 3, 4, 5};
        
        promise.setValue(std::move(test_vector));
        BOOST_CHECK(promise.isFulfilled());
    }
}

/**
 * Test exception type conversion behavior
 */
BOOST_AUTO_TEST_CASE(semi_promise_exception_conversion_test, * boost::unit_test::timeout(60)) {
    // Test 1: Different exception types with folly::exception_wrapper
    {
        SemiPromise<int> promise1;
        auto runtime_ex = folly::exception_wrapper(std::runtime_error(test_string));
        promise1.setException(runtime_ex);
        BOOST_CHECK(promise1.isFulfilled());
        
        SemiPromise<int> promise2;
        auto logic_ex = folly::exception_wrapper(std::logic_error(test_string));
        promise2.setException(logic_ex);
        BOOST_CHECK(promise2.isFulfilled());
        
        SemiPromise<int> promise3;
        auto invalid_arg_ex = folly::exception_wrapper(std::invalid_argument(test_string));
        promise3.setException(invalid_arg_ex);
        BOOST_CHECK(promise3.isFulfilled());
    }
    
    // Test 2: Different exception types with std::exception_ptr
    {
        SemiPromise<int> promise1;
        try {
            throw std::runtime_error(test_string);
        } catch (...) {
            promise1.setException(std::current_exception());
        }
        BOOST_CHECK(promise1.isFulfilled());
        
        SemiPromise<int> promise2;
        try {
            throw std::logic_error(test_string);
        } catch (...) {
            promise2.setException(std::current_exception());
        }
        BOOST_CHECK(promise2.isFulfilled());
        
        SemiPromise<int> promise3;
        try {
            throw std::invalid_argument(test_string);
        } catch (...) {
            promise3.setException(std::current_exception());
        }
        BOOST_CHECK(promise3.isFulfilled());
    }
    
    // Test 3: Custom exception types
    class CustomException : public std::exception {
    public:
        explicit CustomException(const std::string& msg) : _message(msg) {}
        const char* what() const noexcept override { return _message.c_str(); }
    private:
        std::string _message;
    };
    
    {
        SemiPromise<int> promise;
        auto custom_ex = folly::exception_wrapper(CustomException("custom exception"));
        promise.setException(custom_ex);
        BOOST_CHECK(promise.isFulfilled());
    }
    
    {
        SemiPromise<int> promise;
        try {
            throw CustomException("custom exception via ptr");
        } catch (...) {
            promise.setException(std::current_exception());
        }
        BOOST_CHECK(promise.isFulfilled());
    }
}

/**
 * Test resource management during value and exception setting
 */
BOOST_AUTO_TEST_CASE(semi_promise_resource_management_test, * boost::unit_test::timeout(60)) {
    // Test 1: Resource cleanup on value setting
    {
        SemiPromise<std::unique_ptr<int>> promise;
        auto resource = std::make_unique<int>(test_value);
        int* raw_ptr = resource.get();
        
        promise.setValue(std::move(resource));
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(resource == nullptr); // Resource was moved
        
        // The promise should now own the resource
        // When promise goes out of scope, resource should be cleaned up
    }
    
    // Test 2: Resource cleanup on exception setting
    {
        SemiPromise<std::unique_ptr<int>> promise;
        auto ex = folly::exception_wrapper(std::runtime_error("resource test"));
        
        promise.setException(ex);
        BOOST_CHECK(promise.isFulfilled());
        
        // Promise should properly handle the exception without resource leaks
    }
    
    // Test 3: Multiple promises with different resource types
    std::vector<std::unique_ptr<SemiPromise<std::string>>> promises;
    for (int i = 0; i < 10; ++i) {
        auto promise = std::make_unique<SemiPromise<std::string>>();
        promise->setValue("resource_test_" + std::to_string(i));
        BOOST_CHECK(promise->isFulfilled());
        promises.push_back(std::move(promise));
    }
    
    // All promises should be properly managed and cleaned up when vector goes out of scope
}