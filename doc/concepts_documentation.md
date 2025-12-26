# C++20 Concepts for Folly-Compatible Types

This document provides comprehensive documentation for the C++20 concepts defined in `include/concepts/future.hpp`. These concepts enable generic programming with Folly-compatible types while maintaining type safety and clear interfaces.

## Overview

The concepts in this library are designed to work seamlessly with Facebook's Folly library types while also supporting custom implementations. They provide compile-time constraints that ensure types conform to expected interfaces, enabling robust generic programming.

### Key Design Principles

1. **Folly Compatibility**: All concepts are designed to work with actual Folly types
2. **Generic Programming**: Enable writing code that works with multiple future implementations
3. **Type Safety**: Compile-time validation of type requirements
4. **Clear Interfaces**: Self-documenting code through concept constraints

## Core Concepts

### try_type Concept

The `try_type` concept models types that can hold either a value or an exception, similar to `folly::Try<T>`.

```cpp
template<typename T, typename ValueType>
concept try_type = /* implementation details */;
```

#### Requirements

A type `T` satisfies `try_type<T, ValueType>` if:

- **Value Access**: Provides `value()` method returning `ValueType&` (non-const) and `const ValueType&` (const)
- **State Checking**: Provides `hasValue()` and `hasException()` methods returning bool-convertible types
- **Exception Access**: Provides `exception()` method for accessing stored exceptions
- **Void Specialization**: Handles `void` value types appropriately

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/Try.h>

// Works with folly::Try
static_assert(try_type<folly::Try<int>, int>);
static_assert(try_type<folly::Try<std::string>, std::string>);
static_assert(try_type<folly::Try<void>, void>);

// Example function using the concept
template<try_type<int> TryType>
auto extract_value_or_default(const TryType& t, int default_val) -> int {
    return t.hasValue() ? t.value() : default_val;
}

// Usage
folly::Try<int> success_try = folly::Try<int>(42);
folly::Try<int> error_try = folly::Try<int>(std::runtime_error("error"));

auto val1 = extract_value_or_default(success_try, 0); // Returns 42
auto val2 = extract_value_or_default(error_try, 0);   // Returns 0
```

#### Folly Type Compatibility

- ✅ `folly::Try<T>` - Full compatibility
- ✅ `kythira::Try<T>` - Full compatibility (if implemented)

### future Concept

The `future` concept models asynchronous computation results, similar to `folly::Future<T>`.

```cpp
template<typename F, typename T>
concept future = /* implementation details */;
```

#### Requirements

A type `F` satisfies `future<F, T>` if:

- **Value Retrieval**: Provides `get()` method (requires move semantics)
- **Status Checking**: Provides `isReady()` method returning bool-convertible type
- **Continuation Chaining**: Provides `thenValue()` method for value-based continuations
- **Void Specialization**: Handles `void` result types appropriately

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Future.h>

// Works with folly::Future
static_assert(future<folly::Future<int>, int>);
static_assert(future<folly::Future<std::string>, std::string>);
static_assert(future<folly::Future<void>, void>);

// Example function using the concept
template<future<int> FutureType>
auto process_async_result(FutureType fut) -> int {
    if (fut.isReady()) {
        return std::move(fut).get();
    }
    
    // Chain continuation
    return std::move(fut)
        .thenValue([](int value) { return value * 2; })
        .get();
}

// Usage
folly::Future<int> folly_future = folly::makeFuture(21);
auto result = process_async_result(std::move(folly_future)); // Returns 42
```

#### Folly Type Compatibility

- ✅ `folly::Future<T>` - Full compatibility
- ✅ `folly::SemiFuture<T>` - Partial compatibility (missing some methods)
- ✅ `kythira::Future<T>` - Full compatibility (if implemented)

### semi_promise Concept

The `semi_promise` concept models promise types that can be fulfilled but don't provide future access.

```cpp
template<typename P, typename T>
concept semi_promise = /* implementation details */;
```

#### Requirements

A type `P` satisfies `semi_promise<P, T>` if:

- **Value Setting**: Provides `setValue()` method (void-aware)
- **Exception Setting**: Provides `setException()` method accepting `folly::exception_wrapper`
- **State Checking**: Provides `isFulfilled()` method returning bool-convertible type

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Promise.h>

// Works with folly::Promise (which includes semi-promise functionality)
static_assert(semi_promise<folly::Promise<int>, int>);
static_assert(semi_promise<folly::Promise<void>, void>);

// Example function using the concept
template<semi_promise<std::string> PromiseType>
auto fulfill_with_greeting(PromiseType& promise, const std::string& name) -> void {
    if (!promise.isFulfilled()) {
        promise.setValue("Hello, " + name + "!");
    }
}

// Usage
folly::Promise<std::string> promise;
fulfill_with_greeting(promise, "World");
auto future = promise.getFuture();
auto greeting = std::move(future).get(); // "Hello, World!"
```

#### Folly Type Compatibility

- ✅ `folly::Promise<T>` - Full compatibility (includes semi-promise functionality)

### promise Concept

The `promise` concept extends `semi_promise` with future access capabilities.

```cpp
template<typename P, typename T>
concept promise = semi_promise<P, T> && /* additional requirements */;
```

#### Requirements

A type `P` satisfies `promise<P, T>` if:

- **Inherits from semi_promise**: All semi-promise requirements
- **Future Access**: Provides `getFuture()` method
- **SemiFuture Access**: Provides `getSemiFuture()` method

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Promise.h>

// Works with folly::Promise
static_assert(promise<folly::Promise<int>, int>);

// Example function using the concept
template<promise<int> PromiseType>
auto create_async_computation(PromiseType promise) -> auto {
    // Get the future before fulfilling
    auto future = promise.getFuture();
    
    // Fulfill the promise asynchronously
    std::thread([p = std::move(promise)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        p.setValue(42);
    }).detach();
    
    return future;
}

// Usage
folly::Promise<int> promise;
auto future = create_async_computation(std::move(promise));
auto result = std::move(future).get(); // Returns 42 after delay
```

#### Folly Type Compatibility

- ✅ `folly::Promise<T>` - Full compatibility

### executor Concept

The `executor` concept models types that can execute work asynchronously.

```cpp
template<typename E>
concept executor = /* implementation details */;
```

#### Requirements

A type `E` satisfies `executor<E>` if:

- **Work Execution**: Provides `add()` method accepting `std::function<void()>`

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Works with folly executors
static_assert(executor<folly::CPUThreadPoolExecutor>);

// Example function using the concept
template<executor ExecutorType>
auto schedule_work(ExecutorType& exec, std::function<void()> work) -> void {
    exec.add(std::move(work));
}

// Usage
folly::CPUThreadPoolExecutor thread_pool(4);
schedule_work(thread_pool, []() {
    std::cout << "Work executed on thread pool!" << std::endl;
});
```

#### Folly Type Compatibility

- ✅ `folly::CPUThreadPoolExecutor` - Full compatibility
- ✅ `folly::IOThreadPoolExecutor` - Full compatibility
- ✅ `folly::InlineExecutor` - Full compatibility

### keep_alive Concept

The `keep_alive` concept models RAII wrappers for executor lifetime management.

```cpp
template<typename K>
concept keep_alive = /* implementation details */;
```

#### Requirements

A type `K` satisfies `keep_alive<K>` if:

- **Executor Access**: Provides `get()` method returning executor pointer
- **Work Delegation**: Provides `add()` method with move semantics
- **RAII Semantics**: Supports copy and move construction

#### Usage Examples

```cpp
#include <concepts/future.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Works with folly::Executor::KeepAlive
using KeepAliveType = folly::Executor::KeepAlive<folly::CPUThreadPoolExecutor>;
static_assert(keep_alive<KeepAliveType>);

// Example function using the concept
template<keep_alive KeepAliveType>
auto safe_schedule_work(KeepAliveType ka, std::function<void()> work) -> void {
    // The keep-alive ensures executor lifetime
    std::move(ka).add([work = std::move(work)](auto&&) {
        work();
    });
}

