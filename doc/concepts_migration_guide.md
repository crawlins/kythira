# Concepts Migration Guide

This guide helps developers migrate to the enhanced C++20 concepts for Folly-compatible types. The enhanced concepts provide better type safety, clearer interfaces, and improved compatibility with Folly library types.

## Overview of Changes

The enhanced concepts introduce several improvements:

1. **Better Folly Compatibility**: All concepts now work seamlessly with actual Folly types
2. **Void Type Handling**: Proper support for `void` specializations in futures and promises
3. **Exception Handling**: Integration with `folly::exception_wrapper` for consistent error handling
4. **Extended Interface Coverage**: Support for more Folly methods and patterns
5. **Compilation Fixes**: Resolution of syntax errors and template constraint issues

## Breaking Changes

### 1. Exception Handling Changes

**Before (Old Concepts):**
```cpp
// Old concepts used std::exception_ptr
template<typename P, typename T>
concept old_semi_promise = requires(P p, std::exception_ptr ex) {
    { p.setException(ex) } -> std::same_as<void>;
};
```

**After (Enhanced Concepts):**
```cpp
// Enhanced concepts use folly::exception_wrapper
template<typename P, typename T>
concept semi_promise = requires(P p) {
    { p.setException(std::declval<folly::exception_wrapper>()) } -> std::same_as<void>;
};
```

**Migration Steps:**
1. Update exception handling code to use `folly::exception_wrapper`
2. Replace `std::exception_ptr` with `folly::exception_wrapper` in promise fulfillment
3. Update error handling patterns to match Folly conventions

### 2. Void Type Handling

**Before (Old Concepts):**
```cpp
// Old concepts didn't handle void properly
template<typename P>
void fulfill_void_promise(P& promise) {
    promise.setValue(); // May not compile with old concepts
}
```

**After (Enhanced Concepts):**
```cpp
// Enhanced concepts handle void with folly::Unit
template<semi_promise<void> PromiseType>
void fulfill_void_promise(PromiseType& promise) {
    promise.setValue(folly::Unit{}); // Explicit Unit for void promises
}
```

**Migration Steps:**
1. Update void promise handling to use `folly::Unit`
2. Ensure void future types are properly constrained
3. Test void specializations with actual Folly types

### 3. Method Signature Changes

**Before (Old Concepts):**
```cpp
// Old concepts had different method requirements
template<typename F, typename T>
concept old_future = requires(F f) {
    { f.get() } -> std::same_as<T>; // No move semantics required
};
```

**After (Enhanced Concepts):**
```cpp
// Enhanced concepts require move semantics for get()
template<typename F, typename T>
concept future = requires(F f) {
    { std::move(f).get() }; // Move semantics required
};
```

**Migration Steps:**
1. Update future usage to use move semantics: `std::move(future).get()`
2. Ensure future objects are properly moved when consuming values
3. Update function signatures to accept futures by value when consuming

## Step-by-Step Migration

### Step 1: Update Include Statements

No changes needed - the enhanced concepts are in the same header:

```cpp
#include <concepts/future.hpp> // Same include, enhanced implementation
```

### Step 2: Update Exception Handling

Replace `std::exception_ptr` usage with `folly::exception_wrapper`:

**Before:**
```cpp
template<typename P>
void handle_error(P& promise, const std::exception& e) {
    promise.setException(std::make_exception_ptr(e));
}
```

**After:**
```cpp
template<semi_promise<int> PromiseType>
void handle_error(PromiseType& promise, const std::exception& e) {
    promise.setException(folly::exception_wrapper(e));
}
```

### Step 3: Update Void Type Handling

Replace void promise handling with `folly::Unit`:

**Before:**
```cpp
void complete_void_operation(auto& promise) {
    // This might not work with enhanced concepts
    promise.setValue();
}
```

**After:**
```cpp
template<semi_promise<void> PromiseType>
void complete_void_operation(PromiseType& promise) {
    promise.setValue(folly::Unit{});
}
```

### Step 4: Update Future Consumption

