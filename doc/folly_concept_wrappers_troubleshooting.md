# Folly Concept Wrappers Troubleshooting Guide

This guide helps developers troubleshoot common issues when using the Folly concept wrapper classes implemented in `include/raft/future.hpp`.

## Table of Contents

- [Compilation Issues](#compilation-issues)
- [Runtime Issues](#runtime-issues)
- [Performance Issues](#performance-issues)
- [Concept Compliance Issues](#concept-compliance-issues)
- [Interoperability Issues](#interoperability-issues)
- [Debugging Techniques](#debugging-techniques)
- [Common Patterns and Solutions](#common-patterns-and-solutions)

## Compilation Issues

### Issue 1: Concept Constraint Failures

**Symptoms:**
```
error: constraints not satisfied for 'MyType'
error: the required expression 'std::move(f).get()' is invalid
```

**Cause:** Your type doesn't satisfy all requirements of the concept.

**Solution:**

1. **Check individual requirements:**
```cpp
// Test specific requirements
static_assert(requires(MyFuture<int> f) {
    { std::move(f).get() };
});

static_assert(requires(MyFuture<int> f) {
    { f.isReady() } -> std::convertible_to<bool>;
});
```

2. **Verify method signatures:**
```cpp
class MyFuture {
public:
    // ✅ Correct - move semantics required
    auto get() && -> int;
    
    // ❌ Wrong - missing move semantics
    auto get() -> int;
    
    // ✅ Correct - bool-convertible return
    auto isReady() const -> bool;
    
    // ❌ Wrong - wrong return type
    auto isReady() const -> void;
};
```

3. **Check concept compliance:**
```cpp
static_assert(kythira::future<MyFuture<int>, int>);
```

### Issue 2: Move Semantics Errors

**Symptoms:**
```
error: cannot bind rvalue reference to lvalue
error: use of deleted function 'Future<T>::get() &'
```

**Cause:** Attempting to call move-only methods on lvalue references.

**Solution:**

```cpp
// ❌ Wrong - calling get() on lvalue
kythira::Future<int> future(42);
auto result = future.get(); // Compilation error

// ✅ Correct - explicit move
kythira::Future<int> future(42);
auto result = std::move(future).get();

// ✅ Correct - temporary object
auto result = kythira::FutureFactory::makeFuture(42).get();
```

### Issue 3: Void Type Handling Errors

**Symptoms:**
```
error: no matching function for call to 'setValue()'
error: 'void' is not a valid type for this context
```

**Cause:** Incorrect handling of void types in promises and futures.

**Solution:**

```cpp
// ❌ Wrong - void cannot be passed as value
kythira::SemiPromise<void> promise;
promise.setValue(void); // Invalid syntax

// ✅ Correct - use folly::Unit for void
kythira::SemiPromise<void> promise;
promise.setValue(folly::Unit{});

// ✅ Correct - use convenience method
promise.setValue(); // If available
```

### Issue 4: Exception Type Mismatches

**Symptoms:**
```
error: no matching function for call to 'setException'
error: cannot convert 'std::runtime_error' to 'folly::exception_wrapper'
```

**Cause:** Using wrong exception types with promise methods.

**Solution:**

```cpp
// ❌ Wrong - direct exception object
promise.setException(std::runtime_error("error"));

// ✅ Correct - folly::exception_wrapper
promise.setException(folly::exception_wrapper(std::runtime_error("error")));

// ✅ Correct - std::exception_ptr (auto-converted)
promise.setException(std::make_exception_ptr(std::runtime_error("error")));
```

### Issue 5: Template Argument Deduction Failures

**Symptoms:**
```
error: no matching function for template argument deduction
error: couldn't deduce template parameter 'T'
```

**Cause:** Compiler cannot deduce template arguments from function calls.

**Solution:**

```cpp
// ❌ Wrong - ambiguous template arguments
auto future = kythira::FutureFactory::makeExceptionalFuture(ex);

// ✅ Correct - explicit template argument
auto future = kythira::FutureFactory::makeExceptionalFuture<int>(ex);

// ✅ Correct - use auto with explicit construction
auto future = kythira::Future<int>(ex);
```

### Issue 6: Header Include Issues

**Symptoms:**
```
error: 'folly::Unit' has not been declared
error: 'kythira::Future' has not been declared
```

**Cause:** Missing required header includes.

**Solution:**

```cpp
// Required headers
#include <raft/future.hpp>          // Wrapper classes
#include <folly/Unit.h>             // For void type handling
#include <folly/ExceptionWrapper.h> // For exception handling
#include <folly/futures/Future.h>   // For Folly interop
#include <folly/futures/Promise.h>  // For Folly interop
```

## Runtime Issues

### Issue 1: Invalid Executor Usage

**Symptoms:**
- Runtime crashes when using executor
- "Executor is invalid" exceptions

**Cause:** Using null or destroyed executor.

**Solution:**

```cpp
// ❌ Wrong - null executor
kythira::Executor executor(nullptr); // Throws std::invalid_argument

// ✅ Correct - valid executor
folly::CPUThreadPoolExecutor thread_pool(4);
kythira::Executor executor(&thread_pool);

// ✅ Correct - check validity
if (executor.is_valid()) {
    executor.add([]() { /* work */ });
}
```

### Issue 2: Promise Already Fulfilled

**Symptoms:**
- Exceptions when setting promise values
- "Promise already fulfilled" errors

**Cause:** Attempting to fulfill an already fulfilled promise.

**Solution:**

```cpp
// ✅ Correct - check before fulfilling
kythira::SemiPromise<int> promise;
if (!promise.isFulfilled()) {
    promise.setValue(42);
}

// ✅ Correct - handle exceptions
try {
    promise.setValue(42);
} catch (const std::runtime_error& e) {
    std::cerr << "Promise already fulfilled: " << e.what() << std::endl;
}
```

### Issue 3: Future Already Consumed

**Symptoms:**
- Exceptions when calling get() multiple times
- "Future already consumed" errors

**Cause:** Calling get() on an already consumed future.

**Solution:**

```cpp
// ❌ Wrong - consuming future multiple times
kythira::Future<int> future(42);
auto result1 = std::move(future).get(); // OK
auto result2 = std::move(future).get(); // Error - future consumed

// ✅ Correct - consume once
kythira::Future<int> future(42);
auto result = std::move(future).get();

// ✅ Correct - share result if needed
kythira::Future<int> future(42);
auto shared_future = std::move(future).share(); // If available
```

### Issue 4: KeepAlive Lifetime Issues

**Symptoms:**
- Executor destroyed while work is pending
- Segmentation faults in async operations

**Cause:** Executor destroyed before KeepAlive releases it.

**Solution:**

```cpp
// ❌ Wrong - executor may be destroyed
{
    folly::CPUThreadPoolExecutor executor(4);
    kythira::KeepAlive keep_alive(&executor);
    
    std::thread([ka = std::move(keep_alive)]() {
        // Executor may be destroyed here
        ka.add([]() { /* work */ });
    }).detach();
} // executor destroyed here

// ✅ Correct - KeepAlive manages lifetime
auto create_keep_alive() -> kythira::KeepAlive {
    folly::CPUThreadPoolExecutor executor(4);
    return kythira::KeepAlive(&executor); // KeepAlive keeps executor alive
}

auto keep_alive = create_keep_alive();
std::thread([ka = std::move(keep_alive)]() {
    ka.add([]() { /* work executes safely */ });
}).detach();
```

## Performance Issues

### Issue 1: Unnecessary Copies

**Symptoms:**
- Slower performance than expected
- High memory usage

**Cause:** Creating unnecessary copies instead of using move semantics.

**Solution:**

```cpp
// ❌ Wrong - unnecessary copies
kythira::Future<std::string> future = create_future();
auto result = future.get(); // Copy

// ✅ Correct - move semantics
kythira::Future<std::string> future = create_future();
auto result = std::move(future).get(); // Move

// ✅ Correct - direct consumption
auto result = create_future().get(); // No intermediate variable
```

### Issue 2: Excessive Wrapper Conversions

**Symptoms:**
- Performance degradation in hot paths
- Unnecessary allocations

**Cause:** Converting between wrapper and Folly types repeatedly.

**Solution:**

```cpp
// ❌ Wrong - repeated conversions
for (int i = 0; i < 1000000; ++i) {
    auto folly_future = folly::makeFuture(i);
    auto wrapper_future = kythira::Future<int>(std::move(folly_future));
    auto folly_again = std::move(wrapper_future).get_folly_future();
    auto result = std::move(folly_again).get();
}

// ✅ Correct - minimize conversions
for (int i = 0; i < 1000000; ++i) {
    auto future = kythira::FutureFactory::makeFuture(i);
    auto result = std::move(future).get();
}
```

### Issue 3: Blocking Operations in Hot Paths

**Symptoms:**
- Poor throughput
- Thread pool starvation

**Cause:** Using blocking get() calls in performance-critical code.

**Solution:**

```cpp
// ❌ Wrong - blocking in hot path
for (auto& future : futures) {
    auto result = std::move(future).get(); // Blocks
    process_result(result);
}

// ✅ Correct - use async chaining
auto process_all = kythira::FutureCollector::collectAll(std::move(futures))
    .thenValue([](std::vector<kythira::Try<int>> results) {
        for (const auto& result : results) {
            if (result.hasValue()) {
                process_result(result.value());
            }
        }
    });
```

## Concept Compliance Issues

### Issue 1: Custom Type Not Satisfying Concepts

**Symptoms:**
- Static assertions failing
- Template instantiation errors

**Cause:** Custom implementation missing required methods or wrong signatures.

**Solution:**

```cpp
// ❌ Wrong - incomplete implementation
class MyFuture {
public:
    auto get() -> int; // Missing move semantics
    // Missing isReady() method
};

// ✅ Correct - complete implementation
class MyFuture {
public:
    auto get() && -> int;           // Move semantics required
    auto isReady() const -> bool;   // Status checking required
    
    template<typename F>
    auto thenValue(F&& func) && -> auto; // Continuation required
};

// Verify compliance
static_assert(kythira::future<MyFuture, int>);
```

### Issue 2: Concept Requirements Misunderstanding

**Symptoms:**
- Unexpected compilation failures
- Confusing error messages

**Cause:** Misunderstanding concept requirements.

**Solution:**

1. **Study concept definitions:**
```cpp
// Look at actual concept requirements
template<typename F, typename T>
concept future = requires(F f) {
    { std::move(f).get() };
    { f.isReady() } -> std::convertible_to<bool>;
    // ... other requirements
};
```

2. **Test individual requirements:**
```cpp
// Test each requirement separately
static_assert(requires(MyType f) {
    { std::move(f).get() };
});
```

3. **Use concept debugging:**
```cpp
// Enable verbose template error messages
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wfatal-errors"
static_assert(kythira::future<MyType, int>);
#pragma GCC diagnostic pop
```

## Interoperability Issues

### Issue 1: Folly Version Compatibility

**Symptoms:**
- Compilation errors with Folly methods
- Runtime behavior differences

**Cause:** Using incompatible Folly version.

**Solution:**

1. **Check Folly version requirements:**
```cpp
// Verify minimum Folly version
#if FOLLY_VERSION < 2021000000000ULL
#error "Folly version 2021.xx.xx.xx or later required"
#endif
```

2. **Use version-specific code:**
```cpp
// Conditional compilation for different Folly versions
#if FOLLY_VERSION >= 2022000000000ULL
    // Use newer Folly API
    auto future = folly::makeFuture(value);
#else
    // Use older Folly API
    auto future = folly::makeFuture<T>(value);
#endif
```

### Issue 2: Mixed Wrapper and Folly Usage

**Symptoms:**
- Type conversion errors
- Performance issues

**Cause:** Inefficient mixing of wrapper and Folly types.

**Solution:**

```cpp
// ❌ Wrong - frequent conversions
folly::Future<int> folly_future = folly::makeFuture(42);
kythira::Future<int> wrapper_future(std::move(folly_future));
folly::Future<int> back_to_folly = std::move(wrapper_future).get_folly_future();

// ✅ Correct - choose one approach
// Option 1: Use wrappers throughout
auto future = kythira::FutureFactory::makeFuture(42);
auto result = std::move(future).get();

// Option 2: Use Folly throughout
auto future = folly::makeFuture(42);
auto result = std::move(future).get();

// Option 3: Convert at boundaries only
auto process_with_wrappers(folly::Future<int> folly_fut) -> folly::Future<int> {
    kythira::Future<int> wrapper_fut(std::move(folly_fut));
    
    // Use wrapper operations
    auto processed = std::move(wrapper_fut)
        .thenValue([](int value) { return value * 2; });
    
    return std::move(processed).get_folly_future();
}
```

## Debugging Techniques

### Technique 1: Static Assertion Debugging

Use static assertions to verify assumptions:

```cpp
// Verify type properties
static_assert(std::is_move_constructible_v<kythira::Future<int>>);
static_assert(std::is_move_assignable_v<kythira::Future<int>>);
static_assert(!std::is_copy_constructible_v<kythira::Promise<int>>);

// Verify concept compliance
static_assert(kythira::future<kythira::Future<int>, int>);

// Verify type conversions
static_assert(std::is_same_v<
    kythira::detail::void_to_unit_t<void>, 
    folly::Unit
>);
```

### Technique 2: Requires Expression Testing

Test individual concept requirements:

```cpp
// Test specific method requirements
template<typename F>
constexpr bool has_get_method() {
    return requires(F f) {
        { std::move(f).get() };
    };
}

template<typename F>
constexpr bool has_is_ready_method() {
    return requires(F f) {
        { f.isReady() } -> std::convertible_to<bool>;
    };
}

// Use in debugging
static_assert(has_get_method<kythira::Future<int>>());
static_assert(has_is_ready_method<kythira::Future<int>>());
```

### Technique 3: Compilation Error Analysis

Enable verbose error messages:

```cpp
// GCC: Enable verbose template errors
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-ftemplate-backtrace-limit=0"

// Clang: Enable verbose template errors
#pragma clang diagnostic push
#pragma clang diagnostic error "-ftemplate-backtrace-limit=0"

// Test problematic code here
static_assert(kythira::future<ProblematicType, int>);

#pragma GCC diagnostic pop
#pragma clang diagnostic pop
```

### Technique 4: Runtime Debugging

Add debug output for runtime issues:

```cpp
// Debug promise fulfillment
template<typename P>
void debug_fulfill_promise(P& promise, int value) {
    std::cout << "Promise fulfilled: " << promise.isFulfilled() << std::endl;
    
    if (!promise.isFulfilled()) {
        promise.setValue(value);
        std::cout << "Promise now fulfilled: " << promise.isFulfilled() << std::endl;
    } else {
        std::cout << "Promise already fulfilled!" << std::endl;
    }
}

// Debug executor state
void debug_executor_state(const kythira::Executor& executor) {
    std::cout << "Executor valid: " << executor.is_valid() << std::endl;
    std::cout << "Executor pointer: " << executor.get() << std::endl;
}
```

### Technique 5: Memory Debugging

Use memory debugging tools:

```cpp
// Valgrind: Check for memory leaks
// valgrind --leak-check=full ./your_program

// AddressSanitizer: Compile with -fsanitize=address
// g++ -fsanitize=address -g your_program.cpp

// Debug memory usage
#include <iostream>
#include <memory>

void debug_memory_usage() {
    std::cout << "Creating futures..." << std::endl;
    
    {
        std::vector<kythira::Future<int>> futures;
        for (int i = 0; i < 1000; ++i) {
            futures.push_back(kythira::FutureFactory::makeFuture(i));
        }
        std::cout << "Futures created, memory usage should be high" << std::endl;
    }
    
    std::cout << "Futures destroyed, memory should be released" << std::endl;
}
```

## Common Patterns and Solutions

### Pattern 1: Safe Async Operations

**Problem:** Ensuring safe async operations with proper lifetime management.

**Solution:**
```cpp
class SafeAsyncProcessor {
public:
    template<kythira::future<int> FutureType>
    auto process_safely(FutureType future) -> kythira::Future<int> {
        return std::move(future)
            .thenValue([this](int value) {
                return this->process_value(value);
            })
            .thenError([](std::exception_ptr ex) {
                std::cerr << "Error in async processing" << std::endl;
                return -1; // Default value
            })
            .ensure([this]() {
                this->cleanup();
            });
    }
    
private:
    auto process_value(int value) -> int { return value * 2; }
    auto cleanup() -> void { /* cleanup code */ }
};
```

### Pattern 2: Generic Future Processing

**Problem:** Writing generic code that works with different future types.

**Solution:**
```cpp
template<kythira::future<T> FutureType, typename T>
auto process_future_generically(FutureType future) -> T {
    if (future.isReady()) {
        return std::move(future).get();
    }
    
    // Add timeout and error handling
    return std::move(future)
        .within(std::chrono::seconds(5))
        .thenError([](std::exception_ptr ex) -> T {
            std::cerr << "Future processing failed" << std::endl;
            if constexpr (std::is_default_constructible_v<T>) {
                return T{};
            } else {
                std::rethrow_exception(ex);
            }
        })
        .get();
}
```

### Pattern 3: Batch Processing with Error Handling

**Problem:** Processing multiple futures with proper error handling.

**Solution:**
```cpp
template<typename T>
auto process_batch_safely(std::vector<kythira::Future<T>> futures) 
    -> kythira::Future<std::vector<T>> {
    
    return kythira::FutureCollector::collectAll(std::move(futures))
        .thenValue([](std::vector<kythira::Try<T>> results) {
            std::vector<T> successful_results;
            
            for (const auto& result : results) {
                if (result.hasValue()) {
                    successful_results.push_back(result.value());
                } else {
                    std::cerr << "Future failed in batch" << std::endl;
                }
            }
            
            return successful_results;
        });
}
```

### Pattern 4: Resource Management with KeepAlive

**Problem:** Ensuring executor lifetime in async operations.

**Solution:**
```cpp
class ResourceManager {
public:
    auto schedule_work_safely(std::function<void()> work) -> void {
        auto keep_alive = _executor.getKeepAliveToken();
        
        std::thread([ka = std::move(keep_alive), work = std::move(work)]() {
            // Executor guaranteed to be alive
            ka.add([work = std::move(work)](auto&&) {
                work();
            });
        }).detach();
    }
    
private:
    folly::CPUThreadPoolExecutor _thread_pool{4};
    kythira::Executor _executor{&_thread_pool};
};
```

### Pattern 5: Type-Safe Promise Fulfillment

**Problem:** Safely fulfilling promises with proper error handling.

**Solution:**
```cpp
template<kythira::semi_promise<T> PromiseType, typename T>
auto fulfill_promise_safely(PromiseType& promise, std::function<T()> producer) -> void {
    if (promise.isFulfilled()) {
        std::cerr << "Promise already fulfilled" << std::endl;
        return;
    }
    
    try {
        if constexpr (std::is_void_v<T>) {
            producer();
            promise.setValue(folly::Unit{});
        } else {
            auto value = producer();
            promise.setValue(std::move(value));
        }
    } catch (const std::exception& e) {
        promise.setException(folly::exception_wrapper(e));
    } catch (...) {
        promise.setException(folly::exception_wrapper(
            std::runtime_error("Unknown error in promise fulfillment")
        ));
    }
}
```

## Getting Help

If you're still experiencing issues:

1. **Check Static Assertions**: Verify your types satisfy the required concepts
2. **Review Examples**: Look at working examples in `examples/folly-wrappers/`
3. **Enable Verbose Errors**: Use compiler flags for detailed error messages
4. **Test Incrementally**: Isolate the problematic code
5. **Check Folly Version**: Ensure compatibility with your Folly version
6. **Review Documentation**: Check the API reference and migration guide

## See Also

- [Folly Concept Wrappers Documentation](folly_concept_wrappers_documentation.md) - Comprehensive usage guide
- [Folly Concept Wrappers API Reference](folly_concept_wrappers_api_reference.md) - Complete API documentation
- [Folly Concept Wrappers Migration Guide](folly_concept_wrappers_migration_guide.md) - Migration from direct Folly usage
- [Example Programs](../examples/folly-wrappers/) - Working code examples
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation