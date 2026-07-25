#define BOOST_TEST_MODULE KythiraPromiseFutureRetrievalPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>
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
 * Property: For any promise wrapper and value/exception, setting the value or exception should
 * properly convert types and make the associated future ready with the correct result
 * **Validates: Requirements 1.5**
 */
BOOST_AUTO_TEST_CASE(kythira_promise_future_retrieval_property_test,
                     *boost::unit_test::timeout(90)) {
    // Test 1: getFuture returns properly wrapped Future instance for int type
    {
        promise_default<int> promise;

        // Get future before setting value
        auto future = promise.getFuture();

        // Verify future is properly wrapped and has correct type
        static_assert(std::is_same_v<decltype(future), future_default<int>>,
                      "getFuture() should return future_default<int>");

        // Future should not be ready initially
        BOOST_CHECK(!future.isReady());

        // Set value and verify future becomes ready with correct value
        promise.setValue(test_value);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), test_value);
    }

    // Test 2: getFuture returns properly wrapped Future instance for std::string type
    {
        promise_default<std::string> promise;

        auto future = promise.getFuture();
        static_assert(std::is_same_v<decltype(future), future_default<std::string>>,
                      "getFuture() should return future_default<std::string>");

        std::string test_str = "hello world";
        BOOST_CHECK(!future.isReady());

        promise.setValue(test_str);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), test_str);
    }

    // Test 3: getFuture returns properly wrapped Future instance for void type
    {
        promise_default<void> promise;

        auto future = promise.getFuture();
        static_assert(std::is_same_v<decltype(future), future_default<void>>,
                      "getFuture() should return future_default<void>");

        BOOST_CHECK(!future.isReady());

        promise.setValue(kythira::unit{});
        BOOST_CHECK(future.isReady());
        std::move(future).get();  // Should not throw
    }

    // Test 4: getSemiFuture returns properly wrapped Future instance for int type
    {
        promise_default<int> promise;

        auto semi_future = promise.getSemiFuture();
        static_assert(std::is_same_v<decltype(semi_future), future_default<int>>,
                      "getSemiFuture() should return future_default<int>");

        BOOST_CHECK(!semi_future.isReady());

        promise.setValue(test_value);
        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_EQUAL(std::move(semi_future).get(), test_value);
    }

    // Test 5: getSemiFuture returns properly wrapped Future instance for void type
    {
        promise_default<void> promise;

        auto semi_future = promise.getSemiFuture();
        static_assert(std::is_same_v<decltype(semi_future), future_default<void>>,
                      "getSemiFuture() should return future_default<void>");

        BOOST_CHECK(!semi_future.isReady());

        promise.setValue(kythira::unit{});
        BOOST_CHECK(semi_future.isReady());
        std::move(semi_future).get();  // Should not throw
    }

    // Test 6: Future retrieval with exception handling - getFuture
    {
        promise_default<int> promise;

        auto future = promise.getFuture();
        BOOST_CHECK(!future.isReady());

        // Set exception and verify future becomes ready with exception
        auto ex = std::make_exception_ptr(std::runtime_error(test_string));
        promise.setException(ex);
        BOOST_CHECK(future.isReady());

        // Future should throw when getting value
        BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    }

    // Test 7: Future retrieval with exception handling - getSemiFuture
    {
        promise_default<int> promise;

        auto semi_future = promise.getSemiFuture();
        BOOST_CHECK(!semi_future.isReady());

        // Set exception using std::exception_ptr
        try {
            throw std::runtime_error(test_string);
        } catch (...) {
            promise.setException(std::current_exception());
        }

        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_THROW(std::move(semi_future).get(), std::runtime_error);
    }

    // Test 8: Property-based testing - verify future retrieval works across multiple types and
    // values
    for (int i = 0; i < 100; ++i) {
        int random_value = i * 7 + 13;  // Simple pseudo-random generation

        // Test getFuture with various values
        {
            promise_default<int> promise;
            auto future = promise.getFuture();

            // Verify type correctness
            static_assert(std::is_same_v<decltype(future), future_default<int>>,
                          "getFuture() should always return future_default<int>");

            BOOST_CHECK(!future.isReady());
            promise.setValue(random_value);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_value);
        }

        // Test getSemiFuture with various values
        {
            promise_default<int> promise;
            auto semi_future = promise.getSemiFuture();

            // Verify type correctness
            static_assert(std::is_same_v<decltype(semi_future), future_default<int>>,
                          "getSemiFuture() should always return future_default<int>");

            BOOST_CHECK(!semi_future.isReady());
            promise.setValue(random_value);
            BOOST_CHECK(semi_future.isReady());
            BOOST_CHECK_EQUAL(std::move(semi_future).get(), random_value);
        }

        // Test with string values
        {
            promise_default<std::string> string_promise;
            std::string test_str = "test string " + std::to_string(i);

            auto future = string_promise.getFuture();
            static_assert(std::is_same_v<decltype(future), future_default<std::string>>,
                          "getFuture() should return future_default<std::string>");

            string_promise.setValue(test_str);
            BOOST_CHECK_EQUAL(std::move(future).get(), test_str);
        }

        // Test with void promises
        {
            promise_default<void> void_promise;

            auto void_future = void_promise.getFuture();
            static_assert(std::is_same_v<decltype(void_future), future_default<void>>,
                          "getFuture() should return future_default<void>");

            void_promise.setValue(kythira::unit{});
            std::move(void_future).get();  // Should not throw
        }

        // Test exception propagation through future retrieval
        {
            promise_default<int> exception_promise;
            auto future = exception_promise.getFuture();

            auto ex =
                std::make_exception_ptr(std::runtime_error("test exception " + std::to_string(i)));
            exception_promise.setException(ex);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
        }
    }
}

/**
 * Test that retrieved futures satisfy the future concept
 */
BOOST_AUTO_TEST_CASE(retrieved_future_concept_compliance_test, *boost::unit_test::timeout(30)) {
    // Test that futures returned by getFuture satisfy future concept
    {
        promise_default<int> promise;
        auto future = promise.getFuture();

        promise.setValue(test_value);
        BOOST_CHECK_EQUAL(std::move(future).get(), test_value);
    }

    // Test that futures returned by getSemiFuture satisfy future concept
    {
        promise_default<std::string> promise;
        auto semi_future = promise.getSemiFuture();

        std::string test_str = "test";
        promise.setValue(test_str);
        BOOST_CHECK_EQUAL(std::move(semi_future).get(), test_str);
    }

    // Test with void type
    {
        promise_default<void> promise;
        auto future = promise.getFuture();

        promise.setValue(kythira::unit{});
        std::move(future).get();  // Should not throw
    }
}

/**
 * Test future retrieval behavior and lifecycle management
 */
BOOST_AUTO_TEST_CASE(future_retrieval_lifecycle_test, *boost::unit_test::timeout(30)) {
    // Test that promise-future relationship is properly maintained
    {
        promise_default<int> promise;

        // Get future before fulfilling promise
        auto future = promise.getFuture();
        BOOST_CHECK(!promise.isFulfilled());
        BOOST_CHECK(!future.isReady());

        // Fulfill promise and verify future becomes ready
        promise.setValue(test_value);
        BOOST_CHECK(promise.isFulfilled());
        BOOST_CHECK(future.isReady());

        // Verify value propagation
        BOOST_CHECK_EQUAL(std::move(future).get(), test_value);
    }

    // Test that getSemiFuture works independently of getFuture
    {
        promise_default<int> promise;

        auto semi_future = promise.getSemiFuture();
        BOOST_CHECK(!semi_future.isReady());

        promise.setValue(test_value);
        BOOST_CHECK(semi_future.isReady());
        BOOST_CHECK_EQUAL(std::move(semi_future).get(), test_value);
    }

    // Test proper resource cleanup when promise goes out of scope
    {
        promise_default<int> promise;
        auto future = promise.getFuture();
        promise.setValue(test_value);

        // Future should still be valid and contain the value
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), test_value);
    }
}

/**
 * Test move semantics with future retrieval
 */
BOOST_AUTO_TEST_CASE(future_retrieval_move_semantics_test, *boost::unit_test::timeout(30)) {
    // Test that promise can be moved after getting future
    {
        promise_default<int> promise1;
        auto future = promise1.getFuture();

        // Move promise
        promise_default<int> promise2 = std::move(promise1);

        // Original future should still work with moved promise
        promise2.setValue(test_value);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_EQUAL(std::move(future).get(), test_value);
    }

    // Test with movable values
    {
        promise_default<std::string> promise;
        auto future = promise.getFuture();

        std::string movable_string = "movable test string";
        promise.setValue(std::move(movable_string));

        auto result = std::move(future).get();
        BOOST_CHECK_EQUAL(result, "movable test string");
    }
}