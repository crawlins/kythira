#pragma once

#include <chrono>
#include <functional>
#include <type_traits>
#include <vector>
#include <tuple>
#include <exception>

namespace kythira {

// Forward declarations for concepts
template<typename T> class Try;
template<typename T> class Future;

// Concept for Try types (matches folly::Try interface)
template<typename T, typename ValueType>
concept try_type = requires(T t) {
    // Access value (throws if contains exception)
    { t.value() } -> std::same_as<ValueType&>;
    { std::as_const(t).value() } -> std::same_as<const ValueType&>;
    
    // Access exception
    { t.exception() } -> std::convertible_to<std::exception_ptr>;
    
    // Check if contains value
    { t.has_value() } -> std::convertible_to<bool>;
    
    // Check if contains exception  
    { t.has_exception() } -> std::convertible_to<bool>;
};

// Concept for Future types (matches folly::Future interface)
template<typename F, typename T>
concept future = requires(F f) {
    // Get value (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
} && (
    // Chain continuation - handle void case
    (std::is_void_v<T> && requires(F f) { 
        f.then(std::declval<std::function<void()>>()); 
    }) ||
    (!std::is_void_v<T> && requires(F f) { 
        f.then(std::declval<std::function<void(T)>>()); 
    })
) && (
    // Error handling - handle void case
    (std::is_void_v<T> && requires(F f) {
        f.onError(std::declval<std::function<void(std::exception_ptr)>>());
    }) ||
    (!std::is_void_v<T> && requires(F f) {
        f.onError(std::declval<std::function<T(std::exception_ptr)>>());
    })
);

// Concept for SemiPromise types (matches folly::SemiPromise interface)
template<typename P, typename T>
concept semi_promise = requires(P p, T value, std::exception_ptr ex) {
    // Set value
    { p.setValue(std::move(value)) } -> std::same_as<void>;
    
    // Set exception
    { p.setException(ex) } -> std::same_as<void>;
    
    // Check if fulfilled
    { p.isFulfilled() } -> std::convertible_to<bool>;
} && (
    // Handle void specialization
    std::is_void_v<T> || requires(P p, T value) {
        { p.setValue(std::move(value)) } -> std::same_as<void>;
    }
);

// Concept for Promise types (matches folly::Promise interface)  
template<typename P, typename T, typename FutureType>
concept promise = semi_promise<P, T> && requires(P p) {
    // Get associated future
    { p.getFuture() } -> std::same_as<FutureType>;
    
    // Get associated semi-future (if supported)
    { p.getSemiFuture() };
} && (
    // Handle void specialization
    std::is_void_v<T> || requires(P p, T value) {
        { p.setValue(std::move(value)) } -> std::same_as<void>;
    }
);

// Concept for Executor types (matches folly::Executor interface)
template<typename E>
concept executor = requires(E e, std::function<void()> func) {
    // Add work to executor
    { e.add(std::move(func)) } -> std::same_as<void>;
    
    // Get number of pending tasks (optional)
    { e.getNumPriorities() } -> std::convertible_to<std::uint8_t>;
} && requires(E e) {
    // Executor should be destructible
    typename E::~E;
};

// Concept for KeepAlive executor wrapper (matches folly::Executor::KeepAlive)
template<typename K, typename ExecutorType>
concept keep_alive = requires(K k, std::function<void()> func) {
    // Add work via keep alive
    { k.add(std::move(func)) } -> std::same_as<void>;
    
    // Get underlying executor
    { k.get() } -> std::convertible_to<ExecutorType*>;
    
    // Copy and move semantics
    { K(k) };
    { K(std::move(k)) };
};

// Concept for future factory functions (matches folly::makeFuture, etc.)
template<typename F>
concept future_factory = requires() {
    // Make future from value
    { F::template make<int>(42) } -> future<int>;
    
    // Make future from exception
    { F::template make_exceptional<int>(std::exception_ptr{}) } -> future<int>;
    
    // Make ready future
    { F::template make_ready<void>() } -> future<void>;
};

// Concept for collective future operations (matches folly::collectAll, collectAny, etc.)
template<typename C>
concept future_collector = requires() {
    // Collect all futures
    { C::template collect_all(std::vector<Future<int>>{}) } -> future<std::vector<Try<int>>>;
    
    // Collect any future  
    { C::template collect_any(std::vector<Future<int>>{}) } -> future<std::tuple<std::size_t, Try<int>>>;
    
    // Collect with timeout
    { C::template collect_all_timeout(std::vector<Future<int>>{}, std::chrono::milliseconds{}) };
};

// Concept for future continuation operations (matches folly::Future::via, etc.)
template<typename F, typename T, typename ExecutorType>
concept future_continuation = future<F, T> && requires(F f, ExecutorType* exec) {
    // Via executor
    { f.via(exec) } -> future<T>;
    
    // Delay execution
    { f.delay(std::chrono::milliseconds{}) } -> future<T>;
    
    // Within context
    { f.within(std::chrono::milliseconds{}) } -> future<T>;
};

// Concept for future transformation operations
template<typename F, typename T>
concept future_transformable = future<F, T> && requires(F f) {
    // Map/transform value
    { f.thenValue(std::declval<std::function<int(T)>>()) } -> future<int>;
    
    // Handle errors
    { f.thenError(std::declval<std::function<T(std::exception_ptr)>>()) } -> future<T>;
    
    // Ensure (finally-like behavior)
    { f.ensure(std::declval<std::function<void()>>()) } -> future<T>;
};

} // namespace kythira