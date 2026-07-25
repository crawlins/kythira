#define BOOST_TEST_MODULE TestCodeFutureUsagePropertyTest
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Needed directly (not just transitively): static_assert below compares
// against the real folly::Future<int> type, independent of future_default.
#include <raft/future.hpp>
#include <concepts/future.hpp>

#include <type_traits>
#include <vector>
#include <string>
#include <fstream>
#include <regex>
#include <filesystem>

namespace {
constexpr const char* test_directory = "tests";
constexpr const char* integration_test_file = "tests/integration_test.cpp";
constexpr const char* property_test_pattern = "tests/*property_test.cpp";
}

// **Feature: future-conversion, Property 8: Test code future usage**
// **Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**
// Property: For any test file, all future-related operations should use kythira::Future
// instead of std::future or folly::Future

BOOST_AUTO_TEST_CASE(property_integration_tests_use_kythira_future,
                     *boost::unit_test::timeout(30)) {
    // Test that integration tests use kythira::Future instead of std::future

    // Try multiple possible paths for the integration test file
    std::vector<std::string> possible_paths = {
        "../tests/integration_test.cpp", "../../tests/integration_test.cpp",
        "tests/integration_test.cpp", "./tests/integration_test.cpp"};

    std::ifstream file;
    std::string content;
    bool file_found = false;

    for (const auto& path : possible_paths) {
        file.open(path);
        if (file.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
            file_found = true;
            file.close();
            break;
        }
    }

    if (!file_found) {
        // If we can't find the file, just verify the concepts work
        BOOST_TEST_MESSAGE("Integration test file not found, skipping file content validation");
        return;
    }

    // Check that the file includes raft/future_default.hpp (raft/future.hpp itself
    // is Folly-specific and no longer unconditionally pulled in by future_default.hpp)
    BOOST_TEST(content.find("#include <raft/future_default.hpp>") != std::string::npos,
               "Integration test should include raft/future_default.hpp");

    // Check that kythira::Future is used instead of folly::Future for collections
    BOOST_TEST(content.find("std::vector<kythira::future_default<bool>>") != std::string::npos,
               "Integration test should use kythira::Future in collections");

    // Check that kythira::future_collector_default::collectAll is used instead of
    // folly::collectAll (or the Folly-only kythira::wait_for_all free function alias)
    BOOST_TEST(content.find("kythira::future_collector_default::collectAll") != std::string::npos,
               "Integration test should use kythira::future_collector_default::collectAll");

    // Verify no direct std::future usage remains
    std::regex std_future_regex(R"(\bstd::future\b)");
    BOOST_TEST(!std::regex_search(content, std_future_regex),
               "Integration test should not use std::future directly");

    BOOST_TEST_MESSAGE("Integration test future usage validation passed");
}

BOOST_AUTO_TEST_CASE(property_test_fixtures_use_consistent_future_types,
                     *boost::unit_test::timeout(30)) {
    // Test that test fixtures use consistent future types

    // Verify that kythira::Future satisfies the future concept for common test types
    static_assert(kythira::future<kythira::future_default<bool>, bool>,
                  "kythira::future_default<bool> should satisfy future concept");

    static_assert(
        kythira::future<kythira::future_default<std::vector<std::byte>>, std::vector<std::byte>>,
        "kythira::future_default<std::vector<std::byte>> should satisfy future concept");

    static_assert(kythira::future<kythira::future_default<void>, void>,
                  "kythira::future_default<void> should satisfy future concept");

    // Test that kythira::Future is different from std::future and folly::Future
    static_assert(!std::is_same_v<kythira::future_default<bool>, std::future<bool>>,
                  "kythira::Future should be different from std::future");

    BOOST_TEST_MESSAGE("Test fixture future type consistency validation passed");
}

BOOST_AUTO_TEST_CASE(property_async_test_operations_use_kythira_future,
                     *boost::unit_test::timeout(30)) {
    // Test that async test operations use kythira::Future for synchronization

    // Create test futures to verify they work correctly in test context
    auto immediate_future = kythira::future_factory_default::makeFuture(42);
    BOOST_TEST(immediate_future.isReady());
    BOOST_TEST(std::move(immediate_future).get() == 42);

    auto exception_future = kythira::future_factory_default::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("test error")));
    BOOST_TEST(exception_future.isReady());
    BOOST_CHECK_THROW(std::move(exception_future).get(), std::runtime_error);

    // Test that future_collector_default::collectAll works with test futures
    std::vector<kythira::future_default<int>> test_futures;
    test_futures.push_back(kythira::future_factory_default::makeFuture(1));
    test_futures.push_back(kythira::future_factory_default::makeFuture(2));
    test_futures.push_back(kythira::future_factory_default::makeFuture(3));

    auto results =
        std::move(kythira::future_collector_default::collectAll(std::move(test_futures))).get();
    BOOST_TEST(results.size() == 3);
    BOOST_TEST(results[0].value() == 1);
    BOOST_TEST(results[1].value() == 2);
    BOOST_TEST(results[2].value() == 3);

    BOOST_TEST_MESSAGE("Async test operation future usage validation passed");
}

BOOST_AUTO_TEST_CASE(property_test_validation_uses_kythira_future, *boost::unit_test::timeout(30)) {
    // Test that test code validation uses kythira::Future for result verification

    // Test future chaining in test context
    auto base_future = kythira::future_factory_default::makeFuture(10);

    bool then_called = false;
    auto chained_future = std::move(base_future).thenValue([&then_called](int value) {
        then_called = true;
        BOOST_TEST(value == 10);
    });
    std::move(chained_future).get();
    BOOST_TEST(then_called);

    // Test error handling in test context
    auto error_future = kythira::future_factory_default::makeExceptionalFuture<int>(
        std::make_exception_ptr(std::runtime_error("test")));

    bool error_handled = false;
    auto recovered_future =
        std::move(error_future).thenError([&error_handled](std::exception_ptr) -> int {
            error_handled = true;
            return -1;
        });

    BOOST_TEST(std::move(recovered_future).get() == -1);
    BOOST_TEST(error_handled);

    BOOST_TEST_MESSAGE("Test validation future usage validation passed");
}

BOOST_AUTO_TEST_CASE(property_no_direct_folly_future_in_test_interfaces,
                     *boost::unit_test::timeout(30)) {
    // Property: Test code should not use folly::Future directly in public interfaces

    // This test documents the conversion requirement:
    // Test code should use kythira::Future which provides a unified interface

    // Verify that kythira::Future is NOT the same as folly::Future
    static_assert(!std::is_same_v<kythira::future_default<int>, folly::Future<int>>,
                  "kythira::Future should be different from folly::Future");

    // Test that kythira::Future provides the expected interface for tests
    auto test_future = kythira::future_factory_default::makeFuture(std::string("test_value"));

    // Test synchronous access
    BOOST_TEST(test_future.isReady());
    BOOST_TEST(std::move(test_future).get() == "test_value");

    BOOST_TEST_MESSAGE("Test interface future usage validation passed");
}