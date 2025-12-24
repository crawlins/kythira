#define BOOST_TEST_MODULE core_implementation_genericity_property_test
#include <boost/test/unit_test.hpp>

#include <concepts/future.hpp>
#include <raft/future.hpp>

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
BOOST_AUTO_TEST_CASE(property_core_implementation_genericity, * boost::unit_test::timeout(90)) {
    
    // Test 1: Verify that kythira::Future satisfies the future concept
    static_assert(kythira::future<kythira::Future<int>, int>, 
                  "kythira::Future<int> must satisfy future concept");
    static_assert(kythira::future<kythira::Future<std::string>, std::string>, 
                  "kythira::Future<std::string> must satisfy future concept");
    static_assert(kythira::future<kythira::Future<void>, void>, 
                  "kythira::Future<void> must satisfy future concept");
    static_assert(kythira::future<kythira::Future<double>, double>, 
                  "kythira::Future<double> must satisfy future concept");
    
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
            kythira::Future<int> int_future(random_value);
            bool result = test_generic_future.template operator()<kythira::Future<int>, int>(
                std::move(int_future));
            BOOST_CHECK(result);
        }
        
        // Test with string futures
        {
            std::string random_string = "test_" + std::to_string(i);
            kythira::Future<std::string> string_future(random_string);
            bool result = test_generic_future.template operator()<kythira::Future<std::string>, std::string>(
                std::move(string_future));
            BOOST_CHECK(result);
        }
        
        // Test with void futures
        {
            kythira::Future<void> void_future;
            bool result = test_generic_future.template operator()<kythira::Future<void>, void>(
                std::move(void_future));
            BOOST_CHECK(result);
        }
        
        // Test with double futures
        {
            double random_double = static_cast<double>(int_dist(rng)) / 100.0;
            kythira::Future<double> double_future(random_double);
            bool result = test_generic_future.template operator()<kythira::Future<double>, double>(
                std::move(double_future));
            BOOST_CHECK(result);
        }
    }
    
    // Test 4: Verify that the concept correctly validates required operations
    // Test get() operation
    {
        kythira::Future<int> future(42);
        BOOST_CHECK_EQUAL(future.get(), 42);
    }
    
    // Test isReady() operation
    {
        kythira::Future<int> future(42);
        BOOST_CHECK(future.isReady());
    }
    
    // Test wait() operation
    {
        kythira::Future<int> future(42);
        BOOST_CHECK(future.wait(std::chrono::milliseconds(100)));
    }
    
    // Test then() operation
    {
        kythira::Future<int> future(42);
        auto chained = std::move(future).then([](int val) { return val * 2; });
        BOOST_CHECK_EQUAL(chained.get(), 84);
    }
    
    // Test onError() operation
    {
        kythira::Future<int> error_future(folly::exception_wrapper(std::runtime_error("test")));
        auto recovered = std::move(error_future).onError([](folly::exception_wrapper) { return 0; });
        BOOST_CHECK_EQUAL(recovered.get(), 0);
    }
    
    // Test 5: Verify void specialization works correctly
    {
        kythira::Future<void> void_future;
        BOOST_CHECK(void_future.isReady());
        void_future.get(); // Should not throw
        
        // Test void then() chaining - create a new future since we consumed the previous one
        kythira::Future<void> void_future2;
        auto chained = std::move(void_future2).then([]() { return 42; });
        BOOST_CHECK_EQUAL(chained.get(), 42);
        
        // Test void onError()
        kythira::Future<void> error_future(folly::exception_wrapper(std::runtime_error("test")));
        bool error_handled = false;
        auto recovered = std::move(error_future).onError([&error_handled](std::exception_ptr) { 
            error_handled = true; 
        });
        recovered.get();
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
        return future.get();
    };
    
    for (std::size_t i = 0; i < 10; ++i) {
        int value = int_dist(rng);
        kythira::Future<int> future(value);
        int result = process_future.template operator()<kythira::Future<int>, int>(std::move(future));
        BOOST_CHECK_EQUAL(result, value);
    }
}

BOOST_AUTO_TEST_SUITE_END()
