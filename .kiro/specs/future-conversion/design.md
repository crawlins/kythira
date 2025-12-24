# Design Document

## Overview

This design document outlines the conversion of all `std::future` and `folly::Future` usage to a unified future interface throughout the codebase, along with a restructuring to make core Raft implementations generic and template-based. The conversion will move the future implementation from `include/future/future.hpp` to `include/raft/future.hpp` while maintaining the `kythira` namespace, and will update core implementations to use future concepts and template parameters for maximum flexibility.

The conversion will ensure that all asynchronous operations in the system use a consistent future interface while making the core implementations generic enough to work with different future implementations. Core implementations will be moved to the `kythira` namespace to better reflect their foundational role in the system architecture.

## Architecture

The conversion follows a layered approach with significant architectural improvements:

1. **Core Future Layer**: The `kythira::Future` and `kythira::Try` classes relocated to `include/raft/future.hpp`
2. **Future Concept Layer**: Generic future concepts that define the interface requirements
3. **Core Implementation Layer**: Generic Raft implementations in the `kythira` namespace that accept future types as template parameters
4. **Transport Layer**: HTTP and CoAP transport implementations that provide async RPC operations
5. **Network Simulator Layer**: Testing infrastructure that simulates network operations
6. **Application Layer**: Raft consensus implementation and other high-level components
7. **Test Layer**: All test code that validates async behavior

The key architectural change is that core implementations will be generic and accept future types as template parameters, making them more flexible and testable.

## Components and Interfaces

### Future Concept Definition

A new `future` concept will be defined in `include/concepts/future.hpp` to specify the interface requirements for future types:

```cpp
#pragma once

#include <chrono>
#include <functional>
#include <type_traits>

namespace kythira {

template<typename F, typename T>
concept future = requires(F f) {
    // Get value (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
    
    // Chain continuation
    { f.then(std::declval<std::function<void(T)>>()) };
    
    // Error handling
    { f.onError(std::declval<std::function<T(std::exception_ptr)>>()) };
};

} // namespace kythira
```

### Core Future Interface

The `kythira::Future<T>` class will be relocated to `include/raft/future.hpp` and remain in the `kythira` namespace. It provides:
- Construction from values, exceptions, and `folly::Future`
- Blocking `get()` method for retrieving results
- Non-blocking `then()` method for chaining continuations
- Error handling with `onError()` method
- Status checking with `isReady()` method
- Timeout support with `wait()` method
- Collective operations: `wait_for_any()` and `wait_for_all()`

### Core Implementation Templates

Core Raft implementations will be moved to the `kythira` namespace and will accept future types as template parameters:

```cpp
namespace kythira {

template<
    typename FutureType,
    typename LogEntry,
    typename StateMachine,
    typename Persistence,
    typename NetworkClient,
    typename NetworkServer,
    typename Membership,
    typename Metrics,
    typename Logger
>
requires 
    future<FutureType, /* appropriate type */> &&
    // ... other concept requirements
class raft_node {
    // Implementation uses FutureType instead of concrete kythira::Future
};

} // namespace kythira
```

### Transport Layer Interfaces

#### Network Client Concept
The `network_client` concept will be updated to use the generic future concept and moved to the `kythira` namespace:
```cpp
namespace kythira {

template<typename C, typename FutureType>
concept network_client = requires(
    C client,
    std::uint64_t target,
    const request_vote_request<>& rvr,
    const append_entries_request<>& aer,
    const install_snapshot_request<>& isr,
    std::chrono::milliseconds timeout
) {
    { client.send_request_vote(target, rvr, timeout) } 
        -> std::same_as<FutureType>;
    { client.send_append_entries(target, aer, timeout) }
        -> std::same_as<FutureType>;
    { client.send_install_snapshot(target, isr, timeout) }
        -> std::same_as<FutureType>;
};

} // namespace kythira
```

#### HTTP Transport
The `cpp_httplib_client` class will be templated on future type and moved to the `kythira` namespace:
```cpp
namespace kythira {

template<typename FutureType, typename RPC_Serializer, typename Metrics>
requires 
    future<FutureType, request_vote_response<>> &&
    rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    metrics<Metrics>
class cpp_httplib_client {
    auto send_request_vote(...) -> FutureType;
    auto send_append_entries(...) -> FutureType;
    auto send_install_snapshot(...) -> FutureType;
};

} // namespace kythira
```

#### CoAP Transport
The `coap_client` class will be templated on future type and moved to the `kythira` namespace:
```cpp
namespace kythira {

template<typename FutureType, typename RPC_Serializer, typename Metrics, typename Logger>
requires 
    future<FutureType, request_vote_response<>> &&
    rpc_serializer<RPC_Serializer, std::vector<std::byte>> && 
    metrics<Metrics> && 
    diagnostic_logger<Logger>
class coap_client {
    auto send_request_vote(...) -> FutureType;
    auto send_append_entries(...) -> FutureType;
    auto send_install_snapshot(...) -> FutureType;
    auto send_multicast_message(...) -> FutureType;
};

} // namespace kythira
```

### Network Simulator Interfaces

#### Connection Interface
The `Connection<Addr, Port>` class will be templated on future type and moved to the `kythira` namespace:
```cpp
namespace kythira {

template<address Addr, port Port, typename FutureType>
requires future<FutureType, std::vector<std::byte>>
class Connection {
    auto read() -> FutureType;
    auto read(std::chrono::milliseconds timeout) -> FutureType;
    auto write(std::vector<std::byte> data) -> FutureType;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> FutureType;
};

} // namespace kythira
```

#### Listener Interface
The `Listener<Addr, Port>` class will be templated on future type and moved to the `kythira` namespace:
```cpp
namespace kythira {

template<address Addr, port Port, typename FutureType, typename ConnectionType>
requires future<FutureType, std::shared_ptr<ConnectionType>>
class Listener {
    auto accept() -> FutureType;
    auto accept(std::chrono::milliseconds timeout) -> FutureType;
};

} // namespace kythira
```

## Data Models

### Future Conversion Patterns

#### Pattern 1: Direct Return Type Conversion
```cpp
// Before
auto send_request_vote(...) -> folly::Future<request_vote_response<>>;

// After - return values in terms of Future template parameter
template<typename FutureType>
auto send_request_vote(...) -> FutureType;
```

#### Pattern 2: Promise/Future Pair Conversion
```cpp
// Before
folly::Promise<Response> promise;
auto future = promise.getFuture();
// ... fulfill promise
return future;

// After
// Use Future constructor patterns
return FutureType(response_value);
// or
return FutureType(exception);
```

#### Pattern 3: Collections Defined in Terms of Future Template Parameter
```cpp
// Before
std::vector<std::future<void>> futures;
futures.emplace_back(std::async(std::launch::async, []() { ... }));

// After - collections in terms of Future template parameter
template<typename FutureType>
auto process_operations() {
    std::vector<FutureType> futures;
    futures.emplace_back(FutureType(/* async operation result */));
    return futures;
}
```

#### Pattern 4: Collective Operations Defined in Terms of Future Template Parameter
```cpp
// Before
std::vector<folly::Future<T>> folly_futures;
auto result = folly::collectAll(folly_futures);

// After - collections in terms of Future template parameter
template<typename FutureType>
auto collect_results(std::vector<FutureType> futures) {
    // Use generic collective operations
    return wait_for_all(std::move(futures));
}
```

### Header Include Patterns

#### Pattern 1: Replace folly::Future includes
```cpp
// Before
#include <folly/futures/Future.h>

// After
#include <raft/future.hpp>
```

#### Pattern 2: Replace std::future includes
```cpp
// Before
#include <future>

// After
#include <raft/future.hpp>
```

## Correctness Properties*A pro
perty is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

