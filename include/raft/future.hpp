#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/ExceptionWrapper.h>
#include <folly/Try.h>
#include <folly/Unit.h>

#include "../concepts/future.hpp"

namespace kythira {

//=============================================================================
// Type Conversion Utilities
//=============================================================================

namespace detail {

// Convert folly::exception_wrapper to std::exception_ptr
inline auto to_std_exception_ptr(const folly::exception_wrapper& ew) -> std::exception_ptr {
    if (ew) {
        return ew.to_exception_ptr();
    }
    return nullptr;
}

// Convert std::exception_ptr to folly::exception_wrapper
inline auto to_folly_exception_wrapper(std::exception_ptr ep) -> folly::exception_wrapper {
    if (ep) {
        return folly::exception_wrapper(ep);
    }
    return folly::exception_wrapper();
}

// Void/Unit type mapping for template specializations
template<typename T>
struct void_to_unit {
    using type = T;
};

template<>
struct void_to_unit<void> {
    using type = folly::Unit;
};

template<typename T>
using void_to_unit_t = typename void_to_unit<T>::type;

// Unit/void type mapping for return values
template<typename T>
struct unit_to_void {
    using type = T;
};

template<>
struct unit_to_void<folly::Unit> {
    using type = void;
};

template<typename T>
using unit_to_void_t = typename unit_to_void<T>::type;

// Move semantics optimization helpers
template<typename T>
struct should_move : std::bool_constant<
    std::is_move_constructible_v<T> && 
    !std::is_trivially_copyable_v<T>
> {};

template<typename T>
inline constexpr bool should_move_v = should_move<T>::value;

// Conditional move function - forwards arguments appropriately
template<typename T>
constexpr decltype(auto) conditional_move(T&& value) noexcept {
    return std::forward<T>(value);
}

// Safe type casting utilities
template<typename To, typename From>
constexpr auto safe_cast(From&& from) -> To {
    if constexpr (std::is_same_v<std::decay_t<To>, std::decay_t<From>>) {
        return std::forward<From>(from);
    } else {
        return static_cast<To>(std::forward<From>(from));
    }
}

// Validation utilities
template<typename T>
auto validate_not_null(T* ptr) -> T* {
    if (ptr == nullptr) {
        throw std::invalid_argument("Pointer cannot be null");
    }
    return ptr;
}

template<typename Container>
auto validate_not_empty(const Container& container) -> const Container& {
    if (container.empty()) {
        throw std::invalid_argument("Container cannot be empty");
    }
    return container;
}

// Type trait utilities
template<typename T>
struct is_void_convertible : std::bool_constant<
    std::is_same_v<T, void> || std::is_same_v<T, folly::Unit>
> {};

template<typename T>
inline constexpr bool is_void_convertible_v = is_void_convertible<T>::value;

template<typename T>
struct is_exception_convertible : std::bool_constant<
    std::is_same_v<T, std::exception_ptr> || 
    std::is_same_v<T, folly::exception_wrapper>
> {};

template<typename T>
inline constexpr bool is_exception_convertible_v = is_exception_convertible<T>::value;

} // namespace detail

//=============================================================================
// Forward Declarations
//=============================================================================

template<typename T> class SemiPromise;
template<typename T> class Promise;
template<typename T> class Future;
class Executor;
class KeepAlive;
class FutureFactory;
class FutureCollector;

//=============================================================================
// Try Wrapper Class
//=============================================================================

/**
 * @brief Try wrapper that adapts folly::Try to satisfy try_type concept
 * 
 * This class provides a unified interface for handling values and exceptions,
 * adapting folly::Try to work with the kythira concept system.
 * 
 * @tparam T The value type (can be void)
 */
template<typename T>
class Try {
public:
    using value_type = T;
    using folly_type = folly::Try<detail::void_to_unit_t<T>>;
    
    // Constructors
    Try() = default;
    explicit Try(folly_type ft) : _folly_try(std::move(ft)) {}
    
    // Construct from value
    template<typename U = T>
    explicit Try(U&& value) 
        requires(!std::is_void_v<T>)
        : _folly_try(std::forward<U>(value)) {}
    
    // Construct from exception
    explicit Try(folly::exception_wrapper ex) : _folly_try(std::move(ex)) {}
    explicit Try(std::exception_ptr ex) : _folly_try(detail::to_folly_exception_wrapper(ex)) {}
    
    // Access value (throws if contains exception)
    template<typename U = T>
    auto value() -> T& requires(!std::is_void_v<U>) {
        return _folly_try.value();
    }
    
    template<typename U = T>
    auto value() const -> const T& requires(!std::is_void_v<U>) {
        return _folly_try.value();
    }
    
    // Access exception - convert folly::exception_wrapper to std::exception_ptr
    auto exception() const -> std::exception_ptr {
        if (_folly_try.hasException()) {
            return detail::to_std_exception_ptr(_folly_try.exception());
        }
        return nullptr;
    }
    
    // Check if contains value (folly naming for concept compliance)
    auto hasValue() const -> bool {
        return _folly_try.hasValue();
    }
    
    // Check if contains exception (folly naming for concept compliance)
    auto hasException() const -> bool {
        return _folly_try.hasException();
    }
    
    // Legacy methods for backward compatibility
    auto has_value() const -> bool { return hasValue(); }
    auto has_exception() const -> bool { return hasException(); }
    
    // Get underlying folly::Try
    auto get_folly_try() const -> const folly_type& {
        return _folly_try;
    }
    
