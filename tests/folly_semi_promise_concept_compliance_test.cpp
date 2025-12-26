#define BOOST_TEST_MODULE folly_semi_promise_concept_compliance_test
#include <boost/test/unit_test.hpp>

#include <concepts/future.hpp>

// Include Folly headers for Promise (Folly doesn't have separate SemiPromise)
#include <folly/futures/Promise.h>
#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>

namespace {
    constexpr const char* test_name = "folly_semi_promise_concept_compliance_test";
}

BOOST_AUTO_TEST_SUITE(folly_semi_promise_concept_compliance_tests)

/**
 * Test that folly::Promise<T> satisfies semi_promise concept
 * Note: Folly doesn't have a separate SemiPromise class, but Promise provides the SemiPromise interface
 * Requirements: 10.1
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_as_semi_promise_concept_compliance, * boost::unit_test::timeout(30)) {
    // Test folly::Promise<int> satisfies semi_promise concept
    static_assert(kythira::semi_promise<folly::Promise<int>, int>, 
                  "folly::Promise<int> must satisfy semi_promise concept");
    
    // Test folly::Promise<std::string> satisfies semi_promise concept
    static_assert(kythira::semi_promise<folly::Promise<std::string>, std::string>, 
                  "folly::Promise<std::string> must satisfy semi_promise concept");
    
    // Test folly::Promise<double> satisfies semi_promise concept
    static_assert(kythira::semi_promise<folly::Promise<double>, double>, 
                  "folly::Promise<double> must satisfy semi_promise concept");
    
    // Test folly::Promise<folly::Unit> satisfies semi_promise concept (folly uses Unit instead of void)
    static_assert(kythira::semi_promise<folly::Promise<folly::Unit>, void>, 
                  "folly::Promise<folly::Unit> must satisfy semi_promise concept for void type");
    
    // Test folly::Promise with custom types
    struct CustomType {
        int value;
        std::string name;
    };
    
    static_assert(kythira::semi_promise<folly::Promise<CustomType>, CustomType>, 
                  "folly::Promise<CustomType> must satisfy semi_promise concept");
    
    // Test folly::Promise with pointer types
    static_assert(kythira::semi_promise<folly::Promise<int*>, int*>, 
                  "folly::Promise<int*> must satisfy semi_promise concept");
    
    // Test folly::Promise with reference wrapper
    static_assert(kythira::semi_promise<folly::Promise<std::reference_wrapper<int>>, std::reference_wrapper<int>>, 
                  "folly::Promise<std::reference_wrapper<int>> must satisfy semi_promise concept");
    
    BOOST_TEST_MESSAGE("All folly::Promise types satisfy semi_promise concept");
}

/**
 * Test runtime behavior of folly::Promise with the semi_promise concept interface
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_runtime_behavior, * boost::unit_test::timeout(30)) {
    // Test non-void Promise
    {
        folly::Promise<int> promise;
        
        // Test isFulfilled before setting value
        BOOST_CHECK(!promise.isFulfilled());
        
        // Set value
        promise.setValue(42);
        
        // Test isFulfilled after setting value
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test Unit Promise (folly uses Unit instead of void)
    {
        folly::Promise<folly::Unit> promise;
        
        // Test isFulfilled before setting value
        BOOST_CHECK(!promise.isFulfilled());
        
        // Set value (Unit value for void-like behavior)
        promise.setValue(folly::Unit{});
        
        // Test isFulfilled after setting value
        BOOST_CHECK(promise.isFulfilled());
    }
    
    // Test exception setting
    {
        folly::Promise<int> promise;
        
        // Test isFulfilled before setting exception
        BOOST_CHECK(!promise.isFulfilled());
        
        // Set exception using folly::exception_wrapper
        auto ex = folly::exception_wrapper(std::runtime_error("test error"));
        promise.setException(ex);
        
        // Test isFulfilled after setting exception
        BOOST_CHECK(promise.isFulfilled());
    }
    
    BOOST_TEST_MESSAGE("folly::Promise runtime behavior matches semi_promise concept requirements");
}

BOOST_AUTO_TEST_SUITE_END()