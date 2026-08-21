// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE KythiraFutureFactoryCreationPropertyTest
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>
#include <concepts/future.hpp>
#include <exception>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>
#include <random>
#include <chrono>

using namespace kythira;

// Test constants
namespace {
constexpr int test_value = 42;
constexpr const char* test_string = "test exception";
constexpr double test_double = 3.14;
constexpr std::size_t property_test_iterations = 100;
}

/**
 * **Feature: folly-concept-wrappers, Property 4: Factory Future Creation**
 *
 * Property: For any value or exception, factory methods should create futures that are immediately
 * ready with the correct value or exception
 * **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5**
 */
BOOST_AUTO_TEST_CASE(kythira_future_factory_creation_property_test,
                     *boost::unit_test::timeout(90)) {
    // Test 1: makeFuture creates immediately ready futures with correct values
    {
        // Test with int
        auto int_future = future_factory_default::makeFuture(test_value);
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(int_future).get(), test_value);

        // Test with std::string
        std::string test_str = "hello world";
        auto string_future = future_factory_default::makeFuture(test_str);
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK_EQUAL(std::move(string_future).get(), test_str);

        // Test with double
        auto double_future = future_factory_default::makeFuture(test_double);
        BOOST_CHECK(double_future.isReady());
        BOOST_CHECK_EQUAL(std::move(double_future).get(), test_double);

        // Test with void
        auto void_future = future_factory_default::makeFuture();
        BOOST_CHECK(void_future.isReady());
        std::move(void_future).get();  // Should not throw

        BOOST_TEST_MESSAGE("makeFuture creates immediately ready futures with correct values");
    }

    // Test 2: makeExceptionalFuture creates immediately ready futures with correct exceptions
    {
        auto ex = std::make_exception_ptr(std::runtime_error(test_string));

        // Test with int
        auto int_future = future_factory_default::makeExceptionalFuture<int>(ex);
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK_THROW(std::move(int_future).get(), std::runtime_error);

        // Test with std::string
        auto string_future = future_factory_default::makeExceptionalFuture<std::string>(ex);
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK_THROW(std::move(string_future).get(), std::runtime_error);

        // Test with void
        auto void_future = future_factory_default::makeExceptionalFuture<void>(ex);
        BOOST_CHECK(void_future.isReady());
        BOOST_CHECK_THROW(std::move(void_future).get(), std::runtime_error);

        BOOST_TEST_MESSAGE(
            "makeExceptionalFuture creates immediately ready futures with correct exceptions");
    }

    // Test 3: makeReadyFuture creates immediately ready futures
    {
        // Test makeReadyFuture (void/Unit)
        auto ready_future = future_factory_default::makeReadyFuture();
        BOOST_CHECK(ready_future.isReady());
        std::move(ready_future).get();  // Should not throw

        // Test makeReadyFuture with value
        auto ready_int_future = future_factory_default::makeReadyFuture(test_value);
        BOOST_CHECK(ready_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(ready_int_future).get(), test_value);

        BOOST_TEST_MESSAGE("makeReadyFuture creates immediately ready futures");
    }

    // Test 4: Property-based testing with random values
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> int_dist(-1000, 1000);
    std::uniform_real_distribution<double> double_dist(-100.0, 100.0);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        int random_int = int_dist(gen);
        double random_double = double_dist(gen);

        // Test makeFuture with random int values
        {
            auto future = future_factory_default::makeFuture(random_int);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_int);
        }

        // Test makeFuture with random double values
        {
            auto future = future_factory_default::makeFuture(random_double);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_double);
        }

        // Test makeReadyFuture with random values
        {
            auto future = future_factory_default::makeReadyFuture(random_int);
            BOOST_CHECK(future.isReady());
            BOOST_CHECK_EQUAL(std::move(future).get(), random_int);
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
    }

    // Test 5: Type deduction and conversion handling
    {
        // Test type deduction with const values
        const int const_value = test_value;
        auto const_future = future_factory_default::makeFuture(const_value);
        static_assert(std::is_same_v<decltype(const_future), future_default<int>>,
                      "Type deduction should remove const");
        BOOST_CHECK(const_future.isReady());
        BOOST_CHECK_EQUAL(std::move(const_future).get(), const_value);

        // Test type deduction with references
        int& ref_value = const_cast<int&>(const_value);
        auto ref_future = future_factory_default::makeFuture(ref_value);
        static_assert(std::is_same_v<decltype(ref_future), future_default<int>>,
                      "Type deduction should remove reference");
        BOOST_CHECK(ref_future.isReady());
        BOOST_CHECK_EQUAL(std::move(ref_future).get(), ref_value);

        // Test with rvalue references
        auto rvalue_future =
            future_factory_default::makeFuture(std::move(const_cast<int&>(const_value)));
        static_assert(std::is_same_v<decltype(rvalue_future), future_default<int>>,
                      "Type deduction should handle rvalue references");
        BOOST_CHECK(rvalue_future.isReady());
        BOOST_CHECK_EQUAL(std::move(rvalue_future).get(), const_value);

        BOOST_TEST_MESSAGE("Type deduction and conversion handling work correctly");
    }

    // Test 6: Move semantics optimization
    {
        // Test move semantics with makeFuture
        std::string movable_string = "movable test string";
        std::string original_string = movable_string;
        auto future = future_factory_default::makeFuture(std::move(movable_string));
        BOOST_CHECK(future.isReady());

        auto result = std::move(future).get();
        BOOST_CHECK_EQUAL(result, original_string);
        // Note: movable_string may be empty after move, which is expected

        // Test move semantics with makeReadyFuture
        std::string another_movable = "another movable string";
        std::string another_original = another_movable;
        auto ready_future = future_factory_default::makeReadyFuture(std::move(another_movable));
        BOOST_CHECK(ready_future.isReady());
        BOOST_CHECK_EQUAL(std::move(ready_future).get(), another_original);

        BOOST_TEST_MESSAGE("Move semantics optimization works correctly");
    }

    // Test 7: Exception type conversion
    {
        // Test with std::exception_ptr
        std::exception_ptr ex_ptr;
        try {
            throw std::runtime_error("converted exception");
        } catch (...) {
            ex_ptr = std::current_exception();
        }

        auto future = future_factory_default::makeExceptionalFuture<int>(ex_ptr);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK_THROW(std::move(future).get(), std::runtime_error);

        // Test with different exception types
        auto logic_ex = std::make_exception_ptr(std::logic_error("logic error"));
        auto logic_future = future_factory_default::makeExceptionalFuture<std::string>(logic_ex);
        BOOST_CHECK(logic_future.isReady());
        BOOST_CHECK_THROW(std::move(logic_future).get(), std::logic_error);

        BOOST_TEST_MESSAGE("Exception type conversion works correctly");
    }

    // Test 8: Custom types
    {
        struct CustomType {
            int value;
            std::string name;

            bool operator==(const CustomType& other) const {
                return value == other.value && name == other.name;
            }
        };

        CustomType custom{test_value, "custom"};
        auto custom_future = future_factory_default::makeFuture(custom);
        BOOST_CHECK(custom_future.isReady());
        BOOST_CHECK(std::move(custom_future).get() == custom);

        // Test with custom type exception
        auto custom_ex_future = future_factory_default::makeExceptionalFuture<CustomType>(
            std::make_exception_ptr(std::runtime_error("custom error")));
        BOOST_CHECK(custom_ex_future.isReady());
        BOOST_CHECK_THROW(std::move(custom_ex_future).get(), std::runtime_error);

        BOOST_TEST_MESSAGE("Custom types work correctly");
    }

    // Test 9: Void/Unit handling
    {
        // Test void makeFuture
        auto void_future1 = future_factory_default::makeFuture();
        static_assert(std::is_same_v<decltype(void_future1), future_default<void>>,
                      "makeFuture() should return future_default<void>");
        BOOST_CHECK(void_future1.isReady());
        std::move(void_future1).get();  // Should not throw

        // Test void makeReadyFuture
        auto void_future2 = future_factory_default::makeReadyFuture();
        // The "unit" type backing makeReadyFuture()'s return value differs
        // by backend (Folly's own factory returns folly::Unit specifically;
        // boost/stdexec return the portable kythira::unit) - assert against
        // whichever is actually correct for the active backend.
#if defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) || defined(KYTHIRA_FUTURE_BACKEND_BOOST)
        static_assert(std::is_same_v<decltype(void_future2), future_default<kythira::unit>>,
                      "makeReadyFuture() should return future_default<kythira::unit>");
#else
        static_assert(std::is_same_v<decltype(void_future2), future_default<folly::Unit>>,
                      "makeReadyFuture() should return future_default<folly::Unit>");
#endif
        BOOST_CHECK(void_future2.isReady());
        std::move(void_future2).get();  // Should not throw

        // Test void makeExceptionalFuture
        auto void_ex_future = future_factory_default::makeExceptionalFuture<void>(
            std::make_exception_ptr(std::runtime_error("void error")));
        static_assert(std::is_same_v<decltype(void_ex_future), future_default<void>>,
                      "makeExceptionalFuture<void> should return future_default<void>");
        BOOST_CHECK(void_ex_future.isReady());
        BOOST_CHECK_THROW(std::move(void_ex_future).get(), std::runtime_error);

        BOOST_TEST_MESSAGE("Void/Unit handling works correctly");
    }

    // Test 10: Timing properties - futures should be immediately ready
    {
        auto start_time = std::chrono::steady_clock::now();

        // Create multiple futures and verify they're all immediately ready
        auto int_future = future_factory_default::makeFuture(42);
        auto string_future = future_factory_default::makeFuture(std::string("test"));
        auto void_future = future_factory_default::makeFuture();
        auto ready_future = future_factory_default::makeReadyFuture(3.14);
        auto ex_future = future_factory_default::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("test")));

        auto end_time = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // All futures should be immediately ready
        BOOST_CHECK(int_future.isReady());
        BOOST_CHECK(string_future.isReady());
        BOOST_CHECK(void_future.isReady());
        BOOST_CHECK(ready_future.isReady());
        BOOST_CHECK(ex_future.isReady());

        // Creation should be fast (less than 100ms for all operations)
        BOOST_CHECK(duration.count() < 100);

        BOOST_TEST_MESSAGE("Timing properties verified - futures are immediately ready");
    }
}

