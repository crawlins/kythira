# Requirements Document

## Introduction

This specification defines the requirements for enhancing the C++20 concepts in `include/concepts/future.hpp`, creating comprehensive wrapper classes that adapt folly library types to satisfy these enhanced concepts, and ensuring consistent usage of the unified future interface throughout the codebase. The current implementation has three main issues: (1) the concepts themselves have compilation errors and don't fully capture Folly's rich API, (2) `include/raft/future.hpp` contains only partial wrapper implementations for `Try` and `Future`, missing wrappers for promises, executors, future factories, and collective operations, and (3) the codebase has inconsistent usage of different future types (`std::future`, `folly::Future`, and `kythira::Future`). This feature will fix the concept definitions, complete the wrapper ecosystem, and ensure all code uses the unified `kythira::Future` interface to provide a consistent approach to asynchronous programming.

## Glossary

- **Concept Enhancement**: Fixing compilation errors and improving C++20 concept definitions to better match Folly's API
- **Concept Wrapper**: A C++ class that adapts an external library type (folly) to satisfy a C++20 concept interface
- **Folly Library**: Facebook's open-source C++ library providing futures, promises, and executors
- **Future Concept**: C++20 concept defining the interface for asynchronous computation results
- **Promise Concept**: C++20 concept defining the interface for setting future values
- **Executor Concept**: C++20 concept defining the interface for task execution scheduling
- **SemiPromise**: Folly's promise type that can be fulfilled but doesn't provide a future
- **Promise**: Folly's promise type that provides both fulfillment and future access
- **KeepAlive**: Folly's RAII wrapper for executor lifetime management
- **Try**: Folly's type that holds either a value or an exception
- **SemiFuture**: Folly's future type without executor attachment
- **Collective Operations**: Functions that operate on multiple futures simultaneously (collectAll, collectAny, etc.)
- **Continuation Operations**: Methods for chaining asynchronous operations (via, delay, within)
- **Factory Operations**: Functions for creating futures from values or exceptions
- **kythira::Future**: The project's unified future wrapper class that provides a consistent interface over `folly::Future`
- **Future Conversion**: The process of replacing direct usage of `std::future` and `folly::Future` with `kythira::Future`
- **API Compatibility**: Ensuring that the converted code maintains the same functional behavior
- **Transport Layer**: Network communication components (HTTP and CoAP transports) that use futures for async operations
- **Network Simulator**: Testing infrastructure that uses futures for simulated network operations

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

### Requirement 11

**User Story:** As a developer using the kythira library, I want complete folly promise wrappers, so that I can create and fulfill promises using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer creates a Promise wrapper THEN the system SHALL provide a class that satisfies the promise concept
2. WHEN a developer creates a SemiPromise wrapper THEN the system SHALL provide a class that satisfies the semi_promise concept
3. WHEN a developer sets a value on a promise THEN the system SHALL properly handle both void and non-void value types
4. WHEN a developer sets an exception on a promise THEN the system SHALL convert between folly::exception_wrapper and std::exception_ptr
5. WHEN a developer gets a future from a promise THEN the system SHALL return a properly wrapped Future instance

### Requirement 12

**User Story:** As a developer using the kythira library, I want complete folly executor wrappers, so that I can schedule and execute asynchronous tasks using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer creates an Executor wrapper THEN the system SHALL provide a class that satisfies the executor concept
2. WHEN a developer creates a KeepAlive wrapper THEN the system SHALL provide a class that satisfies the keep_alive concept
3. WHEN a developer adds work to an executor THEN the system SHALL properly forward the work to the underlying folly executor
4. WHEN a developer accesses the underlying executor from KeepAlive THEN the system SHALL provide proper pointer-like access
5. WHEN a developer copies or moves KeepAlive instances THEN the system SHALL maintain proper reference counting

### Requirement 13

**User Story:** As a developer using the kythira library, I want complete future factory wrappers, so that I can create futures from values and exceptions using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer uses makeFuture with a value THEN the system SHALL create a ready future containing that value
2. WHEN a developer uses makeExceptionalFuture with an exception THEN the system SHALL create a failed future containing that exception
3. WHEN a developer uses makeReadyFuture THEN the system SHALL create a ready future with void/Unit value
4. WHEN a developer creates futures from different value types THEN the system SHALL properly handle type deduction and conversion
5. WHEN a developer creates exceptional futures THEN the system SHALL convert between folly::exception_wrapper and std::exception_ptr

### Requirement 14

**User Story:** As a developer using the kythira library, I want complete future collector wrappers, so that I can coordinate multiple asynchronous operations using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer uses collectAll with multiple futures THEN the system SHALL wait for all futures and return results in order
2. WHEN a developer uses collectAny with multiple futures THEN the system SHALL return the first completed future with its index
3. WHEN a developer uses collectAnyWithoutException with multiple futures THEN the system SHALL return the first successfully completed future
4. WHEN a developer uses collectN with multiple futures THEN the system SHALL return the first N completed futures
5. WHEN a developer performs collection operations THEN the system SHALL properly handle timeout and cancellation scenarios

### Requirement 15

**User Story:** As a developer using the kythira library, I want complete future continuation wrappers, so that I can chain and schedule asynchronous operations using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer uses via with an executor THEN the system SHALL schedule the continuation on the specified executor
2. WHEN a developer uses delay with a duration THEN the system SHALL delay the future completion by the specified time
3. WHEN a developer uses within with a timeout THEN the system SHALL add timeout behavior to the future
4. WHEN a developer chains continuation operations THEN the system SHALL maintain proper type safety and error propagation
5. WHEN a developer uses continuation operations with void futures THEN the system SHALL handle Unit conversion properly

### Requirement 16

**User Story:** As a developer using the kythira library, I want complete future transformation wrappers, so that I can transform and handle errors in asynchronous operations using a consistent interface.

#### Acceptance Criteria

1. WHEN a developer uses thenValue with a transformation function THEN the system SHALL apply the function to successful results
2. WHEN a developer uses thenError with an error handler THEN the system SHALL handle exceptions and convert error types
3. WHEN a developer uses ensure with a cleanup function THEN the system SHALL execute cleanup regardless of success or failure
4. WHEN a developer chains transformation operations THEN the system SHALL maintain proper type deduction and error propagation
5. WHEN a developer transforms void futures THEN the system SHALL handle Unit conversion and void return types properly

### Requirement 17

**User Story:** As a developer using the kythira library, I want proper concept compliance validation, so that I can ensure all wrappers satisfy their respective concepts at compile time.

#### Acceptance Criteria

1. WHEN the system compiles wrapper classes THEN all wrappers SHALL satisfy their corresponding C++20 concepts
2. WHEN a developer uses static_assert with concept checks THEN the system SHALL validate concept compliance at compile time
3. WHEN a developer instantiates wrapper templates THEN the system SHALL provide clear compilation errors for invalid usage
4. WHEN a developer uses wrappers in generic code THEN the system SHALL work seamlessly with concept-constrained templates
5. WHEN the system validates concepts THEN it SHALL check both interface requirements and semantic constraints

### Requirement 18

**User Story:** As a developer using the kythira library, I want proper error handling and type conversion, so that I can work with consistent error types across different future implementations.

#### Acceptance Criteria

1. WHEN the system converts between folly::exception_wrapper and std::exception_ptr THEN it SHALL preserve exception information
2. WHEN the system handles void/Unit conversions THEN it SHALL maintain semantic equivalence between void and folly::Unit
3. WHEN the system propagates errors through wrapper operations THEN it SHALL maintain proper exception safety guarantees
4. WHEN a developer catches exceptions from wrapper operations THEN the system SHALL provide meaningful error messages
5. WHEN the system performs type conversions THEN it SHALL avoid unnecessary copies and maintain move semantics

### Requirement 19

**User Story:** As a developer using the kythira library, I want comprehensive testing of wrapper functionality, so that I can rely on correct behavior across all supported operations.

#### Acceptance Criteria

1. WHEN the system tests promise operations THEN it SHALL verify value setting, exception setting, and future retrieval
2. WHEN the system tests executor operations THEN it SHALL verify work scheduling and KeepAlive behavior
3. WHEN the system tests factory operations THEN it SHALL verify future creation from values and exceptions
4. WHEN the system tests collector operations THEN it SHALL verify all collection strategies and timeout handling
5. WHEN the system tests continuation and transformation operations THEN it SHALL verify chaining, error handling, and type conversions

### Requirement 20

**User Story:** As a developer using the kythira library, I want proper integration with existing code, so that I can use the new wrappers alongside existing Try and Future implementations.

#### Acceptance Criteria

1. WHEN a developer uses new wrappers with existing Try and Future classes THEN the system SHALL maintain API compatibility
2. WHEN a developer mixes wrapper types in the same code THEN the system SHALL provide seamless interoperability
3. WHEN a developer upgrades from partial to complete wrapper implementation THEN the system SHALL maintain backward compatibility
4. WHEN a developer uses wrappers in template code THEN the system SHALL work with existing concept-constrained functions
5. WHEN the system integrates new wrappers THEN it SHALL not break existing compilation or runtime behavior

