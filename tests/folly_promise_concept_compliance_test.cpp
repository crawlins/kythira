#define BOOST_TEST_MODULE folly_promise_concept_compliance_test
#include <boost/test/unit_test.hpp>

#include <concepts/future.hpp>

// Include Folly headers for Promise
#include <folly/futures/Promise.h>
#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>

namespace {
    constexpr const char* test_name = "folly_promise_concept_compliance_test";
}

BOOST_AUTO_TEST_SUITE(folly_promise_concept_compliance_tests)

/**
 * Test that folly::Promise<T> satisfies promise concept
 * Requirements: 10.2
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_concept_compliance, * boost::unit_test::timeout(30)) {
    // Test folly::Promise<int> satisfies promise concept
    static_assert(kythira::promise<folly::Promise<int>, int>, 
                  "folly::Promise<int> must satisfy promise concept");
    
    // Test folly::Promise<std::string> satisfies promise concept
    static_assert(kythira::promise<folly::Promise<std::string>, std::string>, 
                  "folly::Promise<std::string> must satisfy promise concept");
    
    // Test folly::Promise<double> satisfies promise concept
    static_assert(kythira::promise<folly::Promise<double>, double>, 
                  "folly::Promise<double> must satisfy promise concept");
    
    // Test folly::Promise<folly::Unit> satisfies promise concept (folly uses Unit instead of void)
    static_assert(kythira::promise<folly::Promise<folly::Unit>, void>, 
                  "folly::Promise<folly::Unit> must satisfy promise concept for void type");
    
    // Test folly::Promise with custom types
    struct CustomType {
        int value;
        std::string name;
    };
    
    static_assert(kythira::promise<folly::Promise<CustomType>, CustomType>, 
                  "folly::Promise<CustomType> must satisfy promise concept");
    
    // Test folly::Promise with pointer types
    static_assert(kythira::promise<folly::Promise<int*>, int*>, 
                  "folly::Promise<int*> must satisfy promise concept");
    
    // Test folly::Promise with reference wrapper
    static_assert(kythira::promise<folly::Promise<std::reference_wrapper<int>>, std::reference_wrapper<int>>, 
                  "folly::Promise<std::reference_wrapper<int>> must satisfy promise concept");
    
    BOOST_TEST_MESSAGE("All folly::Promise types satisfy promise concept");
}

/**
 * Test runtime behavior of folly::Promise with the promise concept interface
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_runtime_behavior, * boost::unit_test::timeout(30)) {
    // Test non-void Promise
    {
        folly::Promise<int> promise;
        
        // Test isFulfilled before setting value (inherited from semi_promise)
        BOOST_CHECK(!promise.isFulfilled());
        
        // Test getFuture method
        auto future = promise.getFuture();
        BOOST_CHECK(!future.isReady());
        
        // Set value
        promise.setValue(42);
        
        // Test isFulfilled after setting value
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), 42);
    }
    
    // Test Unit Promise (folly uses Unit instead of void)
    {
        folly::Promise<folly::Unit> promise;
        
        // Test getSemiFuture method
        auto semi_future = promise.getSemiFuture();
        BOOST_CHECK(!semi_future.isReady());
        
        // Set value (Unit value for void-like behavior)
        promise.setValue(folly::Unit{});
        
        // Test that future is ready
        BOOST_CHECK(semi_future.isReady());
        std::move(semi_future).get(); // Should not throw
    }
    
    // Test exception setting with getFuture
    {
        folly::Promise<int> promise;
        auto future = promise.getFuture();
        
        // Set exception using folly::exception_wrapper
        auto ex = folly::exception_wrapper(std::runtime_error("test error"));
        promise.setException(ex);
        
        // Test that future has exception
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    }
    
    BOOST_TEST_MESSAGE("folly::Promise runtime behavior matches promise concept requirements");
}

BOOST_AUTO_TEST_SUITE_END()