# Design Document

## Overview

This design document outlines the implementation of comprehensive wrapper classes that adapt folly library types to satisfy the C++20 concepts defined in `include/concepts/future.hpp`. The current implementation in `include/raft/future.hpp` provides partial coverage with `Try` and `Future` wrappers, but lacks wrappers for promises, executors, factory functions, and advanced collective operations.

The design follows the adapter pattern, creating lightweight wrapper classes that delegate to underlying folly types while providing the interface required by the concepts. This approach ensures compatibility with existing concept-constrained code while leveraging folly's mature and performant implementations.

## Architecture

The wrapper architecture consists of several layers:

1. **Concept Layer**: C++20 concepts defining interfaces (`include/concepts/future.hpp`)
2. **Wrapper Layer**: Adapter classes implementing concept interfaces (`include/raft/future.hpp`)
3. **Folly Layer**: Underlying folly library implementations
4. **Integration Layer**: Utility functions and type conversions

The design maintains a clear separation between the concept definitions and their implementations, allowing for future extensibility and alternative implementations.

## Components and Interfaces

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
- Properties 1.1 and 2.1 (concept compliance) can be combined into a single comprehensive concept compliance property
- Properties 3.1, 3.2, 3.3 (factory operations) can be combined into a factory functionality property
- Properties 4.1-4.4 (collection operations) can be combined into a collection functionality property
- Properties 5.1-5.3 (continuation operations) can be combined into a continuation functionality property
- Properties 6.1-6.3 (transformation operations) can be combined into a transformation functionality property

**Property 1: Concept Compliance**
*For any* wrapper class and its corresponding concept, the wrapper should satisfy all concept requirements at compile time and runtime
**Validates: Requirements 1.1, 1.2, 2.1, 2.2, 7.1, 7.2**

**Property 2: Promise Value and Exception Handling**
*For any* promise wrapper and value/exception, setting the value or exception should properly convert types and make the associated future ready with the correct result
**Validates: Requirements 1.3, 1.4, 1.5**

**Property 3: Executor Work Submission**
*For any* executor wrapper and work item, submitting work should properly forward to the underlying executor and execute the work
**Validates: Requirements 2.3, 2.4, 2.5**

**Property 4: Factory Future Creation**
*For any* value or exception, factory methods should create futures that are immediately ready with the correct value or exception
**Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5**

**Property 5: Collection Operations**
*For any* collection of futures, collection operations should return results according to their specified strategy (all, any, first N) with proper ordering and timeout handling
**Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5**

**Property 6: Continuation Operations**
*For any* future and continuation operation, the operation should properly schedule, delay, or timeout the future while maintaining type safety and error propagation
**Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

**Property 7: Transformation Operations**
*For any* future and transformation function, transformation operations should apply functions to values, handle errors, and execute cleanup while maintaining proper type deduction and void/Unit conversion
**Validates: Requirements 6.1, 6.2, 6.3, 6.4, 6.5**

**Property 8: Exception and Type Conversion**
*For any* exception or type conversion operation, the system should preserve information and maintain semantic equivalence while using move semantics where appropriate
**Validates: Requirements 8.1, 8.2, 8.3, 8.5**

**Property 9: Generic Template Compatibility**
*For any* concept-constrained template function, wrapper types should work seamlessly as template arguments and maintain proper type deduction
**Validates: Requirements 7.4, 10.4**

**Property 10: Backward Compatibility and Interoperability**
*For any* combination of existing and new wrapper types, the system should maintain API compatibility and provide seamless interoperability without breaking existing code
**Validates: Requirements 10.1, 10.2, 10.3, 10.5**

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