Property 1: Future usage consistency
*For any* source file in the codebase (excluding the kythira::Future implementation), all future-related operations should use only `kythira::Future` types
**Validates: Requirements 1.1**

Property 2: Header include consistency
*For any* header file in the codebase (excluding the kythira::Future implementation), future functionality should be accessed only through `#include <raft/future.hpp>`
**Validates: Requirements 1.4, 6.1**

Property 3: Transport method return types
*For any* transport client method (HTTP or CoAP), the return type should be `kythira::Future<ResponseType>` instead of `folly::Future<ResponseType>`
**Validates: Requirements 2.1, 2.2**

Property 4: Network concept compliance
*For any* type that satisfies the network_client concept, all RPC methods should return `kythira::Future` types
**Validates: Requirements 2.3, 2.4**

Property 5: Multicast operation return types
*For any* multicast operation, the return type should be `kythira::Future<std::vector<std::vector<std::byte>>>`
**Validates: Requirements 2.5**

Property 6: Network simulator return types
*For any* network simulator operation (connection read/write, listener accept), the return type should be the appropriate `kythira::Future` specialization
**Validates: Requirements 3.1, 3.2, 3.3**

Property 7: Timeout operation support
*For any* operation that accepts timeout parameters, it should return `kythira::Future` and handle timeouts correctly
**Validates: Requirements 3.5**

Property 8: Test code future usage
*For any* test file, all future-related operations should use `kythira::Future` instead of `std::future` or `folly::Future`
**Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**

Property 9: Behavioral preservation
*For any* async operation, the timing, ordering, error handling, and thread safety behavior should be equivalent before and after conversion
**Validates: Requirements 5.1, 5.2, 5.3, 5.4**

Property 10: Build dependency consistency
*For any* user code file, compilation should succeed without requiring direct includes of `<future>` or `<folly/futures/Future.h>`
**Validates: Requirements 6.2, 6.3**

Property 11: Promise/Future pattern conversion
*For any* Promise/Future usage pattern, it should be converted to use `kythira::Future` construction patterns instead of `makeFuture`, `getFuture()`, or promise fulfillment
**Validates: Requirements 7.1, 7.2, 7.3, 7.4, 7.5**

Property 13: Core implementation genericity
*For any* core Raft implementation, it should accept future types as template parameters and use future concepts instead of concrete future types
**Validates: Requirements 8.1, 8.2**

Property 14: Future implementation location
*For any* future-related functionality, it should be accessible through `include/raft/future.hpp` and remain in the `kythira` namespace
**Validates: Requirements 8.3, 8.4**

Property 15: Core implementation namespace
*For any* core implementation (including network_client concept, cpp_httplib_client, coap_client, Connection, and Listener classes), it should be placed in the `kythira` namespace instead of the `raft` namespace
**Validates: Requirements 8.5, 8.6, 8.7, 8.8, 8.9, 8.10**

Property 16: Complete conversion validation
*For any* search of the codebase, there should be no remaining `std::future` or direct `folly::Future` usage in public interfaces (excluding kythira::Future implementation)
**Validates: Requirements 9.1, 9.2**

Property 17: Build success
*For any* build configuration, the system should compile successfully with no future-related errors after conversion
**Validates: Requirements 9.3**

Property 18: Performance equivalence
*For any* performance benchmark, the system should demonstrate equivalent performance characteristics before and after conversion
**Validates: Requirements 9.5**

## Error Handling

The conversion must preserve all existing error handling patterns:

### Exception Propagation
- `kythira::Future` wraps `folly::Future` internally, so exception propagation behavior is preserved
- Error handling through `onError()` method maintains the same semantics
- Exception constructors allow direct creation of failed futures

### Timeout Handling
- All timeout-based operations must continue to work with the same timeout semantics
- The `wait()` method provides timeout support equivalent to the original implementations
- Timeout errors should propagate in the same manner

