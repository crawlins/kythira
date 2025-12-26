#pragma once

#include <chrono>
#include <functional>
#include <type_traits>
#include <vector>
#include <tuple>
#include <exception>

// Include folly headers for proper concept definitions
#include <folly/ExceptionWrapper.h>
#include <folly/Unit.h>
#include <folly/Try.h>

namespace kythira {

// Forward declarations for concepts
template<typename T> class Try;
template<typename T> class Future;

// Concept for Try types (matches folly::Try interface)
template<typename T, typename ValueType>
concept try_type = requires(T t, const T ct) {
    // Check if contains value (folly naming)
    { t.hasValue() } -> std::convertible_to<bool>;
    
    // Check if contains exception (folly naming)  
    { t.hasException() } -> std::convertible_to<bool>;
    
    // Access exception (folly::Try uses exception_wrapper)
    { t.exception() }; // Returns folly::exception_wrapper or similar
} && (
    // Handle void case - no value() method required
    std::is_void_v<ValueType> || requires(T t, const T ct) {
        // Access value (throws if contains exception) - proper const correctness
        { t.value() } -> std::same_as<ValueType&>;
        { ct.value() } -> std::same_as<const ValueType&>;
    }
);

// Concept for Future types (matches folly::Future interface)
template<typename F, typename T>
concept future = requires(F f, const F cf) {
    // Get value (blocking) - requires move semantics for proper resource management
    { std::move(f).get() }; // Returns T for folly::Future<T>
    
    // Check if ready - const correctness
    { cf.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout - note: not const because folly::Future::wait is not const
    // folly::Future::wait returns the future itself, so we check if it's convertible to bool
    // or if calling .isReady() on the result is convertible to bool
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
} || requires(F f) {
    // Alternative: wait() returns a future-like object with isReady() method
    { f.wait(std::chrono::milliseconds{}).isReady() } -> std::convertible_to<bool>;
} && (
    // Chain continuation with value - handle void case properly
    // Use then() for compatibility with folly::Future, thenValue() for our wrappers
    std::is_void_v<T> ? 
        (requires(F f2) {
            { std::move(f2).then(std::declval<std::function<void()>>()) };
        } || requires(F f2) {
            { std::move(f2).thenValue(std::declval<std::function<void()>>()) };
        }) : 
        (requires(F f2) {
            { std::move(f2).then(std::declval<std::function<void(T)>>()) };
        } || requires(F f2) {
            { std::move(f2).thenValue(std::declval<std::function<void(T)>>()) };
        })
);

// Concept for SemiPromise types (matches folly::Promise interface since folly doesn't have separate SemiPromise)
template<typename P, typename T>
concept semi_promise = requires(P p, const P cp) {
    // Check if fulfilled - const correctness
    { cp.isFulfilled() } -> std::convertible_to<bool>;
    
    // Set exception - use folly::exception_wrapper for compatibility
    { p.setException(std::declval<folly::exception_wrapper>()) } -> std::same_as<void>;
} && (
    // Set value - handle void specialization properly (folly uses Unit instead of void)
    std::is_void_v<T> ? 
        requires(P p) { 
            { p.setValue(folly::Unit{}) } -> std::same_as<void>; 
        } : 
        requires(P p, T value) { 
            { p.setValue(std::move(value)) } -> std::same_as<void>; 
        }
);

// Concept for Promise types (matches folly::Promise interface)  
template<typename P, typename T>
concept promise = semi_promise<P, T> && requires(P p, const P cp) {
    // Get associated future
    { p.getFuture() }; // Returns folly::Future<T> or similar
    
    // Get associated semi-future  
    { p.getSemiFuture() }; // Returns folly::SemiFuture<T> or similar
};

// Concept for Executor types (matches folly::Executor interface)
template<typename E>
concept executor = requires(E e, const E ce, std::function<void()> func) {
    // Add work to executor - proper parameter handling
    { e.add(std::move(func)) } -> std::same_as<void>;
    
    // Note: getKeepAliveToken is available via folly::getKeepAliveToken(executor) 
    // rather than as a direct method, so we don't require it in the concept
};

// Concept for KeepAlive executor wrapper (matches folly::Executor::KeepAlive)
template<typename K>
concept keep_alive = requires(K k, const K ck) {
    // Get underlying executor - should return a pointer-like type, const correctness
    { ck.get() } -> std::convertible_to<void*>;
    
    // Copy and move semantics - proper template constraint syntax
    requires std::copy_constructible<K>;
    requires std::move_constructible<K>;
    
    // Note: folly::Executor::KeepAlive may not have add method directly
    // Work is typically submitted through the underlying executor
};

// Concept for future factory functions (matches folly::makeFuture, etc.)
template<typename Factory>
concept future_factory = requires() {
    // Make future from value - matches folly::makeFuture signature
    { Factory::makeFuture(std::declval<int>()) } -> future<int>;
    
    // Make future from exception - matches folly::makeExceptionalFuture signature  
    { Factory::template makeExceptionalFuture<int>(std::declval<folly::exception_wrapper>()) } -> future<int>;
    
    // Make ready future - matches folly::makeReadyFuture signature
    { Factory::makeReadyFuture() } -> future<folly::Unit>;
};

// Concept for collective future operations (matches folly::collectAll, collectAny, etc.)
template<typename C>
concept future_collector = requires() {
    // Collect all futures - takes future collections and returns combined results
    { C::collectAll(std::declval<std::vector<Future<int>>>()) };
    
    // Collect any future - returns the first completed future
    { C::collectAny(std::declval<std::vector<Future<int>>>()) };
    
    // Collect any without exception - returns the first successfully completed future
    { C::collectAnyWithoutException(std::declval<std::vector<Future<int>>>()) };
    
    // Collect N futures - collects the first N completed futures
    { C::collectN(std::declval<std::vector<Future<int>>>(), std::size_t{}) };
};

// Concept for future continuation operations (matches folly::Future::via, etc.)
template<typename F, typename T>
concept future_continuation = future<F, T> && requires(F f) {
    // Via executor (using generic executor pointer)
    { f.via(std::declval<void*>()) } -> future<T>;
    
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