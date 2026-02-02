# Implementation Plan

- [x] 1. Fix compilation issues in existing concepts
  - Fix std::as_const syntax error by using proper const reference access
  - Fix destructor concept syntax by removing invalid destructor requirements
  - Fix template parameter constraint syntax issues
  - Ensure all concepts compile without errors
  - _Requirements: 1.1, 1.2, 1.3, 1.4_

- [x] 1.1 Write property test for concept compilation and constraint validation
  - **Property 1: Concept Compilation and Constraint Validation**
  - **Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5**

- [x] 2. Enhance try_type concept to match folly::Try interface
  - Update try_type concept to use hasValue() and hasException() methods (folly naming)
  - Fix const correctness for value() method access
  - Add proper exception access method requirements
  - Remove std::as_const usage and use proper const reference parameters
  - _Requirements: 9.1, 9.2, 9.3, 9.4_

- [x] 3. Enhance semi_promise concept to match folly::SemiPromise interface
  - Update semi_promise concept to handle void specialization properly
  - Add proper setValue method requirements for both void and non-void types
  - Add setException method requirements with folly::exception_wrapper support
  - Add isFulfilled method requirements
  - _Requirements: 2.1, 2.2, 2.3_

- [x] 4. Enhance promise concept to match folly::Promise interface
  - Ensure promise concept extends semi_promise concept properly
  - Add getFuture method requirements
  - Add getSemiFuture method requirements
  - Ensure type consistency between promise and returned future types
  - _Requirements: 3.1, 3.2, 3.3, 3.4_

- [x] 5. Enhance executor concept to match folly::Executor interface
  - Update executor concept to require add method for function execution
  - Add getKeepAliveToken method requirements
  - Remove invalid destructor requirements
  - Make priority support optional (not all executors support priorities)
  - _Requirements: 4.1, 4.3_

- [x] 6. Enhance keep_alive concept to match folly::Executor::KeepAlive interface
  - Update keep_alive concept to require add method delegation
  - Add get method requirements for executor access
  - Add copy and move construction requirements
  - Ensure proper template parameter handling
  - _Requirements: 5.1, 5.2, 5.3_

- [x] 7. Enhance future concept to match folly::Future interface
  - Update future concept to require get method (with move semantics)
  - Add isReady method requirements
  - Add wait method requirements with timeout support
  - Add thenValue and thenTry method requirements for continuation chaining
  - Add thenError method requirements for error handling
  - Add via method requirements for executor attachment
  - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [x] 8. Add concepts for global future factory and collection functions
  - Create future_factory concept for makeFuture and makeExceptionalFuture functions
  - Create future_collector concept for collectAll, collectAny, collectAnyWithoutException, and collectN
  - Add requirements for creating futures from values and exceptions
  - Add requirements for collection function signatures
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5, 7.6_

- [x] 9. Add concepts for future continuation operations
  - Create future_continuation concept for via, delay, and within methods
  - Add requirements for executor-aware continuation operations
  - Ensure proper executor attachment semantics
  - _Requirements: 8.1, 8.2, 8.3_

- [x] 9.1 Write property test for enhanced concept interface requirements
  - **Property 2: Enhanced Concept Interface Requirements**
  - **Validates: Requirements 2.1, 2.2, 2.3, 3.1, 3.2, 3.3, 4.1, 4.3, 5.1, 5.2, 5.3, 6.1, 6.2, 6.3, 6.4, 6.5, 9.1, 9.2, 9.3, 9.4**

- [x] 10. Validate enhanced concepts with actual Folly types
  - Add static assertions for folly::SemiPromise concept compliance
  - Add static assertions for folly::Promise concept compliance
  - Add static assertions for folly::Executor concept compliance
  - Add static assertions for folly::Future concept compliance
  - Add static assertions for kythira::Future concept compliance
  - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5_

- [x] 10.1 Write property test for Folly type concept compliance
  - **Property 3: Folly Type Concept Compliance**
  - **Validates: Requirements 10.1, 10.2, 10.3, 10.4, 10.5**

- [x] 11. Implement core type conversion utilities
  - Create exception conversion functions between folly::exception_wrapper and std::exception_ptr
  - Implement void/Unit type mapping utilities and template specializations
  - Add move semantics optimization helpers for type conversions
  - Create utility functions for safe type casting and validation
  - _Requirements: 18.1, 18.2, 18.5_

- [x] 11.1 Write property test for exception and type conversion
  - **Property 11: Exception and Type Conversion**
  - **Validates: Requirements 18.1, 18.2, 18.3, 18.5**

- [x] 12. Implement SemiPromise wrapper class
  - Create SemiPromise<T> class template that wraps folly::Promise<T>
  - Implement setValue() method with proper void/Unit handling
  - Implement setException() method with exception type conversion
  - Add isFulfilled() method for state checking
  - Ensure proper move semantics and resource management
  - _Requirements: 11.2, 11.3, 11.4_

- [x] 13. Implement Promise wrapper class
  - Create Promise<T> class template extending SemiPromise functionality
  - Implement getFuture() method returning wrapped Future<T>
  - Implement getSemiFuture() method for semi-future access
  - Ensure proper promise-future relationship and lifecycle management
  - Add constructors and proper resource cleanup
  - _Requirements: 11.1, 11.5_

- [x] 13.1 Write property test for wrapper concept compliance and promise handling
  - **Property 4: Wrapper Concept Compliance and Generic Compatibility**
  - **Property 5: Promise Value and Exception Handling**
  - **Validates: Requirements 11.1, 11.2, 11.3, 11.4, 11.5**

- [x] 14. Implement Executor wrapper class
  - Create Executor class that wraps folly::Executor pointer
  - Implement add() method for work submission with proper forwarding
  - Add constructor taking folly::Executor pointer
  - Ensure proper lifetime management and null pointer handling
  - Implement proper copy/move semantics
  - _Requirements: 12.1, 12.3_

- [x] 15. Implement KeepAlive wrapper class
  - Create KeepAlive class that wraps folly::Executor::KeepAlive
  - Implement get() method for pointer-like access
  - Ensure proper copy and move constructors with reference counting
  - Add destructor with proper cleanup
  - Implement assignment operators with proper semantics
  - _Requirements: 12.2, 12.4, 12.5_

- [x] 15.1 Write property test for executor work submission
  - **Property 6: Executor Work Submission**
  - **Validates: Requirements 12.3, 12.4, 12.5**

- [x] 16. Implement FutureFactory static class
  - Create FutureFactory class with static factory methods
  - Implement makeFuture() template method for value-based future creation
  - Implement makeExceptionalFuture() template method for exception-based futures
  - Implement makeReadyFuture() method for void/Unit futures
  - Add proper type deduction and conversion handling
  - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.5_

- [x] 16.1 Write property test for factory future creation
  - **Property 7: Factory Future Creation**
  - **Validates: Requirements 13.1, 13.2, 13.3, 13.4, 13.5**

- [x] 17. Implement FutureCollector static class
  - Create FutureCollector class with static collection methods
  - Implement collectAll() method for waiting on all futures
  - Implement collectAny() method for first-completed future
  - Implement collectAnyWithoutException() method for first successful future
  - Implement collectN() method for first N completed futures
  - Add proper timeout and cancellation handling
  - _Requirements: 14.1, 14.2, 14.3, 14.4, 14.5_

- [x] 17.1 Write property test for collection operations
  - **Property 8: Collection Operations**
  - **Validates: Requirements 14.1, 14.2, 14.3, 14.4, 14.5**

- [x] 18. Enhance existing Future class with continuation operations
  - Add via() method for executor-based continuation scheduling
  - Add delay() method for time-based future delays
  - Add within() method for timeout-based future constraints
  - Ensure proper integration with existing Future implementation
  - Maintain backward compatibility with current API
  - _Requirements: 15.1, 15.2, 15.3, 15.4, 15.5_

- [x] 18.1 Write property test for continuation operations
  - **Property 9: Continuation Operations**
  - **Validates: Requirements 15.1, 15.2, 15.3, 15.4, 15.5**

- [x] 19. Enhance existing Future class with transformation operations
  - Add ensure() method for cleanup functionality
  - Enhance existing thenValue() and thenError() methods if needed
  - Ensure proper void/Unit handling in all transformation operations
  - Add proper type deduction and error propagation
  - Maintain compatibility with existing transformation methods
  - _Requirements: 16.1, 16.2, 16.3, 16.4, 16.5_

- [x] 19.1 Write property test for transformation operations
  - **Property 10: Transformation Operations**
  - **Validates: Requirements 16.1, 16.2, 16.3, 16.4, 16.5**

- [x] 20. Add comprehensive concept compliance validation
  - Create static_assert statements for all wrapper classes and their concepts
  - Add compile-time validation for concept requirements
  - Create template test functions that use concept-constrained parameters
  - Ensure all wrappers work with existing concept-constrained code
  - Add validation for proper type deduction in generic contexts
  - _Requirements: 17.1, 17.2, 17.4_

- [x] 21. Implement backward compatibility and interoperability
  - Ensure new wrappers work seamlessly with existing Try and Future classes
  - Add conversion functions between wrapper types where appropriate
  - Test integration with existing codebase and APIs
  - Verify no breaking changes to existing functionality
  - Add interoperability utilities for mixed wrapper usage
  - _Requirements: 20.1, 20.2, 20.3, 20.4, 20.5_

- [x] 21.1 Write property test for backward compatibility and interoperability
  - **Property 12: Backward Compatibility and Interoperability**
  - **Validates: Requirements 20.1, 20.2, 20.3, 20.4, 20.5**

- [x] 22. Add comprehensive error handling and exception safety
  - Implement proper exception safety guarantees for all wrapper operations
  - Add error handling for edge cases and invalid usage scenarios
  - Ensure proper resource cleanup in exception scenarios
  - Add validation and error reporting for invalid wrapper states
  - Implement timeout and cancellation error handling
  - _Requirements: 18.3_

- [x] 23. Create comprehensive unit tests for wrapper functionality
  - Write unit tests for each wrapper class covering basic functionality
  - Test edge cases, error conditions, and boundary scenarios
  - Verify proper resource management and cleanup
  - Test integration between different wrapper types
  - Add performance validation for critical operations
  - _Requirements: 19.1, 19.2, 19.3, 19.4, 19.5_

- [x] 24. Create integration tests with existing codebase
  - Test new wrappers with existing Raft implementation
  - Verify compatibility with existing future-based operations
  - Test mixed usage of old and new wrapper types
  - Validate no regressions in existing functionality
  - Test concept-constrained template functions with new wrappers
  - _Requirements: 20.1, 20.2, 20.3, 20.4, 20.5_

- [x] 25. Update include/raft/future.hpp with new implementations
  - Integrate all new wrapper classes into the existing header file
  - Organize code with proper namespacing and documentation
  - Add comprehensive API documentation for new classes
  - Ensure proper header dependencies and include ordering
  - Add usage examples in comments for complex operations
  - _Requirements: All requirements_

- [x] 26. Add static assertions for concept compliance
  - Add static_assert statements at the end of the header file
  - Verify all wrapper types satisfy their corresponding concepts
  - Test concept compliance with various template instantiations
  - Add compile-time validation for type conversion utilities
  - Ensure proper error messages for concept violations
  - _Requirements: 17.1, 17.2_

- [x] 27. Create example programs demonstrating wrapper usage
  - Create examples/folly-wrappers/promise_example.cpp showing promise usage
  - Create examples/folly-wrappers/executor_example.cpp showing executor usage
  - Create examples/folly-wrappers/factory_example.cpp showing factory operations
  - Create examples/folly-wrappers/collector_example.cpp showing collection operations
  - Create examples/folly-wrappers/continuation_example.cpp showing continuation operations
  - Follow example program guidelines (run all scenarios, clear pass/fail, exit codes)
  - _Requirements: All requirements_

- [x] 28. Update CMakeLists.txt for wrapper examples
  - Add wrapper example executables to examples/CMakeLists.txt
  - Ensure proper linking with folly and other required libraries
  - Set appropriate output directories for example programs
  - Add CTest integration for example validation
  - Configure proper compiler flags and dependencies
  - _Requirements: All requirements_

- [x] 29. Create comprehensive documentation
  - Document all enhanced concepts and their usage patterns
  - Document all new wrapper classes and their usage patterns
  - Add migration guide for developers using folly directly
  - Document concept compliance and type conversion behavior
  - Add troubleshooting guide for common usage issues
  - Create API reference documentation for all public interfaces
  - _Requirements: All requirements_

- [x] 30. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 31. Convert codebase to use unified kythira::Future interface
  - Search for and replace all std::future usage with kythira::Future
  - Search for and replace all direct folly::Future usage with kythira::Future
  - Update header includes to use raft/future.hpp instead of <future> or <folly/futures/Future.h>
  - Ensure consistent future interface throughout codebase
  - _Requirements: 21.1, 21.2, 21.3, 21.4_

- [x] 31.1 Write property test for future usage consistency
  - **Property 13: Future Usage Consistency**
  - **Validates: Requirements 21.1, 21.2, 21.3**

- [x] 31.2 Write property test for header include consistency
  - **Property 14: Header Include Consistency**
  - **Validates: Requirements 21.4, 26.1**

- [x] 32. Generify transport layer interfaces with future concepts
  - Template HTTP transport methods on future types
  - Template CoAP transport methods on future types
  - Update network client concepts to use future concepts as template parameters
  - Move transport implementations to kythira namespace
  - _Requirements: 22.1, 22.2, 22.3, 22.4, 22.5_

- [x] 32.1 Write property test for transport method return types
  - **Property 15: Transport Method Return Types**
  - **Validates: Requirements 22.1, 22.2**

- [x] 32.2 Write property test for network concept compliance
  - **Property 16: Network Concept Compliance**
  - **Validates: Requirements 22.3, 22.4**

- [x] 33. Generify network simulator components with future concepts
  - Template Connection class on future types
  - Template Listener class on future types
  - Update network simulator operations to use templated future types
  - Move network simulator classes to kythira namespace
  - Add timeout support with templated future types
  - _Requirements: 23.1, 23.2, 23.3, 23.4, 23.5_

- [x] 33.1 Write property test for network simulator return types
  - **Property 17: Network Simulator Return Types**
  - **Validates: Requirements 23.1, 23.2, 23.3, 23.4**

- [x] 34. Convert test code to use kythira::Future consistently
  - Update property-based tests to use kythira::Future
  - Update integration tests to use kythira::Future
  - Update test fixtures to use kythira::Future
  - Ensure test synchronization uses kythira::Future methods
  - Update test validation to use kythira::Future
  - _Requirements: 24.1, 24.2, 24.3, 24.4, 24.5_

- [x] 34.1 Write property test for test code future usage
  - **Property 18: Test Code Future Usage**
  - **Validates: Requirements 24.1, 24.2, 24.3, 24.4, 24.5**

- [x] 35. Validate behavioral preservation during conversion
  - Test async operation timing and ordering behavior
  - Validate exception handling and error propagation
  - Test thread safety and synchronization behavior
  - Validate timeout handling behavior
  - Ensure all existing tests pass without modification
  - _Requirements: 25.1, 25.2, 25.3, 25.4, 25.5_

- [x] 35.1 Write property test for behavioral preservation
  - **Property 19: Behavioral Preservation**
  - **Validates: Requirements 25.1, 25.2, 25.3, 25.4**

- [x] 36. Maintain clean build dependencies
  - Ensure compilation works without direct std::future or folly::Future includes
  - Maintain same library dependencies
  - Validate build produces same executable functionality
  - Verify reduced direct dependencies on future implementations
  - _Requirements: 26.2, 26.3, 26.4, 26.5_

- [x] 36.1 Write property test for build dependency consistency
  - **Property 20: Build Dependency Consistency**
  - **Validates: Requirements 26.2, 26.3**

- [x] 37. Convert Promise/Future patterns to kythira::Future
  - Replace Promise/Future pairs with kythira::Future construction patterns
  - Replace makeFuture calls with kythira::Future constructors
  - Replace getFuture() calls with appropriate kythira::Future creation
  - Update promise fulfillment to use kythira::Future constructors
  - Update future chaining to use kythira::Future continuation methods
  - _Requirements: 27.1, 27.2, 27.3, 27.4, 27.5_

- [x] 37.1 Write property test for Promise/Future pattern conversion
  - **Property 21: Promise/Future Pattern Conversion**
  - **Validates: Requirements 27.1, 27.2, 27.3, 27.4, 27.5**

- [x] 38. Make core implementations generic with future concepts
  - Template core Raft implementations on future types
  - Use future concepts instead of concrete future types
  - Move include/future/future.hpp to include/raft/future.hpp
  - Maintain kythira namespace for future classes
  - Move core implementations to kythira namespace
  - _Requirements: 28.1, 28.2, 28.3, 28.4, 28.5, 28.6, 28.7, 28.8, 28.9, 28.10_

- [x] 38.1 Write property test for core implementation genericity
  - **Property 22: Core Implementation Genericity**
  - **Validates: Requirements 28.1, 28.2**

- [x] 38.2 Write property test for namespace organization
  - **Property 23: Namespace Organization**
  - **Validates: Requirements 28.5, 28.6, 28.7, 28.8, 28.9, 28.10**

- [x] 39. Validate complete conversion
  - Search codebase for remaining std::future usage
  - Search codebase for remaining direct folly::Future usage in public interfaces
  - Ensure successful compilation with no future-related errors
  - Validate all existing tests pass
  - Demonstrate equivalent performance characteristics
  - _Requirements: 29.1, 29.2, 29.3, 29.4, 29.5_

- [x] 39.1 Write property test for complete conversion validation
  - **Property 24: Complete Conversion Validation**
  - **Validates: Requirements 29.1, 29.2**

- [x] 40. Final comprehensive validation
  - Run complete test suite with all conversions
  - Validate all property-based tests pass
  - Ensure no regressions in functionality
  - Verify performance characteristics are maintained
  - Document the unified future architecture
  - _Requirements: All requirements_

- [x] 41. Final checkpoint - Complete validation
  - Ensure all tests pass, ask the user if questions arise.

## Phase 2: Complete Missing Wrapper Implementations

**Note**: Tasks 1-41 above created the concept definitions, test infrastructure, and placeholder tests. The following tasks implement the actual wrapper functionality documented in `tests/WRAPPER_UNIT_TEST_SUMMARY.md`.

- [x] 42. Implement kythira::SemiPromise<T> wrapper class
  - Implement setValue() method with proper void/Unit handling
  - Implement setException() method with exception type conversion
  - Implement isFulfilled() method for state checking
  - Ensure proper move semantics and resource management
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 11.2, 11.3, 11.4_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (SemiPromise tests)_

- [x] 43. Implement kythira::Promise<T> wrapper class
  - Extend SemiPromise functionality
  - Implement getFuture() method returning wrapped Future<T>
  - Implement getSemiFuture() method for semi-future access
  - Ensure proper promise-future relationship and lifecycle management
  - Convert 2 placeholder tests to actual implementation tests
  - _Requirements: 11.1, 11.5_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Promise tests)_

- [x] 44. Implement kythira::Executor wrapper class
  - Create Executor class that wraps folly::Executor pointer
  - Implement add() method for work submission with proper forwarding
  - Ensure proper lifetime management and null pointer handling
  - Implement proper copy/move semantics
  - Convert 2 placeholder tests to actual implementation tests
  - _Requirements: 12.1, 12.3_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Executor tests)_

- [x] 45. Implement kythira::KeepAlive wrapper class
  - Create KeepAlive class that wraps folly::Executor::KeepAlive
  - Implement get() method for pointer-like access
  - Ensure proper copy and move constructors with reference counting
  - Implement assignment operators with proper semantics
  - Convert 2 placeholder tests to actual implementation tests
  - _Requirements: 12.2, 12.4, 12.5_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (KeepAlive tests)_

- [x] 46. Implement kythira::FutureFactory static class
  - Create FutureFactory class with static factory methods
  - Implement makeFuture() template method for value-based future creation
  - Implement makeExceptionalFuture() template method for exception-based futures
  - Implement makeReadyFuture() method for void/Unit futures
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.5_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (FutureFactory tests)_

- [x] 47. Implement kythira::FutureCollector static class
  - Create FutureCollector class with static collection methods
  - Implement collectAll() method for waiting on all futures
  - Implement collectAny() method for first-completed future
  - Implement collectN() method for first N completed futures
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 14.1, 14.2, 14.3, 14.4_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (FutureCollector tests)_

- [x] 48. Add missing Future continuation methods
  - Implement via() method for executor-based continuation scheduling
  - Implement delay() method for time-based future delays
  - Implement within() method for timeout-based future constraints
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 15.1, 15.2, 15.3_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Continuation tests)_

- [x] 49. Add missing Future transformation methods
  - Rename then() to thenValue() for concept compliance
  - Rename onError() to thenError() for concept compliance
  - Implement ensure() method for cleanup functionality
  - Maintain backward compatibility with old method names
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 16.1, 16.2, 16.3_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Transformation tests)_

- [x] 50. Implement interoperability utilities
  - Implement exception wrapper conversion (folly::exception_wrapper ↔ std::exception_ptr)
  - Implement void/Unit conversion utilities
  - Implement move semantics optimization helpers
  - Implement folly::Future ↔ kythira::Future conversion utilities
  - Implement folly::Try ↔ kythira::Try conversion utilities
  - Convert 16 placeholder tests to actual implementation tests
  - _Requirements: 18.1, 18.2, 18.5_
  - _Reference: tests/wrapper_interop_utilities_unit_test.cpp (all tests)_

- [x] 51. Add integration tests for wrapper interactions
  - Test Promise-Future integration
  - Test Executor-Future integration
  - Test Factory-Collector integration
  - Convert 3 placeholder tests to actual implementation tests
  - _Requirements: 20.1, 20.2, 20.3_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Integration tests)_

- [x] 52. Add performance validation tests
  - Validate wrapper overhead is minimal
  - Validate memory usage is acceptable
  - Convert 2 placeholder tests to actual implementation tests
  - _Requirements: 19.5_
  - _Reference: tests/missing_wrapper_functionality_unit_test.cpp (Performance tests)_

- [x] 53. Update include/raft/future.hpp with all new implementations
  - Integrate all new wrapper classes (SemiPromise, Promise, Executor, KeepAlive)
  - Integrate factory and collector classes
  - Add all new continuation and transformation methods
  - Add all interoperability utilities
  - Update API documentation
  - _Requirements: All requirements_

- [x] 54. Verify all 42 placeholder tests now pass with real implementations
  - Run missing_wrapper_functionality_unit_test.cpp and verify all tests pass
  - Run wrapper_interop_utilities_unit_test.cpp and verify all tests pass
  - Ensure no placeholder tests remain
  - _Requirements: 19.1, 19.2, 19.3, 19.4_

- [x] 55. Final validation checkpoint
  - Ensure all 87 unit tests pass (45 existing + 42 newly implemented)
  - Verify concept compliance for all wrapper classes
  - Validate performance characteristics
  - Confirm backward compatibility maintained
  - _Requirements: All requirements_

## Phase 3: Async Retry Support

**Note**: These tasks add support for Future-returning callbacks in `thenTry` and `thenError` to enable non-blocking async retry patterns.

- [x] 56. Add thenTry overload for Future-returning callbacks
  - Add template overload that detects Future-returning callbacks using SFINAE/concepts
  - Implement automatic future flattening by extracting folly::Future from wrapper
  - Handle both void and non-void future types correctly
  - Ensure proper error propagation through async chains
  - Maintain compatibility with existing thenTry overload
  - _Requirements: 30.1, 30.2, 30.3, 30.4, 30.5_

- [x] 56.1 Write property test for thenTry with Future-returning callbacks
  - **Property 25: Future-Returning Callback Support in thenTry**
  - **Validates: Requirements 30.1, 30.2, 30.3, 30.4, 30.5**

- [x] 57. Add thenError overload for Future-returning callbacks
  - Add template overload that detects Future-returning callbacks using SFINAE/concepts
  - Implement automatic future flattening by extracting folly::Future from wrapper
  - Handle both void and non-void future types correctly
  - Ensure proper error recovery semantics
  - Maintain compatibility with existing thenError overload
  - _Requirements: 31.1, 31.2, 31.3, 31.4, 31.5_

- [x] 57.1 Write property test for thenError with Future-returning callbacks
  - **Property 26: Future-Returning Callback Support in thenError**
  - **Validates: Requirements 31.1, 31.2, 31.3, 31.4, 31.5**

- [x] 58. Add void specialization support for Future-returning callbacks
  - Ensure thenTry overload works with Future<void> callbacks
  - Ensure thenError overload works with Future<void> callbacks
  - Handle Unit/void conversions correctly in async chains
  - Test with both void and non-void future types
  - _Requirements: 30.5, 31.5_

- [x] 59. Refactor ErrorHandler to use async retry pattern
  - Replace std::this_thread::sleep_for with Future.delay()
  - Use thenTry with Future-returning callback for retry logic
  - Ensure no threads are blocked during retry delays
  - Maintain proper exception propagation through async chains
  - Update error handler tests to validate async behavior
  - _Requirements: 32.1, 32.2, 32.3, 32.4, 32.5_

- [x] 59.1 Write property test for async retry without blocking
  - **Property 27: Async Retry Without Blocking**
  - **Validates: Requirements 32.1, 32.2, 32.3, 32.4, 32.5**

- [x] 60. Update error handler unit tests for async behavior
  - Modify existing unit tests to work with async delays
  - Verify delays are still applied correctly
  - Ensure exponential backoff calculation remains correct
  - Validate jitter is still applied properly
  - Confirm no threads are blocked during delays
  - _Requirements: 32.1, 32.2, 32.3_

- [x] 61. Validate async retry with property tests
  - Run raft_heartbeat_retry_backoff_property_test with async implementation
  - Run raft_append_entries_retry_handling_property_test with async implementation
  - Run raft_snapshot_transfer_retry_property_test with async implementation
  - Run raft_vote_request_failure_handling_property_test with async implementation
  - Ensure all property tests pass with async retry logic
  - _Requirements: 32.1, 32.2, 32.3, 32.4, 32.5_

- [x] 62. Add documentation for async retry patterns
  - Document the new thenTry and thenError overloads
  - Provide examples of async retry patterns
  - Explain benefits of async vs blocking approach
  - Add migration guide for converting blocking retry to async
  - Document performance characteristics
  - _Requirements: 30.1, 31.1, 32.1_

- [x] 63. Final async retry validation
  - Ensure all tests pass with async retry implementation
  - Verify no threads are blocked during retry delays
  - Validate performance improvements over blocking approach
  - Confirm backward compatibility maintained
  - Document the async retry architecture
  - _Requirements: 30.1, 30.2, 30.3, 30.4, 30.5, 31.1, 31.2, 31.3, 31.4, 31.5, 32.1, 32.2, 32.3, 32.4, 32.5_