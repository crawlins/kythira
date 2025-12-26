# Folly Concept Wrappers Migration Guide

This guide helps developers migrate to using the Folly concept wrapper classes implemented in `include/raft/future.hpp`. The wrappers provide a concept-compliant interface for Folly types while maintaining full compatibility and performance.

## Overview

The Folly concept wrappers allow you to:

1. **Use Folly types with concept-constrained templates**
2. **Write generic asynchronous code** that works with multiple future implementations
3. **Maintain type safety** through compile-time concept validation
4. **Preserve performance** with zero-overhead abstractions
5. **Integrate seamlessly** with existing Folly-based codebases

## Migration Scenarios

### Scenario 1: Direct Folly Usage → Wrapper Usage

**When to migrate**: You want to use concept-constrained templates or write generic asynchronous code.

**Before (Direct Folly):**
```cpp
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

void process_folly_types() {
    // Direct Folly usage
    folly::Promise<int> promise;
    auto future = promise.getFuture();
    
    promise.setValue(42);
    auto result = std::move(future).get();
    
    std::cout << "Result: " << result << std::endl;
}
```

**After (Wrapper Usage):**
```cpp
#include <raft/future.hpp>

void process_wrapper_types() {
    // Wrapper usage - same interface, concept compliant
    kythira::Promise<int> promise;
    auto future = promise.getFuture();
    
    promise.setValue(42);
    auto result = std::move(future).get();
    
    std::cout << "Result: " << result << std::endl;
}
```

**Benefits:**
- Same performance and behavior
- Concept compliance enables generic programming
- Type safety through compile-time validation

### Scenario 2: Template Functions → Concept-Constrained Templates

**When to migrate**: You want to replace SFINAE or unconstrained templates with clear concept constraints.

**Before (Unconstrained Templates):**
```cpp
// Unconstrained template - works with any type
template<typename FutureType>
auto process_future(FutureType future) -> auto {
    // No compile-time validation of interface
    return std::move(future).get();
}

// SFINAE-based constraints (complex and error-prone)
template<typename F, 
         typename = std::enable_if_t<
             std::is_same_v<F, folly::Future<int>>
         >>
auto process_folly_future(F future) -> int {
    return std::move(future).get();
}
```

**After (Concept-Constrained Templates):**
```cpp
#include <raft/future.hpp>

// Clear concept constraints with compile-time validation
template<kythira::future<int> FutureType>
auto process_future(FutureType future) -> int {
    // Guaranteed to have future interface
    return std::move(future).get();
}

// Works with both Folly futures and other implementations
template<kythira::future<std::string> FutureType>
auto process_string_future(FutureType future) -> std::string {
    return std::move(future)
        .thenValue([](const std::string& s) {
            return "Processed: " + s;
        })
        .get();
}
```

**Benefits:**
- Clear interface requirements
- Better error messages
- Works with multiple future implementations
- Self-documenting code

### Scenario 3: Library Development → Generic Library

**When to migrate**: You're developing a library that should work with different future implementations.

**Before (Folly-Specific Library):**
```cpp
// Library tied to Folly types
class HttpClient {
public:
    auto get(const std::string& url) -> folly::Future<Response> {
        folly::Promise<Response> promise;
        auto future = promise.getFuture();
        
        // Async HTTP request implementation
        make_request(url, std::move(promise));
        
        return future;
    }
    
private:
    void make_request(const std::string& url, folly::Promise<Response> promise) {
        // Implementation using Folly types
    }
};
```

**After (Generic Library):**
```cpp
#include <raft/future.hpp>

// Generic library that works with any future implementation
template<template<typename> class FutureType = kythira::Future,
         template<typename> class PromiseType = kythira::Promise>
class HttpClient {
public:
    auto get(const std::string& url) -> FutureType<Response> 
        requires kythira::future<FutureType<Response>, Response> {
        
        PromiseType<Response> promise;
        auto future = promise.getFuture();
        
        // Same async HTTP request implementation
        make_request(url, std::move(promise));
        
        return future;
    }
    
private:
    template<kythira::promise<Response> P>
    void make_request(const std::string& url, P promise) {
        // Implementation works with any promise type
    }
};

// Usage with Folly wrappers (default)
HttpClient<> folly_client;

// Usage with custom future implementation
HttpClient<MyFuture, MyPromise> custom_client;
```

**Benefits:**
- Library works with multiple future implementations
- Users can choose their preferred future library
- Maintains type safety through concepts
- Easy to test with mock implementations

## Step-by-Step Migration Process

### Step 1: Add Required Headers

Replace or supplement Folly headers with wrapper headers:

```cpp
// Add wrapper header
#include <raft/future.hpp>

// Keep Folly headers if needed for interop
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
```

### Step 2: Replace Type Declarations

