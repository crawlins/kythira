# Folly Concept Wrappers API Reference

This document provides a complete API reference for the Folly concept wrapper classes implemented in `include/raft/future.hpp`.

## Table of Contents

- [Core Wrapper Classes](#core-wrapper-classes)
  - [Try<T>](#tryt)
  - [SemiPromise<T>](#semipromise)
  - [Promise<T>](#promiset)
  - [Future<T>](#futuret)
  - [Executor](#executor)
  - [KeepAlive](#keepalive)
- [Factory Classes](#factory-classes)
  - [FutureFactory](#futurefactory)
  - [FutureCollector](#futurecollector)
- [Type Conversion Utilities](#type-conversion-utilities)
- [Static Assertions](#static-assertions)
- [Legacy Functions](#legacy-functions)

## Core Wrapper Classes

### Try<T>

Adapts `folly::Try<T>` to satisfy the `try_type` concept.

#### Template Parameters

- `T`: The value type (can be `void`)

#### Type Aliases

```cpp
using value_type = T;
using folly_type = folly::Try<detail::void_to_unit_t<T>>;
```

#### Constructors

```cpp
// Default constructor
Try() = default;

// Construct from folly::Try
explicit Try(folly_type ft);

// Construct from value (non-void types only)
template<typename U = T>
explicit Try(U&& value) requires(!std::is_void_v<T>);

// Construct from exception
explicit Try(folly::exception_wrapper ex);
explicit Try(std::exception_ptr ex);
```

#### Member Functions

##### Value Access (Non-Void Types)

```cpp
// Non-const value access
template<typename U = T>
auto value() -> T& requires(!std::is_void_v<U>);

// Const value access
template<typename U = T>
auto value() const -> const T& requires(!std::is_void_v<U>);
```

##### State Checking

```cpp
// Check if contains value (Folly naming for concept compliance)
auto hasValue() const -> bool;

// Check if contains exception (Folly naming for concept compliance)
auto hasException() const -> bool;

// Legacy methods for backward compatibility
auto has_value() const -> bool;
auto has_exception() const -> bool;
```

##### Exception Access

```cpp
// Access exception - converts folly::exception_wrapper to std::exception_ptr
auto exception() const -> std::exception_ptr;
```

##### Folly Interoperability

```cpp
// Get underlying folly::Try (const)
auto get_folly_try() const -> const folly_type&;

// Get underlying folly::Try (non-const)
auto get_folly_try() -> folly_type&;
```

#### Void Specialization

The `Try<void>` specialization has the same interface except:
- No `value()` methods
- Constructs from `folly::Unit` instead of value

#### Usage Example

```cpp
// Non-void Try
kythira::Try<int> success_try(42);
if (success_try.hasValue()) {
    std::cout << "Value: " << success_try.value() << std::endl;
}

// Exception Try
kythira::Try<int> error_try(std::runtime_error("Error"));
if (error_try.hasException()) {
    std::rethrow_exception(error_try.exception());
}

// Void Try
kythira::Try<void> void_try(folly::Unit{});
std::cout << "Has value: " << void_try.hasValue() << std::endl;
```

---

### SemiPromise<T>

Adapts `folly::Promise<T>` to satisfy the `semi_promise` concept.

#### Template Parameters

- `T`: The value type (can be `void`)

#### Type Aliases

```cpp
using value_type = T;
using folly_type = folly::Promise<detail::void_to_unit_t<T>>;
```

#### Constructors

```cpp
// Default constructor
SemiPromise() = default;

// Construct from folly::Promise
explicit SemiPromise(folly_type fp);

// Move semantics only (no copy)
SemiPromise(SemiPromise&&) = default;
SemiPromise& operator=(SemiPromise&&) = default;

// Deleted copy semantics
SemiPromise(const SemiPromise&) = delete;
SemiPromise& operator=(const SemiPromise&) = delete;
```

#### Member Functions

##### Value Setting

```cpp
// Set value (non-void types)
template<typename U = T>
auto setValue(U&& value) -> void requires(!std::is_void_v<T>);
```

##### Exception Setting

```cpp
// Set exception using folly::exception_wrapper (for concept compliance)
auto setException(folly::exception_wrapper ex) -> void;

// Set exception using std::exception_ptr (convenience method)
auto setException(std::exception_ptr ex) -> void;
```

##### State Checking

```cpp
// Check if fulfilled (for concept compliance)
auto isFulfilled() const -> bool;
```

##### Folly Interoperability

```cpp
// Get underlying folly::Promise (non-const)
auto get_folly_promise() -> folly_type&;

// Get underlying folly::Promise (const)
auto get_folly_promise() const -> const folly_type&;
```

#### Void Specialization

The `SemiPromise<void>` specialization adds:

```cpp
// Set value using folly::Unit (for concept compliance)
auto setValue(folly::Unit) -> void;

// Convenience method for void
auto setValue() -> void;
```

#### Usage Example

```cpp
// Non-void promise
kythira::SemiPromise<int> promise;
if (!promise.isFulfilled()) {
    promise.setValue(42);
}

// Exception handling
try {
    throw std::runtime_error("Error");
} catch (const std::exception& e) {
    promise.setException(folly::exception_wrapper(e));
}

// Void promise
kythira::SemiPromise<void> void_promise;
void_promise.setValue(); // Convenience method
// or
void_promise.setValue(folly::Unit{}); // Explicit Unit
```

---

### Promise<T>

Extends `SemiPromise<T>` to satisfy the `promise` concept by adding future access.

#### Template Parameters

- `T`: The value type (can be `void`)

#### Type Aliases

```cpp
using value_type = T;
using base_type = SemiPromise<T>;
using folly_type = typename base_type::folly_type;
```

#### Constructors

```cpp
// Default constructor
Promise() = default;

// Construct from folly::Promise
explicit Promise(folly_type fp);

// Move semantics only (no copy)
Promise(Promise&&) = default;
Promise& operator=(Promise&&) = default;

// Deleted copy semantics
Promise(const Promise&) = delete;
Promise& operator=(const Promise&) = delete;
```

#### Member Functions

Inherits all `SemiPromise<T>` functionality plus:

##### Future Access

```cpp
// Get associated future (for concept compliance)
auto getFuture() -> Future<T>;

// Get associated semi-future (for concept compliance)
auto getSemiFuture() -> Future<T>;
```

#### Usage Example

```cpp
// Create promise and get future
kythira::Promise<int> promise;
auto future = promise.getFuture();

// Fulfill promise
promise.setValue(42);

// Get result from future
auto result = std::move(future).get(); // Returns 42
```

---

### Future<T>

Adapts `folly::Future<T>` to satisfy multiple concepts: `future`, `future_continuation`, and `future_transformable`.

#### Template Parameters

- `T`: The value type (can be `void`)

#### Type Aliases

```cpp
using value_type = T;
using folly_type = folly::Future<detail::void_to_unit_t<T>>;
```

#### Constructors

```cpp
// Default constructor
Future() = default;

// Construct from folly::Future
explicit Future(folly_type ff);

// Construct from value (non-void types)
template<typename U = T>
explicit Future(U&& value) requires(!std::is_void_v<T>);

// Construct from exception
explicit Future(folly::exception_wrapper ex);
explicit Future(std::exception_ptr ex);
```

#### Member Functions

##### Value Retrieval

```cpp
// Get value (blocking, requires move semantics)
auto get() -> T requires(!std::is_void_v<T>);
auto get() -> void; // Void specialization
```

##### Status Checking

```cpp
// Check if ready (concept compliance)
auto isReady() const -> bool;

// Wait with timeout (concept compliance)
auto wait(std::chrono::milliseconds timeout) -> bool;
```

##### Transformation Operations

```cpp
// Chain continuation with value (concept compliance)
template<typename F>
auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>> requires(!std::is_void_v<T>);

// Error handling (concept compliance)
template<typename F>
auto thenError(F&& func) -> Future<T>;

// Ensure (cleanup functionality)
template<typename F>
auto ensure(F&& func) -> Future<T>;
```

##### Continuation Operations

```cpp
// Execute on specific executor
auto via(void* executor) -> Future<T>;
auto via(folly::Executor* executor) -> Future<T>;
auto via(folly::Executor& executor) -> Future<T>;

// Add delay
auto delay(std::chrono::milliseconds duration) -> Future<T>;

// Add timeout
auto within(std::chrono::milliseconds timeout) -> Future<T>;
```

##### Legacy Methods

```cpp
// Legacy methods for backward compatibility
template<typename F>
auto then(F&& func) -> Future<std::invoke_result_t<F, T>>;

template<typename F>
auto onError(F&& func) -> Future<T>;
```

##### Folly Interoperability

```cpp
// Get underlying folly::Future (move semantics)
auto get_folly_future() && -> folly_type;
```

#### Void Specialization

The `Future<void>` specialization has adapted signatures for void handling:

```cpp
// Construct from folly::Unit
Future() : _folly_future(folly::makeFuture(folly::Unit{})) {}

// Chain continuation with void (concept compliance)
template<typename F>
auto thenValue(F&& func) -> Future<std::invoke_result_t<F>>;
```

#### Usage Example

```cpp
// Basic future operations
kythira::Future<int> future(42);
std::cout << "Ready: " << future.isReady() << std::endl;
auto result = std::move(future).get();

// Transformation chaining
auto processed = kythira::Future<int>(21)
    .thenValue([](int value) { return value * 2; })
    .thenValue([](int doubled) { return std::to_string(doubled); });

// Error handling
auto safe_future = risky_operation()
    .thenError([](std::exception_ptr ex) {
        std::cerr << "Error occurred" << std::endl;
        return default_value();
    });

// Continuation operations
folly::CPUThreadPoolExecutor executor(4);
auto scheduled = kythira::Future<int>(100)
    .via(&executor)
    .delay(std::chrono::milliseconds(500))
    .within(std::chrono::seconds(2));
```

---

### Executor

Adapts `folly::Executor*` to satisfy the `executor` concept.

#### Type Aliases

```cpp
using folly_type = folly::Executor*;
```

#### Constructors

```cpp
// Default constructor (creates invalid executor)
Executor();

// Construct from folly::Executor pointer
explicit Executor(folly::Executor* executor);

// Copy and move semantics (safe since storing pointer)
Executor(const Executor&) = default;
Executor& operator=(const Executor&) = default;
Executor(Executor&&) = default;
Executor& operator=(Executor&&) = default;
```

#### Member Functions

##### Work Execution

```cpp
// Add work to executor (concept compliance)
template<typename F>
auto add(F&& func) -> void;
```

##### State Checking

```cpp
// Check if executor is valid
auto is_valid() const -> bool;
```

##### Accessor

```cpp
// Get underlying executor
auto get() const -> folly::Executor*;
```

##### KeepAlive Creation

```cpp
// Get KeepAlive token
auto getKeepAliveToken() -> KeepAlive;

// Legacy method name for backward compatibility
auto get_keep_alive() -> KeepAlive;
```

#### Usage Example

```cpp
// Create executor wrapper
folly::CPUThreadPoolExecutor thread_pool(4);
kythira::Executor executor(&thread_pool);

// Schedule work
executor.add([]() {
    std::cout << "Work executed!" << std::endl;
});

// Check validity
if (executor.is_valid()) {
    // Safe to use
}

// Get KeepAlive for safe async usage
auto keep_alive = executor.getKeepAliveToken();
```

---

### KeepAlive

Adapts `folly::Executor::KeepAlive` to satisfy the `keep_alive` concept.

#### Type Aliases

```cpp
using folly_type = folly::Executor::KeepAlive<>;
```

#### Constructors

```cpp
// Default constructor
KeepAlive() = default;

// Construct from folly::KeepAlive
explicit KeepAlive(folly_type ka);

// Construct from folly::Executor pointer
explicit KeepAlive(folly::Executor* executor);

// Copy and move semantics (folly::KeepAlive supports both)
KeepAlive(const KeepAlive&) = default;
KeepAlive& operator=(const KeepAlive&) = default;
KeepAlive(KeepAlive&&) = default;
KeepAlive& operator=(KeepAlive&&) = default;
```

#### Member Functions

##### Executor Access

```cpp
// Get underlying executor (concept compliance)
auto get() const -> folly::Executor*;
```

##### Work Delegation

```cpp
// Add work to underlying executor (for convenience)
template<typename F>
auto add(F&& func) -> void;
```

##### State Checking

```cpp
// Check if valid
auto is_valid() const -> bool;
```

##### Folly Interoperability

```cpp
// Get underlying KeepAlive (const)
auto get_folly_keep_alive() const -> const folly_type&;

// Get underlying KeepAlive (non-const)
auto get_folly_keep_alive() -> folly_type&;
```

#### Usage Example

```cpp
// Create KeepAlive from executor
folly::CPUThreadPoolExecutor executor(4);
kythira::KeepAlive keep_alive(&executor);

// Safe work scheduling
keep_alive.add([]() {
    std::cout << "Safe work execution" << std::endl;
});

// RAII semantics ensure executor lifetime
auto schedule_delayed_work = [](kythira::KeepAlive ka) {
    std::thread([ka = std::move(ka)]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ka.add([]() {
            std::cout << "Delayed work executed safely" << std::endl;
        });
    }).detach();
};
```

## Factory Classes

### FutureFactory

Static factory class that satisfies the `future_factory` concept.

#### Static Methods

##### Value-Based Factory Methods

```cpp
// Make future from value
template<typename T>
static auto makeFuture(T&& value) -> Future<std::decay_t<T>>;

// Make future with no arguments (void future)
static auto makeFuture() -> Future<void>;

// Make future from folly::Unit (for concept compliance)
static auto makeFuture(folly::Unit) -> Future<void>;
```

##### Exception-Based Factory Methods

```cpp
// Make exceptional future from folly::exception_wrapper
template<typename T>
static auto makeExceptionalFuture(folly::exception_wrapper ex) -> Future<T>;

// Make exceptional future from std::exception_ptr
template<typename T>
static auto makeExceptionalFuture(std::exception_ptr ex) -> Future<T>;
```

##### Ready Future Factory Methods

```cpp
// Make ready future (returns Future<folly::Unit>)
static auto makeReadyFuture() -> Future<folly::Unit>;

// Make ready future with value
template<typename T>
static auto makeReadyFuture(T&& value) -> Future<std::decay_t<T>>;
```

#### Usage Example

```cpp
// Create futures from values
auto int_future = kythira::FutureFactory::makeFuture(42);
auto string_future = kythira::FutureFactory::makeFuture(std::string("Hello"));
auto void_future = kythira::FutureFactory::makeFuture();

// Create exceptional futures
auto error_future = kythira::FutureFactory::makeExceptionalFuture<int>(
    folly::exception_wrapper(std::runtime_error("Error"))
);

// Generic factory usage
template<typename T>
auto safe_compute(T input) -> kythira::Future<T> {
    try {
        auto result = expensive_computation(input);
        return kythira::FutureFactory::makeFuture(std::move(result));
    } catch (const std::exception& e) {
        return kythira::FutureFactory::makeExceptionalFuture<T>(
            folly::exception_wrapper(e)
        );
    }
}
```

---

### FutureCollector

Static collector class that satisfies the `future_collector` concept.

#### Static Methods

##### Collection Operations

```cpp
// Collect all futures
template<typename T>
static auto collectAll(std::vector<Future<T>> futures) 
    -> Future<std::vector<Try<T>>>;

// Collect any future (first completed)
template<typename T>
static auto collectAny(std::vector<Future<T>> futures)
    -> Future<std::tuple<std::size_t, Try<T>>>;

// Collect any without exception (first successful)
template<typename T>
static auto collectAnyWithoutException(std::vector<Future<T>> futures) -> auto;

// Collect N futures (first N completed)
template<typename T>
static auto collectN(std::vector<Future<T>> futures, std::size_t n)
    -> Future<std::vector<std::tuple<std::size_t, Try<T>>>>;
```

#### Return Type Details

##### collectAnyWithoutException Return Types

```cpp
// For void futures: returns Future<std::size_t> (just the index)
auto collectAnyWithoutException(std::vector<Future<void>> futures) 
    -> Future<std::size_t>;

// For non-void futures: returns Future<std::tuple<std::size_t, T>>
template<typename T>
auto collectAnyWithoutException(std::vector<Future<T>> futures)
    -> Future<std::tuple<std::size_t, T>> requires(!std::is_void_v<T>);
```

#### Usage Example

```cpp
// Create batch of futures
std::vector<kythira::Future<int>> futures;
for (int i = 0; i < 5; ++i) {
    futures.push_back(kythira::FutureFactory::makeFuture(i * 10));
}

// Collect all
auto all_results = kythira::FutureCollector::collectAll(std::move(futures))
    .thenValue([](std::vector<kythira::Try<int>> results) {
        int sum = 0;
        for (const auto& result : results) {
            if (result.hasValue()) {
                sum += result.value();
            }
        }
        return sum;
    });

// Collect any
auto any_result = kythira::FutureCollector::collectAny(create_futures())
    .thenValue([](std::tuple<std::size_t, kythira::Try<int>> result) {
        auto [index, try_value] = result;
        return index;
    });

// Collect first N
auto first_three = kythira::FutureCollector::collectN(create_futures(), 3);
```

## Type Conversion Utilities

Located in the `kythira::detail` namespace.

### Exception Conversion

```cpp
namespace kythira::detail {
    // Convert folly::exception_wrapper to std::exception_ptr
    inline auto to_std_exception_ptr(const folly::exception_wrapper& ew) -> std::exception_ptr;
    
    // Convert std::exception_ptr to folly::exception_wrapper
    inline auto to_folly_exception_wrapper(std::exception_ptr ep) -> folly::exception_wrapper;
}
```

### Void/Unit Type Mapping

```cpp
namespace kythira::detail {
    // Map void to folly::Unit for template specializations
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
    
    // Map folly::Unit back to void for return types
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
}
```

## Static Assertions

The implementation includes comprehensive static assertions for concept compliance validation.

### Core Type Assertions

```cpp
// Try wrapper concept compliance
static_assert(kythira::try_type<kythira::Try<int>, int>);
static_assert(kythira::try_type<kythira::Try<void>, void>);
static_assert(kythira::try_type<kythira::Try<std::string>, std::string>);

// Promise wrapper concept compliance
static_assert(kythira::semi_promise<kythira::SemiPromise<int>, int>);
static_assert(kythira::promise<kythira::Promise<int>, int>);

// Future wrapper concept compliance
static_assert(kythira::future<kythira::Future<int>, int>);
static_assert(kythira::future<kythira::Future<void>, void>);

// Executor wrapper concept compliance
static_assert(kythira::executor<kythira::Executor>);
static_assert(kythira::keep_alive<kythira::KeepAlive>);

// Factory concept compliance
static_assert(kythira::future_factory<kythira::FutureFactory>);
static_assert(kythira::future_collector<kythira::FutureCollector>);
```

### Advanced Concept Assertions

```cpp
// Continuation operations concept compliance
static_assert(kythira::future_continuation<kythira::Future<int>, int>);
static_assert(kythira::future_continuation<kythira::Future<void>, void>);

// Transformation operations concept compliance
static_assert(kythira::future_transformable<kythira::Future<int>, int>);
static_assert(kythira::future_transformable<kythira::Future<std::string>, std::string>);
```

### Type Conversion Validation

```cpp
// Validate void/Unit type mapping utilities
static_assert(std::is_same_v<kythira::detail::void_to_unit_t<void>, folly::Unit>);
static_assert(std::is_same_v<kythira::detail::void_to_unit_t<int>, int>);

// Validate Unit/void type mapping utilities
static_assert(std::is_same_v<kythira::detail::unit_to_void_t<folly::Unit>, void>);
static_assert(std::is_same_v<kythira::detail::unit_to_void_t<int>, int>);
```

## Legacy Functions

For backward compatibility, the implementation provides legacy collective operation functions.

### Legacy Collective Operations

```cpp
// Wait for any future to complete (modeled after folly::collectAny)
template<typename T>
auto wait_for_any(std::vector<Future<T>> futures) -> Future<std::tuple<std::size_t, Try<T>>>;

// Wait for all futures to complete (modeled after folly::collectAll)
template<typename T>
auto wait_for_all(std::vector<Future<T>> futures) -> Future<std::vector<Try<T>>>;
```

#### Usage Example

```cpp
// Legacy usage (for backward compatibility)
std::vector<kythira::Future<int>> futures = create_futures();

// Wait for any
auto any_result = kythira::wait_for_any(std::move(futures));

// Wait for all
auto all_results = kythira::wait_for_all(std::move(futures));
```

## Error Handling

### Exception Types

All wrapper classes handle the following exception scenarios:

1. **Invalid State**: Operations on invalid/moved objects
2. **Type Conversion**: Errors during exception type conversion
3. **Resource Exhaustion**: Memory allocation failures
4. **Timeout**: Operations that exceed specified timeouts

### Exception Safety Guarantees

- **Basic Guarantee**: All operations leave objects in valid state
- **Strong Guarantee**: Operations either succeed completely or have no effect
- **No-throw Guarantee**: Destructors and move operations never throw

### Common Exceptions

```cpp
// Executor-related exceptions
std::invalid_argument  // Null executor pointer
std::runtime_error     // Invalid executor state

// Promise-related exceptions
std::runtime_error     // Promise already fulfilled
std::bad_alloc         // Memory allocation failure

// Future-related exceptions
std::runtime_error     // Future already consumed
folly::FutureTimeout   // Timeout exceeded (from underlying Folly)
```

## Performance Characteristics

### Memory Overhead

```cpp
sizeof(kythira::Try<T>) ≈ sizeof(folly::Try<T>)
sizeof(kythira::Promise<T>) ≈ sizeof(folly::Promise<T>)
sizeof(kythira::Future<T>) ≈ sizeof(folly::Future<T>)
sizeof(kythira::Executor) ≈ sizeof(folly::Executor*)
sizeof(kythira::KeepAlive) ≈ sizeof(folly::Executor::KeepAlive<>)
```

### Runtime Performance

- **Zero-overhead abstractions**: No virtual function calls
- **Move semantics**: Efficient resource transfer
- **Inlining**: Small wrapper methods are inlined
- **Template specialization**: Compile-time optimization

### Compilation Impact

- **Header-only**: No additional compilation units
- **Template instantiation**: Minimal impact on compile times
- **Static assertions**: Compile-time validation with clear error messages

## See Also

- [Folly Concept Wrappers Documentation](folly_concept_wrappers_documentation.md) - Comprehensive usage guide
- [Folly Concept Wrappers Migration Guide](folly_concept_wrappers_migration_guide.md) - Migration from direct Folly usage
- [Concepts API Reference](concepts_api_reference.md) - C++20 concepts reference
- [Example Programs](../examples/folly-wrappers/) - Working code examples
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation