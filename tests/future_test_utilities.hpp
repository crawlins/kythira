#pragma once

#include <raft/future_default.hpp>

#include <raft/future.hpp>
#include <vector>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

namespace test_utilities {

/**
 * Test utility function to create a future with a value
 * This demonstrates how test utilities should use kythira::Future
 */
template<typename T> auto create_ready_future(T value) -> kythira::future_default<T> {
    return kythira::future_factory_default::makeFuture(std::move(value));
}

/**
 * Test utility function to create a future with an exception
 * This demonstrates error handling in test utilities
 */
template<typename T>
auto create_failed_future(const std::string& error_message) -> kythira::future_default<T> {
    return kythira::future_factory_default::makeExceptionalFuture<T>(
        std::make_exception_ptr(std::runtime_error(error_message)));
}

/**
 * Test utility function to wait for all futures to complete
 * This demonstrates how test utilities should work with future collections
 */
template<typename T>
auto wait_for_all_futures(std::vector<kythira::future_default<T>> futures) -> std::vector<T> {
    auto results = kythira::future_collector_default::collectAll(std::move(futures)).get();
    std::vector<T> values;
    values.reserve(results.size());

    for (const auto& result : results) {
        values.push_back(result.value());
    }

    return values;
}

/**
 * Test utility function to create a collection of ready futures
 * This demonstrates batch future creation for testing
 */
template<typename T>
auto create_ready_futures(const std::vector<T>& values) -> std::vector<kythira::future_default<T>> {
    std::vector<kythira::future_default<T>> futures;
    futures.reserve(values.size());

    for (const auto& value : values) {
        futures.emplace_back(kythira::future_factory_default::makeFuture(value));
    }

    return futures;
}

/**
 * Test utility function to verify future readiness
 * This demonstrates how to check future state in tests
 */
template<typename T> auto is_future_ready(const kythira::future_default<T>& future) -> bool {
    return future.isReady();
}

/**
 * Test utility function to wait for a future with timeout
 * This demonstrates timeout handling in test utilities
 */
template<typename T>
auto wait_for_future_with_timeout(kythira::future_default<T>& future,
                                  std::chrono::milliseconds timeout) -> bool {
    return future.wait(timeout);
}

}  // namespace test_utilities