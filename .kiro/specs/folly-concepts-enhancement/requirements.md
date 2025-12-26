# Requirements Document

## Introduction

This specification defines the requirements for enhancing the existing future concepts in `include/concepts/future.hpp` to better match Folly's actual API and fix compilation issues. The current concepts have some problems and don't fully capture the rich interface provided by folly::SemiPromise, folly::Promise, folly::Executor, and related global functions.

## Glossary

- **Folly**: Facebook's open-source C++ library providing futures, promises, and executors
- **Concept**: C++20 language feature for constraining template parameters
- **SemiPromise**: Folly's promise type that can be fulfilled but doesn't provide a future
- **Promise**: Folly's promise type that provides both fulfillment and future access
- **Executor**: Folly's interface for executing work asynchronously
- **KeepAlive**: Folly's RAII wrapper for executor lifetime management
- **Future**: Folly's future type for asynchronous computation results
- **Try**: Folly's type that holds either a value or an exception
- **SemiFuture**: Folly's future type without executor attachment

## Requirements

### Requirement 1

**User Story:** As a developer using the future concepts, I want the concepts to compile without errors, so that I can use them in template constraints.

#### Acceptance Criteria

1. WHEN the concepts header is included THEN the system SHALL compile without syntax errors
2. WHEN std::as_const is used THEN the system SHALL use the correct C++17 syntax
3. WHEN destructor concepts are defined THEN the system SHALL use valid C++20 syntax
4. WHEN template parameters are constrained THEN the system SHALL use proper concept syntax
5. WHEN the concepts are instantiated THEN the system SHALL validate template arguments correctly

### Requirement 2

**User Story:** As a developer working with Folly futures, I want concepts that accurately reflect folly::SemiPromise interface, so that I can write generic code that works with SemiPromise-like types.

#### Acceptance Criteria

1. WHEN a type satisfies semi_promise concept THEN the system SHALL require setValue method for non-void types
2. WHEN a type satisfies semi_promise concept THEN the system SHALL require setException method
3. WHEN a type satisfies semi_promise concept THEN the system SHALL require isFulfilled method
4. WHEN a void SemiPromise is used THEN the system SHALL handle setValue() without parameters
5. WHEN SemiPromise is fulfilled THEN the system SHALL prevent further fulfillment attempts

### Requirement 3

**User Story:** As a developer working with Folly promises, I want concepts that accurately reflect folly::Promise interface, so that I can write generic code that works with Promise-like types.

#### Acceptance Criteria

1. WHEN a type satisfies promise concept THEN the system SHALL extend semi_promise requirements
2. WHEN a type satisfies promise concept THEN the system SHALL require getFuture method
3. WHEN a type satisfies promise concept THEN the system SHALL require getSemiFuture method
4. WHEN Promise provides futures THEN the system SHALL ensure type consistency between promise and future
5. WHEN Promise is moved THEN the system SHALL maintain proper move semantics

### Requirement 4

**User Story:** As a developer working with Folly executors, I want concepts that accurately reflect folly::Executor interface, so that I can write generic code that works with Executor-like types.

#### Acceptance Criteria

1. WHEN a type satisfies executor concept THEN the system SHALL require add method for function execution
2. WHERE priority support is available, WHEN a type satisfies executor concept THEN the system SHALL support priority-based execution
3. WHEN a type satisfies executor concept THEN the system SHALL provide getKeepAliveToken method
4. WHEN Executor manages work THEN the system SHALL handle function objects properly
5. WHEN Executor is destroyed THEN the system SHALL ensure proper cleanup semantics

### Requirement 5

**User Story:** As a developer working with Folly executor lifetime, I want concepts that accurately reflect folly::Executor::KeepAlive interface, so that I can manage executor lifetime safely.

#### Acceptance Criteria

1. WHEN a type satisfies keep_alive concept THEN the system SHALL require add method delegation
2. WHEN a type satisfies keep_alive concept THEN the system SHALL require get method for executor access
3. WHEN a type satisfies keep_alive concept THEN the system SHALL support copy and move construction
4. WHEN KeepAlive is copied THEN the system SHALL maintain reference counting semantics
5. WHEN KeepAlive is destroyed THEN the system SHALL release executor reference properly

### Requirement 6

**User Story:** As a developer working with Folly futures, I want concepts that accurately reflect folly::Future interface, so that I can write generic code that works with Future-like types.

#### Acceptance Criteria

1. WHEN a type satisfies future concept THEN the system SHALL require get method for value retrieval
2. WHEN a type satisfies future concept THEN the system SHALL require isReady method for status checking
3. WHEN a type satisfies future concept THEN the system SHALL require wait method with timeout support
4. WHEN a type satisfies future concept THEN the system SHALL require then method for continuation chaining
5. WHEN a type satisfies future concept THEN the system SHALL require onError method for error handling

### Requirement 7

**User Story:** As a developer working with Folly future operations, I want concepts for global functions that work with futures, so that I can write generic algorithms over future collections.

#### Acceptance Criteria

1. WHEN collectAll is used THEN the system SHALL require a function that takes future collections and returns combined results
2. WHEN collectAny is used THEN the system SHALL require a function that returns the first completed future
3. WHEN collectAnyWithoutException is used THEN the system SHALL require a function that returns the first successfully completed future
4. WHEN collectN is used THEN the system SHALL require a function that collects the first N completed futures
5. WHEN makeFuture is used THEN the system SHALL require factory functions for creating futures from values
6. WHEN makeExceptionalFuture is used THEN the system SHALL require factory functions for creating futures from exceptions
7. WHEN future timeouts are used THEN the system SHALL support timeout-based operations

### Requirement 8

**User Story:** As a developer working with Folly future continuations, I want concepts that support executor-aware operations, so that I can control where continuations execute.

#### Acceptance Criteria

1. WHEN via method is used THEN the system SHALL require executor attachment for continuations
2. WHEN delay method is used THEN the system SHALL require time-based future scheduling
3. WHEN within method is used THEN the system SHALL require timeout-based future operations
4. WHEN future scheduling is used THEN the system SHALL maintain proper executor semantics
5. WHEN continuation chains are built THEN the system SHALL preserve executor context appropriately

### Requirement 9

**User Story:** As a developer working with Folly Try types, I want concepts that accurately reflect folly::Try interface, so that I can handle success and error cases uniformly.

#### Acceptance Criteria

1. WHEN a type satisfies try_type concept THEN the system SHALL require value method for success case access
2. WHEN a type satisfies try_type concept THEN the system SHALL require exception method for error case access
3. WHEN a type satisfies try_type concept THEN the system SHALL require hasValue method for success checking
4. WHEN a type satisfies try_type concept THEN the system SHALL require hasException method for error checking
5. WHEN Try contains exceptions THEN the system SHALL provide proper exception_wrapper integration

### Requirement 10

**User Story:** As a developer using the enhanced concepts, I want comprehensive validation that the concepts work with actual Folly types, so that I can trust the concept definitions.

#### Acceptance Criteria

1. WHEN folly::SemiPromise is tested THEN the system SHALL validate it satisfies semi_promise concept
2. WHEN folly::Promise is tested THEN the system SHALL validate it satisfies promise concept
3. WHEN folly::Executor implementations are tested THEN the system SHALL validate they satisfy executor concept
4. WHEN folly::Future is tested THEN the system SHALL validate it satisfies future concept
5. WHEN kythira::Future is tested THEN the system SHALL validate it satisfies all relevant concepts