Replace Folly types with wrapper types:

```cpp
// Before
folly::Promise<int> promise;
folly::Future<int> future;
folly::Try<int> try_result;

// After
kythira::Promise<int> promise;
kythira::Future<int> future;
kythira::Try<int> try_result;
```

### Step 3: Update Function Signatures

Add concept constraints to function parameters:

```cpp
// Before
template<typename F>
auto process_future(F future) -> auto;

void handle_promise(folly::Promise<int>& promise);

// After
template<kythira::future<int> FutureType>
auto process_future(FutureType future) -> int;

template<kythira::promise<int> PromiseType>
void handle_promise(PromiseType& promise);
```

### Step 4: Handle Void Types

Update void type handling to use `folly::Unit`:

```cpp
// Before (may not work consistently)
folly::Promise<void> void_promise;
void_promise.setValue();

// After (explicit and consistent)
kythira::Promise<void> void_promise;
void_promise.setValue(folly::Unit{});
// or use convenience method
void_promise.setValue();
```

### Step 5: Update Exception Handling

Ensure exception handling uses `folly::exception_wrapper`:

```cpp
// Before (may use std::exception_ptr)
try {
    // some operation
} catch (const std::exception& e) {
    promise.setException(std::make_exception_ptr(e));
}

// After (consistent with Folly)
try {
    // some operation
} catch (const std::exception& e) {
    promise.setException(folly::exception_wrapper(e));
}
// Note: std::exception_ptr is also supported via automatic conversion
```

### Step 6: Verify Move Semantics

Ensure proper move semantics for future consumption:

```cpp
// Before (may work with some implementations)
auto result = future.get();

// After (explicit move required)
auto result = std::move(future).get();
```

### Step 7: Add Static Assertions

Verify concept compliance at compile time:

```cpp
// Add static assertions to verify concept compliance
static_assert(kythira::future<kythira::Future<int>, int>);
static_assert(kythira::promise<kythira::Promise<int>, int>);
static_assert(kythira::try_type<kythira::Try<int>, int>);
```

## Common Migration Patterns

### Pattern 1: Async Function Migration

**Before:**
```cpp
folly::Future<std::string> fetch_data(const std::string& url) {
    folly::Promise<std::string> promise;
    auto future = promise.getFuture();
    
    // Async operation
    start_async_fetch(url, [p = std::move(promise)](const std::string& data) mutable {
        p.setValue(data);
    });
    
    return future;
}
```

**After:**
```cpp
template<template<typename> class FutureType = kythira::Future,
         template<typename> class PromiseType = kythira::Promise>
auto fetch_data(const std::string& url) -> FutureType<std::string>
    requires kythira::future<FutureType<std::string>, std::string> &&
             kythira::promise<PromiseType<std::string>, std::string> {
    
    PromiseType<std::string> promise;
    auto future = promise.getFuture();
    
    // Same async operation
    start_async_fetch(url, [p = std::move(promise)](const std::string& data) mutable {
        p.setValue(data);
    });
    
    return future;
}

// Convenience alias for default usage
auto fetch_data_simple(const std::string& url) -> kythira::Future<std::string> {
    return fetch_data(url);
}
```

### Pattern 2: Future Chaining Migration

**Before:**
```cpp
auto process_data_chain() -> folly::Future<int> {
    return fetch_string_data()
        .then([](const std::string& data) {
            return parse_number(data);
        })
        .then([](int number) {
            return number * 2;
        });
}
```

**After:**
```cpp
template<kythira::future<std::string> StringFutureType = kythira::Future<std::string>>
auto process_data_chain() -> kythira::Future<int> {
    return fetch_string_data()
        .thenValue([](const std::string& data) {
            return parse_number(data);
        })
        .thenValue([](int number) {
            return number * 2;
        });
}
```

### Pattern 3: Error Handling Migration

**Before:**
```cpp
auto safe_operation() -> folly::Future<int> {
    return risky_operation()
        .onError([](folly::exception_wrapper ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
            return 0; // Default value
        });
}
```

**After:**
```cpp
template<kythira::future<int> FutureType = kythira::Future<int>>
auto safe_operation() -> kythira::Future<int> {
    return risky_operation()
        .thenError([](std::exception_ptr ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
            return 0; // Default value
        });
}
```

### Pattern 4: Executor Usage Migration

**Before:**
```cpp
void schedule_work_folly(folly::Executor* executor) {
    executor->add([]() {
        std::cout << "Work executed" << std::endl;
    });
}
```

**After:**
```cpp
template<kythira::executor ExecutorType>
void schedule_work_generic(ExecutorType& executor) {
    executor.add([]() {
        std::cout << "Work executed" << std::endl;
    });
}

// Usage with Folly executor via wrapper
void example_usage() {
    folly::CPUThreadPoolExecutor thread_pool(4);
    kythira::Executor executor(&thread_pool);
    schedule_work_generic(executor);
}
```

