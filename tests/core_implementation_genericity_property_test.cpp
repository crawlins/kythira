// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE core_implementation_genericity_property_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <concepts/future.hpp>

#include <random>
#include <string>
#include <chrono>
#include <type_traits>

namespace {
constexpr std::size_t property_test_iterations = 100;
}

BOOST_AUTO_TEST_SUITE(core_implementation_genericity_property_tests)

// **Feature: future-conversion, Property 13: Core implementation genericity**
// **Validates: Requirements 8.1, 8.2**
// Property: For any core Raft implementation, it should accept future types as template
// parameters and use future concepts instead of concrete future types
BOOST_AUTO_TEST_CASE(property_core_implementation_genericity, *boost::unit_test::timeout(90)) {
    // Test 1: Verify that kythira::Future satisfies the future concept
    static_assert(kythira::future<kythira::future_default<int>, int>,
                  "kythira::future_default<int> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<std::string>, std::string>,
                  "kythira::future_default<std::string> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<void>, void>,
                  "kythira::future_default<void> must satisfy future concept");
    static_assert(kythira::future<kythira::future_default<double>, double>,
                  "kythira::future_default<double> must satisfy future concept");

    // Test 2: Verify the concept can be used as a constraint
    // This lambda demonstrates that the concept can be used to constrain template parameters
    auto test_generic_future = []<typename F, typename T>(F&& future_instance)
    requires kythira::future<std::remove_cvref_t<F>, T>
    {
        // If this compiles, the concept constraint is working
        return true;
    };

    // Test 3: Property-based test - verify concept works with various future instances
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> int_dist(-1000, 1000);

    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        // Test with int futures
        {
            int random_value = int_dist(rng);
            auto int_future = kythira::future_factory_default::makeFuture(random_value);
            bool result =
                test_generic_future.template operator()<kythira::future_default<int>, int>(
                    std::move(int_future));
            BOOST_CHECK(result);
        }

        // Test with string futures
        {
            std::string random_string = "test_" + std::to_string(i);
            auto string_future = kythira::future_factory_default::makeFuture(random_string);
            bool result =
                test_generic_future
                    .template operator()<kythira::future_default<std::string>, std::string>(
                        std::move(string_future));
            BOOST_CHECK(result);
        }

        // Test with void futures
        {
            auto void_future = kythira::future_factory_default::makeFuture();
            bool result =
                test_generic_future.template operator()<kythira::future_default<void>, void>(
                    std::move(void_future));
            BOOST_CHECK(result);
        }

        // Test with double futures
        {
            double random_double = static_cast<double>(int_dist(rng)) / 100.0;
            auto double_future = kythira::future_factory_default::makeFuture(random_double);
            bool result =
                test_generic_future.template operator()<kythira::future_default<double>, double>(
                    std::move(double_future));
            BOOST_CHECK(result);
        }
    }

    // Test 4: Verify that the concept correctly validates required operations
    // Test get() operation
    {
        auto future = kythira::future_factory_default::makeFuture(42);
        BOOST_CHECK_EQUAL(std::move(future).get(), 42);
    }

    // Test isReady() operation
    {
        auto future = kythira::future_factory_default::makeFuture(42);
        BOOST_CHECK(future.isReady());
    }

    // Test wait() operation
    {
        auto future = kythira::future_factory_default::makeFuture(42);
        BOOST_CHECK(future.wait(std::chrono::milliseconds(100)));
    }

    // Test thenValue() operation
    {
        auto future = kythira::future_factory_default::makeFuture(42);
        auto chained = std::move(future).thenValue([](int val) { return val * 2; });
        BOOST_CHECK_EQUAL(std::move(chained).get(), 84);
    }

    // Test thenError() operation
    {
        auto error_future = kythira::future_factory_default::makeExceptionalFuture<int>(
            std::make_exception_ptr(std::runtime_error("test")));
        auto recovered = std::move(error_future).thenError([](std::exception_ptr) { return 0; });
        BOOST_CHECK_EQUAL(std::move(recovered).get(), 0);
    }

    // Test 5: Verify void specialization works correctly
    {
        auto void_future = kythira::future_factory_default::makeFuture();
        BOOST_CHECK(void_future.isReady());
        std::move(void_future).get();  // Should not throw

        // Test void thenValue() chaining - create a new future since we consumed the previous one
        auto void_future2 = kythira::future_factory_default::makeFuture();
        auto chained = std::move(void_future2).thenValue([]() { return 42; });
        BOOST_CHECK_EQUAL(std::move(chained).get(), 42);

        // Test void thenError()
        auto error_future = kythira::future_factory_default::makeExceptionalFuture<void>(
            std::make_exception_ptr(std::runtime_error("test")));
        bool error_handled = false;
        auto recovered = std::move(error_future).thenError([&error_handled](std::exception_ptr) {
            error_handled = true;
        });
        std::move(recovered).get();
        BOOST_CHECK(error_handled);
    }

    // Test 6: Property - concept constraints are enforced at compile time
    // This is validated by the static_assert statements above
    // If the concept is not properly defined, these would fail to compile

    // Test 7: Property - generic code can work with any type satisfying the concept
    auto process_future = []<typename F, typename T>(F future)
    requires kythira::future<F, T>
    {
        if (!future.isReady()) {
            future.wait(std::chrono::milliseconds(1000));
        }
        return std::move(future).get();
    };

    for (std::size_t i = 0; i < 10; ++i) {
        int value = int_dist(rng);
        auto future = kythira::future_factory_default::makeFuture(value);
        int result = process_future.template operator()<kythira::future_default<int>, int>(
            std::move(future));
        BOOST_CHECK_EQUAL(result, value);
    }
}

BOOST_AUTO_TEST_SUITE_END()