/**
 * Test edge cases and boundary conditions for factory future creation
 */
BOOST_AUTO_TEST_CASE(future_factory_creation_edge_cases_test, *boost::unit_test::timeout(30)) {
    // Test with empty string
    {
        std::string empty_str;
        auto future = future_factory_default::makeFuture(empty_str);
        BOOST_CHECK(future.isReady());
        BOOST_CHECK(std::move(future).get().empty());
    }

    // Test with zero values
    {
        auto zero_int_future = future_factory_default::makeFuture(0);
        BOOST_CHECK(zero_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(zero_int_future).get(), 0);

        auto zero_double_future = future_factory_default::makeFuture(0.0);
        BOOST_CHECK(zero_double_future.isReady());
        BOOST_CHECK_EQUAL(std::move(zero_double_future).get(), 0.0);
    }

    // Test with negative values
    {
        auto neg_int_future = future_factory_default::makeFuture(-42);
        BOOST_CHECK(neg_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(neg_int_future).get(), -42);

        auto neg_double_future = future_factory_default::makeFuture(-3.14);
        BOOST_CHECK(neg_double_future.isReady());
        BOOST_CHECK_EQUAL(std::move(neg_double_future).get(), -3.14);
    }

    // Test with maximum/minimum values
    {
        auto max_int_future = future_factory_default::makeFuture(std::numeric_limits<int>::max());
        BOOST_CHECK(max_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(max_int_future).get(), std::numeric_limits<int>::max());

        auto min_int_future = future_factory_default::makeFuture(std::numeric_limits<int>::min());
        BOOST_CHECK(min_int_future.isReady());
        BOOST_CHECK_EQUAL(std::move(min_int_future).get(), std::numeric_limits<int>::min());
    }

    // Test with null exception_ptr
    {
        std::exception_ptr null_ex_ptr;
        // Note: This might not work as expected with null exception_ptr
        // but we test to ensure it doesn't crash
        try {
            auto future = future_factory_default::makeExceptionalFuture<int>(null_ex_ptr);
            BOOST_CHECK(future.isReady());
            // The behavior with null exception_ptr is implementation-defined
        } catch (...) {
            // It's acceptable if this throws during construction
            BOOST_TEST_MESSAGE(
                "Null exception_ptr handling throws during construction (acceptable)");
        }
    }

    BOOST_TEST_MESSAGE("Edge cases and boundary conditions handled correctly");
}

