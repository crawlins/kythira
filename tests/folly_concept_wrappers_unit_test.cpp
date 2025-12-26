#define BOOST_TEST_MODULE folly_concept_wrappers_unit_test
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "../include/raft/future.hpp"

namespace {
    constexpr int test_value = 42;
    constexpr int test_value_2 = 84;
    constexpr const char* test_string = "test_message";
    constexpr auto test_timeout = std::chrono::milliseconds{100};
    constexpr auto short_timeout = std::chrono::milliseconds{10};
}

// ============================================================================
// Try Wrapper Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(try_wrapper_tests)

BOOST_AUTO_TEST_CASE(try_default_constructor, * boost::unit_test::timeout(15)) {
    kythira::Try<int> t;
    
    // Default constructed Try should not have value or exception
    BOOST_CHECK(!t.has_value());
    BOOST_CHECK(!t.has_exception());
}

BOOST_AUTO_TEST_CASE(try_value_constructor, * boost::unit_test::timeout(15)) {
    kythira::Try<int> t(test_value);
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK(!t.has_exception());
    BOOST_CHECK_EQUAL(t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(try_exception_constructor, * boost::unit_test::timeout(15)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Try<int> t(ex);
    
    BOOST_CHECK(!t.has_value());
    BOOST_CHECK(t.has_exception());
    
    // Accessing value should throw
    BOOST_CHECK_THROW(t.value(), std::exception);
    
    // Exception should be convertible to std::exception_ptr
    auto ex_ptr = t.exception();
    BOOST_CHECK(ex_ptr != nullptr);
}

BOOST_AUTO_TEST_CASE(try_folly_try_constructor, * boost::unit_test::timeout(15)) {
    folly::Try<int> folly_try(test_value);
    kythira::Try<int> t(std::move(folly_try));
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK_EQUAL(t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(try_const_value_access, * boost::unit_test::timeout(15)) {
    const kythira::Try<int> t(test_value);
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK_EQUAL(t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(try_string_type, * boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    kythira::Try<std::string> t(test_str);
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK_EQUAL(t.value(), test_str);
}

BOOST_AUTO_TEST_CASE(try_move_semantics, * boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    kythira::Try<std::string> t(std::move(test_str));
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK_EQUAL(t.value(), test_string);
}

BOOST_AUTO_TEST_CASE(try_folly_interop, * boost::unit_test::timeout(15)) {
    kythira::Try<int> t(test_value);
    
    // Should be able to get underlying folly::Try
    auto& folly_try = t.get_folly_try();
    BOOST_CHECK(folly_try.hasValue());
    BOOST_CHECK_EQUAL(folly_try.value(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Future Wrapper Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(future_wrapper_tests)

BOOST_AUTO_TEST_CASE(future_default_constructor, * boost::unit_test::timeout(15)) {
    // Note: folly::Future doesn't have default constructor, so we test with promise
    folly::Promise<int> promise;
    kythira::Future<int> f(promise.getFuture());
    
    // Future from unfulfilled promise should not be ready
    BOOST_CHECK(!f.isReady());
    
    // Clean up
    promise.setValue(test_value);
}

BOOST_AUTO_TEST_CASE(future_value_constructor, * boost::unit_test::timeout(15)) {
    kythira::Future<int> f(test_value);
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(f.get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_exception_constructor_folly_wrapper, * boost::unit_test::timeout(15)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> f(ex);
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_THROW(f.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(future_exception_constructor_std_ptr, * boost::unit_test::timeout(15)) {
    auto ex_ptr = std::make_exception_ptr(std::runtime_error(test_string));
    kythira::Future<int> f(ex_ptr);
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_THROW(f.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(future_folly_future_constructor, * boost::unit_test::timeout(15)) {
    auto folly_future = folly::makeFuture(test_value);
    kythira::Future<int> f(std::move(folly_future));
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(f.get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_then_chaining, * boost::unit_test::timeout(30)) {
    kythira::Future<int> f(test_value);
    
    auto f2 = f.then([](int val) { return val * 2; });
    
    BOOST_CHECK_EQUAL(f2.get(), test_value * 2);
}

BOOST_AUTO_TEST_CASE(future_then_void_return, * boost::unit_test::timeout(30)) {
    kythira::Future<int> f(test_value);
    
    bool callback_called = false;
    auto f2 = f.then([&callback_called](int val) { 
        callback_called = true; 
    });
    
    f2.get(); // Should not throw
    BOOST_CHECK(callback_called);
}

BOOST_AUTO_TEST_CASE(future_on_error_handling, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> f(ex);
    
    auto f2 = f.onError([](folly::exception_wrapper) { return test_value; });
    
    BOOST_CHECK_EQUAL(f2.get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_wait_timeout, * boost::unit_test::timeout(60)) {
    folly::Promise<int> promise;
    kythira::Future<int> f(promise.getFuture());
    
    // Should not be ready yet
    BOOST_CHECK(!f.isReady());
    
    // Wait with short timeout should return false
    BOOST_CHECK(!f.wait(short_timeout));
    
    // Fulfill the promise
    promise.setValue(test_value);
    
    // Now should be ready
    BOOST_CHECK(f.wait(test_timeout));
    BOOST_CHECK(f.isReady());
}

BOOST_AUTO_TEST_CASE(future_string_type, * boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    kythira::Future<std::string> f(test_str);
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(f.get(), test_str);
}

BOOST_AUTO_TEST_CASE(future_move_semantics, * boost::unit_test::timeout(15)) {
    std::string test_str(test_string);
    kythira::Future<std::string> f(std::move(test_str));
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_EQUAL(f.get(), test_string);
}

BOOST_AUTO_TEST_CASE(future_folly_interop, * boost::unit_test::timeout(15)) {
    kythira::Future<int> f(test_value);
    
    // Should be able to get underlying folly::Future
    auto folly_future = std::move(f).get_folly_future();
    BOOST_CHECK(folly_future.isReady());
    BOOST_CHECK_EQUAL(std::move(folly_future).get(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Future<void> Specialization Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(future_void_tests)

BOOST_AUTO_TEST_CASE(future_void_default_constructor, * boost::unit_test::timeout(15)) {
    kythira::Future<void> f;
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_NO_THROW(f.get());
}

BOOST_AUTO_TEST_CASE(future_void_exception_constructor, * boost::unit_test::timeout(15)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<void> f(ex);
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_THROW(f.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(future_void_folly_constructor, * boost::unit_test::timeout(15)) {
    auto folly_future = folly::makeFuture();
    kythira::Future<void> f(std::move(folly_future));
    
    BOOST_CHECK(f.isReady());
    BOOST_CHECK_NO_THROW(f.get());
}

BOOST_AUTO_TEST_CASE(future_void_then_void_return, * boost::unit_test::timeout(30)) {
    kythira::Future<void> f;
    
    bool callback_called = false;
    auto f2 = f.then([&callback_called]() { 
        callback_called = true; 
    });
    
    BOOST_CHECK_NO_THROW(f2.get());
    BOOST_CHECK(callback_called);
}

BOOST_AUTO_TEST_CASE(future_void_then_value_return, * boost::unit_test::timeout(30)) {
    kythira::Future<void> f;
    
    auto f2 = f.then([]() { return test_value; });
    
    BOOST_CHECK_EQUAL(f2.get(), test_value);
}

BOOST_AUTO_TEST_CASE(future_void_on_error, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<void> f(ex);
    
    bool error_handled = false;
    auto f2 = f.onError([&error_handled](std::exception_ptr) { 
        error_handled = true; 
    });
    
    BOOST_CHECK_NO_THROW(f2.get());
    BOOST_CHECK(error_handled);
}

BOOST_AUTO_TEST_CASE(future_void_wait_timeout, * boost::unit_test::timeout(60)) {
    folly::Promise<folly::Unit> promise;
    kythira::Future<void> f(promise.getFuture());
    
    // Should not be ready yet
    BOOST_CHECK(!f.isReady());
    
    // Wait with short timeout should return false
    BOOST_CHECK(!f.wait(short_timeout));
    
    // Fulfill the promise
    promise.setValue(folly::Unit{});
    
    // Now should be ready
    BOOST_CHECK(f.wait(test_timeout));
    BOOST_CHECK(f.isReady());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Collective Operations Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(collective_operations_tests)

BOOST_AUTO_TEST_CASE(wait_for_any_basic, * boost::unit_test::timeout(90)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::Future<int>(promise1.getFuture()));
    futures.push_back(kythira::Future<int>(promise2.getFuture()));
    futures.push_back(kythira::Future<int>(promise3.getFuture()));
    
    // Fulfill the second promise in a separate thread
    std::thread t([&promise2]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        promise2.setValue(test_value);
    });
    
    // Wait for any future to complete
    auto result_future = kythira::wait_for_any(std::move(futures));
    auto [index, try_result] = result_future.get();
    
    // Should be the second future (index 1)
    BOOST_CHECK_EQUAL(index, 1);
    BOOST_CHECK(try_result.has_value());
    BOOST_CHECK_EQUAL(try_result.value(), test_value);
    
    t.join();
    
    // Clean up remaining promises
    promise1.setValue(0);
    promise3.setValue(0);
}

BOOST_AUTO_TEST_CASE(wait_for_any_with_exception, * boost::unit_test::timeout(90)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    
    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::Future<int>(promise1.getFuture()));
    futures.push_back(kythira::Future<int>(promise2.getFuture()));
    
    // Fulfill the first promise with exception in a separate thread
    std::thread t([&promise1]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        promise1.setException(std::runtime_error(test_string));
    });
    
    // Wait for any future to complete
    auto result_future = kythira::wait_for_any(std::move(futures));
    auto [index, try_result] = result_future.get();
    
    // Should be the first future (index 0) with exception
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK(!try_result.has_value());
    BOOST_CHECK(try_result.has_exception());
    
    t.join();
    
    // Clean up remaining promise
    promise2.setValue(0);
}

BOOST_AUTO_TEST_CASE(wait_for_all_basic, * boost::unit_test::timeout(90)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::Future<int>(promise1.getFuture()));
    futures.push_back(kythira::Future<int>(promise2.getFuture()));
    futures.push_back(kythira::Future<int>(promise3.getFuture()));
    
    // Fulfill all promises in separate threads
    std::thread t1([&promise1]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        promise1.setValue(1);
    });
    
    std::thread t2([&promise2]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        promise2.setValue(2);
    });
    
    std::thread t3([&promise3]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        promise3.setValue(3);
    });
    
    // Wait for all futures to complete
    auto result_future = kythira::wait_for_all(std::move(futures));
    auto results = result_future.get();
    
    // Should have 3 results in order
    BOOST_CHECK_EQUAL(results.size(), 3);
    
    // All should have values
    BOOST_CHECK(results[0].has_value());
    BOOST_CHECK(results[1].has_value());
    BOOST_CHECK(results[2].has_value());
    
    // Check values (should preserve order)
    BOOST_CHECK_EQUAL(results[0].value(), 1);
    BOOST_CHECK_EQUAL(results[1].value(), 2);
    BOOST_CHECK_EQUAL(results[2].value(), 3);
    
    t1.join();
    t2.join();
    t3.join();
}

BOOST_AUTO_TEST_CASE(wait_for_all_with_mixed_results, * boost::unit_test::timeout(60)) {
    folly::Promise<int> promise1;
    folly::Promise<int> promise2;
    folly::Promise<int> promise3;
    
    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::Future<int>(promise1.getFuture()));
    futures.push_back(kythira::Future<int>(promise2.getFuture()));
    futures.push_back(kythira::Future<int>(promise3.getFuture()));
    
    // Fulfill promises with mix of values and exceptions
    promise1.setValue(test_value);
    promise2.setException(std::runtime_error(test_string));
    promise3.setValue(test_value_2);
    
    // Wait for all futures to complete
    auto result_future = kythira::wait_for_all(std::move(futures));
    auto results = result_future.get();
    
    // Should have 3 results
    BOOST_CHECK_EQUAL(results.size(), 3);
    
    // First should have value
    BOOST_CHECK(results[0].has_value());
    BOOST_CHECK_EQUAL(results[0].value(), test_value);
    
    // Second should have exception
    BOOST_CHECK(!results[1].has_value());
    BOOST_CHECK(results[1].has_exception());
    
    // Third should have value
    BOOST_CHECK(results[2].has_value());
    BOOST_CHECK_EQUAL(results[2].value(), test_value_2);
}

BOOST_AUTO_TEST_CASE(wait_for_all_empty_vector, * boost::unit_test::timeout(30)) {
    std::vector<kythira::Future<int>> futures;
    
    auto result_future = kythira::wait_for_all(std::move(futures));
    auto results = result_future.get();
    
    BOOST_CHECK(results.empty());
}

BOOST_AUTO_TEST_CASE(wait_for_any_single_future, * boost::unit_test::timeout(30)) {
    std::vector<kythira::Future<int>> futures;
    futures.push_back(kythira::Future<int>(test_value));
    
    auto result_future = kythira::wait_for_any(std::move(futures));
    auto [index, try_result] = result_future.get();
    
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK(try_result.has_value());
    BOOST_CHECK_EQUAL(try_result.value(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Edge Cases and Error Conditions Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(edge_cases_tests)

BOOST_AUTO_TEST_CASE(try_exception_ptr_conversion, * boost::unit_test::timeout(15)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Try<int> t(ex);
    
    auto ex_ptr = t.exception();
    BOOST_CHECK(ex_ptr != nullptr);
    
    // Should be able to rethrow
    try {
        std::rethrow_exception(ex_ptr);
        BOOST_FAIL("Should have thrown exception");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), test_string);
    }
}

BOOST_AUTO_TEST_CASE(future_chaining_with_exceptions, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> f(ex);
    
    // Exception should propagate through then chain
    auto f2 = f.then([](int val) { return val * 2; });
    
    BOOST_CHECK_THROW(f2.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(future_void_exception_propagation, * boost::unit_test::timeout(30)) {
    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<void> f(ex);
    
    // Exception should propagate through then chain
    auto f2 = f.then([]() { return test_value; });
    
    BOOST_CHECK_THROW(f2.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(large_value_types, * boost::unit_test::timeout(30)) {
    std::vector<int> large_vector(1000, test_value);
    kythira::Future<std::vector<int>> f(std::move(large_vector));
    
    BOOST_CHECK(f.isReady());
    auto result = f.get();
    BOOST_CHECK_EQUAL(result.size(), 1000);
    BOOST_CHECK_EQUAL(result[0], test_value);
    BOOST_CHECK_EQUAL(result[999], test_value);
}

BOOST_AUTO_TEST_CASE(nested_future_types, * boost::unit_test::timeout(30)) {
    kythira::Future<int> inner_future(test_value);
    kythira::Future<kythira::Future<int>> outer_future(std::move(inner_future));
    
    BOOST_CHECK(outer_future.isReady());
    auto inner = outer_future.get();
    BOOST_CHECK(inner.isReady());
    BOOST_CHECK_EQUAL(inner.get(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Resource Management Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(resource_management_tests)

BOOST_AUTO_TEST_CASE(try_move_only_types, * boost::unit_test::timeout(15)) {
    auto unique_ptr = std::make_unique<int>(test_value);
    kythira::Try<std::unique_ptr<int>> t(std::move(unique_ptr));
    
    BOOST_CHECK(t.has_value());
    BOOST_CHECK(t.value() != nullptr);
    BOOST_CHECK_EQUAL(*t.value(), test_value);
}

BOOST_AUTO_TEST_CASE(future_move_only_types, * boost::unit_test::timeout(15)) {
    auto unique_ptr = std::make_unique<int>(test_value);
    kythira::Future<std::unique_ptr<int>> f(std::move(unique_ptr));
    
    BOOST_CHECK(f.isReady());
    auto result = f.get();
    BOOST_CHECK(result != nullptr);
    BOOST_CHECK_EQUAL(*result, test_value);
}

BOOST_AUTO_TEST_CASE(future_rvalue_reference_handling, * boost::unit_test::timeout(15)) {
    kythira::Future<int> f(test_value);
    
    // Moving future should work
    auto folly_future = std::move(f).get_folly_future();
    BOOST_CHECK(folly_future.isReady());
    BOOST_CHECK_EQUAL(std::move(folly_future).get(), test_value);
}

BOOST_AUTO_TEST_CASE(exception_safety_in_constructors, * boost::unit_test::timeout(15)) {
    // Test that exceptions during construction are handled properly
    struct ThrowingType {
        ThrowingType() { throw std::runtime_error(test_string); }
    };
    
    // Note: kythira::Try constructor doesn't directly construct the value,
    // it wraps folly::Try which handles exceptions differently
    // This test documents the current behavior
    try {
        auto ex_wrapper = folly::exception_wrapper(std::runtime_error(test_string));
        folly::Try<ThrowingType> folly_try{ex_wrapper};
        kythira::Try<ThrowingType> t(std::move(folly_try));
        BOOST_CHECK(t.has_exception());
    } catch (...) {
        BOOST_FAIL("Unexpected exception during Try construction");
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Performance and Boundary Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(performance_tests)

BOOST_AUTO_TEST_CASE(many_futures_creation, * boost::unit_test::timeout(60)) {
    constexpr std::size_t num_futures = 1000;
    std::vector<kythira::Future<int>> futures;
    futures.reserve(num_futures);
    
    // Create many futures
    for (std::size_t i = 0; i < num_futures; ++i) {
        futures.emplace_back(static_cast<int>(i));
    }
    
    // Verify all are ready and have correct values
    for (std::size_t i = 0; i < num_futures; ++i) {
        BOOST_CHECK(futures[i].isReady());
        BOOST_CHECK_EQUAL(futures[i].get(), static_cast<int>(i));
    }
}

BOOST_AUTO_TEST_CASE(deep_then_chaining, * boost::unit_test::timeout(60)) {
    kythira::Future<int> f(1);
    
    // Chain many then operations
    for (int i = 0; i < 100; ++i) {
        f = f.then([](int val) { return val + 1; });
    }
    
    BOOST_CHECK_EQUAL(f.get(), 101);
}

BOOST_AUTO_TEST_CASE(concurrent_future_access, * boost::unit_test::timeout(90)) {
    constexpr int num_threads = 4;
    constexpr int operations_per_thread = 100;
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&success_count, operations_per_thread]() {
            for (int i = 0; i < operations_per_thread; ++i) {
                kythira::Future<int> f(i);
                if (f.isReady() && f.get() == i) {
                    success_count.fetch_add(1);
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    BOOST_CHECK_EQUAL(success_count.load(), num_threads * operations_per_thread);
}

BOOST_AUTO_TEST_SUITE_END()