### Requirement 21

**User Story:** As a developer, I want all future-related code to use a consistent `kythira::Future` interface, so that the codebase has a unified approach to asynchronous operations.

#### Acceptance Criteria

1. WHEN the codebase is examined THEN the system SHALL use only `kythira::Future` for all future-related operations, except within the `kythira::Future` implementation itself which MAY use `folly::Future` internally
2. WHEN existing `std::future` usage is found THEN the system SHALL replace it with `kythira::Future`
3. WHEN existing `folly::Future` usage is found THEN the system SHALL replace it with `kythira::Future`
4. WHEN header includes are examined THEN the system SHALL include only `raft/future.hpp` for future functionality
5. WHEN the conversion is complete THEN the system SHALL maintain all existing functional behavior

### Requirement 22

**User Story:** As a network transport developer, I want the transport layer interfaces to use the future concept defined as a template argument, so that all async RPC operations have a consistent and flexible return type.

#### Acceptance Criteria

1. WHEN HTTP transport methods are called THEN the system SHALL return future types defined by template arguments instead of `folly::Future`
2. WHEN CoAP transport methods are called THEN the system SHALL return future types defined by template arguments instead of `folly::Future`
3. WHEN network client concepts are defined THEN the system SHALL specify future concepts as template parameters for the required return type
4. WHEN RPC methods are invoked THEN the system SHALL handle request_vote, append_entries, and install_snapshot operations with templated future types
5. WHEN multicast operations are performed THEN the system SHALL return templated future types for async multicast message delivery

### Requirement 23

**User Story:** As a network simulator developer, I want the network simulator components to use the future concept defined as a template argument, so that simulated network operations are consistent with the rest of the system.

#### Acceptance Criteria

1. WHEN connection read operations are performed THEN the system SHALL return future types defined by template arguments
2. WHEN connection write operations are performed THEN the system SHALL return future types defined by template arguments
3. WHEN listener accept operations are performed THEN the system SHALL return future types defined by template arguments
4. WHEN network simulator operations are executed THEN the system SHALL use templated future types for all async operations
5. WHEN timeout-based operations are performed THEN the system SHALL support timeout parameters with templated future types

### Requirement 24

**User Story:** As a test developer, I want all test code to use `kythira::Future`, so that tests are consistent with the production code they validate.

#### Acceptance Criteria

1. WHEN property-based tests are executed THEN the system SHALL use `kythira::Future` instead of `folly::Future` for async operations
2. WHEN integration tests are executed THEN the system SHALL use `kythira::Future` instead of `std::future` for concurrent operations
3. WHEN test fixtures create futures THEN the system SHALL use `kythira::Future` consistently
4. WHEN test code waits for async operations THEN the system SHALL use `kythira::Future` methods for synchronization
5. WHEN test code validates async behavior THEN the system SHALL use `kythira::Future` for result verification

### Requirement 25

**User Story:** As a developer, I want the conversion to preserve all existing functionality, so that no behavioral changes are introduced during the refactoring.

#### Acceptance Criteria

1. WHEN async operations are performed THEN the system SHALL maintain the same timing and ordering behavior
2. WHEN error handling is executed THEN the system SHALL preserve all exception handling and error propagation
3. WHEN concurrent operations are performed THEN the system SHALL maintain thread safety and synchronization behavior
4. WHEN timeout operations are executed THEN the system SHALL preserve all timeout handling behavior
5. WHEN the conversion is complete THEN the system SHALL pass all existing tests without modification

### Requirement 26

**User Story:** As a build system maintainer, I want the conversion to maintain clean dependencies, so that the build system remains efficient and the dependency graph is simplified.

#### Acceptance Criteria

1. WHEN header files are examined THEN the system SHALL include `raft/future.hpp` instead of `<future>` or `<folly/futures/Future.h>`
2. WHEN compilation is performed THEN the system SHALL not require direct `std::future` or `folly::Future` includes in user code
3. WHEN linking is performed THEN the system SHALL maintain the same library dependencies
4. WHEN the build completes THEN the system SHALL produce the same executable functionality
5. WHEN dependency analysis is performed THEN the system SHALL show reduced direct dependencies on future implementations

### Requirement 27

**User Story:** As a code maintainer, I want the conversion to handle Promise/Future patterns correctly, so that all async coordination mechanisms work properly.

#### Acceptance Criteria