    auto get_folly_try() -> folly_type& {
        return _folly_try;
    }

private:
    folly_type _folly_try;
};

// Specialization for void
template<>
class Try<void> {
public:
    using value_type = void;
    using folly_type = folly::Try<folly::Unit>;
    
    // Constructors
    Try() : _folly_try(folly::Unit{}) {}  // Initialize with Unit value
    explicit Try(folly_type ft) : _folly_try(std::move(ft)) {}
    
    // Construct from Unit
    explicit Try(folly::Unit) : _folly_try(folly::Unit{}) {}
    
    // Construct from exception
    explicit Try(folly::exception_wrapper ex) : _folly_try(std::move(ex)) {}
    explicit Try(std::exception_ptr ex) : _folly_try(detail::to_folly_exception_wrapper(ex)) {}
    
    // No value() method for void specialization
    
    // Access exception
    auto exception() const -> std::exception_ptr {
        if (_folly_try.hasException()) {
            return detail::to_std_exception_ptr(_folly_try.exception());
        }
        return nullptr;
    }
    
    // Check if contains value (folly naming for concept compliance)
    auto hasValue() const -> bool {
        return _folly_try.hasValue();
    }
    
    // Check if contains exception (folly naming for concept compliance)
    auto hasException() const -> bool {
        return _folly_try.hasException();
    }
    
    // Legacy methods for backward compatibility
    auto has_value() const -> bool { return hasValue(); }
    auto has_exception() const -> bool { return hasException(); }
    
    // Get underlying folly::Try
    auto get_folly_try() const -> const folly_type& {
        return _folly_try;
    }
    
    auto get_folly_try() -> folly_type& {
        return _folly_try;
    }

private:
    folly_type _folly_try;
};

//=============================================================================
// SemiPromise Wrapper Class
//=============================================================================

/**
 * @brief SemiPromise wrapper that adapts folly::Promise to satisfy semi_promise concept
 * 
 * This class provides the basic promise functionality for setting values and exceptions.
 * It wraps folly::Promise and provides the interface required by the semi_promise concept.
 * 
 * @tparam T The value type (can be void)
 */
template<typename T>
class SemiPromise {
public:
    using value_type = T;
    using folly_type = folly::Promise<detail::void_to_unit_t<T>>;
    
    // Constructors
    SemiPromise() = default;
    explicit SemiPromise(folly_type fp) : _folly_promise(std::move(fp)) {}
    
    // Move semantics
    SemiPromise(SemiPromise&&) = default;
    SemiPromise& operator=(SemiPromise&&) = default;
    
    // No copy semantics (folly::Promise is move-only)
    SemiPromise(const SemiPromise&) = delete;
    SemiPromise& operator=(const SemiPromise&) = delete;
    
    // Set value
    template<typename U = T>
    auto setValue(U&& value) -> void requires(!std::is_void_v<T>) {
        _folly_promise.setValue(std::forward<U>(value));
    }
    
    // Set exception using folly::exception_wrapper (for concept compliance)
    auto setException(folly::exception_wrapper ex) -> void {
        if (!ex) {
            throw std::invalid_argument("Exception wrapper cannot be empty");
        }
        _folly_promise.setException(std::move(ex));
    }
    
    // Set exception using std::exception_ptr (convenience method)
    auto setException(std::exception_ptr ex) -> void {
        if (!ex) {
            throw std::invalid_argument("Exception pointer cannot be null");
        }
        _folly_promise.setException(detail::to_folly_exception_wrapper(ex));
    }
    
    // Check if fulfilled (for concept compliance)
    auto isFulfilled() const -> bool {
        return _folly_promise.isFulfilled();
    }
    
    // Get underlying folly::Promise
    auto get_folly_promise() -> folly_type& {
        return _folly_promise;
    }
    
    auto get_folly_promise() const -> const folly_type& {
        return _folly_promise;
    }

protected:
    folly_type _folly_promise;
};

// Specialization for void
template<>
class SemiPromise<void> {
public:
    using value_type = void;
    using folly_type = folly::Promise<folly::Unit>;
    
    // Constructors
    SemiPromise() = default;
    explicit SemiPromise(folly_type fp) : _folly_promise(std::move(fp)) {}
    
    // Move semantics
    SemiPromise(SemiPromise&&) = default;
    SemiPromise& operator=(SemiPromise&&) = default;
    
    // No copy semantics
    SemiPromise(const SemiPromise&) = delete;
    SemiPromise& operator=(const SemiPromise&) = delete;
    
    // Set value using folly::Unit (for concept compliance)
    auto setValue(folly::Unit) -> void {
        _folly_promise.setValue(folly::Unit{});
    }
    
    // Convenience method for void
    auto setValue() -> void {
        _folly_promise.setValue(folly::Unit{});
    }
    
    // Set exception using folly::exception_wrapper (for concept compliance)
    auto setException(folly::exception_wrapper ex) -> void {
        if (!ex) {
            throw std::invalid_argument("Exception wrapper cannot be empty");
        }
        _folly_promise.setException(std::move(ex));
    }
    
    // Set exception using std::exception_ptr (convenience method)
    auto setException(std::exception_ptr ex) -> void {
        if (!ex) {
            throw std::invalid_argument("Exception pointer cannot be null");
        }
        _folly_promise.setException(detail::to_folly_exception_wrapper(ex));
    }
    
    // Check if fulfilled (for concept compliance)
    auto isFulfilled() const -> bool {
        return _folly_promise.isFulfilled();
    }
    
    // Get underlying folly::Promise
    auto get_folly_promise() -> folly_type& {
        return _folly_promise;
    }
    
