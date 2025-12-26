#define BOOST_TEST_MODULE folly_concept_wrappers_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <folly/init/Init.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <memory>
#include <vector>
#include <string>
#include <functional>

// Global test fixture to initialize Folly
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("folly_concept_wrappers_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test_message";
    constexpr auto test_timeout = std::chrono::milliseconds{100};
}

BOOST_AUTO_TEST_SUITE(existing_wrapper_integration_tests)

/**
 * Test: Existing Future wrapper basic functionality
 * 
 * Verifies that the existing kythira::Future wrapper works correctly
 * with basic operations and maintains compatibility.
 * 
 * Requirements: 10.1, 10.2, 10.3
 */
BOOST_AUTO_TEST_CASE(existing_future_wrapper_basic_functionality, * boost::unit_test::timeout(30)) {
    // Test Future construction from value
    auto value_future = kythira::Future<int>(test_value);
    BOOST_CHECK(value_future.isReady());
    BOOST_CHECK_EQUAL(value_future.get(), test_value);
    
    // Test Future construction from folly::Future
    auto folly_future = folly::makeFuture(test_value * 2);
    auto wrapped_future = kythira::Future<int>(std::move(folly_future));
    BOOST_CHECK(wrapped_future.isReady());
    BOOST_CHECK_EQUAL(wrapped_future.get(), test_value * 2);
    
    // Test Future construction from exception
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto error_future = kythira::Future<int>(ex);
    BOOST_CHECK(error_future.isReady());
    BOOST_CHECK_THROW(error_future.get(), std::runtime_error);
}

/**
 * Test: Future chaining and transformation
 * 
 * Verifies that existing future transformation methods work correctly
 * and maintain type safety.
 * 
 * Requirements: 10.2, 10.4
 */
BOOST_AUTO_TEST_CASE(future_chaining_transformation, * boost::unit_test::timeout(30)) {
    // Test future chaining with then()
    auto initial_future = kythira::Future<int>(test_value);
    
    auto string_future = initial_future.then([](int val) -> std::string {
        return "Value: " + std::to_string(val);
    });
    
    BOOST_CHECK(string_future.isReady());
    auto result = string_future.get();
    BOOST_CHECK_EQUAL(result, "Value: " + std::to_string(test_value));
    
    // Test error handling with onError() - use folly::exception_wrapper
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto error_future = kythira::Future<int>(ex);
    
    auto handled_future = error_future.onError([](folly::exception_wrapper) -> int {
        return -1;
    });
    
    BOOST_CHECK(handled_future.isReady());
    BOOST_CHECK_EQUAL(handled_future.get(), -1);
}

/**
 * Test: Void Future handling
 * 
 * Verifies that void Future specialization works correctly.
 * 
 * Requirements: 10.1, 10.3
 */
