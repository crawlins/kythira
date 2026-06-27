#pragma once

/// @file future.hpp
/// @brief C++20 concepts modelling the Folly future/promise interface.
///
/// These concepts serve as the formal interface contract between the Raft core and
/// any future/promise back-end.  The default implementations (`kythira::Future`,
/// `kythira::Promise`, `kythira::Try`) are Folly wrappers defined in `raft/future.hpp`.

#include <chrono>
#include <functional>
#include <type_traits>
#include <vector>
#include <tuple>
#include <exception>

#include <folly/ExceptionWrapper.h>
#include <folly/Unit.h>
#include <folly/Try.h>

namespace kythira {

// Forward declarations
template<typename T> class Try;
template<typename T> class Future;

/// @brief Concept modelling a `folly::Try`-compatible result container.
///
/// A `Try<T>` holds either a value of type `T` or an exception.  For `void`
/// specialisations no `value()` accessor is required.
///
/// @tparam T         Concrete Try type.
/// @tparam ValueType Wrapped value type; use `void` for unit results.
template<typename T, typename ValueType>
concept try_type = requires(T t, const T ct) {
    { t.hasValue() } -> std::convertible_to<bool>;
    { t.hasException() } -> std::convertible_to<bool>;
    { t.exception() };  ///< Returns `folly::exception_wrapper` or equivalent.
} && (std::is_void_v<ValueType> || requires(T t, const T ct) {
                       { t.value() } -> std::same_as<ValueType&>;
                       { ct.value() } -> std::same_as<const ValueType&>;
                   });

/// @brief Concept modelling a `folly::Future`-compatible asynchronous value.
///
/// A `Future<T>` represents a value that may not be available yet.
/// `.get()` blocks until the value is ready or the future contains an exception.
///
/// @tparam F Concrete Future type.
/// @tparam T Wrapped value type.
template<typename F, typename T>
concept future = requires(F f, const F cf) {
    { std::move(f).get() };
    { cf.isReady() } -> std::convertible_to<bool>;
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
} || requires(F f) {
    { f.wait(std::chrono::milliseconds{}).isReady() } -> std::convertible_to<bool>;
};

/// @brief Concept modelling the write-side of a `folly::Promise` without the `getFuture` accessor.
///
/// @tparam P Concrete SemiPromise type.
/// @tparam T Wrapped value type; use `void` for unit results.
template<typename P, typename T>
concept semi_promise = requires(P p, const P cp) {
    { cp.isFulfilled() } -> std::convertible_to<bool>;
    { p.setException(std::declval<folly::exception_wrapper>()) } -> std::same_as<void>;
} && (std::is_void_v<T> ? requires(P p) {
                           { p.setValue(folly::Unit{}) } -> std::same_as<void>;
                       } : requires(P p, T value) {
                           { p.setValue(std::move(value)) } -> std::same_as<void>;
                       });

/// @brief Concept modelling a `folly::Promise`-compatible write-side with associated future access.
///
/// @tparam P Concrete Promise type.
/// @tparam T Wrapped value type.
template<typename P, typename T>
concept promise = semi_promise<P, T> && requires(P p, const P cp) {
    { p.getFuture() };
    { p.getSemiFuture() };
};

/// @brief Concept modelling a `folly::Executor`-compatible work dispatcher.
/// @tparam E Concrete executor type.
template<typename E>
concept executor = requires(E e, const E ce, std::function<void()> func) {
    { e.add(std::move(func)) } -> std::same_as<void>;
};

/// @brief Concept modelling a `folly::Executor::KeepAlive`-compatible reference wrapper.
/// @tparam K Concrete KeepAlive type.
template<typename K>
concept keep_alive = requires(K k, const K ck) {
    { ck.get() } -> std::convertible_to<void*>;
    requires std::copy_constructible<K>;
    requires std::move_constructible<K>;
};

/// @brief Concept modelling a factory that can create ready and exceptional futures.
/// @tparam Factory Concrete factory type.
template<typename Factory>
concept future_factory = requires() {
    { Factory::makeFuture(std::declval<int>()) } -> future<int>;
    {
        Factory::template makeExceptionalFuture<int>(std::declval<folly::exception_wrapper>())
    } -> future<int>;
    { Factory::makeReadyFuture() } -> future<folly::Unit>;
};

/// @brief Concept modelling collective future operations (`collectAll`, `collectAny`, etc.).
/// @tparam C Concrete collector type.
template<typename C>
concept future_collector = requires() {
    { C::collectAll(std::declval<std::vector<Future<int>>>()) };
    { C::collectAny(std::declval<std::vector<Future<int>>>()) };
    { C::collectAnyWithoutException(std::declval<std::vector<Future<int>>>()) };
    { C::collectN(std::declval<std::vector<Future<int>>>(), std::size_t{}) };
};

/// @brief Concept for futures that support continuation scheduling via an executor.
///
/// Extends `future` with `via`, `delay`, and `within`.
///
/// @tparam F Concrete Future type.
/// @tparam T Wrapped value type.
template<typename F, typename T>
concept future_continuation = future<F, T> && requires(F f) {
    { f.via(std::declval<void*>()) } -> future<T>;
    { f.delay(std::chrono::milliseconds{}) } -> future<T>;
    { f.within(std::chrono::milliseconds{}) } -> future<T>;
};

/// @brief Concept for futures that support value and error transformation.
///
/// Extends `future` with `thenValue`, `thenError`, and `ensure`.
///
/// @tparam F Concrete Future type.
/// @tparam T Wrapped value type.
template<typename F, typename T>
concept future_transformable = future<F, T> && requires(F f) {
    { f.thenValue(std::declval<std::function<int(T)>>()) } -> future<int>;
    { f.thenError(std::declval<std::function<T(std::exception_ptr)>>()) } -> future<T>;
    { f.ensure(std::declval<std::function<void()>>()) } -> future<T>;
};

}  // namespace kythira