### Resource Management
- RAII patterns must be preserved during conversion
- Future destruction and cleanup behavior must remain unchanged
- Memory management patterns should not be affected

## Testing Strategy

### Dual Testing Approach

The conversion will use both unit testing and property-based testing approaches:

**Unit Testing**:
- Verify specific conversion examples work correctly
- Test edge cases like timeout handling and error propagation
- Validate that specific transport methods return correct types
- Test Promise/Future pattern conversions

**Property-Based Testing**:
- Use **Boost.Test** as the property-based testing framework
- Configure each property-based test to run a minimum of 100 iterations
- Tag each property-based test with comments referencing the design document properties
- Use the format: `**Feature: future-conversion, Property {number}: {property_text}**`

**Testing Requirements**:
- Each correctness property must be implemented by a single property-based test
- Tests must validate universal properties across all inputs
- Unit tests complement property tests by covering specific examples
- Both types of tests provide comprehensive coverage

### Conversion Validation Tests

#### Static Analysis Tests
- Scan codebase for remaining `std::future` and `folly::Future` usage
- Validate header include patterns
- Check return type consistency across transport layers

#### Functional Tests
- Run all existing tests to ensure behavioral preservation
- Validate async operation timing and ordering
- Test error handling and exception propagation
- Verify timeout behavior consistency

#### Performance Tests
- Benchmark async operations before and after conversion
- Measure memory usage and allocation patterns
- Validate that performance characteristics are equivalent

#### Integration Tests
- Test transport layer integration with converted futures
- Validate network simulator behavior with new future types
- Test end-to-end async workflows

### Test Implementation Strategy

1. **Implementation-first development**: Implement the conversion before writing corresponding tests
2. **Incremental validation**: Test each component as it's converted
3. **Regression prevention**: Ensure all existing tests continue to pass
4. **Property validation**: Write property-based tests for universal behaviors
5. **Edge case coverage**: Use unit tests for specific scenarios and error conditions

## Implementation Plan

The conversion will proceed in the following phases:

### Phase 1: Future Concept and Core Infrastructure
1. Move `include/future/future.hpp` to `include/raft/future.hpp`
2. Create `include/concepts/future.hpp` with the generic `future` concept
3. Update `kythira::Future` to satisfy the future concept
4. Create future concept validation tests

### Phase 2: Core Implementation Restructuring
1. Move core Raft implementations to the `kythira` namespace
2. Template core implementations on future types
3. Update core implementations to use future concepts
4. Update internal interfaces to be generic

### Phase 3: Transport Layer Generification
1. Template HTTP transport implementations on future types
2. Template CoAP transport implementations on future types
3. Update transport concepts to be generic
4. Convert internal Promise/Future patterns

### Phase 4: Network Simulator Generification
1. Template Connection class on future types
2. Template Listener class on future types
3. Update network simulator concepts to be generic
4. Update internal async operation handling

### Phase 5: Interface Updates
1. Update network concepts to use generic future concepts
2. Update all public interfaces to use template parameters
3. Update concept definitions and static assertions
4. Ensure backward compatibility with kythira::Future as default

### Phase 6: Test Code Conversion
1. Convert integration tests to use generic future interfaces
2. Convert property-based tests to use template parameters
3. Update test utilities and fixtures
4. Add tests for generic future concept compliance

### Phase 7: Header Cleanup and Organization
1. Remove unnecessary `#include <future>` statements
2. Remove unnecessary `#include <folly/futures/Future.h>` statements
3. Update includes to use `#include <raft/future.hpp>` and `#include <concepts/future.hpp>`
4. Organize namespace usage consistently

### Phase 8: Validation and Testing
1. Run comprehensive test suite with generic implementations
2. Validate performance characteristics with different future types
3. Perform static analysis validation
4. Test instantiation with kythira::Future as default
5. Document the new generic architecture

Each phase will include thorough testing to ensure no regressions are introduced and that the conversion maintains all existing functionality while adding the new generic capabilities.