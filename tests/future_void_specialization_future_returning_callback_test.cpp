/**
 * @file future_void_specialization_future_returning_callback_test.cpp
 * @brief Comprehensive tests for void specialization with Future-returning callbacks
 * 
 * This test validates that Future<void> properly handles Future-returning callbacks
 * in both thenTry and thenError methods, with proper Unit/void conversions.
 * 
 * **Validates: Requirements 30.5, 31.5**
 */

#define BOOST_TEST_MODULE future_void_specialization_future_returning_callback_test
#include <boost/test/unit_test.hpp>
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <chrono>
#include <atomic>
#include <string>

using namespace kythira;

namespace {
    constexpr std::chrono::milliseconds short_delay{10};
    constexpr std::chrono::milliseconds medium_delay{50};
}

// Global fixture to initialize Folly once for all tests
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("future_void_specialization_future_returning_callback_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    
    ~FollyInitFixture() = default;
    
    std::unique_ptr<folly::Init> _init;
};

BOOST_GLOBAL_FIXTURE(FollyInitFixture);

BOOST_AUTO_TEST_SUITE(future_void_specialization_tests)

/**
 * Test 1: Future<void>::thenTry with Future<void>-returning callback
 * 
 * Validates that thenTry on Future<void> can accept callbacks that return Future<void>
 * and properly flatten the result.
 */
BOOST_AUTO_TEST_CASE(test_void_then_try_returns_void_future, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    // Create void future and chain with thenTry returning Future<void>
    FutureFactory::makeFuture()
        .thenTry([&counter](Try<void> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            counter++;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
}

/**
 * Test 2: Future<void>::thenTry with Future<int>-returning callback
 * 
 * Validates that thenTry on Future<void> can accept callbacks that return Future<int>
 * and properly convert types.
 */
BOOST_AUTO_TEST_CASE(test_void_then_try_returns_int_future, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    
    // Create void future and chain with thenTry returning Future<int>
    auto result = FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<int> {
            BOOST_CHECK(t.hasValue());
            return FutureFactory::makeFuture(42);
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(result, 42);
}

/**
 * Test 3: Future<void>::thenTry with Future<void>-returning callback and delay
 * 
 * Validates that thenTry on Future<void> supports async operations with delay.
 */
BOOST_AUTO_TEST_CASE(test_void_then_try_with_delay, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Create void future with async delay
    FutureFactory::makeFuture()
        .thenTry([&counter](Try<void> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            counter++;
            return FutureFactory::makeFuture()
                .delay(short_delay);
        })
        .via(&executor)
        .get();
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
    BOOST_CHECK_GE(elapsed.count(), short_delay.count());
}

/**
 * Test 4: Future<void>::thenTry with exception handling
 * 
 * Validates that thenTry on Future<void> properly handles exceptions in Try<void>.
 */
BOOST_AUTO_TEST_CASE(test_void_then_try_handles_exception, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> success_counter{0};
    std::atomic<int> error_counter{0};
    
    // Create exceptional void future
    FutureFactory::makeExceptionalFuture<void>(
            folly::exception_wrapper(std::runtime_error("Test error")))
        .thenTry([&success_counter, &error_counter](Try<void> t) -> Future<void> {
            if (t.hasValue()) {
                success_counter++;
            } else {
                error_counter++;
                BOOST_CHECK(t.hasException());
            }
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(success_counter.load(), 0);
    BOOST_CHECK_EQUAL(error_counter.load(), 1);
}

/**
 * Test 5: Future<void>::thenError with Future<void>-returning callback
 * 
 * Validates that thenError on Future<void> can accept callbacks that return Future<void>
 * and properly flatten the result.
 */
BOOST_AUTO_TEST_CASE(test_void_then_error_returns_void_future, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    // Create exceptional void future and recover with thenError
    FutureFactory::makeExceptionalFuture<void>(
            folly::exception_wrapper(std::runtime_error("Test error")))
        .thenError([&counter](folly::exception_wrapper ex) -> Future<void> {
            BOOST_CHECK(ex);
            counter++;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
}

/**
 * Test 6: Future<void>::thenError with Future<void>-returning callback and delay
 * 
 * Validates that thenError on Future<void> supports async operations with delay.
 */
BOOST_AUTO_TEST_CASE(test_void_then_error_with_delay, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Create exceptional void future with async delay in recovery
    FutureFactory::makeExceptionalFuture<void>(
            folly::exception_wrapper(std::runtime_error("Test error")))
        .thenError([&counter](folly::exception_wrapper ex) -> Future<void> {
            BOOST_CHECK(ex);
            counter++;
            return FutureFactory::makeFuture()
                .delay(short_delay);
        })
        .via(&executor)
        .get();
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
    BOOST_CHECK_GE(elapsed.count(), short_delay.count());
}

/**
 * Test 7: Future<void>::thenError with std::exception_ptr
 * 
 * Validates that thenError on Future<void> works with std::exception_ptr callbacks.
 */
BOOST_AUTO_TEST_CASE(test_void_then_error_with_exception_ptr, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    // Create exceptional void future and recover with std::exception_ptr callback
    FutureFactory::makeExceptionalFuture<void>(
            folly::exception_wrapper(std::runtime_error("Test error")))
        .thenError([&counter](std::exception_ptr ex) -> Future<void> {
            BOOST_CHECK(ex != nullptr);
            counter++;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
}

/**
 * Test 8: Chaining Future<void> operations with Future-returning callbacks
 * 
 * Validates that multiple Future<void> operations can be chained together.
 */
BOOST_AUTO_TEST_CASE(test_void_chaining_future_returning_callbacks, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    // Chain multiple void future operations
    FutureFactory::makeFuture()
        .thenTry([&counter](Try<void> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            counter++;
            return FutureFactory::makeFuture();
        })
        .thenTry([&counter](Try<void> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            counter++;
            return FutureFactory::makeFuture();
        })
        .thenTry([&counter](Try<void> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            counter++;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(counter.load(), 3);
}

/**
 * Test 9: Future<void> to Future<int> to Future<void> conversion chain
 * 
 * Validates that type conversions work correctly in chains involving void.
 */
BOOST_AUTO_TEST_CASE(test_void_type_conversion_chain, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> final_value{0};
    
    // Chain: void -> int -> void
    FutureFactory::makeFuture()
        .thenTry([](Try<void> t) -> Future<int> {
            BOOST_CHECK(t.hasValue());
            return FutureFactory::makeFuture(42);
        })
        .thenTry([&final_value](Try<int> t) -> Future<void> {
            BOOST_CHECK(t.hasValue());
            final_value.store(t.value());
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(final_value.load(), 42);
}

/**
 * Test 10: Future<void>::thenError propagating new exception
 * 
 * Validates that thenError can return an exceptional Future<void>.
 */
BOOST_AUTO_TEST_CASE(test_void_then_error_propagates_exception, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::string first_error = "First error";
    std::string second_error = "Second error";
    
    try {
        FutureFactory::makeExceptionalFuture<void>(
                folly::exception_wrapper(std::runtime_error(first_error)))
            .thenError([&second_error](folly::exception_wrapper ex) -> Future<void> {
                BOOST_CHECK(ex);
                // Return a new exceptional future
                return FutureFactory::makeExceptionalFuture<void>(
                    folly::exception_wrapper(std::runtime_error(second_error)));
            })
            .via(&executor)
            .get();
        
        BOOST_FAIL("Expected exception to be thrown");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), second_error);
    }
}

/**
 * Test 11: Future<void>::thenValue with Future-returning callback
 * 
 * Validates that thenValue on Future<void> works with Future-returning callbacks.
 */
BOOST_AUTO_TEST_CASE(test_void_then_value_returns_future, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> counter{0};
    
    // Create void future and chain with thenValue returning Future<void>
    FutureFactory::makeFuture()
        .thenValue([&counter]() -> Future<void> {
            counter++;
            return FutureFactory::makeFuture();
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(counter.load(), 1);
}

/**
 * Test 12: Future<void>::thenValue with Future<int>-returning callback
 * 
 * Validates that thenValue on Future<void> can return Future<int>.
 */
BOOST_AUTO_TEST_CASE(test_void_then_value_returns_int_future, * boost::unit_test::timeout(30)) {
    folly::CPUThreadPoolExecutor executor(2);
    
    // Create void future and chain with thenValue returning Future<int>
    auto result = FutureFactory::makeFuture()
        .thenValue([]() -> Future<int> {
            return FutureFactory::makeFuture(99);
        })
        .via(&executor)
        .get();
    
    BOOST_CHECK_EQUAL(result, 99);
}

/**
 * Test 13: Complex async retry pattern with Future<void>
 * 
 * Validates that Future<void> supports complex async retry patterns.
 */
BOOST_AUTO_TEST_CASE(test_void_async_retry_pattern, * boost::unit_test::timeout(60)) {
    folly::CPUThreadPoolExecutor executor(2);
    std::atomic<int> attempt_count{0};
    constexpr int max_attempts = 3;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Simulate async retry pattern with void futures
    std::function<Future<void>(int)> retry_operation = [&](int attempt) -> Future<void> {
        attempt_count++;
        
        if (attempt < max_attempts) {
            // Simulate failure and retry with delay
            return FutureFactory::makeFuture()
                .delay(short_delay)
                .thenValue([&retry_operation, attempt]() -> Future<void> {
                    return retry_operation(attempt + 1);
                });
        } else {
            // Success after retries
            return FutureFactory::makeFuture();
        }
    };
    
    retry_operation(1)
        .via(&executor)
        .get();
    
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    BOOST_CHECK_EQUAL(attempt_count.load(), max_attempts);
    // Should have at least (max_attempts - 1) delays
    BOOST_CHECK_GE(elapsed.count(), short_delay.count() * (max_attempts - 1));
}

BOOST_AUTO_TEST_SUITE_END()
