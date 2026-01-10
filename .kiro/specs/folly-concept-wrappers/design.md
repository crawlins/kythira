# Design Document

## Overview

This design document outlines the enhancement of C++20 concepts in `include/concepts/future.hpp`, the implementation of comprehensive wrapper classes that adapt folly library types to satisfy these enhanced concepts, and the conversion of all future usage to a unified `kythira::Future` interface throughout the codebase. The current implementation has three main issues: (1) the concepts themselves have compilation errors and don't fully capture Folly's rich API, (2) `include/raft/future.hpp` provides only partial coverage with `Try` and `Future` wrappers, lacking wrappers for promises, executors, factory functions, and advanced collective operations, and (3) the codebase has inconsistent usage of different future types.

The design follows a three-phase approach: first enhancing the concept definitions to accurately reflect Folly's interfaces and fix compilation issues, then creating lightweight wrapper classes that delegate to underlying folly types while providing the interface required by the enhanced concepts, and finally converting all future usage throughout the codebase to use the unified `kythira::Future` interface. This approach ensures compatibility with existing concept-constrained code while leveraging folly's mature and performant implementations and providing a consistent asynchronous programming model.

## Architecture

The enhanced architecture consists of several layers:

1. **Enhanced Concept Layer**: Fixed and improved C++20 concepts defining interfaces (`include/concepts/future.hpp`)
2. **Wrapper Layer**: Adapter classes implementing concept interfaces (`include/raft/future.hpp`)
3. **Folly Layer**: Underlying folly library implementations
4. **Integration Layer**: Utility functions and type conversions
5. **Unified Interface Layer**: Consistent `kythira::Future` usage throughout the codebase
6. **Generic Template Layer**: Core implementations that accept future types as template parameters

The design maintains a clear separation between the concept definitions and their implementations, allowing for future extensibility and alternative implementations. The three phases focus on: (1) fixing compilation issues and enhancing concept accuracy, (2) implementing comprehensive wrapper classes, and (3) converting all future usage to the unified interface while making core implementations generic.

## Components and Interfaces

### Phase 1: Concept Enhancement

**Compilation Fixes**:
- Fix std::as_const usage with proper C++17 syntax
- Remove invalid destructor concept requirements
- Improve template parameter constraint syntax
- Ensure all concepts compile without errors

**Enhanced Try Concept**:
```cpp
template<typename T, typename ValueType>
concept try_type = requires(T t, const T ct) {
    // Access value (throws if contains exception)
    { t.value() } -> std::same_as<ValueType&>;
    { ct.value() } -> std::same_as<const ValueType&>;
    
    // Check if contains value (folly naming)
    { t.hasValue() } -> std::convertible_to<bool>;
    
    // Check if contains exception (folly naming)
    { t.hasException() } -> std::convertible_to<bool>;
    
    // Access exception (folly::Try uses exception_wrapper)
    { t.exception() }; // Returns folly::exception_wrapper or similar
};
```

**Enhanced SemiPromise Concept**:
```cpp
template<typename P, typename T>
concept semi_promise = requires(P p, T value, folly::exception_wrapper ex) {
    // Set value - handle void specialization
    requires std::is_void_v<T> ? requires { p.setValue(); } : requires { p.setValue(std::move(value)); };
    
    // Set exception
    { p.setException(ex) } -> std::same_as<void>;
    
    // Check if fulfilled
    { p.isFulfilled() } -> std::convertible_to<bool>;
};
```

**Enhanced Promise Concept**:
```cpp
template<typename P, typename T>
concept promise = semi_promise<P, T> && requires(P p) {
    // Get associated future
    { p.getFuture() }; // Returns folly::Future<T> or similar
    
    // Get associated semi-future
    { p.getSemiFuture() }; // Returns folly::SemiFuture<T> or similar
};
```

**Enhanced Executor and Future Concepts**: Similar enhancements to match Folly's actual API.

### Phase 3: Future Conversion and Generic Templates

**Future Usage Unification**:
- Convert all `std::future` usage to `kythira::Future`
- Convert all direct `folly::Future` usage to `kythira::Future`
- Update header includes to use `raft/future.hpp`
- Ensure consistent future interface throughout codebase

**Generic Template Implementation**:
- Template core implementations on future types
- Use future concepts as template constraints
- Move core implementations to `kythira` namespace
- Make transport layers generic over future types

**Transport Layer Generification**:
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

**Network Simulator Generification**:
```cpp
namespace kythira {

template<address Addr, port Port, typename FutureType>
requires future<FutureType, std::vector<std::byte>>
class Connection {
    auto read() -> FutureType;
    auto write(std::vector<std::byte> data) -> FutureType;
};

} // namespace kythira
```

### Promise Wrappers

**SemiPromise<T>**: Adapts `folly::Promise<T>` to satisfy the `semi_promise` concept
- Provides `setValue()` and `setException()` methods
- Handles void/Unit type conversions
- Manages fulfillment state tracking

**Promise<T>**: Extends SemiPromise to satisfy the `promise` concept
- Adds `getFuture()` and `getSemiFuture()` methods
- Returns properly wrapped Future instances
- Maintains promise-future relationship integrity

### Executor Wrappers

**Executor**: Adapts `folly::Executor*` to satisfy the `executor` concept
- Provides `add()` method for work submission
- Handles function object forwarding
- Manages executor lifetime and validity

**KeepAlive**: Adapts `folly::Executor::KeepAlive` to satisfy the `keep_alive` concept
- Provides pointer-like access via `get()`
- Implements proper copy/move semantics
- Maintains reference counting behavior

### Factory Operations

**FutureFactory**: Static factory class satisfying the `future_factory` concept
- `makeFuture()`: Creates futures from values
- `makeExceptionalFuture()`: Creates futures from exceptions
- `makeReadyFuture()`: Creates immediately ready futures
- Handles type deduction and conversion

### Collective Operations

**FutureCollector**: Static collector class satisfying the `future_collector` concept
- `collectAll()`: Waits for all futures, preserves order
- `collectAny()`: Returns first completed future with index
- `collectAnyWithoutException()`: Returns first successful future
- `collectN()`: Returns first N completed futures

### Continuation and Transformation Extensions

Enhanced Future class with additional methods:
- `via()`: Schedule continuation on specific executor
- `delay()`: Add time-based delay
- `within()`: Add timeout behavior
- `ensure()`: Add cleanup functionality

## Data Models

### Type Conversion Strategy

The design handles several critical type conversions:

**Exception Conversion**:
```cpp
// folly::exception_wrapper ↔ std::exception_ptr
std::exception_ptr to_std_exception_ptr(const folly::exception_wrapper& ew);
folly::exception_wrapper to_folly_exception_wrapper(std::exception_ptr ep);
```

**Void/Unit Conversion**:
```cpp
// void ↔ folly::Unit for template specializations
template<typename T> struct void_to_unit { using type = T; };
template<> struct void_to_unit<void> { using type = folly::Unit; };
```

**Future Type Mapping**:
```cpp
// Consistent mapping between wrapper and folly types
template<typename T> using folly_future_type = folly::Future<typename void_to_unit<T>::type>;
template<typename T> using wrapper_future_type = Future<T>;
```

### Memory Management

The design ensures proper resource management:
- RAII for all wrapper objects
- Move semantics for expensive operations
- Proper exception safety guarantees
- Reference counting for KeepAlive instances

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

**Property Reflection**: After reviewing all identified properties, several can be consolidated:
- Concept compilation and constraint validation properties can be combined
- Folly type concept compliance properties can be grouped together
- Wrapper concept compliance properties can be combined with generic template compatibility
- Factory, collection, continuation, and transformation operation properties remain distinct due to different behavioral requirements

**Property 1: Concept Compilation and Constraint Validation**
*For any* C++ compiler, including the enhanced concepts header should result in successful compilation without syntax errors, and the concepts should correctly accept or reject types based on their interface
**Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**

**Property 2: Enhanced Concept Interface Requirements**
*For any* type satisfying an enhanced concept (semi_promise, promise, executor, keep_alive, future, try_type), it should provide all required methods with correct signatures and behavior
**Validates: Requirements 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 4.1, 4.3, 5.1, 5.2, 5.3, 6.1, 6.2, 6.3, 6.4, 6.5, 9.1, 9.2, 9.3, 9.4**

**Property 3: Folly Type Concept Compliance**
*For any* folly type (folly::SemiPromise, folly::Promise, folly::Executor, folly::Future), it should satisfy its corresponding enhanced concept
**Validates: Requirements 10.1, 10.2, 10.3, 10.4, 10.5**

**Property 4: Wrapper Concept Compliance and Generic Compatibility**
*For any* wrapper class and its corresponding concept, the wrapper should satisfy all concept requirements at compile time and runtime, and work seamlessly in concept-constrained templates
**Validates: Requirements 11.1, 11.2, 12.1, 12.2, 17.1, 17.2, 17.4**

**Property 5: Promise Value and Exception Handling**
*For any* promise wrapper and value/exception, setting the value or exception should properly convert types and make the associated future ready with the correct result
**Validates: Requirements 11.3, 11.4, 11.5**

**Property 6: Executor Work Submission**
*For any* executor wrapper and work item, submitting work should properly forward to the underlying executor and execute the work
**Validates: Requirements 12.3, 12.4, 12.5**

**Property 7: Factory Future Creation**
*For any* value or exception, factory methods should create futures that are immediately ready with the correct value or exception
**Validates: Requirements 13.1, 13.2, 13.3, 13.4, 13.5**

**Property 8: Collection Operations**
*For any* collection of futures, collection operations should return results according to their specified strategy (all, any, first N) with proper ordering and timeout handling
**Validates: Requirements 14.1, 14.2, 14.3, 14.4, 14.5**

**Property 9: Continuation Operations**
*For any* future and continuation operation, the operation should properly schedule, delay, or timeout the future while maintaining type safety and error propagation
**Validates: Requirements 15.1, 15.2, 15.3, 15.4, 15.5**

**Property 10: Transformation Operations**
*For any* future and transformation function, transformation operations should apply functions to values, handle errors, and execute cleanup while maintaining proper type deduction and void/Unit conversion
**Validates: Requirements 16.1, 16.2, 16.3, 16.4, 16.5**

**Property 11: Exception and Type Conversion**
*For any* exception or type conversion operation, the system should preserve information and maintain semantic equivalence while using move semantics where appropriate
**Validates: Requirements 18.1, 18.2, 18.3, 18.5**

**Property 12: Backward Compatibility and Interoperability**
*For any* combination of existing and new wrapper types, the system should maintain API compatibility and provide seamless interoperability without breaking existing code
**Validates: Requirements 20.1, 20.2, 20.3, 20.4, 20.5**

**Property 13: Future Usage Consistency**
*For any* source file in the codebase (excluding the kythira::Future implementation), all future-related operations should use only `kythira::Future` types
**Validates: Requirements 21.1, 21.2, 21.3**

**Property 14: Header Include Consistency**
*For any* header file in the codebase (excluding the kythira::Future implementation), future functionality should be accessed only through `#include <raft/future.hpp>`
**Validates: Requirements 21.4, 26.1**

**Property 15: Transport Method Return Types**
*For any* transport client method (HTTP or CoAP), the return type should be templated future types instead of concrete `folly::Future` types
**Validates: Requirements 22.1, 22.2**

**Property 16: Network Concept Compliance**
*For any* type that satisfies the network_client concept, all RPC methods should return templated future types
**Validates: Requirements 22.3, 22.4**

**Property 17: Network Simulator Return Types**
*For any* network simulator operation (connection read/write, listener accept), the return type should be templated future types
**Validates: Requirements 23.1, 23.2, 23.3, 23.4**

**Property 18: Test Code Future Usage**
*For any* test file, all future-related operations should use `kythira::Future` instead of `std::future` or `folly::Future`
**Validates: Requirements 24.1, 24.2, 24.3, 24.4, 24.5**

**Property 19: Behavioral Preservation**
*For any* async operation, the timing, ordering, error handling, and thread safety behavior should be equivalent before and after conversion
**Validates: Requirements 25.1, 25.2, 25.3, 25.4**

**Property 20: Build Dependency Consistency**
*For any* user code file, compilation should succeed without requiring direct includes of `<future>` or `<folly/futures/Future.h>`
**Validates: Requirements 26.2, 26.3**

**Property 21: Promise/Future Pattern Conversion**
*For any* Promise/Future usage pattern, it should be converted to use `kythira::Future` construction patterns instead of `makeFuture`, `getFuture()`, or promise fulfillment
**Validates: Requirements 27.1, 27.2, 27.3, 27.4, 27.5**

**Property 22: Core Implementation Genericity**
*For any* core implementation, it should accept future types as template parameters and use future concepts instead of concrete future types
**Validates: Requirements 28.1, 28.2**

**Property 23: Namespace Organization**
*For any* core implementation (including network_client concept, transport clients, and network simulator classes), it should be placed in the `kythira` namespace
**Validates: Requirements 28.5, 28.6, 28.7, 28.8, 28.9, 28.10**

**Property 24: Complete Conversion Validation**
*For any* search of the codebase, there should be no remaining `std::future` or direct `folly::Future` usage in public interfaces (excluding kythira::Future implementation)
**Validates: Requirements 29.1, 29.2**

## Error Handling

The design implements comprehensive error handling:

### Exception Safety Guarantees
- **Basic Guarantee**: All operations leave objects in valid state
- **Strong Guarantee**: Operations either succeed completely or have no effect
- **No-throw Guarantee**: Destructors and move operations never throw

### Error Propagation Strategy
1. Convert folly exceptions to standard exceptions at wrapper boundaries
2. Preserve exception information during type conversions
3. Provide meaningful error messages for common failure modes
4. Handle timeout and cancellation scenarios gracefully

### Resource Cleanup
- Automatic cleanup on wrapper destruction
- Proper handling of partially constructed objects
- Exception-safe resource acquisition and release

## Testing Strategy

### Dual Testing Approach

The implementation will use both unit testing and property-based testing:

**Unit Tests**:
- Verify specific examples and edge cases
- Test integration points between wrapper components
- Validate error conditions and exception handling
- Ensure proper resource cleanup

**Property-Based Tests**:
- Verify universal properties across all valid inputs
- Test concept compliance with generated wrapper instances
- Validate type conversions with random data
- Ensure correctness across different value types and scenarios

**Property-Based Testing Framework**: The implementation will use **Boost.Test** with property-based testing extensions for C++. Each property-based test will run a minimum of 100 iterations to ensure thorough coverage of the random input space.

**Test Tagging**: Each property-based test will be tagged with a comment explicitly referencing the correctness property from this design document using the format: `**Feature: folly-concept-wrappers, Property {number}: {property_text}**`

**Concept Validation Tests**: Static assertions will verify that all wrapper types satisfy their corresponding concepts at compile time, ensuring type safety and interface compliance.

**Integration Tests**: Tests will verify that new wrappers work seamlessly with existing `Try` and `Future` implementations, maintaining backward compatibility and interoperability.