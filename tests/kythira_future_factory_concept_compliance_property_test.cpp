#define BOOST_TEST_MODULE KythiraFutureFactoryConceptCompliancePropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/future.hpp>
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
constexpr std::size_t property_test_iterations = 100;
}

/**
 * **Feature: folly-concept-wrappers, Property 1: Concept Compliance**
 *
 * Property: For any FutureFactory class and its corresponding concept, the factory should satisfy
 * all concept requirements at compile time and runtime
 * **Validates: Requirements 3.1, 3.2, 3.3**
 */
BOOST_AUTO_TEST_CASE(kythira_future_factory_concept_compliance_property_test,
                     *boost::unit_test::timeout(90)) {
    // Test 1: Static assertion for concept compliance
    {
        // Test kythira::FutureFactory satisfies future_factory concept
        static_assert(future_factory<future_factory_default>,
                      "kythira::FutureFactory must satisfy future_factory concept");

        BOOST_TEST_MESSAGE("kythira::FutureFactory satisfies future_factory concept");
    }

    // Test 2: makeFuture method with various types
    {
        // Test makeFuture with int
        auto int_future = future_factory_default::makeFuture(test_value);
        static_assert(future<decltype(int_future), int>,
                      "makeFuture result must satisfy future concept");
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(int_future).get(), test_value);

        // Test makeFuture with std::string
        std::string test_str = "hello world";
        auto string_future = future_factory_default::makeFuture(test_str);
        static_assert(future<decltype(string_future), std::string>,
                      "makeFuture result must satisfy future concept");
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK_EQUAL(std::move(string_future).get(), test_str);

        // Test makeFuture with double
        auto double_future = future_factory_default::makeFuture(test_double);
        static_assert(future<decltype(double_future), double>,
                      "makeFuture result must satisfy future concept");
        BOOST_CHECK(double_future.isReady());
        BOOST_CHECK_EQUAL(std::move(double_future).get(), test_double);

        // Test makeFuture for void
        auto void_future = future_factory_default::makeFuture();
        static_assert(future<decltype(void_future), void>,
                      "makeFuture void result must satisfy future concept");
        BOOST_CHECK(void_future.isReady());
        std::move(void_future).get();  // Should not throw

        BOOST_TEST_MESSAGE("makeFuture methods work correctly with various types");
    }

    // Test 3: makeExceptionalFuture method with folly::exception_wrapper
    {
        auto ex = std::make_exception_ptr(std::runtime_error(test_string));

        // Test makeExceptionalFuture with int
        auto int_future = future_factory_default::makeExceptionalFuture<int>(ex);
        static_assert(future<decltype(int_future), int>,
                      "makeExceptionalFuture result must satisfy future concept");
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK_THROW(std::move(int_future).get(), std::runtime_error);

        // Test makeExceptionalFuture with std::string
        auto string_future = future_factory_default::makeExceptionalFuture<std::string>(ex);
        static_assert(future<decltype(string_future), std::string>,
                      "makeExceptionalFuture result must satisfy future concept");
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK_THROW(std::move(string_future).get(), std::runtime_error);

        // Test makeExceptionalFuture with void
        auto void_future = future_factory_default::makeExceptionalFuture<void>(ex);
        static_assert(future<decltype(void_future), void>,
                      "makeExceptionalFuture void result must satisfy future concept");
        BOOST_CHECK(void_future.isReady());
        BOOST_CHECK_THROW(std::move(void_future).get(), std::runtime_error);

        BOOST_TEST_MESSAGE(
            "makeExceptionalFuture methods work correctly with folly::exception_wrapper");
    }

    // Test 4: makeExceptionalFuture method with std::exception_ptr
    {
        std::exception_ptr ex_ptr;
        try {
            throw std::runtime_error(test_string);
        } catch (...) {
            ex_ptr = std::current_exception();
        }

        // Test makeExceptionalFuture with int
        auto int_future = future_factory_default::makeExceptionalFuture<int>(ex_ptr);
        static_assert(future<decltype(int_future), int>,
                      "makeExceptionalFuture result must satisfy future concept");
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK_THROW(std::move(int_future).get(), std::runtime_error);

        // Test makeExceptionalFuture with std::string
        auto string_future = future_factory_default::makeExceptionalFuture<std::string>(ex_ptr);
        static_assert(future<decltype(string_future), std::string>,
                      "makeExceptionalFuture result must satisfy future concept");
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK_THROW(std::move(string_future).get(), std::runtime_error);

        BOOST_TEST_MESSAGE("makeExceptionalFuture methods work correctly with std::exception_ptr");
    }

    // Test 5: makeReadyFuture method
    {
        // Test makeReadyFuture (void/Unit) - the "unit" type backing this
        // differs by backend (Folly's own factory returns folly::Unit
        // specifically; boost/stdexec return the portable kythira::unit).
        auto ready_future = future_factory_default::makeReadyFuture();
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) || defined(KYTHIRA_FUTURE_BACKEND_BOOST)
        static_assert(future<decltype(ready_future), kythira::unit>,
                      "makeReadyFuture result must satisfy future concept");
#else
        static_assert(future<decltype(ready_future), folly::Unit>,
                      "makeReadyFuture result must satisfy future concept");
#endif
        BOOST_CHECK(ready_future.isReady());
        std::move(ready_future).get();  // Should not throw

        // Test makeReadyFuture with value
        auto ready_int_future = future_factory_default::makeReadyFuture(test_value);
        static_assert(future<decltype(ready_int_future), int>,
                      "makeReadyFuture with value result must satisfy future concept");
        BOOST_CHECK(ready_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(ready_int_future).get(), test_value);

        BOOST_TEST_MESSAGE("makeReadyFuture methods work correctly");
    }

    // Test 6: Property-based testing - generate multiple test cases
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        int random_value = static_cast<int>(i * 7 + 13);  // Simple pseudo-random generation

        // Test makeFuture with random values
        {
            auto future = future_factory_default::makeFuture(random_value);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_value);
        }

        // Test makeExceptionalFuture with different exception messages
        {
            std::string exception_msg = "test exception " + std::to_string(i);
            auto ex = std::make_exception_ptr(std::runtime_error(exception_msg));
            auto future = future_factory_default::makeExceptionalFuture<int>(ex);
            BOOST_CHECK(future.isReady());

            try {
                std::move(future).get();
                BOOST_FAIL("Expected exception was not thrown");
            } catch (const std::runtime_error& e) {
                std::string error_msg = e.what();
                BOOST_CHECK(error_msg.find("test exception") != std::string::npos);
            }
        }

        // Test makeReadyFuture with random values
        {
            auto future = future_factory_default::makeReadyFuture(random_value);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_value);
        }

        // Test move semantics with makeFuture
        {
            std::string movable_string = "movable test string " + std::to_string(i);
            auto future = future_factory_default::makeFuture(std::move(movable_string));
            BOOST_CHECK(future.isReady());

            auto result = std::move(future).get();
            BOOST_CHECK(result.find("movable test string") != std::string::npos);
        }
    }

    // Test 7: Type deduction and conversion handling
    {
        // Test type deduction with const values
        const int const_value = test_value;
        auto const_future = future_factory_default::makeFuture(const_value);
        static_assert(std::is_same_v<decltype(const_future), future_default<int>>,
                      "Type deduction should remove const");
        BOOST_CHECK_EQUAL(std::move(const_future).get(), const_value);

        // Test type deduction with references
        int& ref_value = const_cast<int&>(const_value);
        auto ref_future = future_factory_default::makeFuture(ref_value);
        static_assert(std::is_same_v<decltype(ref_future), future_default<int>>,
                      "Type deduction should remove reference");
        BOOST_CHECK_EQUAL(std::move(ref_future).get(), ref_value);

        // Test with custom types
        struct CustomType {
            int value;
            std::string name;

            bool operator==(const CustomType& other) const {
                return value == other.value && name == other.name;
            }
        };

        CustomType custom{test_value, "custom"};
        auto custom_future = future_factory_default::makeFuture(custom);
        static_assert(future<decltype(custom_future), CustomType>,
                      "makeFuture with custom type must satisfy future concept");
        BOOST_CHECK(std::move(custom_future).get() == custom);

        BOOST_TEST_MESSAGE("Type deduction and conversion handling work correctly");
    }
}

/**
 * Test that types NOT satisfying future_factory concept are properly rejected
 */