### Pattern 5: Collection Operations Migration

**Before:**
```cpp
auto collect_results() -> folly::Future<std::vector<folly::Try<int>>> {
    std::vector<folly::Future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(folly::makeFuture(i));
    }
    
    return folly::collectAll(futures.begin(), futures.end());
}
```

**After:**
```cpp
auto collect_results() -> kythira::Future<std::vector<kythira::Try<int>>> {
    std::vector<kythira::Future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(kythira::FutureFactory::makeFuture(i));
    }
    
    return kythira::FutureCollector::collectAll(std::move(futures));
}
```

## Interoperability Strategies

### Strategy 1: Gradual Migration

Migrate incrementally by wrapping Folly types at API boundaries:

```cpp
// Existing Folly-based internal implementation
namespace internal {
    folly::Future<Data> fetch_data_impl(const Request& req) {
        // Existing implementation using Folly
    }
}

// New concept-compliant public API
template<kythira::future<Data> FutureType = kythira::Future<Data>>
auto fetch_data(const Request& req) -> FutureType {
    // Wrap internal Folly future
    auto folly_future = internal::fetch_data_impl(req);
    return kythira::Future<Data>(std::move(folly_future));
}
```

### Strategy 2: Dual Interface Support

Provide both Folly and wrapper interfaces during transition:

```cpp
class DataService {
public:
    // Legacy Folly interface
    auto fetch_folly(const std::string& id) -> folly::Future<Data> {
        return fetch_impl(id).get_folly_future();
    }
    
    // New wrapper interface
    auto fetch(const std::string& id) -> kythira::Future<Data> {
        return fetch_impl(id);
    }
    
private:
    auto fetch_impl(const std::string& id) -> kythira::Future<Data> {
        // Implementation using wrappers
    }
};
```

### Strategy 3: Template-Based Abstraction

Use templates to support both Folly and wrapper types:

```cpp
template<typename FutureType>
class AsyncProcessor {
public:
    auto process(const Input& input) -> FutureType {
        if constexpr (std::is_same_v<FutureType, folly::Future<Output>>) {
            // Return unwrapped Folly future
            return process_impl(input).get_folly_future();
        } else {
            // Return wrapper future
            return process_impl(input);
        }
    }
    
private:
    auto process_impl(const Input& input) -> kythira::Future<Output> {
        // Implementation using wrappers
    }
};

// Usage
AsyncProcessor<folly::Future<Output>> folly_processor;
AsyncProcessor<kythira::Future<Output>> wrapper_processor;
```

## Testing Migration

### Unit Testing

Create tests to verify migration correctness:

```cpp
#include <boost/test/unit_test.hpp>
#include <raft/future.hpp>

BOOST_AUTO_TEST_CASE(test_wrapper_migration, * boost::unit_test::timeout(30)) {
    // Test basic promise/future functionality
    kythira::Promise<int> promise;
    auto future = promise.getFuture();
    
    promise.setValue(42);
    BOOST_CHECK_EQUAL(std::move(future).get(), 42);
    
    // Test exception handling
    kythira::Promise<int> error_promise;
    auto error_future = error_promise.getFuture();
    error_promise.setException(folly::exception_wrapper(std::runtime_error("test")));
    
    BOOST_CHECK_THROW(std::move(error_future).get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_concept_compliance, * boost::unit_test::timeout(30)) {
    // Verify concept compliance
    static_assert(kythira::future<kythira::Future<int>, int>);
    static_assert(kythira::promise<kythira::Promise<int>, int>);
    static_assert(kythira::try_type<kythira::Try<int>, int>);
    
    // Test with concept-constrained function
    auto test_function = []<kythira::future<int> F>(F future) -> int {
        return std::move(future).get();
    };
    
    kythira::Future<int> wrapper_future(42);
    BOOST_CHECK_EQUAL(test_function(std::move(wrapper_future)), 42);
}
```

### Integration Testing

Test interoperability with existing Folly code:

```cpp
BOOST_AUTO_TEST_CASE(test_folly_interop, * boost::unit_test::timeout(60)) {
    // Test conversion from Folly to wrapper
    folly::Future<int> folly_future = folly::makeFuture(42);
    kythira::Future<int> wrapper_future(std::move(folly_future));
    BOOST_CHECK_EQUAL(std::move(wrapper_future).get(), 42);
    
    // Test conversion from wrapper to Folly
    kythira::Future<int> wrapper_future2(84);
    folly::Future<int> folly_future2 = std::move(wrapper_future2).get_folly_future();
    BOOST_CHECK_EQUAL(std::move(folly_future2).get(), 84);
}
```

### Performance Testing

