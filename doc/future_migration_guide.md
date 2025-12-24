# Future Migration Guide

This guide helps developers migrate from the old future usage patterns to the new generic future architecture.

## Quick Reference

| Old Pattern | New Pattern |
|-------------|-------------|
| `#include <future>` | `#include <raft/future.hpp>` |
| `#include <folly/futures/Future.h>` | `#include <raft/future.hpp>` |
| `std::future<T>` | `kythira::Future<T>` |
| `folly::Future<T>` | `kythira::Future<T>` |
| `folly::makeFuture<T>(value)` | `kythira::Future<T>(value)` |
| `promise.getFuture()` | `kythira::Future<T>(value)` |
| `raft::cpp_httplib_client` | `kythira::cpp_httplib_client<FutureType, ...>` |
| `raft::coap_client` | `kythira::coap_client<FutureType, ...>` |

## Step-by-Step Migration

### Step 1: Update Includes

Replace future-related includes:

```cpp
// Remove these
#include <future>
#include <folly/futures/Future.h>

// Add this
#include <raft/future.hpp>
```

### Step 2: Update Type Declarations

Replace future type declarations:

```cpp
// Before
std::future<int> my_future;
folly::Future<std::string> another_future;

// After
kythira::Future<int> my_future;
kythira::Future<std::string> another_future;
```

### Step 3: Update Function Signatures

Replace return types in function signatures:

```cpp
// Before
auto send_request() -> folly::Future<Response> {
    // ...
}

// After
auto send_request() -> kythira::Future<Response> {
    // ...
}
```

### Step 4: Update Future Construction

Replace future construction patterns:

```cpp
// Before - folly::makeFuture
auto future = folly::makeFuture<int>(42);

// After - direct construction
auto future = kythira::Future<int>(42);

// Before - promise/future pattern
folly::Promise<int> promise;
auto future = promise.getFuture();
promise.setValue(42);

// After - direct construction
auto future = kythira::Future<int>(42);

// Before - exception construction
auto error_future = folly::makeFuture<int>(folly::exception_wrapper(std::runtime_error("error")));

// After - exception construction
auto error_future = kythira::Future<int>(std::make_exception_ptr(std::runtime_error("error")));
```

### Step 5: Update Template Instantiations

Update transport and network simulator instantiations:

```cpp
// Before - concrete types
raft::cpp_httplib_client http_client(config);

// After - templated types
kythira::cpp_httplib_client<
    kythira::Future<raft::request_vote_response<>>,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics
> http_client(config, metrics);
```

### Step 6: Update Namespace Usage

Core implementations have moved to the `kythira` namespace:

```cpp
// Before
raft::cpp_httplib_client client;
raft::coap_client coap_client;
network_simulator::Connection connection;

// After
kythira::cpp_httplib_client<FutureType, ...> client;
kythira::coap_client<FutureType, ...> coap_client;
kythira::Connection<Addr, Port, FutureType> connection;
```

## Common Migration Patterns

### Pattern 1: Async Function Migration

**Before:**
```cpp
#include <future>

std::future<int> calculate_async() {
    return std::async(std::launch::async, []() {
        // Some computation
        return 42;
    });
}

void use_async() {
    auto future = calculate_async();
    int result = future.get();
    std::cout << "Result: " << result << std::endl;
}
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<int> calculate_async() {
    // Direct construction with computed value
    return kythira::Future<int>(42);
}

void use_async() {
    auto future = calculate_async();
    int result = future.get();
    std::cout << "Result: " << result << std::endl;
}
```

### Pattern 2: Future Chaining Migration

**Before:**
```cpp
#include <folly/futures/Future.h>

folly::Future<std::string> process_data() {
    return folly::makeFuture<int>(42)
        .thenValue([](int value) {
            return value * 2;
        })
        .thenValue([](int doubled) {
            return std::to_string(doubled);
        });
}
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<std::string> process_data() {
    return kythira::Future<int>(42)
        .then([](int value) {
            return value * 2;
        })
        .then([](int doubled) {
            return std::to_string(doubled);
        });
}
```

### Pattern 3: Error Handling Migration

**Before:**
```cpp
#include <folly/futures/Future.h>

folly::Future<int> safe_operation() {
    return risky_operation()
        .thenError([](folly::exception_wrapper ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
            return 0; // Default value
        });
}
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<int> safe_operation() {
    return risky_operation()
        .onError([](std::exception_ptr ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
            return 0; // Default value
        });
}
```

### Pattern 4: Collection Operations Migration

**Before:**
```cpp
#include <folly/futures/Future.h>

folly::Future<std::vector<int>> collect_results() {
    std::vector<folly::Future<int>> futures;
    futures.push_back(folly::makeFuture<int>(1));
    futures.push_back(folly::makeFuture<int>(2));
    futures.push_back(folly::makeFuture<int>(3));
    
    return folly::collectAll(futures.begin(), futures.end())
        .thenValue([](std::vector<folly::Try<int>> results) {
            std::vector<int> values;
            for (auto& result : results) {
                values.push_back(result.value());
            }
            return values;
        });
}
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<std::vector<int>> collect_results() {
    std::vector<kythira::Future<int>> futures;
    futures.emplace_back(kythira::Future<int>(1));
    futures.emplace_back(kythira::Future<int>(2));
    futures.emplace_back(kythira::Future<int>(3));
    
    return kythira::wait_for_all(std::move(futures))
        .then([](std::vector<kythira::Try<int>> results) {
            std::vector<int> values;
            for (auto& result : results) {
                if (result.has_value()) {
                    values.push_back(result.value());
                }
            }
            return values;
        });
}
```

### Pattern 5: Transport Client Migration

**Before:**
```cpp
#include <raft/http_transport.hpp>

void setup_http_client() {
    raft::cpp_httplib_client client(node_map, config);
    
    auto future = client.send_request_vote(target, request, timeout);
    auto response = std::move(future).get();
}
```

**After:**
```cpp
#include <raft/http_transport.hpp>
#include <raft/future.hpp>

void setup_http_client() {
    kythira::cpp_httplib_client<
        kythira::Future<raft::request_vote_response<>>,
        raft::json_rpc_serializer<std::vector<std::byte>>,
        raft::noop_metrics
    > client(node_map, config, metrics);
    
    auto future = client.send_request_vote(target, request, timeout);
    auto response = future.get();
}
```

### Pattern 6: Network Simulator Migration

**Before:**
```cpp
#include <network_simulator/connection.hpp>

void use_connection() {
    network_simulator::Connection<std::string, std::uint16_t> connection(local, remote, simulator);
    
    auto read_future = connection.read();
    auto data = std::move(read_future).get();
}
```

**After:**
```cpp
#include <network_simulator/connection.hpp>
#include <raft/future.hpp>

void use_connection() {
    kythira::Connection<
        std::string, 
        std::uint16_t, 
        kythira::Future<std::vector<std::byte>>
    > connection(local, remote, simulator);
    
    auto read_future = connection.read();
    auto data = read_future.get();
}
```

## Testing Migration

### Test Code Migration

**Before:**
```cpp
#include <future>
#include <folly/futures/Future.h>

BOOST_AUTO_TEST_CASE(test_async_operation) {
    auto future = std::async(std::launch::async, []() { return 42; });
    BOOST_CHECK_EQUAL(future.get(), 42);
    
    auto folly_future = folly::makeFuture<int>(24);
    BOOST_CHECK_EQUAL(std::move(folly_future).get(), 24);
}
```

**After:**
```cpp
#include <raft/future.hpp>

BOOST_AUTO_TEST_CASE(test_async_operation, * boost::unit_test::timeout(30)) {
    auto future = kythira::Future<int>(42);
    BOOST_CHECK_EQUAL(future.get(), 42);
    
    auto another_future = kythira::Future<int>(24);
    BOOST_CHECK_EQUAL(another_future.get(), 24);
}
```

### Property-Based Test Migration

**Before:**
```cpp
BOOST_AUTO_TEST_CASE(property_test_futures) {
    for (int i = 0; i < 100; ++i) {
        int value = generate_random_int();
        auto future = folly::makeFuture<int>(value);
        BOOST_CHECK_EQUAL(std::move(future).get(), value);
    }
}
```

**After:**
```cpp
BOOST_AUTO_TEST_CASE(property_test_futures, * boost::unit_test::timeout(60)) {
    // **Feature: future-conversion, Property 1: Future usage consistency**
    for (int i = 0; i < 100; ++i) {
        int value = generate_random_int();
        auto future = kythira::Future<int>(value);
        BOOST_CHECK_EQUAL(future.get(), value);
    }
}
```

## Troubleshooting

### Common Compilation Errors

1. **Missing include error:**
   ```
   error: 'kythira::Future' was not declared in this scope
   ```
   **Solution:** Add `#include <raft/future.hpp>`

2. **Template argument error:**
   ```
   error: template argument deduction/substitution failed
   ```
   **Solution:** Ensure all template parameters are provided for generic implementations

3. **Concept constraint error:**
   ```
   error: constraints not satisfied
   ```
   **Solution:** Verify that your future type satisfies the `kythira::future` concept

4. **Namespace error:**
   ```
   error: 'cpp_httplib_client' is not a member of 'raft'
   ```
   **Solution:** Use `kythira::cpp_httplib_client` instead of `raft::cpp_httplib_client`

### Runtime Issues

1. **Deadlock in future.get():**
   - Ensure the future is properly constructed with a value or exception
   - Check that async operations are actually completing

2. **Exception not propagated:**
   - Use `kythira::Future<T>(std::make_exception_ptr(exception))` for exception construction
   - Handle exceptions with `.onError()` method

3. **Performance regression:**
   - The new implementation should have equivalent performance
   - If you notice issues, check that you're not creating unnecessary future copies

## Validation

After migration, validate your changes:

1. **Compile successfully:**
   ```bash
   cmake --build build
   ```

2. **Run tests:**
   ```bash
   cd build && ctest --output-on-failure
   ```

3. **Run specific future tests:**
   ```bash
   ctest -R "future.*property_test"
   ```

4. **Check for remaining old patterns:**
   ```bash
   grep -r "std::future\|folly::Future" include/ src/ --exclude-dir=build
   ```

## Getting Help

If you encounter issues during migration:

1. Check the [Generic Future Architecture](generic_future_architecture.md) documentation
2. Look at existing test files for usage examples
3. Run the property-based tests to validate your changes
4. Ensure all static assertions pass during compilation

The migration preserves all existing functionality while providing a more flexible and consistent future interface.