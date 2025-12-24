#define BOOST_TEST_MODULE FutureTestUtilitiesTest
#include <boost/test/unit_test.hpp>

#include "future_test_utilities.hpp"
#include <string>
#include <vector>

namespace {
    constexpr int test_value = 42;
    constexpr const char* test_string = "test_value";
    constexpr const char* error_message = "test_error";
    constexpr std::chrono::milliseconds short_timeout{10};
}

BOOST_AUTO_TEST_CASE(test_create_ready_future, * boost::unit_test::timeout(30)) {
    // Test creating a ready future with an integer value
    auto int_future = test_utilities::create_ready_future(test_value);
    
    BOOST_TEST(test_utilities::is_future_ready(int_future));
    BOOST_TEST(int_future.get() == test_value);
    
    // Test creating a ready future with a string value
    auto string_future = test_utilities::create_ready_future(std::string(test_string));
    
    BOOST_TEST(test_utilities::is_future_ready(string_future));
    BOOST_TEST(string_future.get() == test_string);
}

BOOST_AUTO_TEST_CASE(test_create_failed_future, * boost::unit_test::timeout(30)) {
    // Test creating a failed future
    auto failed_future = test_utilities::create_failed_future<int>(error_message);
    
    BOOST_TEST(test_utilities::is_future_ready(failed_future));
    BOOST_CHECK_THROW(failed_future.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_wait_for_all_futures, * boost::unit_test::timeout(30)) {
    // Test waiting for multiple futures
    std::vector<int> test_values = {1, 2, 3, 4, 5};
    auto futures = test_utilities::create_ready_futures(test_values);
    
    auto results = test_utilities::wait_for_all_futures(std::move(futures));
    
    BOOST_TEST(results.size() == test_values.size());
    for (std::size_t i = 0; i < test_values.size(); ++i) {
        BOOST_TEST(results[i] == test_values[i]);
    }
}

BOOST_AUTO_TEST_CASE(test_create_ready_futures, * boost::unit_test::timeout(30)) {
    // Test creating multiple ready futures
    std::vector<std::string> test_strings = {"one", "two", "three"};
    auto futures = test_utilities::create_ready_futures(test_strings);
    
    BOOST_TEST(futures.size() == test_strings.size());
    
    for (std::size_t i = 0; i < futures.size(); ++i) {
        BOOST_TEST(test_utilities::is_future_ready(futures[i]));
        BOOST_TEST(futures[i].get() == test_strings[i]);
    }
}

BOOST_AUTO_TEST_CASE(test_wait_for_future_with_timeout, * boost::unit_test::timeout(30)) {
    // Test waiting for a ready future with timeout
    auto ready_future = test_utilities::create_ready_future(test_value);
    
    BOOST_TEST(test_utilities::wait_for_future_with_timeout(ready_future, short_timeout));
    
    // The future should still be accessible after timeout check
    BOOST_TEST(ready_future.get() == test_value);
}

BOOST_AUTO_TEST_CASE(test_future_utilities_integration, * boost::unit_test::timeout(30)) {
    // Test integration of multiple utility functions
    std::vector<int> values = {10, 20, 30};
    
    // Create futures
    auto futures = test_utilities::create_ready_futures(values);
    
    // Verify all are ready
    for (const auto& future : futures) {
        BOOST_TEST(test_utilities::is_future_ready(future));
    }
    
    // Wait for all and verify results
    auto results = test_utilities::wait_for_all_futures(std::move(futures));
    
    BOOST_TEST(results == values);
}