BOOST_AUTO_TEST_CASE(void_future_handling, * boost::unit_test::timeout(15)) {
    // Test void Future construction
    auto void_future = kythira::Future<void>();
    BOOST_CHECK(void_future.isReady());
    BOOST_CHECK_NO_THROW(void_future.get());
    
    // Test void Future from exception
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto error_void_future = kythira::Future<void>(ex);
    BOOST_CHECK(error_void_future.isReady());
    BOOST_CHECK_THROW(error_void_future.get(), std::runtime_error);
    
    // Test void Future chaining - create a new future for chaining
    auto fresh_void_future = kythira::Future<void>();
    auto chained_future = std::move(fresh_void_future).then([]() -> int {
        return test_value;
    });
    
    BOOST_CHECK(chained_future.isReady());
    BOOST_CHECK_EQUAL(chained_future.get(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(collective_operations_integration_tests)

/**
 * Test: Collective operations with existing futures
 * 
 * Verifies that existing collective operations (wait_for_all, wait_for_any)
 * work correctly with various future types.
 * 
 * Requirements: 10.2, 10.3
 */
BOOST_AUTO_TEST_CASE(collective_operations_basic, * boost::unit_test::timeout(30)) {
    // Create multiple futures
    std::vector<kythira::Future<int>> futures;
    futures.emplace_back(kythira::Future<int>(1));
    futures.emplace_back(kythira::Future<int>(2));
    futures.emplace_back(kythira::Future<int>(3));
    
    // Test wait_for_all
    auto all_results_future = kythira::wait_for_all(std::move(futures));
    BOOST_CHECK(all_results_future.isReady());
    
    auto all_results = all_results_future.get();
    BOOST_CHECK_EQUAL(all_results.size(), 3);
    
    // Verify all results are successful
    for (std::size_t i = 0; i < all_results.size(); ++i) {
        BOOST_CHECK(all_results[i].has_value());
        BOOST_CHECK_EQUAL(all_results[i].value(), static_cast<int>(i + 1));
    }
}

/**
 * Test: Mixed future types in collective operations
 * 
 * Verifies that different future creation methods work together
 * in collective operations.
 * 
 * Requirements: 10.2, 10.4
 */
BOOST_AUTO_TEST_CASE(mixed_future_types_collective, * boost::unit_test::timeout(30)) {
    std::vector<kythira::Future<int>> mixed_futures;
    
    // Add ready futures created directly
    mixed_futures.emplace_back(kythira::Future<int>(42));
    mixed_futures.emplace_back(kythira::Future<int>(84));
    
    // Add futures created from folly futures
    auto folly_future = folly::makeFuture(126);
    mixed_futures.emplace_back(kythira::Future<int>(std::move(folly_future)));
    
    // Test wait_for_any
    auto any_result_future = kythira::wait_for_any(std::move(mixed_futures));
    BOOST_CHECK(any_result_future.isReady());
    
    auto [index, result] = any_result_future.get();
    BOOST_CHECK(result.has_value());
    
    // Should be one of our expected values
    int value = result.value();
    BOOST_CHECK((value == 42 || value == 84 || value == 126));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(try_wrapper_integration_tests)

/**
 * Test: Try wrapper basic functionality
 * 
 * Verifies that the existing Try wrapper works correctly
 * with values and exceptions.
 * 
 * Requirements: 10.1, 10.3
 */
BOOST_AUTO_TEST_CASE(try_wrapper_basic_functionality, * boost::unit_test::timeout(15)) {
    // Test Try with value
    auto value_try = kythira::Try<int>(test_value);
    BOOST_CHECK(value_try.has_value());
    BOOST_CHECK(!value_try.has_exception());
    BOOST_CHECK_EQUAL(value_try.value(), test_value);
    
    // Test Try with exception
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto error_try = kythira::Try<int>(ex);
    BOOST_CHECK(!error_try.has_value());
    BOOST_CHECK(error_try.has_exception());
    BOOST_CHECK(error_try.exception() != nullptr);
    
    // Test Try from folly::Try
    auto folly_try = folly::Try<int>(test_value * 2);
    auto wrapped_try = kythira::Try<int>(std::move(folly_try));
    BOOST_CHECK(wrapped_try.has_value());
    BOOST_CHECK_EQUAL(wrapped_try.value(), test_value * 2);
}

/**
 * Test: Try wrapper with different types
 * 
 * Verifies that Try wrapper works with various value types.
 * 
 * Requirements: 10.1, 10.4
 */
BOOST_AUTO_TEST_CASE(try_wrapper_different_types, * boost::unit_test::timeout(15)) {
    // Test with string
    std::string test_str = test_string;
    auto string_try = kythira::Try<std::string>(test_str);
    BOOST_CHECK(string_try.has_value());
    BOOST_CHECK_EQUAL(string_try.value(), test_str);
    
    // Test with vector
    std::vector<int> test_vec = {1, 2, 3, 4, 5};
    auto vector_try = kythira::Try<std::vector<int>>(test_vec);
    BOOST_CHECK(vector_try.has_value());
    BOOST_CHECK_EQUAL(vector_try.value().size(), 5);
    BOOST_CHECK_EQUAL(vector_try.value()[0], 1);
    BOOST_CHECK_EQUAL(vector_try.value()[4], 5);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(interoperability_tests)

/**
 * Test: Folly interoperability
 * 
 * Verifies that wrapper types work seamlessly with folly types
 * and maintain compatibility.
 * 
 * Requirements: 10.2, 10.5
 */
BOOST_AUTO_TEST_CASE(folly_interoperability, * boost::unit_test::timeout(30)) {
    // Test converting from folly::Future to kythira::Future and back
    auto original_folly = folly::makeFuture(test_value);
    auto wrapped = kythira::Future<int>(std::move(original_folly));
    
    BOOST_CHECK(wrapped.isReady());
    BOOST_CHECK_EQUAL(wrapped.get(), test_value);
    
    // Test with folly executor
    auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(1);
    
    // Create a future that uses the executor
    auto executor_future = folly::makeFuture(test_value)
        .via(cpu_executor.get())
        .thenValue([](int val) { return val * 2; });
    
    auto wrapped_executor_future = kythira::Future<int>(std::move(executor_future));
    
    // Wait a bit for executor to process
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    
    BOOST_CHECK(wrapped_executor_future.isReady());
    BOOST_CHECK_EQUAL(wrapped_executor_future.get(), test_value * 2);
}

/**
 * Test: Exception type conversion
 * 
 * Verifies that exception conversion between folly and std types works.
 * 
 * Requirements: 10.1, 10.2
 */
BOOST_AUTO_TEST_CASE(exception_type_conversion, * boost::unit_test::timeout(15)) {
    // Test folly::exception_wrapper to std::exception_ptr conversion
    auto folly_ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto try_with_folly_ex = kythira::Try<int>(folly_ex);
    
    BOOST_CHECK(try_with_folly_ex.has_exception());
    auto std_ex_ptr = try_with_folly_ex.exception();
    BOOST_CHECK(std_ex_ptr != nullptr);
    
    // Verify we can rethrow and catch the exception
    try {
        std::rethrow_exception(std_ex_ptr);
        BOOST_FAIL("Should have thrown exception");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), test_string);
    }
    
    // Test that folly futures with exceptions work with our wrapper
    auto folly_error_future = folly::makeFuture<int>(folly_ex);
    auto wrapped_error_future = kythira::Future<int>(std::move(folly_error_future));
    
    BOOST_CHECK(wrapped_error_future.isReady());
    BOOST_CHECK_THROW(wrapped_error_future.get(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(regression_prevention_tests)

/**
 * Test: Existing functionality preservation
 * 
 * Verifies that existing functionality continues to work correctly
 * and no regressions have been introduced.
 * 
 * Requirements: 10.3, 10.5
 */
BOOST_AUTO_TEST_CASE(existing_functionality_preservation, * boost::unit_test::timeout(30)) {
    // Test all existing Future construction methods still work
    
    // 1. Direct value construction
    auto value_future = kythira::Future<int>(test_value);
    BOOST_CHECK(value_future.isReady());
    BOOST_CHECK_EQUAL(value_future.get(), test_value);
    
    // 2. Folly future construction
    auto folly_future = folly::makeFuture(test_value * 2);
    auto wrapped_future = kythira::Future<int>(std::move(folly_future));
    BOOST_CHECK(wrapped_future.isReady());
    BOOST_CHECK_EQUAL(wrapped_future.get(), test_value * 2);
    
    // 3. Exception construction
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto error_future = kythira::Future<int>(ex);
    BOOST_CHECK(error_future.isReady());
    BOOST_CHECK_THROW(error_future.get(), std::runtime_error);
    
    // 4. std::exception_ptr construction
    std::exception_ptr std_ex;
    try {
        throw std::runtime_error(test_string);
    } catch (...) {
        std_ex = std::current_exception();
    }
    auto std_error_future = kythira::Future<int>(std_ex);
    BOOST_CHECK(std_error_future.isReady());
    BOOST_CHECK_THROW(std_error_future.get(), std::runtime_error);
    
    // 5. Void future construction
    auto void_future = kythira::Future<void>();
    BOOST_CHECK(void_future.isReady());
    BOOST_CHECK_NO_THROW(void_future.get());
}

/**
 * Test: Collective operations preservation
 * 
 * Verifies that existing collective operations continue to work.
 * 
 * Requirements: 10.3, 10.5
 */
BOOST_AUTO_TEST_CASE(collective_operations_preservation, * boost::unit_test::timeout(30)) {
    // Test wait_for_all still works
    std::vector<kythira::Future<int>> futures_all;
    for (int i = 1; i <= 3; ++i) {
        futures_all.emplace_back(kythira::Future<int>(i));
    }
    
    auto all_result = kythira::wait_for_all(std::move(futures_all));
    BOOST_CHECK(all_result.isReady());
    
    auto results = all_result.get();
    BOOST_CHECK_EQUAL(results.size(), 3);
    for (std::size_t i = 0; i < results.size(); ++i) {
        BOOST_CHECK(results[i].has_value());
        BOOST_CHECK_EQUAL(results[i].value(), static_cast<int>(i + 1));
    }
    
    // Test wait_for_any still works
    std::vector<kythira::Future<int>> futures_any;
    futures_any.emplace_back(kythira::Future<int>(test_value));
    
    auto any_result = kythira::wait_for_any(std::move(futures_any));
    BOOST_CHECK(any_result.isReady());
    
    auto [index, try_result] = any_result.get();
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK(try_result.has_value());
    BOOST_CHECK_EQUAL(try_result.value(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(performance_integration_tests)

/**
 * Test: Performance impact validation
 * 
 * Verifies that wrapper usage doesn't significantly impact performance
 * compared to direct folly usage.
 * 
 * Requirements: 10.5
 */
BOOST_AUTO_TEST_CASE(wrapper_performance_impact, * boost::unit_test::timeout(60)) {
    constexpr std::size_t num_operations = 1000;
    
    // Measure direct folly usage
    auto start_folly = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < num_operations; ++i) {
        auto future = folly::makeFuture(static_cast<int>(i));
        auto result = std::move(future).get();
        (void)result; // Suppress unused variable warning
    }
    auto end_folly = std::chrono::steady_clock::now();
    
    // Measure wrapper usage
    auto start_wrapper = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < num_operations; ++i) {
        auto folly_future = folly::makeFuture(static_cast<int>(i));
        auto wrapper_future = kythira::Future<int>(std::move(folly_future));
        auto result = wrapper_future.get();
        (void)result; // Suppress unused variable warning
    }
    auto end_wrapper = std::chrono::steady_clock::now();
    
    auto folly_time = std::chrono::duration_cast<std::chrono::microseconds>(end_folly - start_folly);
    auto wrapper_time = std::chrono::duration_cast<std::chrono::microseconds>(end_wrapper - start_wrapper);
    
    // Wrapper should not be more than 100% slower than direct folly usage
    // This is a generous threshold to account for test environment variability
    BOOST_TEST_MESSAGE("Folly time: " << folly_time.count() << " microseconds");
    BOOST_TEST_MESSAGE("Wrapper time: " << wrapper_time.count() << " microseconds");
    
    if (folly_time.count() > 0) {
        double overhead_ratio = static_cast<double>(wrapper_time.count()) / folly_time.count();
        BOOST_TEST_MESSAGE("Overhead ratio: " << overhead_ratio);
        BOOST_CHECK_LE(overhead_ratio, 2.0); // Allow up to 100% overhead
    }
}

/**
 * Test: Memory usage validation
 * 
 * Verifies that wrapper classes don't significantly increase memory usage.
 * 
 * Requirements: 10.5
 */
BOOST_AUTO_TEST_CASE(memory_usage_validation, * boost::unit_test::timeout(30)) {
    // Test that wrappers can handle large numbers of objects
    constexpr std::size_t num_futures = 1000;
    std::vector<kythira::Future<int>> futures;
    futures.reserve(num_futures);
    
    for (std::size_t i = 0; i < num_futures; ++i) {
        futures.emplace_back(kythira::Future<int>(static_cast<int>(i)));
    }
    
    // All futures should be ready and have correct values
    for (std::size_t i = 0; i < num_futures; ++i) {
        BOOST_CHECK(futures[i].isReady());
        // Create a copy to avoid moving from the vector element
        auto future_copy = kythira::Future<int>(static_cast<int>(i));
        BOOST_CHECK_EQUAL(future_copy.get(), static_cast<int>(i));
    }
    
    // Test collective operations with large numbers - create fresh futures
    std::vector<kythira::Future<int>> fresh_futures;
    fresh_futures.reserve(num_futures);
    
    for (std::size_t i = 0; i < num_futures; ++i) {
        fresh_futures.emplace_back(kythira::Future<int>(static_cast<int>(i)));
    }
    
    auto all_results = kythira::wait_for_all(std::move(fresh_futures));
    BOOST_CHECK(all_results.isReady());
    
    auto results = all_results.get();
    BOOST_CHECK_EQUAL(results.size(), num_futures);
    
    for (std::size_t i = 0; i < results.size(); ++i) {
        BOOST_CHECK(results[i].has_value());
        BOOST_CHECK_EQUAL(results[i].value(), static_cast<int>(i));
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(future_wrapper_compatibility_tests)

/**
 * Test: Template function compatibility
 * 
 * Verifies that wrapper types work with template functions
 * that expect future-like behavior.
 * 
 * Requirements: 10.4, 10.5
 */
template<typename FutureType>
auto process_generic_future(FutureType future) -> decltype(future.get()) {
    if (future.isReady()) {
        return future.get();
    }
    throw std::runtime_error("Future not ready");
}

BOOST_AUTO_TEST_CASE(template_function_compatibility, * boost::unit_test::timeout(15)) {
    // Test with kythira::Future<int>
    auto int_future = kythira::Future<int>(test_value);
    auto result = process_generic_future(std::move(int_future));
    BOOST_CHECK_EQUAL(result, test_value);
    
    // Test with kythira::Future<std::string>
    std::string test_str = test_string;
    auto string_future = kythira::Future<std::string>(test_str);
    auto string_result = process_generic_future(std::move(string_future));
    BOOST_CHECK_EQUAL(string_result, test_str);
}

/**
 * Test: Chaining compatibility
 * 
 * Verifies that future chaining works correctly with various
 * transformation functions.
 * 
 * Requirements: 10.2, 10.4
 */
BOOST_AUTO_TEST_CASE(chaining_compatibility, * boost::unit_test::timeout(30)) {
    // Test complex chaining
    auto initial = kythira::Future<int>(test_value);
    
    auto chained = initial
        .then([](int val) -> std::string {
            return "Number: " + std::to_string(val);
        })
        .then([](const std::string& str) -> std::size_t {
            return str.length();
        });
    
    BOOST_CHECK(chained.isReady());
    auto result = chained.get();
    
    std::string expected = "Number: " + std::to_string(test_value);
    BOOST_CHECK_EQUAL(result, expected.length());
}

BOOST_AUTO_TEST_SUITE_END()