# Requirements Document

## Introduction

This document specifies the requirements for converting all uses of `std::future` and `folly::Future` to use the existing `kythira::Future` wrapper throughout the codebase. The `kythira::Future` class provides a unified interface that wraps `folly::Future` internally while providing a consistent API that can be used across the entire project.

## Glossary

- **kythira::Future**: The project's future wrapper class that provides a unified interface over `folly::Future`
- **folly::Future**: Facebook's Folly library future implementation used internally by `kythira::Future`
- **std::future**: Standard library future implementation that needs to be replaced
- **Future Conversion**: The process of replacing direct usage of `std::future` and `folly::Future` with `kythira::Future`
- **API Compatibility**: Ensuring that the converted code maintains the same functional behavior
- **Transport Layer**: Network communication components (HTTP and CoAP transports) that use futures for async operations
- **Network Simulator**: Testing infrastructure that uses futures for simulated network operations
- **Test Suite**: All test files that currently use futures for testing async operations

## Requirements

### Requirement 1

**User Story:** As a developer, I want all future-related code to use a consistent `kythira::Future` interface, so that the codebase has a unified approach to asynchronous operations.

#### Acceptance Criteria

1. WHEN the codebase is examined THEN the system SHALL use only `kythira::Future` for all future-related operations, except within the `kythira::Future` implementation itself which MAY use `folly::Future` internally
2. WHEN existing `std::future` usage is found THEN the system SHALL replace it with `kythira::Future`
3. WHEN existing `folly::Future` usage is found THEN the system SHALL replace it with `kythira::Future`
4. WHEN header includes are examined THEN the system SHALL include only `future/future.hpp` for future functionality
5. WHEN the conversion is complete THEN the system SHALL maintain all existing functional behavior

### Requirement 2

**User Story:** As a network transport developer, I want the transport layer interfaces to use the future concept defined as a template argument, so that all async RPC operations have a consistent and flexible return type.

#### Acceptance Criteria

1. WHEN HTTP transport methods are called THEN the system SHALL return future types defined by template arguments instead of `folly::Future`
2. WHEN CoAP transport methods are called THEN the system SHALL return future types defined by template arguments instead of `folly::Future`
3. WHEN network client concepts are defined THEN the system SHALL specify future concepts as template parameters for the required return type
4. WHEN RPC methods are invoked THEN the system SHALL handle request_vote, append_entries, and install_snapshot operations with templated future types
5. WHEN multicast operations are performed THEN the system SHALL return templated future types for async multicast message delivery

### Requirement 3

**User Story:** As a network simulator developer, I want the network simulator components to use the future concept defined as a template argument, so that simulated network operations are consistent with the rest of the system.

#### Acceptance Criteria

1. WHEN connection read operations are performed THEN the system SHALL return future types defined by template arguments
2. WHEN connection write operations are performed THEN the system SHALL return future types defined by template arguments
3. WHEN listener accept operations are performed THEN the system SHALL return future types defined by template arguments
4. WHEN network simulator operations are executed THEN the system SHALL use templated future types for all async operations
5. WHEN timeout-based operations are performed THEN the system SHALL support timeout parameters with templated future types

### Requirement 4

**User Story:** As a test developer, I want all test code to use `kythira::Future`, so that tests are consistent with the production code they validate.

#### Acceptance Criteria

1. WHEN property-based tests are executed THEN the system SHALL use `kythira::Future` instead of `folly::Future` for async operations
2. WHEN integration tests are executed THEN the system SHALL use `kythira::Future` instead of `std::future` for concurrent operations
3. WHEN test fixtures create futures THEN the system SHALL use `kythira::Future` consistently
4. WHEN test code waits for async operations THEN the system SHALL use `kythira::Future` methods for synchronization
5. WHEN test code validates async behavior THEN the system SHALL use `kythira::Future` for result verification

### Requirement 5

**User Story:** As a developer, I want the conversion to preserve all existing functionality, so that no behavioral changes are introduced during the refactoring.

#### Acceptance Criteria

1. WHEN async operations are performed THEN the system SHALL maintain the same timing and ordering behavior
2. WHEN error handling is executed THEN the system SHALL preserve all exception handling and error propagation
3. WHEN concurrent operations are performed THEN the system SHALL maintain thread safety and synchronization behavior
4. WHEN timeout operations are executed THEN the system SHALL preserve all timeout handling behavior
5. WHEN the conversion is complete THEN the system SHALL pass all existing tests without modification

### Requirement 6

**User Story:** As a build system maintainer, I want the conversion to maintain clean dependencies, so that the build system remains efficient and the dependency graph is simplified.

#### Acceptance Criteria

1. WHEN header files are examined THEN the system SHALL include `future/future.hpp` instead of `<future>` or `<folly/futures/Future.h>`
2. WHEN compilation is performed THEN the system SHALL not require direct `std::future` or `folly::Future` includes in user code
3. WHEN linking is performed THEN the system SHALL maintain the same library dependencies
4. WHEN the build completes THEN the system SHALL produce the same executable functionality
5. WHEN dependency analysis is performed THEN the system SHALL show reduced direct dependencies on future implementations

### Requirement 7

**User Story:** As a code maintainer, I want the conversion to handle Promise/Future patterns correctly, so that all async coordination mechanisms work properly.

#### Acceptance Criteria

1. WHEN Promise/Future pairs are used THEN the system SHALL convert them to use `kythira::Future` construction patterns
2. WHEN `makeFuture` calls are found THEN the system SHALL replace them with `kythira::Future` constructors
3. WHEN `getFuture()` calls are found THEN the system SHALL replace them with appropriate `kythira::Future` creation
4. WHEN promise fulfillment occurs THEN the system SHALL use `kythira::Future` value and exception constructors
5. WHEN future chaining is performed THEN the system SHALL use `kythira::Future` continuation methods

### Requirement 8

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

### Requirement 9

**User Story:** As a developer, I want comprehensive validation of the conversion, so that I can be confident the refactoring is complete and correct.

#### Acceptance Criteria

1. WHEN the codebase is searched THEN the system SHALL contain no remaining `std::future` usage
2. WHEN the codebase is searched THEN the system SHALL contain no remaining direct `folly::Future` usage in public interfaces
3. WHEN compilation is performed THEN the system SHALL build successfully with no future-related errors
4. WHEN tests are executed THEN the system SHALL pass all existing tests
5. WHEN the conversion is validated THEN the system SHALL demonstrate equivalent performance characteristics