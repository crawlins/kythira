#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <tuple>
#include <utility>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/Try.h>

#include "../concepts/future.hpp"

namespace kythira {

// Try wrapper that adapts folly::Try to satisfy try_type concept
template<typename T>
class Try {
public:
    // Constructors
    Try() = default;
    explicit Try(folly::Try<T> ft) : _folly_try(std::move(ft)) {}
    
    // Construct from value
    explicit Try(T value) : _folly_try(std::move(value)) {}
    
    // Construct from exception
    explicit Try(folly::exception_wrapper ex) : _folly_try(std::move(ex)) {}
    
    // Access value (throws if contains exception)
    auto value() -> T& {
        return _folly_try.value();
    }
    
    auto value() const -> const T& {
        return _folly_try.value();
    }
    
    // Access exception - convert folly::exception_wrapper to std::exception_ptr
    auto exception() const -> std::exception_ptr {
        if (_folly_try.hasException()) {
            return _folly_try.exception().to_exception_ptr();
        }
        return nullptr;
    }
    
    // Check if contains value
    auto has_value() const -> bool {
        return _folly_try.hasValue();
    }
    
    // Check if contains exception
    auto has_exception() const -> bool {
        return _folly_try.hasException();
    }
    
    // Get underlying folly::Try
    auto get_folly_try() const -> const folly::Try<T>& {
        return _folly_try;
    }
    
    auto get_folly_try() -> folly::Try<T>& {
        return _folly_try;
    }

private:
    folly::Try<T> _folly_try;
};

// Future wrapper that adapts folly::Future to satisfy future concept
template<typename T>
class Future {
public:
    // Constructors
    Future() = default;
    explicit Future(folly::Future<T> ff) : _folly_future(std::move(ff)) {}
    
    // Construct from value
    explicit Future(T value) : _folly_future(folly::makeFuture<T>(std::move(value))) {}
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex) : _folly_future(folly::makeFuture<T>(std::move(ex))) {}
    
    // Construct from std::exception_ptr
    explicit Future(std::exception_ptr ex) : _folly_future(folly::makeFuture<T>(folly::exception_wrapper(std::move(ex)))) {}
    
    // Get value (blocking)
    auto get() -> T {
        return std::move(_folly_future).get();
    }
    
    // Chain continuation
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F, T>> {
        using ReturnType = std::invoke_result_t<F, T>;
        return Future<ReturnType>(std::move(_folly_future).thenValue(std::forward<F>(func)));
    }
    
    // Error handling
    template<typename F>
    auto onError(F&& func) -> Future<T> {
        return Future<T>(std::move(_folly_future).thenError(std::forward<F>(func)));
    }
    
    // Check if ready
    auto isReady() const -> bool {
        return _folly_future.isReady();
    }
    
    // Wait with timeout
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _folly_future.wait(timeout).isReady();
    }
    
    // Get underlying folly::Future
    auto get_folly_future() && -> folly::Future<T> {
        return std::move(_folly_future);
    }

private:
    folly::Future<T> _folly_future;
};

// Specialization for void
template<>
class Future<void> {
public:
    // Constructors
    Future() : _folly_future(folly::makeFuture()) {}
    explicit Future(folly::Future<folly::Unit> ff) : _folly_future(std::move(ff)) {}
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex) : _folly_future(folly::makeFuture<folly::Unit>(std::move(ex))) {}
    
    // Get value (blocking)
    auto get() -> void {
        std::move(_folly_future).get();
    }
    
    // Chain continuation
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F>> {
        using ReturnType = std::invoke_result_t<F>;
        if constexpr (std::is_void_v<ReturnType>) {
            return Future<void>(std::move(_folly_future).thenValue([func = std::forward<F>(func)](folly::Unit) {
                func();
                return folly::Unit{};
            }));
        } else {
            return Future<ReturnType>(std::move(_folly_future).thenValue([func = std::forward<F>(func)](folly::Unit) {
                return func();
            }));
        }
    }
    
    // Error handling
    template<typename F>
    auto onError(F&& func) -> Future<void> {
        return Future<void>(std::move(_folly_future).thenError([func = std::forward<F>(func)](folly::exception_wrapper ex) {
            func(ex.to_exception_ptr());
            return folly::Unit{};
        }));
    }
    
    // Check if ready
    auto isReady() const -> bool {
        return _folly_future.isReady();
    }
    
    // Wait with timeout
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _folly_future.wait(timeout).isReady();
    }
    
    // Get underlying folly::Future
    auto get_folly_future() && -> folly::Future<folly::Unit> {
        return std::move(_folly_future);
    }

private:
    folly::Future<folly::Unit> _folly_future;
};

// Collective future operations

// Wait for any future to complete (modeled after folly::collectAny)
// Returns a future of tuple containing the index and Try<T> of the first completed future
template<typename T>
auto wait_for_any(std::vector<Future<T>> futures) -> Future<std::tuple<std::size_t, Try<T>>> {
    // Convert our Future wrappers to folly::Future
    std::vector<folly::Future<T>> folly_futures;
    folly_futures.reserve(futures.size());
    for (auto& fut : futures) {
        folly_futures.push_back(std::move(fut).get_folly_future());
    }
    
    // Use folly::collectAny - returns SemiFuture, convert to Future
    auto result_future = folly::collectAny(folly_futures.begin(), folly_futures.end())
        .toUnsafeFuture()
        .thenValue([](std::pair<std::size_t, folly::Try<T>> result) {
            return std::make_tuple(result.first, Try<T>(std::move(result.second)));
        });
    
    return Future<std::tuple<std::size_t, Try<T>>>(std::move(result_future));
}

// Wait for all futures to complete (modeled after folly::collectAll)
// Returns a future of vector containing Try<T> for each future (preserving order)
template<typename T>
auto wait_for_all(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
    // Convert our Future wrappers to folly::Future
    std::vector<folly::Future<T>> folly_futures;
    folly_futures.reserve(futures.size());
    for (auto& fut : futures) {
        folly_futures.push_back(std::move(fut).get_folly_future());
    }
    
    // Use folly::collectAll - returns SemiFuture, convert to Future
    auto result_future = folly::collectAll(folly_futures.begin(), folly_futures.end())
        .toUnsafeFuture()
        .thenValue([](std::vector<folly::Try<T>> results) {
            std::vector<Try<T>> wrapped_results;
            wrapped_results.reserve(results.size());
            for (auto& result : results) {
                wrapped_results.push_back(Try<T>(std::move(result)));
            }
            return wrapped_results;
        });
    
    return Future<std::vector<Try<T>>>(std::move(result_future));
}

} // namespace kythira

// Static assertions to ensure kythira::Future satisfies the future concept
static_assert(kythira::future<kythira::Future<int>, int>, "kythira::Future<int> must satisfy future concept");
static_assert(kythira::future<kythira::Future<std::string>, std::string>, "kythira::Future<std::string> must satisfy future concept");
static_assert(kythira::future<kythira::Future<void>, void>, "kythira::Future<void> must satisfy future concept");