// Usage
folly::CPUThreadPoolExecutor thread_pool(4);
auto keep_alive = thread_pool.getKeepAliveToken();
safe_schedule_work(std::move(keep_alive), []() {
    std::cout << "Safely executed work!" << std::endl;
});
```

#### Folly Type Compatibility

- ✅ `folly::Executor::KeepAlive<T>` - Full compatibility

## Advanced Concepts

### future_factory Concept

Models types that provide factory functions for creating futures.

```cpp
template<typename Factory>
concept future_factory = /* implementation details */;
```

#### Requirements

- **Value Construction**: `makeFuture(value)` static method
- **Exception Construction**: `makeExceptionalFuture<T>(exception)` static method  
- **Ready Construction**: `makeReadyFuture()` static method

#### Usage Examples

```cpp
// Example factory implementation
struct MyFutureFactory {
    template<typename T>
    static auto makeFuture(T value) -> folly::Future<T> {
        return folly::makeFuture(std::move(value));
    }
    
    template<typename T>
    static auto makeExceptionalFuture(std::exception_ptr ex) -> folly::Future<T> {
        return folly::makeFuture<T>(folly::exception_wrapper(ex));
    }
    
    static auto makeReadyFuture() -> folly::Future<void> {
        return folly::makeFuture();
    }
};

static_assert(future_factory<MyFutureFactory>);

// Usage
template<future_factory Factory>
auto create_computation() -> auto {
    try {
        int result = compute_something();
        return Factory::makeFuture(result);
    } catch (...) {
        return Factory::template makeExceptionalFuture<int>(std::current_exception());
    }
}
```

### future_collector Concept

Models types that provide collection operations for futures.

```cpp
template<typename C>
concept future_collector = /* implementation details */;
```

#### Requirements

- **Collect All**: `collect_all()` method for waiting on all futures
- **Collect Any**: `collect_any()` method for waiting on first completion
- **Timeout Support**: `collect_all_timeout()` method with timeout parameter

#### Usage Examples

```cpp
// Example collector implementation
struct MyFutureCollector {
    template<typename T>
    static auto collect_all(std::vector<folly::Future<T>> futures) 
        -> folly::Future<std::vector<folly::Try<T>>> {
        return folly::collectAll(futures.begin(), futures.end());
    }
    
    template<typename T>
    static auto collect_any(std::vector<folly::Future<T>> futures)
        -> folly::Future<std::tuple<std::size_t, folly::Try<T>>> {
        return folly::collectAny(futures.begin(), futures.end());
    }
    
    template<typename T>
    static auto collect_all_timeout(std::vector<folly::Future<T>> futures, 
                                   std::chrono::milliseconds timeout) -> auto {
        return folly::collectAll(futures.begin(), futures.end())
            .within(timeout);
    }
};

static_assert(future_collector<MyFutureCollector>);

// Usage
template<future_collector Collector>
auto process_batch(std::vector<folly::Future<int>> futures) -> auto {
    return Collector::collect_all(std::move(futures))
        .thenValue([](std::vector<folly::Try<int>> results) {
            int sum = 0;
            for (const auto& result : results) {
                if (result.hasValue()) {
                    sum += result.value();
                }
            }
            return sum;
        });
}
```

### future_continuation Concept

Models futures that support executor-aware continuation operations.

```cpp
template<typename F, typename T>
concept future_continuation = future<F, T> && /* additional requirements */;
```

#### Requirements

- **Executor Attachment**: `via()` method for specifying execution context
- **Delayed Execution**: `delay()` method for time-based scheduling
- **Timeout Operations**: `within()` method for timeout handling

#### Usage Examples

```cpp
// Works with folly::Future
static_assert(future_continuation<folly::Future<int>, int>);

// Example function using the concept
template<future_continuation<std::string> FutureType, executor ExecutorType>
auto process_with_timeout(FutureType fut, ExecutorType& exec) -> auto {
    return std::move(fut)
        .via(&exec)                                    // Execute on specific executor
        .delay(std::chrono::milliseconds(100))         // Add delay
        .within(std::chrono::seconds(5))               // Add timeout
        .thenValue([](const std::string& value) {
            return "Processed: " + value;
        });
}

// Usage
folly::CPUThreadPoolExecutor executor(4);
auto future = folly::makeFuture<std::string>("Hello");
auto result = process_with_timeout(std::move(future), executor);
```

## Migration from Folly Types

### Direct Folly Usage

If you're currently using Folly types directly:

```cpp
// Before - Folly-specific code
void process_folly_future(folly::Future<int> future) {
    auto result = std::move(future).get();
    std::cout << "Result: " << result << std::endl;
}

