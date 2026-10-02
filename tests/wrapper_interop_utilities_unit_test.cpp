// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE wrapper_interop_utilities_unit_test
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "../include/raft/future.hpp"

namespace {
constexpr int test_value = 42;
constexpr const char* test_string = "test_message";
constexpr std::size_t large_string_size = 1000;
}  // namespace

// ============================================================================
// Type Conversion Utilities Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(type_conversion_tests)

// Requirement 18.1: exception information survives both directions
BOOST_AUTO_TEST_CASE(exception_wrapper_conversion, *boost::unit_test::timeout(15)) {
    auto folly_ex = folly::exception_wrapper(std::runtime_error(test_string));
    auto std_ex_ptr = kythira::interop::to_std_exception_ptr(folly_ex);
    BOOST_REQUIRE(std_ex_ptr != nullptr);

    try {
        std::rethrow_exception(std_ex_ptr);
        BOOST_FAIL("Should have thrown exception");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), test_string);
    }

    auto ex_ptr = std::make_exception_ptr(std::invalid_argument(test_string));
    auto folly_wrapper = kythira::interop::to_folly_exception_wrapper(ex_ptr);
    BOOST_REQUIRE(static_cast<bool>(folly_wrapper));
    BOOST_CHECK(folly_wrapper.is_compatible_with<std::invalid_argument>());
    BOOST_CHECK_EQUAL(std::string(folly_wrapper.get_exception<std::invalid_argument>()->what()),
                      test_string);

    // Empty in, empty out
    BOOST_CHECK(kythira::interop::to_std_exception_ptr(folly::exception_wrapper{}) == nullptr);
    BOOST_CHECK(!kythira::interop::to_folly_exception_wrapper(std::exception_ptr{}));
}

// Requirement 18.2: void and folly::Unit are mapped onto each other
BOOST_AUTO_TEST_CASE(void_unit_conversion, *boost::unit_test::timeout(15)) {
    static_assert(std::is_same_v<kythira::interop::void_to_unit_t<void>, folly::Unit>);
    static_assert(std::is_same_v<kythira::interop::void_to_unit_t<int>, int>);
    static_assert(std::is_same_v<kythira::interop::unit_to_void_t<folly::Unit>, void>);
    static_assert(std::is_same_v<kythira::interop::unit_to_void_t<int>, int>);

    static_assert(std::is_same_v<decltype(kythira::interop::from_folly_future(folly::makeFuture())),
                                 kythira::Future<void>>);
    static_assert(
        std::is_same_v<decltype(kythira::interop::to_folly_future(kythira::Future<void>{})),
                       folly::Future<folly::Unit>>);
    static_assert(
        std::is_same_v<decltype(kythira::interop::from_folly_try(folly::Try<folly::Unit>{})),
                       kythira::Try<void>>);
    static_assert(std::is_same_v<decltype(kythira::interop::to_folly_try(kythira::Try<void>{})),
                                 folly::Try<folly::Unit>>);

    // A successful void result and a folly::Unit result are the same thing
    auto void_try = kythira::interop::from_folly_try(folly::Try<folly::Unit>(folly::Unit{}));
    BOOST_CHECK(void_try.has_value());
    auto unit_try = kythira::interop::to_folly_try(std::move(void_try));
    BOOST_REQUIRE(unit_try.hasValue());
    BOOST_CHECK(unit_try.value() == folly::Unit{});
}