Add move semantics to future consumption:

**Before:**
```cpp
template<typename F>
auto consume_future(F& future) {
    return future.get(); // May not work with enhanced concepts
}
```

**After:**
```cpp
template<future<int> FutureType>
auto consume_future(FutureType future) -> int {
    return std::move(future).get(); // Explicit move required
}
```

### Step 5: Update Concept Constraints

Replace old concept names with enhanced versions (if you were using different names):

**Before:**
```cpp
template<typename F>
requires old_future_concept<F>
void process_future(F future) { /* ... */ }
```

**After:**
```cpp
template<future<int> FutureType>
void process_future(FutureType future) { /* ... */ }
```

### Step 6: Add Required Headers

Include Folly headers for proper concept definitions:

```cpp
#include <concepts/future.hpp>
#include <folly/ExceptionWrapper.h>  // For exception handling
#include <folly/Unit.h>              // For void type handling
#include <folly/Try.h>               // For Try types
```

## Common Migration Patterns

### Pattern 1: Promise Fulfillment

**Before:**
```cpp
void fulfill_promise(auto& promise, int value) {
    try {
        promise.setValue(value);
    } catch (const std::exception& e) {
        promise.setException(std::make_exception_ptr(e));
    }
}
```

**After:**
```cpp
template<semi_promise<int> PromiseType>
void fulfill_promise(PromiseType& promise, int value) {
    try {
        promise.setValue(value);
    } catch (const std::exception& e) {
        promise.setException(folly::exception_wrapper(e));
    }
}
```

### Pattern 2: Future Chaining

**Before:**
```cpp
auto chain_operations(auto future) {
    return future.then([](auto value) {
        return process_value(value);
    });
}
```

**After:**
```cpp
template<future<int> FutureType>
auto chain_operations(FutureType future) -> auto {
    return std::move(future).thenValue([](int value) {
        return process_value(value);
    });
}
```

### Pattern 3: Executor Usage

**Before:**
```cpp
void schedule_work(auto& executor, std::function<void()> work) {
    executor.execute(std::move(work)); // Old method name
}
```

**After:**
```cpp
template<executor ExecutorType>
void schedule_work(ExecutorType& executor, std::function<void()> work) {
    executor.add(std::move(work)); // Folly-compatible method name
}
```

### Pattern 4: Try Type Handling

**Before:**
```cpp
auto extract_value(auto& try_obj) {
    if (try_obj.has_value()) { // Old method name
        return try_obj.get_value(); // Old method name
    }
    throw try_obj.get_exception(); // Old method name
}
```

**After:**
```cpp
template<try_type<int> TryType>
auto extract_value(const TryType& try_obj) -> int {
    if (try_obj.hasValue()) { // Folly-compatible method name
        return try_obj.value(); // Folly-compatible method name
    }
    std::rethrow_exception(try_obj.exception().to_exception_ptr());
}
```

## Validation and Testing

### Step 1: Compile-Time Validation

Add static assertions to verify your types work with enhanced concepts:

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

// Verify Folly types satisfy enhanced concepts
static_assert(kythira::future<folly::Future<int>, int>);
static_assert(kythira::promise<folly::Promise<int>, int>);
static_assert(kythira::try_type<folly::Try<int>, int>);

