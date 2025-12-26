# Folly Concept Wrappers Documentation

This document provides comprehensive documentation for the Folly concept wrapper classes implemented in `include/raft/future.hpp`. These wrappers adapt Facebook's Folly library types to satisfy the C++20 concepts defined in `include/concepts/future.hpp`, providing a unified interface for asynchronous programming.

## Overview

The Folly concept wrappers provide a bridge between Folly's mature asynchronous programming primitives and the kythira library's concept-based interfaces. This allows developers to write generic code that works with both Folly types and other future implementations while maintaining type safety and performance.

### Key Benefits

1. **Concept Compliance**: All wrappers satisfy their corresponding C++20 concepts
2. **Type Safety**: Compile-time validation of interface requirements
3. **Performance**: Zero-overhead abstractions with move semantics optimization
4. **Interoperability**: Seamless integration with existing Folly-based code
5. **Exception Safety**: Proper exception handling and resource management

## Architecture

The wrapper architecture follows the adapter pattern:

```
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│   C++20         │    │   Kythira       │    │   Folly         │
│   Concepts      │◄───┤   Wrappers      │◄───┤   Types         │
│                 │    │                 │    │                 │
└─────────────────┘    └─────────────────┘    └─────────────────┘
```

Each wrapper class:
- Implements the interface required by its corresponding concept
- Delegates operations to the underlying Folly type
- Handles type conversions (void/Unit, exception types)
- Provides move semantics and proper resource management

## Core Wrapper Classes

### Try<T> - Try Type Wrapper

Adapts `folly::Try<T>` to satisfy the `try_type` concept.

#### Interface

```cpp
template<typename T>
class Try {
public:
    using value_type = T;
    
    // Constructors
    Try() = default;
    explicit Try(folly::Try<detail::void_to_unit_t<T>> ft);
    explicit Try(T&& value) requires(!std::is_void_v<T>);
    explicit Try(folly::exception_wrapper ex);
    explicit Try(std::exception_ptr ex);
    
    // Value access (non-void types only)
    auto value() -> T& requires(!std::is_void_v<T>);
    auto value() const -> const T& requires(!std::is_void_v<T>);
    
    // State checking
    auto hasValue() const -> bool;
    auto hasException() const -> bool;
    
    // Exception access
    auto exception() const -> std::exception_ptr;
    
    // Folly interop
    auto get_folly_try() -> folly::Try<detail::void_to_unit_t<T>>&;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>

// Create Try from value
kythira::Try<int> success_try(42);
if (success_try.hasValue()) {
    std::cout << "Value: " << success_try.value() << std::endl; // Prints: Value: 42
}

// Create Try from exception
kythira::Try<int> error_try(std::runtime_error("Something went wrong"));
if (error_try.hasException()) {
    try {
        std::rethrow_exception(error_try.exception());
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl; // Prints: Error: Something went wrong
    }
}

// Void specialization
kythira::Try<void> void_try(folly::Unit{});
std::cout << "Void try has value: " << void_try.hasValue() << std::endl; // Prints: true
```

#### Type Conversions

The Try wrapper handles automatic conversions between void and `folly::Unit`:

```cpp
// Internal mapping
void → folly::Unit (for folly compatibility)
folly::Unit → void (for concept compliance)

// Exception conversion
folly::exception_wrapper ↔ std::exception_ptr
```

### SemiPromise<T> - Semi-Promise Wrapper

Adapts `folly::Promise<T>` to satisfy the `semi_promise` concept.

#### Interface

```cpp
template<typename T>
class SemiPromise {
public:
    using value_type = T;
    
    // Constructors (move-only)
    SemiPromise() = default;
    explicit SemiPromise(folly::Promise<detail::void_to_unit_t<T>> fp);
    
    // Value setting
    auto setValue(T&& value) -> void requires(!std::is_void_v<T>);
    auto setValue(folly::Unit) -> void; // For void specialization
    auto setValue() -> void; // Convenience for void
    
    // Exception setting
    auto setException(folly::exception_wrapper ex) -> void;
    auto setException(std::exception_ptr ex) -> void;
    
    // State checking
    auto isFulfilled() const -> bool;
    
    // Folly interop
    auto get_folly_promise() -> folly::Promise<detail::void_to_unit_t<T>>&;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>

// Basic promise usage
kythira::SemiPromise<int> promise;
if (!promise.isFulfilled()) {
    promise.setValue(42);
}

// Exception handling
kythira::SemiPromise<std::string> error_promise;
try {
    throw std::runtime_error("Network error");
} catch (const std::exception& e) {
    error_promise.setException(folly::exception_wrapper(e));
}

// Void promise
kythira::SemiPromise<void> void_promise;
void_promise.setValue(); // Convenience method
// or
void_promise.setValue(folly::Unit{}); // Explicit Unit
```

### Promise<T> - Full Promise Wrapper

Extends `SemiPromise<T>` to satisfy the `promise` concept by adding future access.

#### Interface

```cpp
template<typename T>
class Promise : public SemiPromise<T> {
public:
    // Inherits all SemiPromise functionality
    
    // Future access
    auto getFuture() -> Future<T>;
    auto getSemiFuture() -> Future<T>;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>
#include <thread>

// Async computation pattern
auto create_async_computation() -> kythira::Future<int> {
    kythira::Promise<int> promise;
    auto future = promise.getFuture();
    
    // Start async work
    std::thread([p = std::move(promise)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        p.setValue(42);
    }).detach();
    
    return future;
}

// Usage
auto future = create_async_computation();
auto result = std::move(future).get(); // Returns 42 after delay
```

### Future<T> - Enhanced Future Wrapper

Adapts `folly::Future<T>` to satisfy multiple concepts: `future`, `future_continuation`, and `future_transformable`.

#### Interface

```cpp
template<typename T>
class Future {
public:
    using value_type = T;
    
    // Constructors
    Future() = default;
    explicit Future(folly::Future<detail::void_to_unit_t<T>> ff);
    explicit Future(T&& value) requires(!std::is_void_v<T>);
    explicit Future(folly::exception_wrapper ex);
    explicit Future(std::exception_ptr ex);
    
    // Value retrieval (move semantics required)
    auto get() -> T requires(!std::is_void_v<T>);
    auto get() -> void; // Void specialization
    
    // Status checking
    auto isReady() const -> bool;
    auto wait(std::chrono::milliseconds timeout) -> bool;
    
    // Transformation operations
    template<typename F>
    auto thenValue(F&& func) -> Future<std::invoke_result_t<F, T>>;
    
    template<typename F>
    auto thenError(F&& func) -> Future<T>;
    
    template<typename F>
    auto ensure(F&& func) -> Future<T>;
    
    // Continuation operations
    auto via(folly::Executor* executor) -> Future<T>;
    auto delay(std::chrono::milliseconds duration) -> Future<T>;
    auto within(std::chrono::milliseconds timeout) -> Future<T>;
    
    // Folly interop
    auto get_folly_future() && -> folly::Future<detail::void_to_unit_t<T>>;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Basic future operations
kythira::Future<int> future(42);
std::cout << "Ready: " << future.isReady() << std::endl; // Prints: true
auto result = std::move(future).get(); // Returns 42

// Transformation chaining
auto process_data() -> kythira::Future<std::string> {
    return kythira::Future<int>(21)
        .thenValue([](int value) {
            return value * 2; // Returns 42
        })
        .thenValue([](int doubled) {
            return std::to_string(doubled); // Returns "42"
        });
}

// Error handling
auto safe_divide(int a, int b) -> kythira::Future<double> {
    if (b == 0) {
        return kythira::Future<double>(std::invalid_argument("Division by zero"));
    }
    return kythira::Future<double>(static_cast<double>(a) / b);
}

auto result_future = safe_divide(10, 2)
    .thenError([](std::exception_ptr ex) -> double {
        std::cerr << "Error occurred, using default value" << std::endl;
        return -1.0;
    });

// Continuation operations with executor
folly::CPUThreadPoolExecutor executor(4);
auto scheduled_future = kythira::Future<int>(100)
    .via(&executor)                                    // Execute on thread pool
    .delay(std::chrono::milliseconds(500))             // Add 500ms delay
    .within(std::chrono::seconds(2))                   // 2 second timeout
    .thenValue([](int value) {
        return value * 2;
    });
```

### Executor - Executor Wrapper

Adapts `folly::Executor*` to satisfy the `executor` concept.

#### Interface