    auto get_folly_promise() const -> const folly_type& {
        return _folly_promise;
    }

protected:
    folly_type _folly_promise;
};

//=============================================================================
// Promise Wrapper Class
//=============================================================================

/**
 * @brief Promise wrapper that extends SemiPromise to satisfy promise concept
 * 
 * This class extends SemiPromise with the ability to retrieve associated futures.
 * It provides both getFuture() and getSemiFuture() methods as required by the concept.
 * 
 * @tparam T The value type (can be void)
 */
template<typename T>
class Promise : public SemiPromise<T> {
public:
    using value_type = T;
    using base_type = SemiPromise<T>;
    using folly_type = typename base_type::folly_type;
    
    // Constructors
    Promise() = default;
    explicit Promise(folly_type fp) : base_type(std::move(fp)) {}
    
    // Move semantics
    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;
    
    // No copy semantics
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    
    // Get associated future (for concept compliance)
    auto getFuture() -> Future<T> {
        if constexpr (std::is_void_v<T>) {
            return Future<void>(this->_folly_promise.getFuture());
        } else {
            return Future<T>(this->_folly_promise.getFuture());
        }
    }
    
    // Get associated semi-future (for concept compliance)
    auto getSemiFuture() -> Future<T> {
        return Future<T>(this->_folly_promise.getSemiFuture().toUnsafeFuture());
    }
};

// Specialization for void is handled by the template above with constexpr if

//=============================================================================
// Future Wrapper Class (Enhanced)
//=============================================================================

/**
 * @brief Future wrapper that adapts folly::Future to satisfy future concept
 * 
 * This class provides a comprehensive wrapper around folly::Future with support for
 * continuation operations, transformation operations, and collective operations.
 * It handles void/Unit conversions transparently.
 * 
 * @tparam T The value type (can be void)
 */
template<typename T>
class Future {
public:
    using value_type = T;
    using folly_type = folly::Future<detail::void_to_unit_t<T>>;
    
    // Constructors
    Future() = default;
    explicit Future(folly_type ff) : _folly_future(std::move(ff)) {}
    
    // Construct from value
    template<typename U = T>
    explicit Future(U&& value) requires(!std::is_void_v<T>)
        : _folly_future(folly::makeFuture(std::forward<U>(value))) {}
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex) 
        : _folly_future(folly::makeFuture<detail::void_to_unit_t<T>>(std::move(ex))) {}
    
    // Construct from std::exception_ptr
    explicit Future(std::exception_ptr ex) 
        : _folly_future(folly::makeFuture<detail::void_to_unit_t<T>>(detail::to_folly_exception_wrapper(ex))) {}
    
    // Get value (blocking) - concept compliance
    auto get() -> T requires(!std::is_void_v<T>) {
        return std::move(_folly_future).get();
    }
    
    // Chain continuation with value (concept compliance)
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>> requires(!std::is_void_v<T>) {
        using ReturnType = std::invoke_result_t<F, T>;
        if constexpr (std::is_void_v<ReturnType>) {
            return Future<void>(std::move(_folly_future).thenValue([func = std::forward<F>(func)](T value) {
                func(std::move(value));
                return folly::Unit{};
            }));
        } else {
            return Future<ReturnType>(std::move(_folly_future).thenValue(std::forward<F>(func)));
        }
    }
    
    // Error handling (concept compliance)
    template<typename F>
    auto thenError(F&& func) -> Future<T> {
        return Future<T>(std::move(_folly_future).thenError([func = std::forward<F>(func)](folly::exception_wrapper ex) {
            if constexpr (std::is_void_v<T>) {
                func(detail::to_std_exception_ptr(ex));
                return folly::Unit{};
            } else {
                return func(detail::to_std_exception_ptr(ex));
            }
        }));
    }
    
    // Ensure (cleanup functionality)
    template<typename F>
    auto ensure(F&& func) -> Future<T> {
        return Future<T>(std::move(_folly_future).ensure(std::forward<F>(func)));
    }
    
    // Continuation operations
    auto via(void* executor) -> Future<T> {
        return Future<T>(std::move(_folly_future).via(static_cast<folly::Executor*>(executor)));
    }
    
    auto via(folly::Executor* executor) -> Future<T> {
        return Future<T>(std::move(_folly_future).via(executor));
    }
    
    auto via(folly::Executor& executor) -> Future<T> {
        return Future<T>(std::move(_folly_future).via(&executor));
    }
    
    auto delay(std::chrono::milliseconds duration) -> Future<T> {
        return Future<T>(std::move(_folly_future).delayed(duration));
    }
    
    auto within(std::chrono::milliseconds timeout) -> Future<T> {
        return Future<T>(std::move(_folly_future).within(timeout));
    }
    
    // Check if ready (concept compliance)
    auto isReady() const -> bool {
        return _folly_future.isReady();
    }
    
    // Wait with timeout (concept compliance)
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _folly_future.wait(timeout).isReady();
    }
    
    // Legacy methods for backward compatibility
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F, T>> {
        return thenValue(std::forward<F>(func));
    }
    
    template<typename F>
    auto onError(F&& func) -> Future<T> {
        return thenError(std::forward<F>(func));
    }
    
    // Get underlying folly::Future
    auto get_folly_future() && -> folly_type {
        return std::move(_folly_future);
    }

private:
    folly_type _folly_future;
};

// Specialization for void
template<>
class Future<void> {
public:
    using value_type = void;
    using folly_type = folly::Future<folly::Unit>;
    
    // Constructors
    Future() : _folly_future(folly::makeFuture(folly::Unit{})) {}
    explicit Future(folly_type ff) : _folly_future(std::move(ff)) {}
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex) 
        : _folly_future(folly::makeFuture<folly::Unit>(std::move(ex))) {}
    
    explicit Future(std::exception_ptr ex) 
        : _folly_future(folly::makeFuture<folly::Unit>(detail::to_folly_exception_wrapper(ex))) {}
    
    // Get value (blocking) - concept compliance
    auto get() -> void {
        std::move(_folly_future).get();
    }
    
    // Chain continuation with value (concept compliance)
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F>> {
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
    
    // Error handling (concept compliance)
    template<typename F>
    auto thenError(F&& func) -> Future<void> {
        return Future<void>(std::move(_folly_future).thenError([func = std::forward<F>(func)](folly::exception_wrapper ex) {
            func(detail::to_std_exception_ptr(ex));
            return folly::Unit{};
        }));
    }
    
    // Ensure (cleanup functionality)
    template<typename F>
    auto ensure(F&& func) -> Future<void> {
        return Future<void>(std::move(_folly_future).ensure(std::forward<F>(func)));
    }
    
    // Continuation operations
    auto via(void* executor) -> Future<void> {
        return Future<void>(std::move(_folly_future).via(static_cast<folly::Executor*>(executor)));
    }
    
    auto via(folly::Executor* executor) -> Future<void> {
        return Future<void>(std::move(_folly_future).via(executor));
    }
    
    auto via(folly::Executor& executor) -> Future<void> {
        return Future<void>(std::move(_folly_future).via(&executor));
    }
    
    auto delay(std::chrono::milliseconds duration) -> Future<void> {
        return Future<void>(std::move(_folly_future).delayed(duration));
    }
    
    auto within(std::chrono::milliseconds timeout) -> Future<void> {
        return Future<void>(std::move(_folly_future).within(timeout));
    }
    
    // Check if ready (concept compliance)
    auto isReady() const -> bool {
        return _folly_future.isReady();
    }
    
    // Wait with timeout (concept compliance)
    auto wait(std::chrono::milliseconds timeout) -> bool {
        return _folly_future.wait(timeout).isReady();
    }
    
    // Legacy methods for backward compatibility
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F>> {
        return thenValue(std::forward<F>(func));
    }
    
    template<typename F>
    auto onError(F&& func) -> Future<void> {
        return thenError(std::forward<F>(func));
    }
    
    // Get underlying folly::Future
    auto get_folly_future() && -> folly_type {
        return std::move(_folly_future);
    }

private:
    folly_type _folly_future;
};

//=============================================================================
// Executor Wrapper Class
//=============================================================================

/**
 * @brief Executor wrapper that adapts folly::Executor to satisfy executor concept
 * 
 * This class provides a wrapper around folly::Executor pointer with proper
 * lifetime management and null pointer handling.
 */
class Executor {
public:
    using folly_type = folly::Executor*;
    
    // Constructors
    Executor() : _executor(nullptr) {} // Default constructor creates invalid executor
    explicit Executor(folly::Executor* executor) : _executor(executor) {
        if (executor == nullptr) {
            throw std::invalid_argument("Executor cannot be null");
        }
    }
    
    // Move semantics
    Executor(Executor&&) = default;
    Executor& operator=(Executor&&) = default;
    
    // Copy semantics (safe since we only store pointer)
    Executor(const Executor&) = default;
    Executor& operator=(const Executor&) = default;
    
    // Add work to executor (concept compliance)
    template<typename F>
    auto add(F&& func) -> void {
        if (!_executor) {
            throw std::runtime_error("Executor is invalid");
        }
        _executor->add(std::forward<F>(func));
    }
    
    // Check if executor is valid
    auto is_valid() const -> bool {
        return _executor != nullptr;
    }
    
    // Get underlying executor
    auto get() const -> folly::Executor* {
        return _executor;
    }
    
    // Get KeepAlive token
    auto getKeepAliveToken() -> KeepAlive;
    
    // Legacy method name for backward compatibility
    auto get_keep_alive() -> KeepAlive;

private:
    folly::Executor* _executor;
};

//=============================================================================
// KeepAlive Wrapper Class
//=============================================================================

/**
 * @brief KeepAlive wrapper that adapts folly::Executor::KeepAlive to satisfy keep_alive concept
 * 
 * This class provides a wrapper around folly::Executor::KeepAlive with proper
 * reference counting and pointer-like access.
 */
class KeepAlive {
public:
    using folly_type = folly::Executor::KeepAlive<>;
    
    // Constructors
    KeepAlive() = default;
    explicit KeepAlive(folly_type ka) : _keep_alive(std::move(ka)) {}
    explicit KeepAlive(folly::Executor* executor) : _keep_alive(folly::getKeepAliveToken(executor)) {}
    
    // Copy and move semantics (folly::KeepAlive supports both)
    KeepAlive(const KeepAlive&) = default;
    KeepAlive& operator=(const KeepAlive&) = default;
    KeepAlive(KeepAlive&&) = default;
    KeepAlive& operator=(KeepAlive&&) = default;
    
    // Get underlying executor (concept compliance)
    auto get() const -> folly::Executor* {
        return _keep_alive.get();
    }
    
    // Add work to underlying executor (for convenience)
    template<typename F>
    auto add(F&& func) -> void {
        if (auto* executor = _keep_alive.get()) {
            executor->add(std::forward<F>(func));
        } else {
            throw std::runtime_error("KeepAlive is invalid");
        }
    }
    
    // Check if valid
    auto is_valid() const -> bool {
        return _keep_alive.get() != nullptr;
    }
    
    // Get underlying KeepAlive
    auto get_folly_keep_alive() const -> const folly_type& {
        return _keep_alive;
    }
    
    auto get_folly_keep_alive() -> folly_type& {
        return _keep_alive;
    }

private:
    folly_type _keep_alive;
};

// Implementation of Executor methods that depend on KeepAlive
inline auto Executor::getKeepAliveToken() -> KeepAlive {
    return KeepAlive(_executor);
}

inline auto Executor::get_keep_alive() -> KeepAlive {
    if (!_executor) {
        throw std::runtime_error("Executor is invalid");
    }
    return KeepAlive(_executor);
}

//=============================================================================
// FutureFactory Static Class
//=============================================================================

/**
 * @brief FutureFactory static class that satisfies future_factory concept
 * 
 * This class provides static factory methods for creating futures from values,
 * exceptions, and ready states. It handles type deduction and conversion.
 */
class FutureFactory {
public:
    // Delete all constructors and assignment operators to make it static-only
    FutureFactory() = delete;
    FutureFactory(const FutureFactory&) = delete;
    FutureFactory(FutureFactory&&) = delete;
    FutureFactory& operator=(const FutureFactory&) = delete;
    FutureFactory& operator=(FutureFactory&&) = delete;
    
    // Make future from value (concept compliance)
    template<typename T>
    static auto makeFuture(T&& value) -> Future<std::decay_t<T>> {
        using ValueType = std::decay_t<T>;
        if constexpr (std::is_void_v<ValueType>) {
            return Future<void>(folly::makeFuture(folly::Unit{}));
        } else {
            return Future<ValueType>(folly::makeFuture(std::forward<T>(value)));
        }
    }
    
    // Make future with no arguments (void future)
    static auto makeFuture() -> Future<void> {
        return Future<void>(folly::makeFuture(folly::Unit{}));
    }
    
    // Make exceptional future (concept compliance)
    template<typename T>
    static auto makeExceptionalFuture(folly::exception_wrapper ex) -> Future<T> {
        if constexpr (std::is_void_v<T>) {
            return Future<void>(folly::makeFuture<folly::Unit>(std::move(ex)));
        } else {
            return Future<T>(folly::makeFuture<T>(std::move(ex)));
        }
    }
    
    // Make exceptional future from std::exception_ptr
    template<typename T>
    static auto makeExceptionalFuture(std::exception_ptr ex) -> Future<T> {
        return makeExceptionalFuture<T>(detail::to_folly_exception_wrapper(ex));
    }
    
    // Make ready future (concept compliance)
    static auto makeReadyFuture() -> Future<folly::Unit> {
        return Future<folly::Unit>(folly::makeFuture(folly::Unit{}));
    }
    
    // Make ready future with value
    template<typename T>
    static auto makeReadyFuture(T&& value) -> Future<std::decay_t<T>> {
        return makeFuture(std::forward<T>(value));
    }
    
    // Make ready future with Unit (for concept compliance)
    static auto makeFuture(folly::Unit) -> Future<void> {
        return Future<void>(folly::makeFuture(folly::Unit{}));
    }
};

//=============================================================================
// FutureCollector Static Class
//=============================================================================

/**
 * @brief FutureCollector static class that satisfies future_collector concept
 * 
 * This class provides static methods for collective future operations including
 * collectAll, collectAny, collectAnyWithoutException, and collectN.
 */
class FutureCollector {
public:
    // Delete all constructors and assignment operators to make it static-only
    FutureCollector() = delete;
    FutureCollector(const FutureCollector&) = delete;
    FutureCollector(FutureCollector&&) = delete;
    FutureCollector& operator=(const FutureCollector&) = delete;
    FutureCollector& operator=(FutureCollector&&) = delete;
    // Collect all futures (concept compliance)
    template<typename T>
    static auto collectAll(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
        // Convert our Future wrappers to folly::Future
        std::vector<folly::Future<detail::void_to_unit_t<T>>> folly_futures;
        folly_futures.reserve(futures.size());
        for (auto& fut : futures) {
            folly_futures.push_back(std::move(fut).get_folly_future());
        }
        
        // Use folly::collectAll
        auto result_future = folly::collectAll(folly_futures.begin(), folly_futures.end())
            .toUnsafeFuture()
            .thenValue([](std::vector<folly::Try<detail::void_to_unit_t<T>>> results) {
                std::vector<Try<T>> wrapped_results;
                wrapped_results.reserve(results.size());
                for (auto& result : results) {
                    if constexpr (std::is_void_v<T>) {
                        wrapped_results.push_back(Try<void>(std::move(result)));
                    } else {
                        wrapped_results.push_back(Try<T>(std::move(result)));
                    }
                }
                return wrapped_results;
            });
        
        return Future<std::vector<Try<T>>>(std::move(result_future));
    }
    
    // Collect any future (concept compliance)
    template<typename T>
    static auto collectAny(std::vector<Future<T>> futures) -> Future<std::tuple<std::size_t, Try<T>>> {
        // Handle empty vector case
        if (futures.empty()) {
            return FutureFactory::makeExceptionalFuture<std::tuple<std::size_t, Try<T>>>(
                folly::exception_wrapper(std::invalid_argument("collectAny requires at least one future"))
            );
        }
        
        // Convert our Future wrappers to folly::Future
        std::vector<folly::Future<detail::void_to_unit_t<T>>> folly_futures;
        folly_futures.reserve(futures.size());
        for (auto& fut : futures) {
            folly_futures.push_back(std::move(fut).get_folly_future());
        }
        
        // Use folly::collectAny
        auto result_future = folly::collectAny(folly_futures.begin(), folly_futures.end())
            .toUnsafeFuture()
            .thenValue([](std::pair<std::size_t, folly::Try<detail::void_to_unit_t<T>>> result) {
                if constexpr (std::is_void_v<T>) {
                    return std::make_tuple(result.first, Try<void>(std::move(result.second)));
                } else {
                    return std::make_tuple(result.first, Try<T>(std::move(result.second)));
                }
            });
        
        return Future<std::tuple<std::size_t, Try<T>>>(std::move(result_future));
    }
    