// After - Generic code using concepts
template<future<int> FutureType>
void process_any_future(FutureType future) {
    auto result = std::move(future).get();
    std::cout << "Result: " << result << std::endl;
}
```

### Template Constraints

Replace ad-hoc template constraints with concepts:

```cpp
// Before - Manual SFINAE
template<typename F, 
         typename = std::enable_if_t<
             std::is_same_v<F, folly::Future<int>>
         >>
void process_future(F future) { /* ... */ }

// After - Clean concept constraints
template<future<int> FutureType>
void process_future(FutureType future) { /* ... */ }
```

### Library Integration

When writing libraries that work with multiple future types:

```cpp
// Library header
template<future<Response> FutureType>
class HttpClient {
public:
    auto send_request(const Request& req) -> FutureType;
    
private:
    template<promise<Response> PromiseType>
    void fulfill_request(PromiseType promise, const Request& req);
};

// Usage with folly
HttpClient<folly::Future<Response>> folly_client;

// Usage with custom future
HttpClient<kythira::Future<Response>> custom_client;
```

## Best Practices

### 1. Concept Composition

Combine concepts for more specific requirements:

```cpp
template<typename F, typename T>
concept async_processor = future<F, T> && 
                         future_continuation<F, T> &&
                         requires(F f) {
    { f.isReady() } -> std::convertible_to<bool>;
};
```

### 2. Concept Refinement

Create more specific concepts for domain-specific needs:

```cpp
template<typename F>
concept http_future = future<F, HttpResponse> &&
                     requires(F f) {
    { f.timeout(std::chrono::seconds{}) } -> future<HttpResponse>;
};
```

### 3. Error Handling

Always consider exception handling in concept-constrained code:

```cpp
template<future<int> FutureType>
auto safe_process(FutureType fut) -> int {
    try {
        return std::move(fut).get();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}
```

### 4. Performance Considerations

Concepts are compile-time constructs with no runtime overhead:

```cpp
// This has zero runtime cost
template<future<int> FutureType>
auto efficient_process(FutureType fut) -> int {
    // Concept checking happens at compile time
    return std::move(fut).get();
}
```

## Troubleshooting

### Common Compilation Errors

1. **Concept not satisfied:**
   ```
   error: constraints not satisfied for 'MyType'
   ```
   **Solution:** Ensure your type implements all required methods with correct signatures.

2. **Template argument deduction failed:**
   ```
   error: no matching function for template argument deduction
   ```
   **Solution:** Explicitly specify template arguments or ensure concept requirements are met.

3. **Ambiguous concept requirements:**
   ```
   error: ambiguous requirements in concept
   ```
   **Solution:** Check for conflicting method signatures or return types.

### Debugging Concept Failures

Use `requires` expressions to test specific requirements:

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Future.h>

// Test specific requirements
static_assert(requires(folly::Future<int> f) {
    { std::move(f).get() };
});

static_assert(requires(folly::Future<int> f) {
    { f.isReady() } -> std::convertible_to<bool>;
});

// Full concept check
static_assert(future<folly::Future<int>, int>);
```

### Performance Validation

Verify that concept-constrained code has equivalent performance:

```cpp
// Benchmark concept-constrained vs direct usage
template<future<int> FutureType>
auto benchmark_generic(FutureType fut) -> int {
    auto start = std::chrono::high_resolution_clock::now();
    auto result = std::move(fut).get();
    auto end = std::chrono::high_resolution_clock::now();
    return result;
}

auto benchmark_direct(folly::Future<int> fut) -> int {
    auto start = std::chrono::high_resolution_clock::now();
    auto result = std::move(fut).get();
    auto end = std::chrono::high_resolution_clock::now();
    return result;
}
```

## See Also

- [Future Migration Guide](future_migration_guide.md) - Migrating from old future patterns
- [Generic Future Architecture](generic_future_architecture.md) - Overall architecture design
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation
- [C++20 Concepts](https://en.cppreference.com/w/cpp/language/constraints) - C++20 concepts reference