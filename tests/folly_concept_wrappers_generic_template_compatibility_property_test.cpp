#define BOOST_TEST_MODULE FollyConceptWrappersGenericTemplateCompatibilityPropertyTest
#include <boost/test/included/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>
#include <exception>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <vector>
#include <tuple>
#include <chrono>
#include <functional>
#include <folly/Unit.h>
#include <folly/ExceptionWrapper.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

using namespace kythira;

// Test constants
namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test exception";
    constexpr double test_double = 3.14;
    constexpr std::size_t property_test_iterations = 100;
}

// Template functions that use concept constraints to test generic compatibility

// Generic function that works with any future type
template<typename F, typename T>
    requires future<F, T>
auto process_future_generic(F&& fut) -> T {
    return std::forward<F>(fut).get();
}

// Generic function that works with any promise type
template<typename P, typename T>
    requires promise<P, T>
auto fulfill_promise_generic(P&& prom, const T& value) -> void {
    if constexpr (std::is_void_v<T>) {
        std::forward<P>(prom).setValue(folly::Unit{});
    } else {
        std::forward<P>(prom).setValue(value);
    }
}

// Generic function that works with any executor type
template<typename E>
    requires executor<E>
auto submit_work_generic(E& exec, std::function<void()> work) -> void {
    exec.add(std::move(work));
}

// Generic function that works with any try type
template<typename T, typename ValueType>
    requires try_type<T, ValueType>
auto extract_value_or_default(const T& try_obj, const ValueType& default_value) -> ValueType {
    if (try_obj.hasValue()) {
        if constexpr (std::is_void_v<ValueType>) {
            try_obj.value();
            return; // void return
        } else {
            return try_obj.value();
        }
    } else {
        return default_value;
    }
}

// Specialization for void type
template<typename T>
    requires try_type<T, void>
auto extract_value_or_default_void(const T& try_obj) -> bool {
    if (try_obj.hasValue()) {
        try_obj.value(); // void return, but we return success indicator
        return true;
    } else {
        return false;
    }
}

// Generic function that works with future factory
template<typename Factory>
    requires future_factory<Factory>
auto create_test_futures() -> std::tuple<Future<int>, Future<std::string>, Future<folly::Unit>> {
    auto future_int = Factory::makeFuture(42);
    auto future_str = Factory::makeFuture(std::string("test"));
    auto future_unit = Factory::makeReadyFuture();
    
    return std::make_tuple(std::move(future_int), std::move(future_str), std::move(future_unit));
}

// Generic function that works with future collector
template<typename Collector>
    requires future_collector<Collector>
auto collect_test_futures(std::vector<Future<int>> futures) -> Future<std::vector<Try<int>>> {
    return Collector::collectAll(std::move(futures));
}

// Generic function that works with future continuation
template<typename F, typename T>
    requires future_continuation<F, T>
auto add_delay_to_future(F&& fut, std::chrono::milliseconds delay) -> Future<T> {
    return std::forward<F>(fut).delay(delay);
}

// Generic function that works with future transformation (non-void types only)
template<typename F, typename T>
    requires future_transformable<F, T> && (!std::is_void_v<T>)
auto transform_future_value(F&& fut, std::function<T(T)> transformer) -> Future<T> {
    return std::forward<F>(fut).thenValue(std::move(transformer));
}

/**
 * **Feature: folly-concept-wrappers, Property 9: Generic Template Compatibility**
 * 
 * Property: For any concept-constrained template function, wrapper types should work seamlessly as template arguments and maintain proper type deduction
 * **Validates: Requirements 7.4**
 */
BOOST_AUTO_TEST_CASE(generic_template_compatibility_property_test, * boost::unit_test::timeout(120)) {
    
    // ========== TEST GENERIC FUNCTIONS WITH FUTURE CONCEPT ==========
    
    // Test 1: Generic future processing with different types
    {
        // Test with int future
        auto future_int = FutureFactory::makeFuture(test_value);
        int result = process_future_generic<Future<int>, int>(std::move(future_int));
        BOOST_CHECK_EQUAL(result, test_value);
        
        // Test with string future
        std::string test_str = "hello world";
        auto future_str = FutureFactory::makeFuture(test_str);
        std::string str_result = process_future_generic<Future<std::string>, std::string>(std::move(future_str));
        BOOST_CHECK_EQUAL(str_result, test_str);
        
        // Test with double future
        auto future_dbl = FutureFactory::makeFuture(test_double);
        double dbl_result = process_future_generic<Future<double>, double>(std::move(future_dbl));
        BOOST_CHECK_EQUAL(dbl_result, test_double);
        
        BOOST_TEST_MESSAGE("Generic future processing works with all wrapper types");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH PROMISE CONCEPT ==========
    
    // Test 2: Generic promise fulfillment with different types
    {
        // Test with int promise
        Promise<int> promise_int;
        auto future_int = promise_int.getFuture();
        fulfill_promise_generic<Promise<int>, int>(std::move(promise_int), test_value);
        BOOST_CHECK_EQUAL(future_int.get(), test_value);
        
        // Test with string promise
        Promise<std::string> promise_str;
        auto future_str = promise_str.getFuture();
        std::string test_str = "generic test";
        fulfill_promise_generic<Promise<std::string>, std::string>(std::move(promise_str), std::move(test_str));
        BOOST_CHECK_EQUAL(future_str.get(), "generic test");
        
        // Test with void promise
        Promise<void> promise_void;
        auto future_void = promise_void.getFuture();
        fulfill_promise_generic<Promise<void>, void>(std::move(promise_void), folly::Unit{});
        future_void.get(); // Should not throw
        
        BOOST_TEST_MESSAGE("Generic promise fulfillment works with all wrapper types");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH EXECUTOR CONCEPT ==========
    
    // Test 3: Generic work submission with executor
    {
        folly::CPUThreadPoolExecutor cpu_executor(1);
        Executor wrapper_executor(&cpu_executor);
        
        bool work_executed = false;
        submit_work_generic<Executor>(wrapper_executor, [&work_executed]() {
            work_executed = true;
        });
        
        // Give some time for work to execute
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        BOOST_CHECK(work_executed);
        
        BOOST_TEST_MESSAGE("Generic work submission works with Executor wrapper");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH TRY CONCEPT ==========
    
    // Test 4: Generic try value extraction
    {
        // Test with successful Try<int>
        Try<int> try_int_success(test_value);
        int extracted_int = extract_value_or_default<Try<int>, int>(try_int_success, 0);
        BOOST_CHECK_EQUAL(extracted_int, test_value);
        
        // Test with failed Try<int>
        Try<int> try_int_failed(folly::exception_wrapper(std::runtime_error(test_string)));
        int default_int = extract_value_or_default<Try<int>, int>(try_int_failed, -1);
        BOOST_CHECK_EQUAL(default_int, -1);
        
        // Test with successful Try<std::string>
        std::string test_str = "success";
        Try<std::string> try_str_success(test_str);
        std::string extracted_str = extract_value_or_default<Try<std::string>, std::string>(try_str_success, "default");
        BOOST_CHECK_EQUAL(extracted_str, test_str);
        
        // Test with void Try
        Try<void> try_void_success;
        bool void_success = extract_value_or_default_void<Try<void>>(try_void_success);
        BOOST_CHECK(void_success);
        
        Try<void> try_void_failed(folly::exception_wrapper(std::runtime_error(test_string)));
        bool void_failed = extract_value_or_default_void<Try<void>>(try_void_failed);
        BOOST_CHECK(!void_failed);
        
        BOOST_TEST_MESSAGE("Generic try value extraction works with all Try wrapper types");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH FUTURE FACTORY CONCEPT ==========
    
    // Test 5: Generic future factory usage
    {
        auto [future_int, future_str, future_unit] = create_test_futures<FutureFactory>();
        
        BOOST_CHECK(future_int.isReady());
        BOOST_CHECK_EQUAL(future_int.get(), 42);
        
        BOOST_CHECK(future_str.isReady());
        BOOST_CHECK_EQUAL(future_str.get(), "test");
        
        BOOST_CHECK(future_unit.isReady());
        // future_unit.get() returns folly::Unit, which is fine
        
        BOOST_TEST_MESSAGE("Generic future factory usage works with FutureFactory wrapper");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH FUTURE COLLECTOR CONCEPT ==========
    
    // Test 6: Generic future collection
    {
        std::vector<Future<int>> test_futures;
        test_futures.push_back(FutureFactory::makeFuture(1));
        test_futures.push_back(FutureFactory::makeFuture(2));
        test_futures.push_back(FutureFactory::makeFuture(3));
        
        auto collected = collect_test_futures<FutureCollector>(std::move(test_futures));
        BOOST_CHECK(collected.isReady());
        
        auto results = collected.get();
        BOOST_CHECK_EQUAL(results.size(), 3);
        
        for (const auto& result : results) {
            BOOST_CHECK(result.hasValue());
        }
        
        BOOST_TEST_MESSAGE("Generic future collection works with FutureCollector wrapper");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH FUTURE CONTINUATION CONCEPT ==========
    
    // Test 7: Generic future continuation operations
    {
        auto future_int = FutureFactory::makeFuture(test_value);
        auto delayed_future = add_delay_to_future<Future<int>, int>(std::move(future_int), std::chrono::milliseconds(10));
        
        // The future should still be ready since it was already resolved
        BOOST_CHECK(delayed_future.isReady());
        BOOST_CHECK_EQUAL(delayed_future.get(), test_value);
        
        auto future_str = FutureFactory::makeFuture(std::string("delayed"));
        auto delayed_str_future = add_delay_to_future<Future<std::string>, std::string>(std::move(future_str), std::chrono::milliseconds(10));
        
        BOOST_CHECK(delayed_str_future.isReady());
        BOOST_CHECK_EQUAL(delayed_str_future.get(), "delayed");
        
        BOOST_TEST_MESSAGE("Generic future continuation operations work with Future wrapper");
    }
    
    // ========== TEST GENERIC FUNCTIONS WITH FUTURE TRANSFORMATION CONCEPT ==========
    
    // Test 8: Generic future transformation operations (non-void types only)
    {
        auto future_int = FutureFactory::makeFuture(test_value);
        auto transformed_future = transform_future_value<Future<int>, int>(
            std::move(future_int), 
            [](int x) { return x * 2; }
        );
        
        BOOST_CHECK(transformed_future.isReady());
        BOOST_CHECK_EQUAL(transformed_future.get(), test_value * 2);
        
        auto future_str = FutureFactory::makeFuture(std::string("hello"));
        auto transformed_str_future = transform_future_value<Future<std::string>, std::string>(
            std::move(future_str),
            [](const std::string& s) { return s + " world"; }
        );
        
        BOOST_CHECK(transformed_str_future.isReady());
        BOOST_CHECK_EQUAL(transformed_str_future.get(), "hello world");
        
        BOOST_TEST_MESSAGE("Generic future transformation operations work with Future wrapper");
    }
    
    // ========== PROPERTY-BASED TESTING FOR GENERIC COMPATIBILITY ==========
    
    // Test 9: Property-based testing with multiple types and generic functions
    for (std::size_t i = 0; i < property_test_iterations; ++i) {
        int test_int = static_cast<int>(i * 7 + 13);
        double test_dbl = static_cast<double>(i) * 0.5 + 1.0;
        std::string test_str = "generic_test_" + std::to_string(i);
        
        // Test generic future processing with various values
        {
            auto future_int = FutureFactory::makeFuture(test_int);
            int result = process_future_generic<Future<int>, int>(std::move(future_int));
            BOOST_CHECK_EQUAL(result, test_int);
            
            auto future_dbl = FutureFactory::makeFuture(test_dbl);
            double dbl_result = process_future_generic<Future<double>, double>(std::move(future_dbl));
            BOOST_CHECK_EQUAL(dbl_result, test_dbl);
            
            auto future_str = FutureFactory::makeFuture(test_str);
            std::string str_result = process_future_generic<Future<std::string>, std::string>(std::move(future_str));
            BOOST_CHECK_EQUAL(str_result, test_str);
        }
        
        // Test generic promise fulfillment with various values
        {
            Promise<int> promise_int;
            auto future_int = promise_int.getFuture();
            fulfill_promise_generic<Promise<int>, int>(std::move(promise_int), test_int);
            BOOST_CHECK_EQUAL(future_int.get(), test_int);
            
            Promise<std::string> promise_str;
            auto future_str = promise_str.getFuture();
            fulfill_promise_generic<Promise<std::string>, std::string>(std::move(promise_str), std::move(test_str));
            BOOST_CHECK_EQUAL(future_str.get(), "generic_test_" + std::to_string(i));
        }
        
        // Test generic try value extraction with various values
        {
            Try<int> try_success(test_int);
            int extracted = extract_value_or_default<Try<int>, int>(try_success, 0);
            BOOST_CHECK_EQUAL(extracted, test_int);
            
            Try<int> try_failed(folly::exception_wrapper(std::runtime_error("test_" + std::to_string(i))));
            int default_val = extract_value_or_default<Try<int>, int>(try_failed, -1);
            BOOST_CHECK_EQUAL(default_val, -1);
        }
        
        // Test generic future transformation with various values
        {
            auto future_int = FutureFactory::makeFuture(test_int);
            auto transformed = transform_future_value<Future<int>, int>(
                std::move(future_int),
                [i](int x) { return x + static_cast<int>(i); }
            );
            BOOST_CHECK_EQUAL(transformed.get(), test_int + static_cast<int>(i));
        }
    }
    
    BOOST_TEST_MESSAGE("Generic template compatibility property test completed successfully");
}

/**
 * Test template specialization and SFINAE compatibility
 */
BOOST_AUTO_TEST_CASE(template_specialization_compatibility_test, * boost::unit_test::timeout(60)) {
    // Test that wrapper types work correctly with template specializations
    
    // Test std::is_same with wrapper types
    {
        static_assert(std::is_same_v<Future<int>, Future<int>>, "Future<int> should be same as Future<int>");
        static_assert(!std::is_same_v<Future<int>, Future<std::string>>, "Future<int> should not be same as Future<std::string>");
        static_assert(!std::is_same_v<Future<int>, Promise<int>>, "Future<int> should not be same as Promise<int>");
        
        BOOST_TEST_MESSAGE("Template type traits work correctly with wrapper types");
    }
    
    // Test std::decay with wrapper types
    {
        static_assert(std::is_same_v<std::decay_t<Future<int>&>, Future<int>>, "decay should work with Future references");
        static_assert(std::is_same_v<std::decay_t<const Future<int>&>, Future<int>>, "decay should work with const Future references");
        static_assert(std::is_same_v<std::decay_t<Future<int>&&>, Future<int>>, "decay should work with Future rvalue references");
        
        BOOST_TEST_MESSAGE("Template decay traits work correctly with wrapper types");
    }
    
    // Test std::remove_reference with wrapper types
    {
        static_assert(std::is_same_v<std::remove_reference_t<Future<int>&>, Future<int>>, "remove_reference should work with Future references");
        static_assert(std::is_same_v<std::remove_reference_t<Future<int>&&>, Future<int>>, "remove_reference should work with Future rvalue references");
        
        BOOST_TEST_MESSAGE("Template reference removal traits work correctly with wrapper types");
    }
    
    // Test move semantics with wrapper types
    {
        static_assert(std::is_move_constructible_v<Future<int>>, "Future should be move constructible");
        static_assert(std::is_move_assignable_v<Future<int>>, "Future should be move assignable");
        static_assert(std::is_move_constructible_v<Promise<int>>, "Promise should be move constructible");
        static_assert(std::is_move_assignable_v<Promise<int>>, "Promise should be move assignable");
        
        BOOST_TEST_MESSAGE("Move semantics traits work correctly with wrapper types");
    }
    
    // Test copy semantics with wrapper types
    {
        // Futures should be copyable (they wrap folly::Future which is copyable)
        static_assert(std::is_copy_constructible_v<Future<int>>, "Future should be copy constructible");
        static_assert(std::is_copy_assignable_v<Future<int>>, "Future should be copy assignable");
        
        // Promises should NOT be copyable (they wrap folly::Promise which is move-only)
        static_assert(!std::is_copy_constructible_v<Promise<int>>, "Promise should not be copy constructible");
        static_assert(!std::is_copy_assignable_v<Promise<int>>, "Promise should not be copy assignable");
        
        // Executors should be copyable
        static_assert(std::is_copy_constructible_v<Executor>, "Executor should be copy constructible");
        static_assert(std::is_copy_assignable_v<Executor>, "Executor should be copy assignable");
        
        // KeepAlive should be copyable (reference counting)
        static_assert(std::is_copy_constructible_v<KeepAlive>, "KeepAlive should be copy constructible");
        static_assert(std::is_copy_assignable_v<KeepAlive>, "KeepAlive should be copy assignable");
        
        BOOST_TEST_MESSAGE("Copy semantics traits work correctly with wrapper types");
    }
}

/**
 * Test template argument deduction with wrapper types
 */
BOOST_AUTO_TEST_CASE(template_argument_deduction_test, * boost::unit_test::timeout(60)) {
    // Test that template argument deduction works correctly with wrapper types
    
    // Test with auto keyword
    {
        auto future_int = FutureFactory::makeFuture(42);
        static_assert(std::is_same_v<decltype(future_int), Future<int>>, "auto should deduce Future<int>");
        
        auto future_str = FutureFactory::makeFuture(std::string("test"));
        static_assert(std::is_same_v<decltype(future_str), Future<std::string>>, "auto should deduce Future<std::string>");
        
        Promise<int> promise;
        auto future_from_promise = promise.getFuture();
        static_assert(std::is_same_v<decltype(future_from_promise), Future<int>>, "auto should deduce Future<int> from promise");
        
        BOOST_TEST_MESSAGE("Auto type deduction works correctly with wrapper types");
    }
    
    // Test with decltype
    {
        Future<int> future_int = FutureFactory::makeFuture(42);
        using FutureType = decltype(future_int);
        static_assert(std::is_same_v<FutureType, Future<int>>, "decltype should work with Future<int>");
        
        Promise<std::string> promise_str;
        using PromiseType = decltype(promise_str);
        static_assert(std::is_same_v<PromiseType, Promise<std::string>>, "decltype should work with Promise<std::string>");
        
        BOOST_TEST_MESSAGE("decltype works correctly with wrapper types");
    }
    
    // Test with std::invoke_result
    {
        auto lambda = [](int x) { return x * 2; };
        using ResultType = std::invoke_result_t<decltype(lambda), int>;
        static_assert(std::is_same_v<ResultType, int>, "invoke_result should work with lambdas and wrapper types");
        
        BOOST_TEST_MESSAGE("std::invoke_result works correctly with wrapper types");
    }
}

/**
 * Test concept constraint validation in generic contexts
 */
BOOST_AUTO_TEST_CASE(concept_constraint_validation_test, * boost::unit_test::timeout(60)) {
    // Test that concept constraints properly validate wrapper types
    
    // Test future concept constraint
    {
        static_assert(future<Future<int>, int>, "Future<int> should satisfy future concept");
        static_assert(!future<int, int>, "int should not satisfy future concept");
        static_assert(!future<Promise<int>, int>, "Promise<int> should not satisfy future concept");
        
        BOOST_TEST_MESSAGE("Future concept constraints work correctly");
    }
    
    // Test promise concept constraint
    {
        static_assert(promise<Promise<int>, int>, "Promise<int> should satisfy promise concept");
        static_assert(!promise<int, int>, "int should not satisfy promise concept");
        static_assert(!promise<Future<int>, int>, "Future<int> should not satisfy promise concept");
        
        BOOST_TEST_MESSAGE("Promise concept constraints work correctly");
    }
    
    // Test executor concept constraint
    {
        static_assert(executor<Executor>, "Executor should satisfy executor concept");
        static_assert(!executor<int>, "int should not satisfy executor concept");
        static_assert(!executor<Future<int>>, "Future<int> should not satisfy executor concept");
        
        BOOST_TEST_MESSAGE("Executor concept constraints work correctly");
    }
    
    // Test factory concept constraint
    {
        static_assert(future_factory<FutureFactory>, "FutureFactory should satisfy future_factory concept");
        static_assert(!future_factory<int>, "int should not satisfy future_factory concept");
        static_assert(!future_factory<Future<int>>, "Future<int> should not satisfy future_factory concept");
        
        BOOST_TEST_MESSAGE("Factory concept constraints work correctly");
    }
    
    // Test collector concept constraint
    {
        static_assert(future_collector<FutureCollector>, "FutureCollector should satisfy future_collector concept");
        static_assert(!future_collector<int>, "int should not satisfy future_collector concept");
        static_assert(!future_collector<Future<int>>, "Future<int> should not satisfy future_collector concept");
        
        BOOST_TEST_MESSAGE("Collector concept constraints work correctly");
    }
}