    // Collect any without exception (concept compliance)
    template<typename T>
    static auto collectAnyWithoutException(std::vector<Future<T>> futures) -> auto {
        // Handle empty vector case
        if (futures.empty()) {
            if constexpr (std::is_void_v<T>) {
                return FutureFactory::makeExceptionalFuture<std::size_t>(
                    folly::exception_wrapper(std::invalid_argument("collectAnyWithoutException requires at least one future"))
                );
            } else {
                return FutureFactory::makeExceptionalFuture<std::tuple<std::size_t, T>>(
                    folly::exception_wrapper(std::invalid_argument("collectAnyWithoutException requires at least one future"))
                );
            }
        }
        
        // Convert our Future wrappers to folly::Future
        std::vector<folly::Future<detail::void_to_unit_t<T>>> folly_futures;
        folly_futures.reserve(futures.size());
        for (auto& fut : futures) {
            folly_futures.push_back(std::move(fut).get_folly_future());
        }
        
        if constexpr (std::is_void_v<T>) {
            // For void futures, return just the index
            auto result_future = folly::collectAnyWithoutException(folly_futures.begin(), folly_futures.end())
                .toUnsafeFuture()
                .thenValue([](std::pair<std::size_t, folly::Unit> result) {
                    return result.first; // Return just the index
                });
            
            return Future<std::size_t>(std::move(result_future));
        } else {
            // For non-void futures, return tuple with index and value
            auto result_future = folly::collectAnyWithoutException(folly_futures.begin(), folly_futures.end())
                .toUnsafeFuture()
                .thenValue([](std::pair<std::size_t, T> result) {
                    return std::make_tuple(result.first, std::move(result.second));
                });
            
            return Future<std::tuple<std::size_t, T>>(std::move(result_future));
        }
    }
    
    // Collect N futures (concept compliance)
    template<typename T>
    static auto collectN(std::vector<Future<T>> futures, std::size_t n) -> Future<std::vector<std::tuple<std::size_t, Try<T>>>> {
        // Handle edge cases
        if (n > futures.size()) {
            return FutureFactory::makeExceptionalFuture<std::vector<std::tuple<std::size_t, Try<T>>>>(
                folly::exception_wrapper(std::invalid_argument("collectN: n cannot be greater than futures.size()"))
            );
        }
        
        if (n == 0) {
            // Return empty result immediately
            std::vector<std::tuple<std::size_t, Try<T>>> empty_result;
            return FutureFactory::makeFuture(std::move(empty_result));
        }
        
        // Convert our Future wrappers to folly::Future
        std::vector<folly::Future<detail::void_to_unit_t<T>>> folly_futures;
        folly_futures.reserve(futures.size());
        for (auto& fut : futures) {
            folly_futures.push_back(std::move(fut).get_folly_future());
        }
        
        // Use folly::collectN
        auto result_future = folly::collectN(folly_futures.begin(), folly_futures.end(), n)
            .toUnsafeFuture()
            .thenValue([](std::vector<std::pair<std::size_t, folly::Try<detail::void_to_unit_t<T>>>> results) {
                std::vector<std::tuple<std::size_t, Try<T>>> wrapped_results;
                wrapped_results.reserve(results.size());
                for (auto& result : results) {
                    if constexpr (std::is_void_v<T>) {
                        wrapped_results.push_back(std::make_tuple(result.first, Try<void>(std::move(result.second))));
                    } else {
                        wrapped_results.push_back(std::make_tuple(result.first, Try<T>(std::move(result.second))));
                    }
                }
                return wrapped_results;
            });
        
        return Future<std::vector<std::tuple<std::size_t, Try<T>>>>(std::move(result_future));
    }
};

//=============================================================================
// Legacy Collective Operations (for backward compatibility)
//=============================================================================

// Wait for any future to complete (modeled after folly::collectAny)
// Returns a future of tuple containing the index and Try<T> of the first completed future
template<typename T>
auto wait_for_any(std::vector<Future<T>> futures) -> Future<std::tuple<std::size_t, Try<T>>> {
    return FutureCollector::collectAny(std::move(futures));
}

// Wait for all futures to complete (modeled after folly::collectAll)
// Returns a future of vector containing Try<T> for each future (preserving order)
template<typename T>
auto wait_for_all(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>> {
    return FutureCollector::collectAll(std::move(futures));
}

} // namespace kythira

//=============================================================================
// Static Assertions for Concept Compliance
//=============================================================================

// Comprehensive concept compliance validation for all wrapper types
// Requirements 7.1, 7.2: Validate all wrappers satisfy their corresponding concepts at compile time

//-----------------------------------------------------------------------------
// Try Wrapper Concept Compliance
//-----------------------------------------------------------------------------

// Basic types
static_assert(kythira::try_type<kythira::Try<int>, int>, 
              "kythira::Try<int> must satisfy try_type concept");
static_assert(kythira::try_type<kythira::Try<void>, void>, 
              "kythira::Try<void> must satisfy try_type concept");

// Additional value types for comprehensive validation
static_assert(kythira::try_type<kythira::Try<std::string>, std::string>, 
              "kythira::Try<std::string> must satisfy try_type concept");