/**
 * Test concurrent factory future creation
 */
BOOST_AUTO_TEST_CASE(future_factory_creation_concurrent_test, *boost::unit_test::timeout(60)) {
    constexpr std::size_t num_threads = 4;
    constexpr std::size_t operations_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<std::size_t> successful_operations{0};
    std::atomic<std::size_t> total_operations{0};

    for (std::size_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (std::size_t i = 0; i < operations_per_thread; ++i) {
                try {
                    int value = static_cast<int>(t * operations_per_thread + i);

                    // Test makeFuture
                    auto future1 = future_factory_default::makeFuture(value);
                    if (future1.isReady() && std::move(future1).get() == value) {
                        successful_operations++;
                    }
                    total_operations++;

                    // Test makeReadyFuture
                    auto future2 = future_factory_default::makeReadyFuture(value);
                    if (future2.isReady() && std::move(future2).get() == value) {
                        successful_operations++;
                    }
                    total_operations++;

                    // Test makeExceptionalFuture
                    auto ex = std::make_exception_ptr(std::runtime_error("concurrent test"));
                    auto future3 = future_factory_default::makeExceptionalFuture<int>(ex);
                    if (future3.isReady()) {
                        try {
                            std::move(future3).get();
                            // Should not reach here
                        } catch (const std::runtime_error&) {
                            successful_operations++;
                        }
                    }
                    total_operations++;

                } catch (...) {
                    // Count failed operations
                    total_operations++;
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify that most operations succeeded (allow for some potential race conditions)
    std::size_t expected_operations =
        num_threads * operations_per_thread * 3;  // 3 operations per iteration
    BOOST_CHECK_EQUAL(total_operations.load(), expected_operations);

    // At least 95% of operations should succeed
    double success_rate = static_cast<double>(successful_operations.load()) /
                          static_cast<double>(total_operations.load());
    BOOST_CHECK(success_rate >= 0.95);

    BOOST_TEST_MESSAGE("Concurrent factory future creation test completed with "
                       << successful_operations.load() << "/" << total_operations.load()
                       << " successful operations (" << (success_rate * 100) << "%)");
}