1. WHEN Promise/Future pairs are used THEN the system SHALL convert them to use `kythira::Future` construction patterns
2. WHEN `makeFuture` calls are found THEN the system SHALL replace them with `kythira::Future` constructors
3. WHEN `getFuture()` calls are found THEN the system SHALL replace them with appropriate `kythira::Future` creation
4. WHEN promise fulfillment occurs THEN the system SHALL use `kythira::Future` value and exception constructors
5. WHEN future chaining is performed THEN the system SHALL use `kythira::Future` continuation methods

### Requirement 28

**User Story:** As a core library developer, I want the core Raft implementations to use generic future concepts and template parameters, so that the system is flexible and can work with different future implementations.

#### Acceptance Criteria

1. WHEN core Raft implementations are defined THEN the system SHALL specify their interfaces using future concepts instead of concrete future types
2. WHEN core Raft classes are instantiated THEN the system SHALL accept a future type as a template argument
3. WHEN the future implementation is relocated THEN the system SHALL move `include/future/future.hpp` to `include/raft/future.hpp`
4. WHEN the future implementation is relocated THEN the system SHALL maintain the `kythira` namespace for future classes
5. WHEN core implementations are organized THEN the system SHALL place core implementations in the `kythira` namespace instead of the `raft` namespace
6. WHEN network concepts are defined THEN the system SHALL place the `network_client` concept in the `kythira` namespace
7. WHEN transport implementations are organized THEN the system SHALL place the `cpp_httplib_client` class in the `kythira` namespace
8. WHEN transport implementations are organized THEN the system SHALL place the `coap_client` class in the `kythira` namespace
9. WHEN network simulator components are organized THEN the system SHALL place the `Connection` class in the `kythira` namespace
10. WHEN network simulator components are organized THEN the system SHALL place the `Listener` class in the `kythira` namespace

### Requirement 29

**User Story:** As a developer, I want comprehensive validation of the conversion, so that I can be confident the refactoring is complete and correct.

#### Acceptance Criteria

1. WHEN the codebase is searched THEN the system SHALL contain no remaining `std::future` usage
2. WHEN the codebase is searched THEN the system SHALL contain no remaining direct `folly::Future` usage in public interfaces
3. WHEN compilation is performed THEN the system SHALL build successfully with no future-related errors
4. WHEN tests are executed THEN the system SHALL pass all existing tests
5. WHEN the conversion is validated THEN the system SHALL demonstrate equivalent performance characteristics

### Requirement 30

**User Story:** As a developer implementing async retry logic, I want `thenTry` to support Future-returning callbacks with automatic flattening, so that I can chain async operations in error handling paths without blocking threads.

#### Acceptance Criteria

1. WHEN a developer uses thenTry with a callback that returns Future<U> THEN the system SHALL return Future<U> instead of Future<Future<U>>
2. WHEN a developer chains async operations in thenTry THEN the system SHALL automatically flatten nested futures
3. WHEN a developer uses thenTry with Future-returning callbacks THEN the system SHALL maintain proper error propagation
4. WHEN a developer uses thenTry with Future-returning callbacks THEN the system SHALL support both success and error cases in the Try parameter
5. WHEN a developer uses thenTry with Future-returning callbacks THEN the system SHALL work with both void and non-void future types

### Requirement 31

**User Story:** As a developer implementing async retry logic, I want `thenError` to support Future-returning callbacks with automatic flattening, so that I can implement non-blocking retry delays in error handlers.

#### Acceptance Criteria

1. WHEN a developer uses thenError with a callback that returns Future<T> THEN the system SHALL return Future<T> instead of attempting to convert Future<T> to T
2. WHEN a developer chains async operations in thenError THEN the system SHALL automatically flatten nested futures
3. WHEN a developer uses thenError with Future-returning callbacks THEN the system SHALL maintain proper error recovery semantics
4. WHEN a developer uses thenError with Future-returning callbacks THEN the system SHALL support async delay operations before retry
5. WHEN a developer uses thenError with Future-returning callbacks THEN the system SHALL work with both void and non-void future types

### Requirement 32

**User Story:** As a developer implementing retry logic, I want to use async delays instead of blocking sleep, so that threads are not blocked during retry backoff periods.

#### Acceptance Criteria

1. WHEN retry logic calculates a delay THEN the system SHALL use Future.delay() instead of std::this_thread::sleep_for
2. WHEN retry logic chains operations THEN the system SHALL use thenTry or thenError with Future-returning callbacks
3. WHEN retry logic executes THEN the system SHALL not block any threads during delay periods
4. WHEN retry logic handles errors THEN the system SHALL maintain proper exception propagation through async chains
5. WHEN retry logic completes THEN the system SHALL return results asynchronously without blocking