Verify no performance regression:

```cpp
BOOST_AUTO_TEST_CASE(test_performance_equivalence, * boost::unit_test::timeout(120)) {
    constexpr int iterations = 10000;
    
    // Benchmark direct Folly usage
    auto start_folly = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto future = folly::makeFuture(i);
        auto result = std::move(future).get();
        (void)result; // Suppress unused variable warning
    }
    auto end_folly = std::chrono::high_resolution_clock::now();
    
    // Benchmark wrapper usage
    auto start_wrapper = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto future = kythira::FutureFactory::makeFuture(i);
        auto result = std::move(future).get();
        (void)result; // Suppress unused variable warning
    }
    auto end_wrapper = std::chrono::high_resolution_clock::now();
    
    auto folly_duration = end_folly - start_folly;
    auto wrapper_duration = end_wrapper - start_wrapper;
    
    // Wrapper should be within 10% of Folly performance
    auto ratio = static_cast<double>(wrapper_duration.count()) / folly_duration.count();
    BOOST_CHECK_LT(ratio, 1.1);
    
    std::cout << "Folly time: " << folly_duration.count() << "ns" << std::endl;
    std::cout << "Wrapper time: " << wrapper_duration.count() << "ns" << std::endl;
    std::cout << "Ratio: " << ratio << std::endl;
}
```

## Troubleshooting Migration Issues

### Issue 1: Concept Constraint Failures

**Problem**: Template functions fail to compile with concept constraints.

**Solution**: Verify that your types satisfy all concept requirements:

```cpp
// Debug concept requirements
static_assert(requires(MyFuture<int> f) {
    { std::move(f).get() };
    { f.isReady() } -> std::convertible_to<bool>;
});

// Check full concept
static_assert(kythira::future<MyFuture<int>, int>);
```

### Issue 2: Move Semantics Errors

**Problem**: Compilation errors about rvalue references.

**Solution**: Ensure proper move semantics usage:

```cpp
// Wrong
auto result = future.get();

// Correct
auto result = std::move(future).get();
```

### Issue 3: Void Type Handling

**Problem**: Compilation errors with void promises/futures.

**Solution**: Use `folly::Unit` explicitly for void types:

```cpp
// Wrong
void_promise.setValue();

// Correct
void_promise.setValue(folly::Unit{});
// or use convenience method if available
void_promise.setValue();
```

### Issue 4: Exception Type Mismatches

**Problem**: Exception handling compilation errors.

**Solution**: Use `folly::exception_wrapper` consistently:

```cpp
// Wrong
promise.setException(std::runtime_error("error"));

// Correct
promise.setException(folly::exception_wrapper(std::runtime_error("error")));
```

### Issue 5: Performance Regression

**Problem**: Wrapper usage is slower than direct Folly usage.

**Solution**: Verify compiler optimizations and check for unnecessary copies:

```cpp
// Ensure move semantics
auto future = std::move(other_future);

// Use compiler optimizations
// -O2 or -O3 for release builds

// Check for unnecessary wrapper conversions
// Minimize wrapping/unwrapping in hot paths
```

## Migration Checklist

Use this checklist to ensure complete migration:

### Pre-Migration
- [ ] Identify all Folly usage in codebase
- [ ] Plan migration strategy (gradual vs. complete)
- [ ] Set up testing framework for validation
- [ ] Benchmark current performance

### During Migration
- [ ] Add wrapper header includes
- [ ] Replace Folly types with wrapper types
- [ ] Add concept constraints to templates
- [ ] Update void type handling
- [ ] Fix move semantics usage
- [ ] Update exception handling
- [ ] Add static assertions

### Post-Migration
- [ ] Run comprehensive tests
- [ ] Verify concept compliance
- [ ] Check performance benchmarks
- [ ] Update documentation
- [ ] Train team on new patterns
- [ ] Monitor for issues in production

### Validation
- [ ] All tests pass
- [ ] No performance regression
- [ ] Concept constraints work correctly
- [ ] Interoperability with Folly maintained
- [ ] Code compiles without warnings

## Getting Help

If you encounter issues during migration:

1. **Check Static Assertions**: Verify concept compliance
2. **Review Examples**: Look at example programs in `examples/folly-wrappers/`
3. **Test Incrementally**: Migrate one component at a time
4. **Validate Performance**: Benchmark before and after migration
5. **Check Documentation**: Review concept documentation for requirements

## See Also

- [Folly Concept Wrappers Documentation](folly_concept_wrappers_documentation.md) - Complete wrapper documentation
- [Concepts Documentation](concepts_documentation.md) - C++20 concepts reference
- [Concepts API Reference](concepts_api_reference.md) - Complete API documentation
- [Example Programs](../examples/folly-wrappers/) - Working migration examples
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation