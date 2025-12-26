# Design Document

## Overview

This design enhances the existing future concepts in `include/concepts/future.hpp` to better match Folly's actual API and fix compilation issues. The current concepts have syntax errors and don't fully capture the rich interface provided by folly::SemiPromise, folly::Promise, folly::Executor, and related global functions.

The design focuses on creating accurate C++20 concepts that can be used to constrain template parameters for generic code that works with Folly-like types, while maintaining compatibility with the existing kythira::Future wrapper implementation.

## Architecture

The enhanced concepts will be organized into several categories:

1. **Core Value Types**: `try_type` concept for folly::Try-like types
2. **Promise Types**: `semi_promise` and `promise` concepts for folly::SemiPromise and folly::Promise
3. **Future Types**: `future` concept for folly::Future-like types with comprehensive method requirements
4. **Executor Types**: `executor` and `keep_alive` concepts for folly::Executor and folly::Executor::KeepAlive
5. **Global Function Concepts**: Concepts for factory functions and collection operations
6. **Continuation Concepts**: Concepts for executor-aware future operations

## Components and Interfaces

### Core Compilation Fixes

The current concepts have several compilation issues that need to be addressed:

1. **std::as_const Usage**: The current code uses `std::as_const` which requires C++17, but uses incorrect syntax
2. **Destructor Concept**: Invalid syntax for destructor requirements
3. **Template Parameter Constraints**: Improve concept constraint syntax

### Enhanced Try Concept

```cpp
template<typename T, typename ValueType>
concept try_type = requires(T t, const T ct) {
    // Access value (throws if contains exception)
    { t.value() } -> std::same_as<ValueType&>;
    { ct.value() } -> std::same_as<const ValueType&>;
    
    // Check if contains value
    { t.hasValue() } -> std::convertible_to<bool>;
    
    // Check if contains exception
    { t.hasException() } -> std::convertible_to<bool>;
    
    // Access exception (folly::Try uses exception_wrapper)
    { t.exception() }; // Returns folly::exception_wrapper or similar
};
```

### Enhanced SemiPromise Concept

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

### Enhanced Promise Concept

```cpp
template<typename P, typename T>
concept promise = semi_promise<P, T> && requires(P p) {
    // Get associated future
    { p.getFuture() }; // Returns folly::Future<T> or similar
    
    // Get associated semi-future
    { p.getSemiFuture() }; // Returns folly::SemiFuture<T> or similar
};
```

### Enhanced Executor Concept

```cpp
template<typename E>
concept executor = requires(E e, std::function<void()> func) {
    // Add work to executor
    { e.add(std::move(func)) } -> std::same_as<void>;
    
    // Get keep alive token for lifetime management
    { e.getKeepAliveToken() }; // Returns Executor::KeepAlive or similar
} && requires(E e) {
    // Optional: Get number of priorities (not all executors support this)
    // { e.getNumPriorities() } -> std::convertible_to<std::uint8_t>;
};
```

### Enhanced KeepAlive Concept

```cpp
template<typename K>
concept keep_alive = requires(K k, std::function<void()> func) {
    // Add work via keep alive
    { k.add(std::move(func)) } -> std::same_as<void>;
    
    // Get underlying executor
    { k.get() }; // Returns Executor* or similar
    
    // Copy and move semantics
    { K(k) };
    { K(std::move(k)) };
};
```

### Enhanced Future Concept

```cpp
template<typename F, typename T>
concept future = requires(F f) {
    // Get value (blocking)
    { std::move(f).get() } -> std::same_as<T>;
    
    // Check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
    
    // Chain continuation with value
    { std::move(f).thenValue(std::declval<std::function<void(T)>>()) };
    
    // Chain continuation with Try
    { std::move(f).thenTry(std::declval<std::function<void(folly::Try<T>)>>()) };
    
    // Error handling
    { std::move(f).thenError(std::declval<std::function<T(folly::exception_wrapper)>>()) };
    
    // Via executor
    { std::move(f).via(std::declval<folly::Executor*>()) };
};
```

## Data Models

### Concept Hierarchy

```
try_type<T, ValueType>
├── Requires value access methods
├── Requires exception checking methods
└── Requires state checking methods

semi_promise<P, T>
├── Requires setValue method (void-aware)
├── Requires setException method
└── Requires isFulfilled method

promise<P, T> : semi_promise<P, T>
├── Requires getFuture method
└── Requires getSemiFuture method

executor<E>
├── Requires add method
└── Requires getKeepAliveToken method

keep_alive<K>
├── Requires add method delegation
├── Requires get method for executor access
└── Requires copy/move semantics

future<F, T>
├── Requires get method
├── Requires isReady method
├── Requires wait method
├── Requires thenValue method
├── Requires thenTry method
├── Requires thenError method
└── Requires via method
```

### Global Function Concepts

```cpp
// Factory function concepts
template<typename Factory>
concept future_factory = requires() {
    // Make future from value
    { Factory::makeFuture(std::declval<int>()) } -> future<int>;
    
    // Make future from exception
    { Factory::makeExceptionalFuture<int>(std::declval<folly::exception_wrapper>()) } -> future<int>;
    
    // Make ready future
    { Factory::makeReadyFuture() } -> future<folly::Unit>;
};

// Collection function concepts
template<typename Collector>
concept future_collector = requires() {
    // Collect all futures
    { Collector::collectAll(std::declval<std::vector<folly::Future<int>>>()) };
    
    // Collect any future
    { Collector::collectAny(std::declval<std::vector<folly::Future<int>>>()) };
    
    // Collect any without exception
    { Collector::collectAnyWithoutException(std::declval<std::vector<folly::Future<int>>>()) };
    
    // Collect N futures
    { Collector::collectN(std::declval<std::vector<folly::Future<int>>>(), std::size_t{}) };
};
```

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*
Property 1: Concept compilation validation
*For any* C++ compiler, including the concepts header should result in successful compilation without syntax errors
**Validates: Requirements 1.1, 1.2, 1.3, 1.4**

Property 2: Concept constraint validation
*For any* type, the concepts should correctly accept or reject the type based on its interface
**Validates: Requirements 1.5**

Property 3: SemiPromise concept requirements
*For any* type that satisfies semi_promise concept, it should provide setValue, setException, and isFulfilled methods
**Validates: Requirements 2.1, 2.2, 2.3**

Property 4: Promise concept inheritance
*For any* type that satisfies promise concept, it should also satisfy semi_promise concept and provide getFuture and getSemiFuture methods
**Validates: Requirements 3.1, 3.2, 3.3, 3.4**

Property 5: Executor concept requirements
*For any* type that satisfies executor concept, it should provide add and getKeepAliveToken methods
**Validates: Requirements 4.1, 4.3**

Property 6: KeepAlive concept requirements
*For any* type that satisfies keep_alive concept, it should provide add, get methods and support copy/move construction
**Validates: Requirements 5.1, 5.2, 5.3**

Property 7: Future concept requirements
*For any* type that satisfies future concept, it should provide get, isReady, wait, then, and error handling methods
**Validates: Requirements 6.1, 6.2, 6.3, 6.4, 6.5**

Property 8: Timeout operation support
*For any* future type, timeout-based operations should be supported consistently
**Validates: Requirements 7.7**

Property 9: Executor attachment support
*For any* future and executor type, via method should enable executor attachment for continuations
**Validates: Requirements 8.1**

Property 10: Try concept requirements
*For any* type that satisfies try_type concept, it should provide value, exception, hasValue, and hasException methods
**Validates: Requirements 9.1, 9.2, 9.3, 9.4**

Property 11: Folly executor concept compliance
*For any* folly::Executor implementation, it should satisfy the executor concept
**Validates: Requirements 10.3**

Property 12: Folly future concept compliance
*For any* value type T, folly::Future<T> should satisfy the future concept
**Validates: Requirements 10.4**

Property 13: Kythira future concept compliance
*For any* value type T, kythira::Future<T> should satisfy all relevant concepts
**Validates: Requirements 10.5**

## Error Handling

### Compilation Error Prevention

The enhanced concepts will fix several categories of compilation errors:

1. **Syntax Errors**: Fix std::as_const usage and destructor concept syntax
2. **Template Constraint Errors**: Improve concept constraint expressions
3. **Type Deduction Errors**: Ensure proper return type specifications

### Concept Validation Errors

The concepts will provide clear error messages when types don't satisfy requirements:

1. **Missing Methods**: Clear indication when required methods are missing
2. **Wrong Return Types**: Specific errors for incorrect return types
3. **Const Correctness**: Proper handling of const and non-const method variants

### Runtime Error Considerations

While concepts are compile-time constructs, they should be designed to work with types that have proper runtime error handling:

1. **Exception Safety**: Concepts should work with types that throw exceptions appropriately
2. **Resource Management**: Concepts should support RAII and proper resource cleanup
3. **Thread Safety**: Concepts should not preclude thread-safe implementations

## Testing Strategy

### Unit Testing Approach

**Unit tests will focus on**:
- Compilation validation of individual concepts
- Static assertion tests for type acceptance/rejection
- Specific examples of concept usage with known types

**Unit test categories**:
- Syntax validation tests for each concept
- Type compatibility tests with folly types
- Edge case handling for void specializations

### Property-Based Testing Approach

**Property tests will verify**:
- Universal properties that should hold across all types satisfying a concept
- Consistency between related concepts (e.g., promise extends semi_promise)
- Behavioral properties that can be validated at compile time

**Property test implementation**:
- Use C++20 concepts and static_assert for compile-time validation
- Generate test cases for different template parameter combinations
- Validate concept relationships and inheritance

**Testing framework**: The design specifies using C++20 concepts with static_assert for property-based testing, as the properties are primarily compile-time constraints.

**Test execution**: All tests will be executed using CTest with appropriate timeouts. Each test case will use the two-argument `BOOST_AUTO_TEST_CASE` format with timeout specifications.

**Property test requirements**:
- Each correctness property will be implemented as a separate property-based test
- Tests will be tagged with comments referencing the design document property
- Test format: `**Feature: folly-concepts-enhancement, Property {number}: {property_text}**`
- Minimum 100 iterations for randomized property tests where applicable

## Implementation Notes

### C++20 Concept Syntax

The implementation will use modern C++20 concept syntax:

```cpp
template<typename T>
concept my_concept = requires(T t) {
    // Requirements here
};
```

### Folly Integration

The concepts will be designed to work seamlessly with:
- `folly::Future<T>` and `folly::SemiFuture<T>`
- `folly::Promise<T>` and `folly::SemiPromise<T>`
- `folly::Executor` and its implementations
- `folly::Try<T>` and `folly::exception_wrapper`

### Backward Compatibility

The enhanced concepts will maintain compatibility with:
- Existing `kythira::Future<T>` wrapper implementation
- Current usage patterns in the codebase
- Existing template constraints and static assertions

### Performance Considerations

Concepts are compile-time constructs and have no runtime performance impact. The design focuses on:
- Fast compilation times through efficient concept expressions
- Clear error messages for failed concept checks
- Minimal template instantiation overhead