```cpp
class Executor {
public:
    // Constructors
    Executor() = default; // Creates invalid executor
    explicit Executor(folly::Executor* executor);
    
    // Work execution
    template<typename F>
    auto add(F&& func) -> void;
    
    // State checking
    auto is_valid() const -> bool;
    
    // Accessor
    auto get() const -> folly::Executor*;
    
    // KeepAlive creation
    auto getKeepAliveToken() -> KeepAlive;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Basic executor usage
folly::CPUThreadPoolExecutor thread_pool(4);
kythira::Executor executor(&thread_pool);

// Schedule work
executor.add([]() {
    std::cout << "Work executed on thread pool!" << std::endl;
});

// Batch work scheduling
for (int i = 0; i < 10; ++i) {
    executor.add([i]() {
        std::cout << "Task " << i << " executing" << std::endl;
    });
}

// Error handling
if (!executor.is_valid()) {
    throw std::runtime_error("Invalid executor");
}
```

### KeepAlive - KeepAlive Wrapper

Adapts `folly::Executor::KeepAlive` to satisfy the `keep_alive` concept.

#### Interface

```cpp
class KeepAlive {
public:
    using folly_type = folly::Executor::KeepAlive<>;
    
    // Constructors
    KeepAlive() = default;
    explicit KeepAlive(folly_type ka);
    explicit KeepAlive(folly::Executor* executor);
    
    // Executor access
    auto get() const -> folly::Executor*;
    
    // Work delegation
    template<typename F>
    auto add(F&& func) -> void;
    
    // State checking
    auto is_valid() const -> bool;
    
    // Folly interop
    auto get_folly_keep_alive() -> folly_type&;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Create KeepAlive from executor
folly::CPUThreadPoolExecutor executor(4);
kythira::KeepAlive keep_alive(&executor);

// Safe work scheduling (keeps executor alive)
auto schedule_safe_work = [](kythira::KeepAlive ka) {
    // Executor guaranteed to be alive during this scope
    ka.add([]() {
        std::cout << "Safe work execution" << std::endl;
    });
};

schedule_safe_work(keep_alive);

// RAII semantics - executor lifetime managed automatically
{
    folly::CPUThreadPoolExecutor local_executor(2);
    kythira::KeepAlive local_keep_alive(&local_executor);
    
    // Schedule work that may outlive local_executor
    std::thread([ka = std::move(local_keep_alive)]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ka.add([]() {
            std::cout << "Delayed work still executes safely" << std::endl;
        });
    }).detach();
    
    // local_executor destructor called here, but KeepAlive keeps it alive
}
```

## Factory and Collector Classes

### FutureFactory - Static Factory Class

Provides static factory methods for creating futures, satisfying the `future_factory` concept.

#### Interface

```cpp
class FutureFactory {
public:
    // Value-based factory methods
    template<typename T>
    static auto makeFuture(T&& value) -> Future<std::decay_t<T>>;
    
    static auto makeFuture() -> Future<void>; // Void future
    
    // Exception-based factory methods
    template<typename T>
    static auto makeExceptionalFuture(folly::exception_wrapper ex) -> Future<T>;
    
    template<typename T>
    static auto makeExceptionalFuture(std::exception_ptr ex) -> Future<T>;
    
    // Ready future factory methods
    static auto makeReadyFuture() -> Future<folly::Unit>;
    
    template<typename T>
    static auto makeReadyFuture(T&& value) -> Future<std::decay_t<T>>;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>

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

### FutureCollector - Static Collector Class

Provides static methods for collective future operations, satisfying the `future_collector` concept.

#### Interface

```cpp
class FutureCollector {
public:
    // Collect all futures
    template<typename T>
    static auto collectAll(std::vector<Future<T>> futures) 
        -> Future<std::vector<Try<T>>>;
    
    // Collect first completed future
    template<typename T>
    static auto collectAny(std::vector<Future<T>> futures)
        -> Future<std::tuple<std::size_t, Try<T>>>;
    
    // Collect first successful future
    template<typename T>
    static auto collectAnyWithoutException(std::vector<Future<T>> futures) -> auto;
    
    // Collect first N completed futures
    template<typename T>
    static auto collectN(std::vector<Future<T>> futures, std::size_t n)
        -> Future<std::vector<std::tuple<std::size_t, Try<T>>>>;
};
```

#### Usage Examples

```cpp
#include <raft/future.hpp>
#include <vector>

// Collect all futures
auto create_batch_futures() -> std::vector<kythira::Future<int>> {
    std::vector<kythira::Future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(kythira::FutureFactory::makeFuture(i * 10));
    }
    return futures;
}

auto all_results = kythira::FutureCollector::collectAll(create_batch_futures())
    .thenValue([](std::vector<kythira::Try<int>> results) {
        int sum = 0;
        for (const auto& result : results) {
            if (result.hasValue()) {
                sum += result.value();
            }
        }
        return sum; // Returns 100 (0+10+20+30+40)
    });

