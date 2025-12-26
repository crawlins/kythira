# Concepts API Reference

This document provides a complete API reference for the enhanced C++20 concepts defined in `include/concepts/future.hpp`.

## Table of Contents

- [Core Concepts](#core-concepts)
  - [try_type](#try_type)
  - [future](#future)
  - [semi_promise](#semi_promise)
  - [promise](#promise)
  - [executor](#executor)
  - [keep_alive](#keep_alive)
- [Advanced Concepts](#advanced-concepts)
  - [future_factory](#future_factory)
  - [future_collector](#future_collector)
  - [future_continuation](#future_continuation)
  - [future_transformable](#future_transformable)
- [Type Compatibility](#type-compatibility)
- [Static Assertions](#static-assertions)

## Core Concepts

### try_type

Models types that can hold either a value or an exception, similar to `folly::Try<T>`.

#### Signature

```cpp
template<typename T, typename ValueType>
concept try_type = /* implementation */;
```

#### Template Parameters

- `T`: The Try-like type to constrain
- `ValueType`: The value type held by the Try (use `void` for void specializations)

#### Requirements

A type `T` satisfies `try_type<T, ValueType>` if it provides:

##### For Non-Void Types

```cpp
// Value access methods
ValueType& value();                    // Non-const value access
const ValueType& value() const;        // Const value access

// State checking methods  
bool hasValue() const;                 // Check if contains value
bool hasException() const;             // Check if contains exception

// Exception access
auto exception() const;                // Access stored exception
```

##### For Void Types

```cpp
// State checking methods (same as non-void)
bool hasValue() const;                 // Check if contains value
bool hasException() const;             // Check if contains exception

// Exception access (same as non-void)
auto exception() const;                // Access stored exception
```

#### Compatible Types

- ✅ `folly::Try<T>` - Full compatibility
- ✅ `kythira::Try<T>` - Full compatibility (if implemented)

#### Usage Example

```cpp
template<try_type<int> TryType>
auto safe_extract(const TryType& t) -> std::optional<int> {
    if (t.hasValue()) {
        return t.value();
    }
    return std::nullopt;
}

// Usage
folly::Try<int> success = folly::Try<int>(42);
folly::Try<int> failure = folly::Try<int>(std::runtime_error("error"));

auto val1 = safe_extract(success);  // Returns std::optional<int>(42)
auto val2 = safe_extract(failure);  // Returns std::nullopt
```

---

### future

Models asynchronous computation results, similar to `folly::Future<T>`.

#### Signature

```cpp
template<typename F, typename T>
concept future = /* implementation */;
```

#### Template Parameters

- `F`: The Future-like type to constrain
- `T`: The result type of the future (use `void` for void futures)

#### Requirements

A type `F` satisfies `future<F, T>` if it provides:

```cpp
// Value retrieval (requires move semantics)
auto get() &&;                        // Get result (consumes future)

// Status checking
bool isReady() const;                  // Check if result is available

// Continuation chaining (void-aware)
auto thenValue(Func&& func) &&;       // Chain value-based continuation
```

#### Void Specialization

For `future<F, void>`, the `thenValue` method accepts functions with signature `void()`:

```cpp
// For void futures
auto thenValue(std::function<void()> func) &&;
```

#### Compatible Types

- ✅ `folly::Future<T>` - Full compatibility
- ✅ `folly::SemiFuture<T>` - Partial compatibility (missing some methods)
- ✅ `kythira::Future<T>` - Full compatibility (if implemented)

#### Usage Example

```cpp
template<future<int> FutureType>
auto process_result(FutureType fut) -> int {
    if (fut.isReady()) {
        return std::move(fut).get();
    }
    
    return std::move(fut)
        .thenValue([](int value) { return value * 2; })
        .get();
}

// Usage
folly::Future<int> f = folly::makeFuture(21);
auto result = process_result(std::move(f)); // Returns 42
```

---

### semi_promise

Models promise types that can be fulfilled but don't provide future access.

#### Signature

```cpp
template<typename P, typename T>
concept semi_promise = /* implementation */;
```

#### Template Parameters

- `P`: The Promise-like type to constrain
- `T`: The value type of the promise (use `void` for void promises)

#### Requirements

A type `P` satisfies `semi_promise<P, T>` if it provides:

```cpp
// State checking
bool isFulfilled() const;              // Check if promise is fulfilled

// Exception setting
void setException(folly::exception_wrapper ex); // Set exception

// Value setting (type-dependent)
void setValue(T value);                // For non-void types
void setValue(folly::Unit unit);       // For void types (use folly::Unit{})
```

#### Void Specialization

For `semi_promise<P, void>`, the `setValue` method accepts `folly::Unit`:

```cpp
promise.setValue(folly::Unit{});
```

#### Compatible Types

- ✅ `folly::Promise<T>` - Full compatibility (includes semi-promise functionality)

#### Usage Example

```cpp
template<semi_promise<std::string> PromiseType>
auto fulfill_greeting(PromiseType& promise, const std::string& name) -> void {
    if (!promise.isFulfilled()) {
        try {
            promise.setValue("Hello, " + name + "!");
        } catch (const std::exception& e) {
            promise.setException(folly::exception_wrapper(e));
        }
    }
}

// Usage
folly::Promise<std::string> promise;
fulfill_greeting(promise, "World");
```

---

### promise

Models promise types that extend semi-promise with future access capabilities.

#### Signature

```cpp
template<typename P, typename T>
concept promise = semi_promise<P, T> && /* additional requirements */;
```

#### Template Parameters

- `P`: The Promise-like type to constrain
- `T`: The value type of the promise

#### Requirements

A type `P` satisfies `promise<P, T>` if it satisfies `semi_promise<P, T>` and provides:

```cpp
// Future access methods
auto getFuture();                      // Get associated future
auto getSemiFuture();                  // Get associated semi-future
```

#### Compatible Types

- ✅ `folly::Promise<T>` - Full compatibility

#### Usage Example

```cpp
template<promise<int> PromiseType>
auto create_async_computation(PromiseType promise) -> auto {
    auto future = promise.getFuture();
    
    std::thread([p = std::move(promise)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        p.setValue(42);
    }).detach();
    
    return future;
}

// Usage
folly::Promise<int> promise;
auto future = create_async_computation(std::move(promise));
auto result = std::move(future).get(); // Returns 42
```

---

### executor

Models types that can execute work asynchronously.

#### Signature

```cpp
template<typename E>
concept executor = /* implementation */;
```

#### Template Parameters

- `E`: The Executor-like type to constrain

#### Requirements

A type `E` satisfies `executor<E>` if it provides:

```cpp
// Work execution
void add(std::function<void()> func);  // Execute function asynchronously
```

#### Compatible Types

- ✅ `folly::CPUThreadPoolExecutor` - Full compatibility
- ✅ `folly::IOThreadPoolExecutor` - Full compatibility
- ✅ `folly::InlineExecutor` - Full compatibility
- ✅ `folly::ManualExecutor` - Full compatibility

#### Usage Example

```cpp
template<executor ExecutorType>
auto schedule_batch_work(ExecutorType& exec, int num_tasks) -> void {
    for (int i = 0; i < num_tasks; ++i) {
        exec.add([i]() {
            std::cout << "Task " << i << " executing\n";
        });
    }
}

// Usage
folly::CPUThreadPoolExecutor thread_pool(4);
schedule_batch_work(thread_pool, 10);
```

---

### keep_alive

Models RAII wrappers for executor lifetime management.

#### Signature

```cpp
template<typename K>
concept keep_alive = /* implementation */;
```

#### Template Parameters

- `K`: The KeepAlive-like type to constrain

#### Requirements

A type `K` satisfies `keep_alive<K>` if it provides:

```cpp
// Executor access
auto get();                            // Get underlying executor pointer

// Copy and move construction
K(const K& other);                     // Copy constructor
K(K&& other);                          // Move constructor

// Work delegation (with move semantics)
void add(std::function<void(K&&)> func) &&; // Add work with keep-alive
```

#### Compatible Types

- ✅ `folly::Executor::KeepAlive<T>` - Full compatibility

#### Usage Example

```cpp
template<keep_alive KeepAliveType>
auto schedule_safe_work(KeepAliveType ka, const std::string& task_name) -> void {
    std::move(ka).add([task_name](auto&&) {
        std::cout << "Executing safe task: " << task_name << "\n";
    });
}

// Usage
folly::CPUThreadPoolExecutor executor(4);
auto keep_alive = executor.getKeepAliveToken();
schedule_safe_work(std::move(keep_alive), "Critical Task");
```

## Advanced Concepts

### future_factory

Models types that provide factory functions for creating futures.

#### Signature

```cpp
template<typename Factory>
concept future_factory = /* implementation */;
```

#### Requirements

A type `Factory` satisfies `future_factory<Factory>` if it provides static methods:

```cpp
// Factory methods
template<typename T>
static auto makeFuture(T value);      // Create future from value

template<typename T>  
static auto makeExceptionalFuture(std::exception_ptr ex); // Create future from exception

static auto makeReadyFuture();        // Create ready void future
```

#### Return Type Requirements

The factory methods must return types that satisfy the `future` concept:

```cpp
static_assert(future<decltype(Factory::makeFuture(42)), int>);
static_assert(future<decltype(Factory::makeReadyFuture()), void>);
```

#### Usage Example

```cpp
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

template<future_factory Factory>
auto safe_divide(int a, int b) -> auto {
    if (b == 0) {
        return Factory::template makeExceptionalFuture<double>(
            std::make_exception_ptr(std::invalid_argument("Division by zero"))
        );
    }
    return Factory::makeFuture(static_cast<double>(a) / b);
}
```

---

### future_collector

Models types that provide collection operations for futures.

#### Signature

```cpp
template<typename C>
concept future_collector = /* implementation */;
```

#### Requirements

A type `C` satisfies `future_collector<C>` if it provides static methods:

```cpp
// Collection methods
template<typename T>
static auto collect_all(std::vector<Future<T>> futures) 
    -> future<std::vector<Try<T>>>;    // Collect all futures

template<typename T>
static auto collect_any(std::vector<Future<T>> futures)
    -> future<std::tuple<std::size_t, Try<T>>>; // Collect first completion

template<typename T>
static auto collect_all_timeout(std::vector<Future<T>> futures, 
                               std::chrono::milliseconds timeout); // Collect with timeout
```

#### Usage Example

```cpp
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
        return folly::collectAll(futures.begin(), futures.end()).within(timeout);
    }
};

static_assert(future_collector<MyFutureCollector>);
```

---

### future_continuation

Models futures that support executor-aware continuation operations.

#### Signature

```cpp
template<typename F, typename T>
concept future_continuation = future<F, T> && /* additional requirements */;
```

#### Requirements

A type `F` satisfies `future_continuation<F, T>` if it satisfies `future<F, T>` and provides:

```cpp
// Executor-aware operations
auto via(void* executor_ptr);          // Attach to executor
auto delay(std::chrono::milliseconds duration); // Add delay
auto within(std::chrono::milliseconds timeout); // Add timeout
```

#### Compatible Types

- ✅ `folly::Future<T>` - Full compatibility
- ✅ `folly::SemiFuture<T>` - Partial compatibility

#### Usage Example

```cpp
template<future_continuation<std::string> FutureType, executor ExecutorType>
auto process_with_scheduling(FutureType fut, ExecutorType& exec) -> auto {
    return std::move(fut)
        .via(&exec)                    // Execute on specific executor
        .delay(std::chrono::milliseconds(100)) // Add 100ms delay
        .within(std::chrono::seconds(5))       // 5 second timeout
        .thenValue([](const std::string& value) {
            return "Processed: " + value;
        });
}
```

---

### future_transformable

Models futures that support transformation and error handling operations.

#### Signature

```cpp
template<typename F, typename T>
concept future_transformable = future<F, T> && /* additional requirements */;
```

#### Requirements

A type `F` satisfies `future_transformable<F, T>` if it satisfies `future<F, T>` and provides:

```cpp
// Transformation operations
template<typename Func>
auto thenValue(Func&& func);           // Transform value

template<typename Func>
auto thenError(Func&& func);           // Handle errors

template<typename Func>
auto ensure(Func&& func);              // Finally-like behavior
```

#### Usage Example

```cpp
template<future_transformable<int> FutureType>
auto safe_process_number(FutureType fut) -> auto {
    return std::move(fut)
        .thenValue([](int value) {
            return value * 2;          // Transform success case
        })
        .thenError([](std::exception_ptr ex) {
            std::cerr << "Error occurred, using default\n";
            return -1;                 // Handle error case
        })
        .ensure([]() {
            std::cout << "Processing completed\n"; // Always execute
        });
}
```

## Type Compatibility

### Folly Types Compatibility Matrix

| Concept | folly::Future<T> | folly::SemiFuture<T> | folly::Promise<T> | folly::Try<T> | folly::Executor |
|---------|------------------|----------------------|-------------------|---------------|-----------------|
| `try_type<T, ValueType>` | ❌ | ❌ | ❌ | ✅ | ❌ |
| `future<F, T>` | ✅ | ⚠️ | ❌ | ❌ | ❌ |
| `semi_promise<P, T>` | ❌ | ❌ | ✅ | ❌ | ❌ |
| `promise<P, T>` | ❌ | ❌ | ✅ | ❌ | ❌ |
| `executor<E>` | ❌ | ❌ | ❌ | ❌ | ✅ |
| `keep_alive<K>` | ❌ | ❌ | ❌ | ❌ | ❌ |
| `future_continuation<F, T>` | ✅ | ⚠️ | ❌ | ❌ | ❌ |
| `future_transformable<F, T>` | ✅ | ⚠️ | ❌ | ❌ | ❌ |

**Legend:**
- ✅ Full compatibility
- ⚠️ Partial compatibility (some methods may be missing)
- ❌ Not compatible

### KeepAlive Types

For `keep_alive` concept, use:
- ✅ `folly::Executor::KeepAlive<folly::CPUThreadPoolExecutor>`
- ✅ `folly::Executor::KeepAlive<folly::IOThreadPoolExecutor>`
- ✅ `folly::Executor::KeepAlive<ExecutorType>` (for any executor type)

## Static Assertions

### Verifying Type Compatibility

Use static assertions to verify that types satisfy concepts:

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

// Verify Folly types
static_assert(kythira::try_type<folly::Try<int>, int>);
static_assert(kythira::future<folly::Future<int>, int>);
static_assert(kythira::promise<folly::Promise<int>, int>);
static_assert(kythira::executor<folly::CPUThreadPoolExecutor>);

// Verify void specializations
static_assert(kythira::try_type<folly::Try<void>, void>);
static_assert(kythira::future<folly::Future<void>, void>);
static_assert(kythira::promise<folly::Promise<void>, void>);

// Verify KeepAlive types
using KeepAliveType = folly::Executor::KeepAlive<folly::CPUThreadPoolExecutor>;
static_assert(kythira::keep_alive<KeepAliveType>);
```

### Testing Custom Types

When implementing custom types, verify they satisfy the concepts:

```cpp
// Custom future implementation
class MyFuture {
    // Implementation...
public:
    auto get() && -> int;
    auto isReady() const -> bool;
    template<typename F>
    auto thenValue(F&& func) && -> auto;
};

// Verify custom type satisfies concept
static_assert(kythira::future<MyFuture, int>);
```

### Debugging Concept Failures

Use `requires` expressions to test specific requirements:

```cpp
// Test individual requirements
static_assert(requires(folly::Future<int> f) {
    { std::move(f).get() };
});

static_assert(requires(folly::Future<int> f) {
    { f.isReady() } -> std::convertible_to<bool>;
});

// Test full concept
static_assert(kythira::future<folly::Future<int>, int>);
```

## See Also

- [Concepts Documentation](concepts_documentation.md) - Comprehensive usage guide
- [Concepts Migration Guide](concepts_migration_guide.md) - Migration from older versions
- [Future Migration Guide](future_migration_guide.md) - General future migration patterns
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation