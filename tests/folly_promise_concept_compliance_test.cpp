#define BOOST_TEST_MODULE folly_promise_concept_compliance_test
#include <boost/test/unit_test.hpp>

#include <concepts/future.hpp>
#include <raft/future.hpp>

// Include Folly headers for Promise (used by the runtime-behavior test below,
// which exercises Folly's own API directly rather than concept compliance).
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
 * Test that kythira::Promise<T> (the Folly-backed wrapper) satisfies the
 * promise concept. See folly_semi_promise_concept_compliance_test.cpp's
 * header comment for why this tests the wrapper rather than bare
 * folly::Promise<T> directly.
 * Requirements: 10.2
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_concept_compliance, *boost::unit_test::timeout(30)) {
    // Test kythira::Promise<int> satisfies promise concept
    static_assert(kythira::promise<kythira::Promise<int>, int>,
                  "kythira::Promise<int> must satisfy promise concept");

    // Test kythira::Promise<std::string> satisfies promise concept
    static_assert(kythira::promise<kythira::Promise<std::string>, std::string>,
                  "kythira::Promise<std::string> must satisfy promise concept");

    // Test kythira::Promise<double> satisfies promise concept
    static_assert(kythira::promise<kythira::Promise<double>, double>,
                  "kythira::Promise<double> must satisfy promise concept");

    // Test kythira::Promise<void> satisfies promise concept
    static_assert(kythira::promise<kythira::Promise<void>, void>,
                  "kythira::Promise<void> must satisfy promise concept for void type");

    // Test kythira::Promise with custom types
    struct CustomType {
        int value;
        std::string name;
    };

    static_assert(kythira::promise<kythira::Promise<CustomType>, CustomType>,
                  "kythira::Promise<CustomType> must satisfy promise concept");

    // Test kythira::Promise with pointer types
    static_assert(kythira::promise<kythira::Promise<int*>, int*>,
                  "kythira::Promise<int*> must satisfy promise concept");

    // Test kythira::Promise with reference wrapper
    static_assert(kythira::promise<kythira::Promise<std::reference_wrapper<int>>,
                                   std::reference_wrapper<int>>,
                  "kythira::Promise<std::reference_wrapper<int>> must satisfy promise concept");

    BOOST_TEST_MESSAGE("All kythira::Promise types satisfy promise concept");
}

/**
 * Test runtime behavior of folly::Promise with the promise concept interface
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_runtime_behavior, *boost::unit_test::timeout(30)) {
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
        std::move(semi_future).get();  // Should not throw
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