# Future Wrapper Requirements for Async Retry Logic

## Current Status

The `kythira::Future` wrapper currently uses **blocking sleep** (`std::this_thread::sleep_for`) for retry delays in the ErrorHandler. While this works correctly, it blocks the thread during delays.

## Goal

Implement **non-blocking async delays** using Folly's future chaining capabilities.

## What Works

The Future wrapper already has:

1. ✅ **`delay(duration)`** - Delays a future by a specified duration (wraps Folly's `.delayed()`)
2. ✅ **`thenValue` with Future-returning callbacks** - Supports automatic flattening when callback returns `Future<U>`
   ```cpp
   template<typename F>
   auto thenValue(F&& func) -> std::invoke_result_t<F, T>
       requires(detail::is_future<std::invoke_result_t<F, T>>::value)
   ```

## What's Missing

### 1. `thenTry` Overload for Future-Returning Callbacks

**Current:** `thenTry` only supports callbacks that return `T` or `void`:
```cpp
template<typename F>
auto thenTry(F&& func) -> Future<std::invoke_result_t<F, Try<T>>>
```

**Needed:** Overload that handles callbacks returning `Future<U>` with automatic flattening:
```cpp
template<typename F>
auto thenTry(F&& func) -> std::invoke_result_t<F, Try<T>>
    requires(detail::is_future<std::invoke_result_t<F, Try<T>>>::value) {
    using FutureReturnType = std::invoke_result_t<F, Try<T>>;
    using InnerType = typename FutureReturnType::value_type;
    
    // Use folly's automatic future flattening
    return FutureReturnType(std::move(_folly_future).thenTry([func = std::forward<F>(func)](folly::Try<T> folly_try) {
        Try<T> kythira_try(std::move(folly_try));
        // Call the lambda which returns Future<U>
        auto inner_future = func(std::move(kythira_try));
        // Extract the folly::Future from our wrapper
        return std::move(inner_future).get_folly_future();
    }));
}
```

### 2. `thenError` Overload for Future-Returning Callbacks

**Current:** `thenError` only supports callbacks that return `T`:
```cpp
template<typename F>
auto thenError(F&& func) -> Future<T>
```

**Needed:** Overload that handles callbacks returning `Future<T>` with automatic flattening:
```cpp
template<typename F>
auto thenError(F&& func) -> Future<T>
    requires(detail::is_future<std::invoke_result_t<F, folly::exception_wrapper>>::value) {
    
    // Use folly's automatic future flattening
    return Future<T>(std::move(_folly_future).thenError([func = std::forward<F>(func)](folly::exception_wrapper ex) mutable {
        // Call the lambda which returns Future<T>
        auto inner_future = func(ex);
        // Extract the folly::Future from our wrapper
        return std::move(inner_future).get_folly_future();
    }));
}
```

## Why These Are Needed

### Current Blocking Approach
```cpp
return op()
    .thenError([](std::exception_ptr eptr) -> Result {
        // Classify error, calculate delay
        std::this_thread::sleep_for(delay);  // ❌ BLOCKS THREAD
        return retry_operation().get();       // ❌ BLOCKS THREAD
    });
```

### Desired Async Approach with `thenTry`
```cpp
return op()
    .thenTry([](Try<Result> result) -> Future<Result> {
        if (result.hasValue()) {
            return makeFuture(result.value());
        }
        // Classify error, calculate delay
        return makeFuture(Unit{})
            .delay(delay)                     // ✅ NON-BLOCKING
            .thenValue([]() -> Future<Result> {
                return retry_operation();      // ✅ NON-BLOCKING
            });
    });
```

**Problem:** `thenTry` doesn't support Future-returning callbacks, so it returns `Future<Future<Result>>` instead of `Future<Result>`.

### Alternative Async Approach with `thenError`
```cpp
return op()
    .thenError([](exception_wrapper ew) -> Future<Result> {
        // Classify error, calculate delay
        return makeFuture(Unit{})
            .delay(delay)                     // ✅ NON-BLOCKING
            .thenValue([]() -> Future<Result> {
                return retry_operation();      // ✅ NON-BLOCKING
            });
    });
```

**Problem:** `thenError` doesn't support Future-returning callbacks, so it tries to convert `Future<Result>` to `Result`.

## Workaround Using Existing API

Since `thenValue` already supports Future-returning callbacks, we can restructure to use it:

```cpp
return op()
    .thenValue([](Result result) -> Future<Result> {
        // Success case - just return the result
        return makeFuture(result);
    })
    .thenError([](exception_wrapper ew) -> Result {
        // Error case - still needs to block or throw
        // Can't return Future<Result> here
        throw ew;
    });
```

**Problem:** This doesn't help because we need to handle errors and return futures from the error handler.

## Recommendation

Add the two missing overloads (`thenTry` and `thenError` for Future-returning callbacks) to the Future wrapper. This will enable fully async retry logic without blocking threads.

## Benefits of Async Approach

1. **Better Resource Utilization** - Threads aren't blocked during retry delays
2. **Scalability** - Can handle many concurrent retry operations without thread exhaustion
3. **Consistency** - Matches Folly's async programming model
4. **Performance** - No thread context switching overhead during delays

## Current Workaround

The blocking approach using `std::this_thread::sleep_for` is acceptable for now because:
- Retries are infrequent (only on failures)
- Delays are intentional (exponential backoff)
- Blocking happens in the error path, not the success path
- All tests pass and delays work correctly

However, for production systems with high retry rates, the async approach would be preferable.
