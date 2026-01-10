# Network Concept Usage Issues Inventory

## Summary
This document provides a comprehensive inventory of network concept usage issues found throughout the codebase. The network concepts (`network_client` and `network_server`) are correctly defined in the `kythira` namespace with two template parameters, but many usages are inconsistent.

## Issue Categories

### 1. Missing Template Parameters (Single Parameter Usage)
Files where network concepts are used with only one template parameter instead of two:

#### Core Header Files:
- **include/raft/raft.hpp** - Multiple `requires` clauses missing FutureType parameter:
  - Lines 429, 454, 480, 506, 532, 557, 587, 664, 712, 778, 834, 1818, 2005, 2339, 2612, 2641, 2670, 2695, 2722, 2749, 2824, 3111
  - Pattern: `network_client<NetworkClient>` instead of `network_client<NetworkClient, FutureType>`
  - Pattern: `network_server<NetworkServer>` instead of `network_server<NetworkServer, FutureType>`

#### Test Files:
- **tests/http_client_test.cpp** - Line 27:
  - `static_assert(kythira::network_client<client_type>)` missing FutureType parameter

- **tests/coap_concept_conformance_test.cpp** - Line 51:
  - `static_assert(kythira::network_server<test_server>)` missing FutureType parameter

- **tests/raft_follower_acknowledgment_tracking_property_test.cpp** - Lines 50-51:
  - `using NetworkClient = raft::simulator_network_client<NodeId>` missing FutureType parameter
  - `using NetworkServer = raft::simulator_network_server<NodeId>` missing FutureType parameter

### 2. Wrong Namespace Usage
Files using `raft::` namespace instead of `kythira::`:

#### Core Header Files:
- **include/raft/raft.hpp** - Multiple locations:
  - Lines 45, 362, 930, 1104, 1306, 1517
  - Pattern: `raft::network_server<NetworkServer>` instead of `kythira::network_server<NetworkServer, FutureType>`

#### Test Files:
- **tests/http_server_test.cpp** - Line 21:
  - `static_assert(raft::network_server<server_type>)` should use `kythira::` namespace

### 3. Missing Namespace Prefix
Files using network concepts without any namespace prefix:

#### Core Header Files:
- **include/raft/raft.hpp** - Multiple `requires` clauses:
  - All the same lines mentioned in category 1 above
  - Pattern: `network_client<NetworkClient>` instead of `kythira::network_client<NetworkClient, FutureType>`

#### Transport Implementation Headers:
- **include/raft/coap_transport.hpp** - Lines 630, 634:
  - `static_assert(network_client<...>)` missing `kythira::` prefix
  - `static_assert(network_server<...>)` missing `kythira::` prefix

### 4. Correct Usage Examples
Files that already use the correct pattern (for reference):

#### Test Files:
- **tests/coap_concept_conformance_test.cpp** - Line 41:
  - `static_assert(kythira::network_client<test_client, future_type>)` ✓ Correct

- **tests/generic_future_concept_validation_test.cpp** - Lines 146, 152:
  - `static_assert(kythira::network_client<...>, TestFutureType>)` ✓ Correct

- **include/raft/simulator_network.hpp** - Lines 410, 414:
  - `static_assert(kythira::network_client<..., SimulatorFutureType>)` ✓ Correct
  - `static_assert(kythira::network_server<..., SimulatorFutureType>)` ✓ Correct

## Files Requiring Fixes

### High Priority (Core Implementation):
1. **include/raft/raft.hpp** - 20+ concept constraint issues
2. **include/raft/coap_transport.hpp** - 2 static assertion issues
3. **include/raft/http_transport.hpp** - No static assertions found (may need to add them)

### Medium Priority (Test Files):
1. **tests/http_client_test.cpp** - 1 static assertion issue
2. **tests/http_server_test.cpp** - 1 static assertion issue  
3. **tests/coap_concept_conformance_test.cpp** - 1 static assertion issue
4. **tests/raft_follower_acknowledgment_tracking_property_test.cpp** - 2 type alias issues

### Low Priority (Integration Tests):
1. **tests/raft_leader_election_integration_test.cpp** - Multiple instantiation issues
2. **tests/raft_log_replication_integration_test.cpp** - Multiple instantiation issues
3. **tests/raft_heartbeat_test.cpp** - Multiple instantiation issues

### Low Priority (Example Files):
1. **examples/raft/basic_cluster.cpp** - Multiple instantiation issues
2. **examples/raft/failure_scenarios.cpp** - Multiple instantiation issues
3. **examples/raft/membership_changes.cpp** - Multiple instantiation issues

## Fix Strategy

### Phase 1: Core Headers
- Fix `include/raft/raft.hpp` concept constraints
- Fix `include/raft/coap_transport.hpp` static assertions
- Add static assertions to `include/raft/http_transport.hpp`

### Phase 2: Test Files
- Fix unit test static assertions
- Fix type aliases in property tests
- Fix integration test instantiations

### Phase 3: Examples
- Fix example program instantiations
- Ensure examples demonstrate correct usage

### Phase 4: Validation
- Compile entire codebase
- Run test suites
- Verify all concept usages are consistent

## Template Parameter Patterns

### Current Incorrect Patterns:
```cpp
// Missing future type parameter
static_assert(kythira::network_client<ClientType>);
static_assert(raft::network_server<ServerType>);

// Wrong namespace
requires network_client<Client> && network_server<Server>
```

### Correct Patterns:
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

## Estimated Impact
- **Total files requiring fixes**: ~15 files
- **Total concept usage issues**: ~50+ individual fixes needed
- **Risk level**: Medium (compilation errors until fixed)
- **Testing impact**: High (many tests will fail to compile until fixed)