// Collect any (first completed)
auto any_result = kythira::FutureCollector::collectAny(create_batch_futures())
    .thenValue([](std::tuple<std::size_t, kythira::Try<int>> result) {
        auto [index, try_value] = result;
        std::cout << "First completed: index " << index;
        if (try_value.hasValue()) {
            std::cout << ", value " << try_value.value();
        }
        std::cout << std::endl;
        return index;
    });

// Collect first N
auto first_three = kythira::FutureCollector::collectN(create_batch_futures(), 3)
    .thenValue([](std::vector<std::tuple<std::size_t, kythira::Try<int>>> results) {
        std::cout << "First 3 completed futures:" << std::endl;
        for (const auto& [index, try_value] : results) {
            std::cout << "  Index " << index;
            if (try_value.hasValue()) {
                std::cout << ": " << try_value.value();
            }
            std::cout << std::endl;
        }
        return results.size();
    });
```

## Type Conversion Utilities

The wrapper implementation includes comprehensive type conversion utilities in the `kythira::detail` namespace.

### Exception Conversion

```cpp
namespace kythira::detail {
    // Convert folly::exception_wrapper to std::exception_ptr
    auto to_std_exception_ptr(const folly::exception_wrapper& ew) -> std::exception_ptr;
    
    // Convert std::exception_ptr to folly::exception_wrapper
    auto to_folly_exception_wrapper(std::exception_ptr ep) -> folly::exception_wrapper;
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

## Error Handling and Exception Safety

### Exception Safety Guarantees

All wrapper classes provide strong exception safety guarantees:

1. **Basic Guarantee**: Operations leave objects in valid state
2. **Strong Guarantee**: Operations either succeed completely or have no effect
3. **No-throw Guarantee**: Destructors and move operations never throw

### Exception Conversion Strategy

The wrappers handle exception conversion transparently:

```cpp
// Folly → Standard conversion
folly::exception_wrapper folly_ex = /* ... */;
std::exception_ptr std_ex = kythira::detail::to_std_exception_ptr(folly_ex);

// Standard → Folly conversion
std::exception_ptr std_ex = std::make_exception_ptr(std::runtime_error("error"));
folly::exception_wrapper folly_ex = kythira::detail::to_folly_exception_wrapper(std_ex);
```

### Resource Management

All wrappers use RAII for proper resource management:

```cpp
// Automatic cleanup on scope exit
{
    kythira::Promise<int> promise;
    auto future = promise.getFuture();
    
    // Resources automatically cleaned up here
} // Promise and Future destructors called
```

## Performance Considerations

### Zero-Overhead Abstractions

The wrappers are designed as zero-overhead abstractions:

1. **No Virtual Functions**: All operations are statically dispatched
2. **Move Semantics**: Efficient resource transfer without copying
3. **Template Specialization**: Compile-time optimization opportunities
4. **Inlining**: Small wrapper methods are inlined by the compiler

### Memory Layout

Wrapper objects have minimal memory overhead:

```cpp
sizeof(kythira::Future<int>) ≈ sizeof(folly::Future<int>)
sizeof(kythira::Promise<int>) ≈ sizeof(folly::Promise<int>)
sizeof(kythira::Try<int>) ≈ sizeof(folly::Try<int>)
```

### Benchmark Results

Performance validation shows equivalent performance to direct Folly usage:

```cpp
// Wrapper usage (same performance as direct Folly)
auto wrapper_future = kythira::FutureFactory::makeFuture(42);
auto result = std::move(wrapper_future).get();

// Direct Folly usage
auto folly_future = folly::makeFuture(42);
auto result = std::move(folly_future).get();
```

## Integration with Existing Code

### Backward Compatibility

The wrappers maintain full backward compatibility with existing Folly-based code:

```cpp
// Existing Folly code continues to work
folly::Future<int> folly_future = folly::makeFuture(42);

// Can be wrapped when needed
kythira::Future<int> wrapper_future(std::move(folly_future));

// Can be unwrapped when needed
folly::Future<int> back_to_folly = std::move(wrapper_future).get_folly_future();
```

### Interoperability Utilities

Helper functions facilitate conversion between wrapper and Folly types:

```cpp
// Convert Folly future to wrapper
template<typename T>
auto wrap_folly_future(folly::Future<T> folly_fut) -> kythira::Future<T> {
    return kythira::Future<T>(std::move(folly_fut));
}

// Convert wrapper future to Folly
template<typename T>
auto unwrap_to_folly(kythira::Future<T> wrapper_fut) -> folly::Future<kythira::detail::void_to_unit_t<T>> {
    return std::move(wrapper_fut).get_folly_future();
}
```

## Best Practices

### 1. Use Move Semantics

Always use move semantics when consuming futures:

```cpp
// ✅ Correct
auto result = std::move(future).get();

// ❌ Incorrect
auto result = future.get(); // Compilation error
```

### 2. Handle Void Types Properly

Use `folly::Unit` for void promise fulfillment:

```cpp
// ✅ Correct
kythira::SemiPromise<void> promise;
promise.setValue(folly::Unit{});
// or
promise.setValue(); // Convenience method

// ❌ Incorrect
promise.setValue(void); // Invalid syntax
```

### 3. Exception Handling

Use `folly::exception_wrapper` for exception setting:

```cpp
// ✅ Correct
promise.setException(folly::exception_wrapper(std::runtime_error("error")));

// ✅ Also correct (automatic conversion)
promise.setException(std::make_exception_ptr(std::runtime_error("error")));
```

### 4. Resource Management

Let RAII handle resource cleanup:

```cpp
// ✅ Correct - automatic cleanup
{
    kythira::Promise<int> promise;
    auto future = promise.getFuture();
    // Use promise and future
} // Automatic cleanup

// ❌ Avoid manual resource management
```

### 5. Concept Constraints

Use concept constraints for generic code:

```cpp
// ✅ Correct - concept-constrained template
template<kythira::future<int> FutureType>
auto process_future(FutureType fut) -> int {
    return std::move(fut).get();
}

// ❌ Less clear - unconstrained template
template<typename F>
auto process_future(F fut) -> auto {
    return std::move(fut).get();
}
```

## Troubleshooting

### Common Issues

1. **Move Semantics Errors**
   ```cpp
   // Error: cannot bind rvalue reference to lvalue
   auto result = future.get(); // Wrong
   auto result = std::move(future).get(); // Correct
   ```

2. **Void Type Handling**
   ```cpp
   // Error: no matching function for setValue()
   void_promise.setValue(); // May fail without convenience method
   void_promise.setValue(folly::Unit{}); // Always works
   ```

3. **Exception Type Mismatches**
   ```cpp
   // Error: no matching function for setException()
   promise.setException(std::runtime_error("error")); // Wrong type
   promise.setException(folly::exception_wrapper(std::runtime_error("error"))); // Correct
   ```

### Debugging Tips

1. **Use Static Assertions**: Verify concept compliance
   ```cpp
   static_assert(kythira::future<MyFuture<int>, int>);
   ```

2. **Check Requirements**: Test individual concept requirements
   ```cpp
   static_assert(requires(MyFuture<int> f) {
       { std::move(f).get() };
   });
   ```

3. **Validate Types**: Ensure proper type mapping
   ```cpp
   static_assert(std::is_same_v<kythira::detail::void_to_unit_t<void>, folly::Unit>);
   ```

## Migration Guide

### From Direct Folly Usage

Replace direct Folly usage with wrapper types:

```cpp
// Before
folly::Promise<int> promise;
auto future = promise.getFuture();
promise.setValue(42);
auto result = std::move(future).get();

// After
kythira::Promise<int> promise;
auto future = promise.getFuture();
promise.setValue(42);
auto result = std::move(future).get();
```

### From Other Future Libraries

Adapt existing future-based code to use wrappers:

```cpp
// Generic function works with any future type
template<kythira::future<int> FutureType>
auto process_async_result(FutureType fut) -> int {
    return std::move(fut)
        .thenValue([](int value) { return value * 2; })
        .get();
}

// Works with Folly futures via wrapper
kythira::Future<int> folly_wrapper(folly::makeFuture(21));
auto result = process_async_result(std::move(folly_wrapper)); // Returns 42
```

## See Also

- [Concepts Documentation](concepts_documentation.md) - C++20 concepts reference
- [Concepts API Reference](concepts_api_reference.md) - Complete API documentation
- [Concepts Migration Guide](concepts_migration_guide.md) - Migration from older versions
- [Future Migration Guide](future_migration_guide.md) - General future migration patterns
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation
- [Example Programs](../examples/folly-wrappers/) - Working code examples