BOOST_AUTO_TEST_CASE(future_factory_concept_rejection_test, *boost::unit_test::timeout(30)) {
    // Test that basic types don't satisfy the concept
    static_assert(!future_factory<int>, "int should not satisfy future_factory concept");
    static_assert(!future_factory<std::string>,
                  "std::string should not satisfy future_factory concept");

    // Test that types missing required methods don't satisfy the concept.
    // Built on the portable future_factory_default::makeFuture/
    // makeExceptionalFuture rather than direct future_default value/
    // exception constructors: boost_backend's Future<T> only has an
    // explicit Future(NativeSenderOrFuture&&) constructor (no direct-value
    // or direct-exception overload), so those don't compile there even
    // though this dead code (never invoked - only its signature matters
    // for the static_assert below) would otherwise not care about actual
    // construction semantics.
    struct IncompleteFutureFactory {
        static auto makeFuture(int value) -> future_default<int> {
            return future_factory_default::makeFuture(value);
        }
        // Missing makeExceptionalFuture and makeReadyFuture
    };

    static_assert(!future_factory<IncompleteFutureFactory>,
                  "IncompleteFutureFactory should not satisfy future_factory concept");

    // Test that non-static methods don't satisfy the concept
    struct NonStaticFutureFactory {
        auto makeFuture(int value) -> future_default<int> {
            return future_factory_default::makeFuture(value);
        }  // Not static
        auto makeExceptionalFuture(std::exception_ptr ex) -> future_default<int> {
            return future_factory_default::makeExceptionalFuture<int>(ex);
        }  // Not static
        // future_default<void> via the zero-arg makeFuture(), not
        // future_default<kythira::unit> via makeFuture(kythira::unit{}):
        // stdexec_backend has a kythira::unit-specific makeFuture overload
        // that returns Future<void> instead of Future<kythira::unit> (an
        // intentional unit-means-void special case there, absent from
        // boost_backend's purely-templated makeFuture), so that call's
        // return type isn't consistent across backends. The zero-arg form
        // is the portable way to get "a ready void future" everywhere.
        auto makeReadyFuture() -> future_default<void> {
            return future_factory_default::makeFuture();
        }  // Not static
    };

    static_assert(!future_factory<NonStaticFutureFactory>,
                  "NonStaticFutureFactory should not satisfy future_factory concept");

    BOOST_TEST_MESSAGE("future_factory concept properly rejects invalid types");
}

/**
 * Test static-only nature of FutureFactory
 */
BOOST_AUTO_TEST_CASE(future_factory_static_only_test, *boost::unit_test::timeout(30)) {
    // Test that FutureFactory cannot be instantiated
    static_assert(!std::is_default_constructible_v<future_factory_default>,
                  "FutureFactory should not be default constructible");
    static_assert(!std::is_copy_constructible_v<future_factory_default>,
                  "FutureFactory should not be copy constructible");
    static_assert(!std::is_move_constructible_v<future_factory_default>,
                  "FutureFactory should not be move constructible");
    static_assert(!std::is_copy_assignable_v<future_factory_default>,
                  "FutureFactory should not be copy assignable");
    static_assert(!std::is_move_assignable_v<future_factory_default>,
                  "FutureFactory should not be move assignable");

    BOOST_TEST_MESSAGE("FutureFactory is properly static-only");
}

/**
 * Test exception safety and error handling
 */
BOOST_AUTO_TEST_CASE(future_factory_exception_safety_test, *boost::unit_test::timeout(30)) {
    // Test that makeExceptionalFuture properly handles different exception types
    {
        // Test with std::runtime_error
        auto runtime_ex = std::make_exception_ptr(std::runtime_error("runtime error"));
        auto future1 = future_factory_default::makeExceptionalFuture<int>(runtime_ex);
        BOOST_CHECK_THROW(std::move(future1).get(), std::runtime_error);

        // Test with std::logic_error
        auto logic_ex = std::make_exception_ptr(std::logic_error("logic error"));
        auto future2 = future_factory_default::makeExceptionalFuture<int>(logic_ex);
        BOOST_CHECK_THROW(std::move(future2).get(), std::logic_error);

        // Test with std::invalid_argument
        auto invalid_ex = std::make_exception_ptr(std::invalid_argument("invalid argument"));
        auto future3 = future_factory_default::makeExceptionalFuture<int>(invalid_ex);
        BOOST_CHECK_THROW(std::move(future3).get(), std::invalid_argument);
    }

    // Test exception conversion between folly::exception_wrapper and std::exception_ptr
    {
        std::exception_ptr ex_ptr;
        try {
            throw std::runtime_error("converted exception");
        } catch (...) {
            ex_ptr = std::current_exception();
        }

        auto future = future_factory_default::makeExceptionalFuture<int>(ex_ptr);
        BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);
    }

    BOOST_TEST_MESSAGE("Exception safety and error handling work correctly");
}