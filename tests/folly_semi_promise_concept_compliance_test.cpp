#define BOOST_TEST_MODULE folly_semi_promise_concept_compliance_test
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
constexpr const char* test_name = "folly_semi_promise_concept_compliance_test";
}

BOOST_AUTO_TEST_SUITE(folly_semi_promise_concept_compliance_tests)

/**
 * Test that kythira::SemiPromise<T> (the Folly-backed wrapper) satisfies the
 * semi_promise concept. This checks the wrapper, not bare folly::Promise<T>
 * directly — the regenericized concept requires setException(std::exception_ptr)
 * and, for the void case, setValue(kythira::unit), and only the wrapper (not
 * raw Folly, which only knows folly::exception_wrapper/folly::Unit) provides
 * those overloads. See include/concepts/future.hpp's Requirement 1.1-1.3 for
 * why the concept itself no longer names any Folly type.
 * Requirements: 10.1
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_as_semi_promise_concept_compliance,
                     *boost::unit_test::timeout(30)) {
    // Test kythira::SemiPromise<int> satisfies semi_promise concept
    static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>,
                  "kythira::SemiPromise<int> must satisfy semi_promise concept");

    // Test kythira::SemiPromise<std::string> satisfies semi_promise concept
    static_assert(kythira::semi_promise<kythira::SemiPromise<std::string>, std::string>,
                  "kythira::SemiPromise<std::string> must satisfy semi_promise concept");

    // Test kythira::SemiPromise<double> satisfies semi_promise concept
    static_assert(kythira::semi_promise<kythira::SemiPromise<double>, double>,
                  "kythira::SemiPromise<double> must satisfy semi_promise concept");

    // Test kythira::SemiPromise<void> satisfies semi_promise concept
    static_assert(kythira::semi_promise<kythira::SemiPromise<void>, void>,
                  "kythira::SemiPromise<void> must satisfy semi_promise concept for void type");

    // Test kythira::SemiPromise with custom types
    struct CustomType {
        int value;
        std::string name;
    };

    static_assert(kythira::semi_promise<kythira::SemiPromise<CustomType>, CustomType>,
                  "kythira::SemiPromise<CustomType> must satisfy semi_promise concept");

    // Test kythira::SemiPromise with pointer types
    static_assert(kythira::semi_promise<kythira::SemiPromise<int*>, int*>,
                  "kythira::SemiPromise<int*> must satisfy semi_promise concept");

    // Test kythira::SemiPromise with reference wrapper
    static_assert(kythira::semi_promise<kythira::SemiPromise<std::reference_wrapper<int>>,
                                        std::reference_wrapper<int>>,
                  "kythira::SemiPromise<std::reference_wrapper<int>> must satisfy semi_promise "
                  "concept");

    BOOST_TEST_MESSAGE("All kythira::SemiPromise types satisfy semi_promise concept");
}

/**
 * Test runtime behavior of folly::Promise with the semi_promise concept interface
 */
BOOST_AUTO_TEST_CASE(test_folly_promise_runtime_behavior, *boost::unit_test::timeout(30)) {
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