static_assert(kythira::try_type<kythira::Try<double>, double>, 
              "kythira::Try<double> must satisfy try_type concept");
static_assert(kythira::try_type<kythira::Try<std::vector<int>>, std::vector<int>>, 
              "kythira::Try<std::vector<int>> must satisfy try_type concept");

// Custom types
struct TestStruct { int value; };
static_assert(kythira::try_type<kythira::Try<TestStruct>, TestStruct>, 
              "kythira::Try<TestStruct> must satisfy try_type concept");

//-----------------------------------------------------------------------------
// SemiPromise Wrapper Concept Compliance
//-----------------------------------------------------------------------------

// Basic types
static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>, 
              "kythira::SemiPromise<int> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<kythira::SemiPromise<void>, void>, 
              "kythira::SemiPromise<void> must satisfy semi_promise concept");

// Additional value types
static_assert(kythira::semi_promise<kythira::SemiPromise<std::string>, std::string>, 
              "kythira::SemiPromise<std::string> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<kythira::SemiPromise<double>, double>, 
              "kythira::SemiPromise<double> must satisfy semi_promise concept");
static_assert(kythira::semi_promise<kythira::SemiPromise<TestStruct>, TestStruct>, 
              "kythira::SemiPromise<TestStruct> must satisfy semi_promise concept");

//-----------------------------------------------------------------------------
// Promise Wrapper Concept Compliance
//-----------------------------------------------------------------------------

// Basic types
static_assert(kythira::promise<kythira::Promise<int>, int>, 
              "kythira::Promise<int> must satisfy promise concept");
static_assert(kythira::promise<kythira::Promise<void>, void>, 
              "kythira::Promise<void> must satisfy promise concept");

// Additional value types
static_assert(kythira::promise<kythira::Promise<std::string>, std::string>, 
              "kythira::Promise<std::string> must satisfy promise concept");
static_assert(kythira::promise<kythira::Promise<double>, double>, 
              "kythira::Promise<double> must satisfy promise concept");
static_assert(kythira::promise<kythira::Promise<TestStruct>, TestStruct>, 
              "kythira::Promise<TestStruct> must satisfy promise concept");

//-----------------------------------------------------------------------------
// Future Wrapper Concept Compliance
//-----------------------------------------------------------------------------

// Basic types
static_assert(kythira::future<kythira::Future<int>, int>, 
              "kythira::Future<int> must satisfy future concept");
static_assert(kythira::future<kythira::Future<void>, void>, 
              "kythira::Future<void> must satisfy future concept");

// Additional value types for comprehensive validation
static_assert(kythira::future<kythira::Future<std::string>, std::string>, 
              "kythira::Future<std::string> must satisfy future concept");
static_assert(kythira::future<kythira::Future<double>, double>, 
              "kythira::Future<double> must satisfy future concept");
static_assert(kythira::future<kythira::Future<std::vector<int>>, std::vector<int>>, 
              "kythira::Future<std::vector<int>> must satisfy future concept");
static_assert(kythira::future<kythira::Future<TestStruct>, TestStruct>, 
              "kythira::Future<TestStruct> must satisfy future concept");

// Pointer types
static_assert(kythira::future<kythira::Future<std::unique_ptr<int>>, std::unique_ptr<int>>, 
              "kythira::Future<std::unique_ptr<int>> must satisfy future concept");
static_assert(kythira::future<kythira::Future<std::shared_ptr<std::string>>, std::shared_ptr<std::string>>, 
              "kythira::Future<std::shared_ptr<std::string>> must satisfy future concept");

//-----------------------------------------------------------------------------
// Executor Wrapper Concept Compliance
//-----------------------------------------------------------------------------

static_assert(kythira::executor<kythira::Executor>, 
              "kythira::Executor must satisfy executor concept");

//-----------------------------------------------------------------------------
// KeepAlive Wrapper Concept Compliance
//-----------------------------------------------------------------------------

static_assert(kythira::keep_alive<kythira::KeepAlive>, 
              "kythira::KeepAlive must satisfy keep_alive concept");

//-----------------------------------------------------------------------------
// Factory Operations Concept Compliance
//-----------------------------------------------------------------------------

static_assert(kythira::future_factory<kythira::FutureFactory>, 
              "kythira::FutureFactory must satisfy future_factory concept");

//-----------------------------------------------------------------------------
// Collector Operations Concept Compliance
//-----------------------------------------------------------------------------

static_assert(kythira::future_collector<kythira::FutureCollector>, 
              "kythira::FutureCollector must satisfy future_collector concept");

//-----------------------------------------------------------------------------
// Continuation Operations Concept Compliance
//-----------------------------------------------------------------------------

// Basic types
static_assert(kythira::future_continuation<kythira::Future<int>, int>, 
              "kythira::Future<int> must satisfy future_continuation concept");
static_assert(kythira::future_continuation<kythira::Future<void>, void>, 
              "kythira::Future<void> must satisfy future_continuation concept");

// Additional value types
static_assert(kythira::future_continuation<kythira::Future<std::string>, std::string>, 
              "kythira::Future<std::string> must satisfy future_continuation concept");
static_assert(kythira::future_continuation<kythira::Future<double>, double>, 
              "kythira::Future<double> must satisfy future_continuation concept");
static_assert(kythira::future_continuation<kythira::Future<TestStruct>, TestStruct>, 
              "kythira::Future<TestStruct> must satisfy future_continuation concept");

//-----------------------------------------------------------------------------
// Transformation Operations Concept Compliance
//-----------------------------------------------------------------------------

// Basic types (excluding void due to concept definition issues)
static_assert(kythira::future_transformable<kythira::Future<int>, int>, 
              "kythira::Future<int> must satisfy future_transformable concept");

// Additional value types
static_assert(kythira::future_transformable<kythira::Future<std::string>, std::string>, 
              "kythira::Future<std::string> must satisfy future_transformable concept");
static_assert(kythira::future_transformable<kythira::Future<double>, double>, 
              "kythira::Future<double> must satisfy future_transformable concept");
static_assert(kythira::future_transformable<kythira::Future<TestStruct>, TestStruct>, 
              "kythira::Future<TestStruct> must satisfy future_transformable concept");

//-----------------------------------------------------------------------------
// Type Conversion Utilities Validation
//-----------------------------------------------------------------------------

// Validate void/Unit type mapping utilities
static_assert(std::is_same_v<kythira::detail::void_to_unit_t<void>, folly::Unit>, 
              "void_to_unit_t<void> must map to folly::Unit");
static_assert(std::is_same_v<kythira::detail::void_to_unit_t<int>, int>, 
              "void_to_unit_t<int> must map to int");
static_assert(std::is_same_v<kythira::detail::void_to_unit_t<std::string>, std::string>, 
              "void_to_unit_t<std::string> must map to std::string");

// Validate Unit/void type mapping utilities
static_assert(std::is_same_v<kythira::detail::unit_to_void_t<folly::Unit>, void>, 
              "unit_to_void_t<folly::Unit> must map to void");
static_assert(std::is_same_v<kythira::detail::unit_to_void_t<int>, int>, 
              "unit_to_void_t<int> must map to int");
static_assert(std::is_same_v<kythira::detail::unit_to_void_t<std::string>, std::string>, 
              "unit_to_void_t<std::string> must map to std::string");

//-----------------------------------------------------------------------------
// Template Instantiation Validation
//-----------------------------------------------------------------------------

// Ensure wrapper types can be instantiated with various template parameters
namespace concept_validation_tests {
    // Test template instantiation with different types
    template<typename T>
    constexpr bool test_wrapper_instantiation() {
        // Test that all wrapper types can be instantiated (except Future which needs explicit construction)
        static_assert(std::is_constructible_v<kythira::Try<T>>, 
                      "Try<T> must be constructible");
        static_assert(std::is_constructible_v<kythira::SemiPromise<T>>, 
                      "SemiPromise<T> must be constructible");
        static_assert(std::is_constructible_v<kythira::Promise<T>>, 
                      "Promise<T> must be constructible");
        
        // Test move semantics
        static_assert(std::is_move_constructible_v<kythira::Try<T>>, 
                      "Try<T> must be move constructible");
        static_assert(std::is_move_constructible_v<kythira::SemiPromise<T>>, 
                      "SemiPromise<T> must be move constructible");
        static_assert(std::is_move_constructible_v<kythira::Promise<T>>, 
                      "Promise<T> must be move constructible");
        static_assert(std::is_move_constructible_v<kythira::Future<T>>, 
                      "Future<T> must be move constructible");
        
        return true;
    }
    
    // Validate instantiation with various types
    static_assert(test_wrapper_instantiation<int>(), 
                  "Wrapper instantiation test failed for int");
    static_assert(test_wrapper_instantiation<void>(), 
                  "Wrapper instantiation test failed for void");
    static_assert(test_wrapper_instantiation<std::string>(), 
                  "Wrapper instantiation test failed for std::string");
    static_assert(test_wrapper_instantiation<TestStruct>(), 
                  "Wrapper instantiation test failed for TestStruct");
}

//-----------------------------------------------------------------------------
// Generic Template Compatibility Validation
//-----------------------------------------------------------------------------

// Test that wrappers work with concept-constrained templates
namespace generic_template_tests {
    // Generic function that accepts any future type
    template<kythira::future<int> F>
    constexpr bool test_future_constraint_int(F&&) {
        return true;
    }
    
    // Generic function that accepts any promise type
    template<kythira::promise<int> P>
    constexpr bool test_promise_constraint_int(P&&) {
        return true;
    }
    
    // Generic function that accepts any executor type
    template<kythira::executor E>
    constexpr bool test_executor_constraint(E&&) {
        return true;
    }
    
    // Validate that our wrappers work with generic templates
    // Note: These are compile-time checks, not runtime tests
    static_assert(requires { 
        test_future_constraint_int(std::declval<kythira::Future<int>>()); 
    }, "Future<int> must work with concept-constrained templates");
    
    static_assert(requires { 
        test_promise_constraint_int(std::declval<kythira::Promise<int>>()); 
    }, "Promise<int> must work with concept-constrained templates");
    
    static_assert(requires { 
        test_executor_constraint(std::declval<kythira::Executor>()); 
    }, "Executor must work with concept-constrained templates");
}

//-----------------------------------------------------------------------------
// Error Message Validation
//-----------------------------------------------------------------------------

// Ensure that concept violations produce clear error messages
// These static_asserts are commented out but serve as documentation
// of what should fail to compile with clear error messages

/*
// These should fail to compile with clear error messages:

// Invalid Try usage
static_assert(kythira::try_type<int, int>, 
              "int should not satisfy try_type concept");

// Invalid Future usage  
static_assert(kythira::future<std::string, int>, 
              "std::string should not satisfy future concept");

// Invalid Promise usage
static_assert(kythira::promise<int, int>, 
              "int should not satisfy promise concept");

// Invalid Executor usage
static_assert(kythira::executor<int>, 
              "int should not satisfy executor concept");
*/