// Requirement 18.5: conversions move the held value instead of copying it
BOOST_AUTO_TEST_CASE(conversions_preserve_move_semantics, *boost::unit_test::timeout(15)) {
    // A heap-allocated string keeps its buffer only if every hop moves it
    std::string large_string(large_string_size, 'x');
    const auto* original_data = large_string.data();

    kythira::Future<std::string> kythira_future(std::move(large_string));
    auto folly_future = kythira::interop::to_folly_future(std::move(kythira_future));
    auto round_tripped = kythira::interop::from_folly_future(std::move(folly_future));
    auto result = round_tripped.get();
    BOOST_CHECK_EQUAL(result.size(), large_string_size);
    BOOST_CHECK_EQUAL(static_cast<const void*>(result.data()),
                      static_cast<const void*>(original_data));

    // Move-only values pass through Try conversions in both directions
    auto owned = std::make_unique<int>(test_value);
    auto* raw = owned.get();
    folly::Try<std::unique_ptr<int>> folly_try(std::move(owned));
    auto kythira_try = kythira::interop::from_folly_try(std::move(folly_try));
    auto back = kythira::interop::to_folly_try(std::move(kythira_try));
    BOOST_REQUIRE(back.hasValue());
    BOOST_CHECK_EQUAL(back.value().get(), raw);
    BOOST_CHECK_EQUAL(*back.value(), test_value);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Future Conversion Utilities Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(future_conversion_tests)

BOOST_AUTO_TEST_CASE(folly_to_kythira_future, *boost::unit_test::timeout(15)) {
    auto kythira_future = kythira::interop::from_folly_future(folly::makeFuture(test_value));
    static_assert(std::is_same_v<decltype(kythira_future), kythira::Future<int>>);

    BOOST_CHECK(kythira_future.isReady());
    BOOST_CHECK_EQUAL(kythira_future.get(), test_value);

    // A pending folly::Future converts too and completes when its promise does
    folly::Promise<int> promise;
    auto pending = kythira::interop::from_folly_future(promise.getFuture());
    BOOST_CHECK(!pending.isReady());
    promise.setValue(test_value + 1);
    BOOST_CHECK_EQUAL(pending.get(), test_value + 1);
}

BOOST_AUTO_TEST_CASE(kythira_to_folly_future, *boost::unit_test::timeout(15)) {
    kythira::Future<int> kythira_future(test_value);
    auto folly_future = kythira::interop::to_folly_future(std::move(kythira_future));
    static_assert(std::is_same_v<decltype(folly_future), folly::Future<int>>);

    BOOST_CHECK(folly_future.isReady());
    BOOST_CHECK_EQUAL(std::move(folly_future).get(), test_value);

    // Exceptions keep their type across the conversion
    kythira::Future<int> failed{folly::exception_wrapper{std::runtime_error{test_string}}};
    auto folly_failed = kythira::interop::to_folly_future(std::move(failed));
    BOOST_CHECK_THROW(std::move(folly_failed).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(void_future_conversion, *boost::unit_test::timeout(15)) {
    auto kythira_void_future = kythira::interop::from_folly_future(folly::makeFuture());
    BOOST_CHECK(kythira_void_future.isReady());
    BOOST_CHECK_NO_THROW(kythira_void_future.get());

    auto folly_unit_future = kythira::interop::to_folly_future(kythira::Future<void>{});
    BOOST_CHECK(folly_unit_future.isReady());
    BOOST_CHECK_NO_THROW(std::move(folly_unit_future).get());

    auto failed = kythira::interop::from_folly_future(
        folly::makeFuture<folly::Unit>(std::logic_error(test_string)));
    BOOST_CHECK_THROW(failed.get(), std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Try Conversion Utilities Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(try_conversion_tests)

BOOST_AUTO_TEST_CASE(folly_to_kythira_try, *boost::unit_test::timeout(15)) {
    auto kythira_try = kythira::interop::from_folly_try(folly::Try<int>(test_value));
    static_assert(std::is_same_v<decltype(kythira_try), kythira::Try<int>>);

    BOOST_CHECK(kythira_try.has_value());
    BOOST_CHECK_EQUAL(kythira_try.value(), test_value);

    auto void_try = kythira::interop::from_folly_try(folly::Try<folly::Unit>(folly::Unit{}));
    BOOST_CHECK(void_try.has_value());
    BOOST_CHECK_NO_THROW(void_try.value());
}

BOOST_AUTO_TEST_CASE(kythira_to_folly_try, *boost::unit_test::timeout(15)) {
    auto folly_try = kythira::interop::to_folly_try(kythira::Try<int>(test_value));
    static_assert(std::is_same_v<decltype(folly_try), folly::Try<int>>);

    BOOST_CHECK(folly_try.hasValue());
    BOOST_CHECK_EQUAL(folly_try.value(), test_value);

    auto unit_try = kythira::interop::to_folly_try(kythira::Try<void>{});
    BOOST_CHECK(unit_try.hasValue());
}

BOOST_AUTO_TEST_CASE(try_exception_conversion, *boost::unit_test::timeout(15)) {
    folly::Try<int> folly_try{folly::exception_wrapper{std::runtime_error{test_string}}};
    auto kythira_try = kythira::interop::from_folly_try(std::move(folly_try));

    BOOST_CHECK(!kythira_try.has_value());
    BOOST_REQUIRE(kythira_try.has_exception());
    BOOST_CHECK_THROW((void)kythira_try.value(), std::runtime_error);

    auto converted_back = kythira::interop::to_folly_try(std::move(kythira_try));
    BOOST_REQUIRE(converted_back.hasException());
    BOOST_CHECK(converted_back.exception().is_compatible_with<std::runtime_error>());
    BOOST_CHECK_EQUAL(
        std::string(converted_back.exception().get_exception<std::runtime_error>()->what()),
        test_string);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Mixed Folly / kythira Usage Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(mixed_usage_tests)

// A value can make a full round trip without changing type or content
BOOST_AUTO_TEST_CASE(round_trip_preserves_type_and_value, *boost::unit_test::timeout(15)) {
    kythira::Future<std::string> original{std::string{test_string}};
    auto round_tripped =
        kythira::interop::from_folly_future(kythira::interop::to_folly_future(std::move(original)));
    static_assert(std::is_same_v<decltype(round_tripped), kythira::Future<std::string>>);
    BOOST_CHECK_EQUAL(round_tripped.get(), test_string);

    auto try_round_tripped = kythira::interop::from_folly_try(
        kythira::interop::to_folly_try(kythira::Try<std::string>(std::string(test_string))));
    BOOST_CHECK_EQUAL(try_round_tripped.value(), test_string);
}

// Futures that started on the Folly side feed kythira's collectors and back
BOOST_AUTO_TEST_CASE(folly_futures_through_kythira_collector, *boost::unit_test::timeout(15)) {
    folly::CPUThreadPoolExecutor executor(2);

    std::vector<kythira::Future<int>> futures;
    for (int i = 0; i < 3; ++i) {
        futures.push_back(kythira::interop::from_folly_future(
            folly::via(&executor, [i] { return test_value + i; })));
    }

    auto collected = kythira::FutureCollector::collectAll(std::move(futures));
    auto folly_collected = kythira::interop::to_folly_future(std::move(collected));
    auto results = std::move(folly_collected).get();

    BOOST_REQUIRE_EQUAL(results.size(), 3U);
    for (std::size_t i = 0; i < results.size(); ++i) {
        BOOST_REQUIRE(results[i].has_value());
        BOOST_CHECK_EQUAL(results[i].value(), test_value + static_cast<int>(i));
        // Each element is a kythira::Try and converts to folly::Try on its own
        auto folly_element = kythira::interop::to_folly_try(std::move(results[i]));
        BOOST_CHECK_EQUAL(folly_element.value(), test_value + static_cast<int>(i));
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Concept Compliance Validation Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(concept_compliance_tests)

BOOST_AUTO_TEST_CASE(current_concept_compliance_status, *boost::unit_test::timeout(15)) {
    kythira::Try<int> try_val(test_value);
    BOOST_CHECK(try_val.has_value());
    BOOST_CHECK(!try_val.has_exception());
    BOOST_CHECK_EQUAL(try_val.value(), test_value);

    kythira::Future<int> future_val(test_value);
    BOOST_CHECK(future_val.isReady());
    // Note: Don't call get() multiple times on the same future
    auto result = future_val.get();
    BOOST_CHECK_EQUAL(result, test_value);

    // Create a new future for wait test
    kythira::Future<int> future_for_wait(test_value);
    BOOST_CHECK(future_for_wait.wait(std::chrono::milliseconds{10}));
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Error Handling and Edge Cases Unit Tests
// ============================================================================

BOOST_AUTO_TEST_SUITE(error_handling_tests)

BOOST_AUTO_TEST_CASE(null_pointer_handling, *boost::unit_test::timeout(15)) {
    // A default Executor is an explicit "no executor" and refuses work
    kythira::Executor default_executor;
    BOOST_CHECK(!default_executor.is_valid());
    BOOST_CHECK(default_executor.get() == nullptr);
    BOOST_CHECK_THROW(default_executor.add([] {}), std::runtime_error);

    // Wrapping a null folly::Executor* is a programming error, caught at construction
    BOOST_CHECK_THROW(kythira::Executor{nullptr}, std::invalid_argument);

    // A default KeepAlive holds nothing and refuses work
    kythira::KeepAlive null_keep_alive;
    BOOST_CHECK(!null_keep_alive.is_valid());
    BOOST_CHECK(null_keep_alive.get() == nullptr);
    BOOST_CHECK_THROW(null_keep_alive.add([] {}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(exception_propagation_validation, *boost::unit_test::timeout(15)) {
    // Test that exceptions propagate correctly through existing wrappers

    auto ex = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> future_with_exception(ex);

    BOOST_CHECK(future_with_exception.isReady());

    // Create separate futures for each test to avoid "Future invalid" errors
    auto ex2 = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> future_for_get(ex2);
    BOOST_CHECK_THROW(future_for_get.get(), std::runtime_error);

    // Test exception propagation through then chain
    auto ex3 = folly::exception_wrapper(std::runtime_error(test_string));
    kythira::Future<int> future_for_chain(ex3);
    auto chained = future_for_chain.then([](int val) { return val * 2; });
    BOOST_CHECK_THROW(chained.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(resource_cleanup_validation, *boost::unit_test::timeout(15)) {
    // Test that wrapper classes properly clean up resources

    // Test with RAII types
    auto unique_ptr = std::make_unique<int>(test_value);
    auto* raw_ptr = unique_ptr.get();

    kythira::Future<std::unique_ptr<int>> future_with_unique_ptr(std::move(unique_ptr));
    BOOST_CHECK(future_with_unique_ptr.isReady());

    auto result = future_with_unique_ptr.get();
    BOOST_CHECK(result.get() == raw_ptr);
    BOOST_CHECK_EQUAL(*result, test_value);
}

BOOST_AUTO_TEST_SUITE_END()
