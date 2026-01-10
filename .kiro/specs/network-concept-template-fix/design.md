# Design Document

## Overview

This design addresses the systematic fix of template parameter mismatches in network client and server concept usages throughout the codebase. The network concepts are correctly defined in the `kythira` namespace with two template parameters, but many usages are inconsistent, causing compilation errors.

## Architecture

The fix involves a systematic update of all network concept usages to ensure:

1. **Consistent Template Parameters**: All network concept usages must provide exactly two template parameters
2. **Correct Namespace**: All references must use the `kythira` namespace
3. **Proper Future Type Inference**: The future type parameter must be correctly inferred or explicitly provided
4. **Backward Compatibility**: Changes should not break existing functionality

## Components and Interfaces

### Network Concept Definitions (Current - Correct)

The concepts are correctly defined in `include/raft/network.hpp`:

```cpp
namespace kythira {
    template<typename C, typename FutureType>
    concept network_client = requires(/* ... */);
    
    template<typename S, typename FutureType>
    concept network_server = requires(/* ... */);
}
```

### Problem Categories

1. **Single Parameter Usage**: `network_client<ClientType>` instead of `network_client<ClientType, FutureType>`
2. **Wrong Namespace**: `raft::network_server<ServerType>` instead of `kythira::network_server<ServerType, FutureType>`
3. **Mixed Usage**: Some places correct, others incorrect within the same file

## Data Models

### Template Parameter Patterns

**Correct Patterns:**
```cpp
// Static assertions
static_assert(kythira::network_client<ClientType, FutureType>);
static_assert(kythira::network_server<ServerType, FutureType>);

// Concept constraints
template<typename Client, typename Future>
requires kythira::network_client<Client, Future>
auto function(Client client) -> void;

// Type aliases
using client_type = SomeClient<FutureType, Serializer>;
static_assert(kythira::network_client<client_type, FutureType>);
```

**Incorrect Patterns (to be fixed):**
```cpp
// Missing future type parameter
static_assert(kythira::network_client<ClientType>);
static_assert(raft::network_server<ServerType>);

// Wrong namespace
requires network_client<Client> && network_server<Server>
```

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Network concept template parameter consistency
*For any* usage of network_client or network_server concepts throughout the codebase, exactly two template parameters should be provided: the implementation type and the future type
**Validates: Requirements 1.1, 1.2, 2.1, 2.2, 4.1, 4.2**

### Property 2: Namespace consistency
*For any* reference to network_client or network_server concepts, the kythira namespace prefix should be used
**Validates: Requirements 1.3, 2.5, 3.3, 4.3**

### Property 3: Static assertion correctness
*For any* static_assert statement using network concepts, both required template parameters should be provided and the assertion should compile successfully
**Validates: Requirements 1.4, 2.3, 3.2**

### Property 4: Concept constraint correctness
*For any* requires clause or concept constraint using network concepts, the correct template parameter count and namespace should be used
**Validates: Requirements 1.5, 3.1, 3.4**

### Property 5: Future type parameter consistency
*For any* network concept usage, the future type parameter should be consistent with the actual future type used by the implementation
**Validates: Requirements 2.4, 3.5, 4.5**

## Error Handling

### Compilation Error Categories

1. **Template Parameter Count Mismatch**: Concepts expect 2 parameters but receive 1
2. **Namespace Resolution Errors**: Concepts not found in wrong namespace
3. **Type Deduction Failures**: Future type cannot be properly inferred

### Error Resolution Strategy

1. **Systematic Search and Replace**: Use pattern matching to find all incorrect usages
2. **Future Type Inference**: Determine the correct future type for each usage context
3. **Validation**: Ensure all changes compile and tests pass
4. **Incremental Fixes**: Fix files in logical groups to minimize disruption

## Testing Strategy

### Unit Testing Approach

- **Compilation Tests**: Verify that all concept usages compile correctly
- **Static Assertion Tests**: Ensure static_assert statements pass with correct parameters
- **Type Constraint Tests**: Verify that template constraints work as expected

### Property-Based Testing Approach

The property-based testing will use **Boost.Test** as the testing framework, configured to run a minimum of 100 iterations per property test.

Each property-based test will be tagged with a comment explicitly referencing the correctness property in the design document using this format: **Feature: network-concept-template-fix, Property {number}: {property_text}**

Property-based tests will focus on:

1. **Template Parameter Validation**: Generate various client/server types and verify concept satisfaction
2. **Namespace Resolution**: Test that concepts are properly resolved in the kythira namespace
3. **Future Type Compatibility**: Verify that different future types work correctly with the concepts

### Integration Testing

- **Build System Integration**: Ensure all files compile after changes
- **Example Program Validation**: Verify that example programs still work correctly
- **Test Suite Execution**: Run existing test suites to ensure no regressions

## Implementation Plan

### Phase 1: Analysis and Inventory
1. Identify all files with network concept usage issues
2. Categorize the types of fixes needed
3. Determine the correct future types for each context

### Phase 2: Core Header Files
1. Fix concept usages in `include/raft/raft.hpp`
2. Fix concept usages in transport implementation headers
3. Update static assertions in header files

### Phase 3: Test Files
1. Fix concept usages in unit test files
2. Fix concept usages in property test files
3. Fix concept usages in integration test files

### Phase 4: Example Files
1. Fix concept usages in example programs
2. Ensure examples demonstrate correct API usage
3. Update example documentation if needed

### Phase 5: Validation
1. Compile entire codebase to verify fixes
2. Run test suites to ensure no regressions
3. Validate that all concept usages are consistent