// Verify your custom types (if any) still work
// static_assert(kythira::future<MyCustomFuture<int>, int>);
```

### Step 2: Runtime Testing

Create test cases to verify migration correctness:

```cpp
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(test_enhanced_concepts_migration, * boost::unit_test::timeout(30)) {
    // Test promise fulfillment with enhanced concepts
    folly::Promise<int> promise;
    auto future = promise.getFuture();
    
    // This should work with enhanced concepts
    promise.setValue(42);
    BOOST_CHECK_EQUAL(std::move(future).get(), 42);
    
    // Test exception handling
    folly::Promise<int> error_promise;
    auto error_future = error_promise.getFuture();
    error_promise.setException(folly::exception_wrapper(std::runtime_error("test")));
    
    BOOST_CHECK_THROW(std::move(error_future).get(), std::runtime_error);
}
```

### Step 3: Integration Testing

Test with actual Folly types in realistic scenarios:

```cpp
BOOST_AUTO_TEST_CASE(test_folly_integration, * boost::unit_test::timeout(60)) {
    // Test with folly::CPUThreadPoolExecutor
    folly::CPUThreadPoolExecutor executor(4);
    
    // This should satisfy the executor concept
    static_assert(kythira::executor<folly::CPUThreadPoolExecutor>);
    
    // Test executor usage
    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
        executor.add([&counter]() {
            counter.fetch_add(1);
        });
    }
    
    // Wait for completion
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(counter.load(), 10);
}
```

## Troubleshooting Migration Issues

### Issue 1: Compilation Errors with Exception Handling

**Error:**
```
error: no matching function for call to 'setException'
```

**Solution:**
Replace `std::exception_ptr` with `folly::exception_wrapper`:

```cpp
// Wrong
promise.setException(std::make_exception_ptr(std::runtime_error("error")));

// Correct
promise.setException(folly::exception_wrapper(std::runtime_error("error")));
```

### Issue 2: Void Type Compilation Errors

**Error:**
```
error: no matching function for call to 'setValue'
```

**Solution:**
Use `folly::Unit` for void promises:

```cpp
// Wrong
void_promise.setValue();

// Correct
void_promise.setValue(folly::Unit{});
```

### Issue 3: Future Get Method Errors

**Error:**
```
error: cannot bind rvalue reference to lvalue
```

**Solution:**
Use move semantics when calling `get()`:

```cpp
// Wrong
auto result = future.get();

// Correct
auto result = std::move(future).get();
```

### Issue 4: Concept Constraint Failures

**Error:**
```
error: constraints not satisfied
```

**Solution:**
Verify your type implements all required methods with correct signatures:

```cpp
// Check specific requirements
static_assert(requires(MyFuture<int> f) {
    { std::move(f).get() };
    { f.isReady() } -> std::convertible_to<bool>;
});
```

## Performance Considerations

The enhanced concepts maintain the same performance characteristics as the original concepts:

1. **Zero Runtime Overhead**: Concepts are compile-time constructs
2. **Template Specialization**: Same optimization opportunities
3. **Inlining**: No impact on function inlining
4. **Memory Usage**: No additional memory overhead

### Performance Validation

Benchmark your migrated code to ensure no performance regression:

```cpp
#include <chrono>

template<future<int> FutureType>
auto benchmark_future_operations(FutureType future) -> std::chrono::nanoseconds {
    auto start = std::chrono::high_resolution_clock::now();
    auto result = std::move(future).get();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
}
```

## Migration Checklist

Use this checklist to ensure complete migration:

- [ ] Updated exception handling to use `folly::exception_wrapper`
- [ ] Added `folly::Unit` for void type handling
- [ ] Updated future consumption to use move semantics
- [ ] Added required Folly header includes
- [ ] Updated concept constraint syntax
- [ ] Added static assertions for type validation
- [ ] Created runtime tests for migration validation
- [ ] Verified performance characteristics
- [ ] Updated documentation and comments
- [ ] Tested with actual Folly types in integration scenarios

## Getting Help

If you encounter issues during migration:

1. **Check Static Assertions**: Verify your types satisfy the enhanced concepts
2. **Review Examples**: Look at `examples/concepts_usage_examples.cpp` for patterns
3. **Test Incrementally**: Migrate one component at a time
4. **Validate with Folly**: Test your migrated code with actual Folly types
5. **Check Compilation**: Ensure all required headers are included

## See Also

- [Concepts Documentation](concepts_documentation.md) - Comprehensive concept reference
- [Future Migration Guide](future_migration_guide.md) - General future migration patterns
- [Generic Future Architecture](generic_future_architecture.md) - Overall architecture design
- [Folly Documentation](https://github.com/facebook/folly) - Official Folly library documentation