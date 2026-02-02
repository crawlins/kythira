# Test Organization Plan

## Overview

This document describes the hierarchical organization of test source files into grouped test executables. Instead of having 200+ individual test executables, tests are organized into 19 logical groups.

## Benefits

1. **Faster Build Times**: Fewer executables means less linking overhead
2. **Logical Organization**: Related tests are grouped together
3. **Easier Maintenance**: Clear structure makes it easier to find and update tests
4. **Better CI/CD**: Can run test categories in parallel
5. **Reduced Disk Usage**: Fewer executables consume less disk space

## Test Executable Groups

### 1. `concepts_test` (60+ test files)
**Purpose**: All C++20/23 concept compliance and validation tests

**Categories**:
- Core concept definitions and requirements
- Folly concept compliance (Promise, Future, Executor, etc.)
- Kythira concept compliance
- Future concept tests and wrappers
- Network, logger, metrics, and persistence concepts

**Timeout**: 120 seconds  
**Labels**: `concepts`, `unit`

**Key Files**:
- `concept_test.cpp`
- `folly_*_concept_*.cpp`
- `kythira_*_concept_*.cpp`
- `future_*_concept_*.cpp`

---

### 2. `network_simulator_test` (30+ test files)
**Purpose**: Network simulator core functionality and integration

**Categories**:
- Core simulator operations
- Topology management
- Lifecycle control
- Node send/receive operations
- Connection pooling and management
- Listener management
- Integration scenarios

**Timeout**: 180 seconds  
**Labels**: `network`, `simulator`, `unit`, `integration`

**Key Files**:
- `simulator_test.cpp`
- `network_simulator_*.cpp`
- `network_node_*.cpp`

---

### 3. `raft_core_test` (25+ test files)
**Purpose**: Core Raft consensus algorithm properties

**Categories**:
- Basic types and concepts
- Election safety
- Log matching and convergence
- Leader properties
- State machine safety
- Linearizability
- Snapshot preservation
- RPC handlers
- Persistence

**Timeout**: 300 seconds  
**Labels**: `raft`, `core`, `unit`, `property`

**Key Files**:
- `raft_types_test.cpp`
- `raft_election_safety_*.cpp`
- `raft_log_*.cpp`
- `raft_*_handler_*.cpp`

---

### 4. `raft_replication_test` (12 test files)
**Purpose**: Raft log replication mechanisms

**Categories**:
- Follower acknowledgment tracking
- Majority commit advancement
- Slow/unresponsive follower handling
- Replication operations
- Retry mechanisms
- Partition handling

**Timeout**: 300 seconds  
**Labels**: `raft`, `replication`, `property`

**Key Files**:
- `raft_follower_*.cpp`
- `raft_replication_*.cpp`
- `raft_*_retry_*.cpp`

---

### 5. `raft_commit_test` (6 test files)
**Purpose**: Commit waiting and state machine application

**Categories**:
- Commit waiting mechanism
- Command submission with timeout
- State machine application (success/failure)
- Applied index catchup
- Batch entry application

**Timeout**: 300 seconds  
**Labels**: `raft`, `commit`, `state_machine`, `property`

**Key Files**:
- `raft_commit_waiting_*.cpp`
- `raft_application_*.cpp`
- `raft_submit_command_*.cpp`

---

### 6. `raft_future_collection_test` (7 test files)
**Purpose**: Future collection mechanisms for distributed operations

**Categories**:
- Heartbeat majority collection
- Election vote collection
- Timeout handling in collections
- Cancellation and cleanup

**Timeout**: 300 seconds  
**Labels**: `raft`, `future`, `collection`, `property`

**Key Files**:
- `raft_*_collection_*.cpp`
- `raft_timeout_handling_*.cpp`

---

### 7. `raft_error_handling_test` (10 test files)
**Purpose**: Error handling, retry logic, and logging

**Categories**:
- Exponential backoff
- Async retry mechanisms
- Timeout classification
- Error logging (RPC, commit, configuration, etc.)

**Timeout**: 300 seconds  
**Labels**: `raft`, `error`, `retry`, `property`

**Key Files**:
- `error_handler_*.cpp`
- `raft_*_retry_*.cpp`
- `raft_*_logging_*.cpp`

---

### 8. `raft_linearizable_reads_test` (6 test files)
**Purpose**: Linearizable read operations

**Categories**:
- Read linearizability verification
- Successful read state return
- Failed read rejection
- Read abortion on leadership loss
- Concurrent read efficiency
- Heartbeat-based read state

**Timeout**: 300 seconds  
**Labels**: `raft`, `reads`, `linearizable`, `property`

**Key Files**:
- `raft_read_*.cpp`
- `raft_heartbeat_based_read_state_*.cpp`

---

### 9. `raft_configuration_test` (12 test files)
**Purpose**: Configuration, membership changes, and lifecycle

**Categories**:
- RPC timeout configuration
- Retry policy configuration
- Heartbeat interval compatibility
- Adaptive timeout behavior
- Membership changes (add/remove server)
- Shutdown and cleanup
- Resource leak prevention

**Timeout**: 300 seconds  
**Labels**: `raft`, `configuration`, `lifecycle`, `property`

**Key Files**:
- `raft_*_configuration_*.cpp`
- `raft_add_server_*.cpp`
- `raft_remove_server_*.cpp`
- `raft_shutdown_*.cpp`

---

### 10. `raft_integration_test` (11 test files)
**Purpose**: End-to-end Raft integration scenarios

**Categories**:
- Leader election
- Log replication
- Commit waiting
- Future collection
- Configuration changes
- Error handling
- State machine synchronization
- RPC handlers
- Snapshot operations
- Client operations
- Cluster management

**Timeout**: 600 seconds  
**Labels**: `raft`, `integration`

**Key Files**:
- `raft_*_integration_test.cpp`

---

### 11. `http_transport_test` (20+ test files)
**Purpose**: HTTP transport layer implementation

**Categories**:
- Basic HTTP operations (client/server)
- HTTP exceptions and configuration
- Property tests
- Integration tests
- HTTPLib validation
- SSL/TLS configuration
- Certificate loading
- Mutual TLS

**Timeout**: 180 seconds  
**Labels**: `http`, `transport`, `unit`, `property`, `integration`

**Key Files**:
- `http_*.cpp`
- `httplib_*.cpp`

---

### 12. `coap_transport_test` (25+ test files)
**Purpose**: CoAP transport layer core functionality

**Categories**:
- Basic CoAP operations
- Message serialization and handling
- Connection and resource management
- Memory pool management
- Concurrent processing
- Thread safety
- Event logging
- Error handling

**Timeout**: 300 seconds  
**Labels**: `coap`, `transport`, `unit`, `property`

**Key Files**:
- `coap_config_test.cpp`
- `coap_message_*.cpp`
- `coap_connection_*.cpp`
- `memory_pool_*.cpp`

---

### 13. `coap_dtls_test` (5 test files)
**Purpose**: CoAP DTLS security features

**Categories**:
- DTLS connection establishment
- Certificate validation
- Handshake procedures
- Cipher suite configuration

**Timeout**: 300 seconds  
**Labels**: `coap`, `dtls`, `security`, `property`

**Key Files**:
- `coap_dtls_*.cpp`
- `coap_certificate_*.cpp`
- `coap_cipher_*.cpp`

---

### 14. `coap_multicast_test` (5 test files)
**Purpose**: CoAP multicast operations

**Categories**:
- Multicast delivery
- Discovery
- Group communication
- Response aggregation

**Timeout**: 300 seconds  
**Labels**: `coap`, `multicast`, `property`

**Key Files**:
- `coap_multicast_*.cpp`

---

### 15. `coap_block_transfer_test` (2 test files)
**Purpose**: CoAP block-wise transfer

**Categories**:
- Real block transfer
- Enhanced block transfer

**Timeout**: 300 seconds  
**Labels**: `coap`, `block_transfer`, `property`

**Key Files**:
- `coap_*_block_transfer_*.cpp`

---

### 16. `coap_integration_test` (5 test files)
**Purpose**: CoAP integration and performance testing

**Categories**:
- Basic integration
- libcoap integration
- Final integration validation
- Performance benchmarks
- Production readiness

**Timeout**: 900 seconds  
**Labels**: `coap`, `integration`, `performance`

**Key Files**:
- `coap_integration_test.cpp`
- `coap_libcoap_integration_test.cpp`
- `coap_final_integration_test.cpp`
- `coap_performance_*.cpp`
- `coap_production_readiness_test.cpp`

---

### 17. `rpc_serialization_test` (3 test files)
**Purpose**: RPC serialization and deserialization

**Categories**:
- Serialization property tests
- Serializer concept compliance
- Malformed message handling

**Timeout**: 120 seconds  
**Labels**: `rpc`, `serialization`, `unit`, `property`

**Key Files**:
- `rpc_serialization_*.cpp`
- `rpc_serializer_*.cpp`

---

### 18. `code_quality_test` (12 test files)
**Purpose**: Code quality, consistency, and performance validation

**Categories**:
- Implementation genericity
- Namespace consistency
- Header include consistency
- Future usage consistency
- Type conversion validation
- Build success validation
- Behavioral preservation
- Performance benchmarks

**Timeout**: 120 seconds  
**Labels**: `quality`, `consistency`, `unit`

**Key Files**:
- `*_consistency_*.cpp`
- `*_validation_*.cpp`
- `performance_*.cpp`

---

### 19. `debug_accept_test` (standalone)
**Purpose**: Manual debugging tool (not registered with CTest)

**Note**: This is built but not added to CTest as it's for manual debugging only.

---

## Migration Guide

### Step 1: Backup Current CMakeLists.txt

```bash
cd tests
cp CMakeLists.txt CMakeLists.txt.backup
```

### Step 2: Review the New Structure

```bash
# Review the new organization
cat CMakeLists_new.txt
```

### Step 3: Test the New Build

```bash
# Replace the old CMakeLists.txt
cd tests
mv CMakeLists.txt CMakeLists.txt.old
mv CMakeLists_new.txt CMakeLists.txt

# Rebuild
cd ..
cmake --build build --clean-first
```

### Step 4: Run Tests

```bash
cd build

# Run all tests
ctest

# Run specific test group
ctest -R concepts_test --verbose
ctest -R raft_core_test --verbose
ctest -R http_transport_test --verbose

# Run tests by label
ctest -L raft
ctest -L coap
ctest -L integration

# Run tests in parallel
ctest -j$(nproc)
```

### Step 5: Verify Results

Compare test results with the old structure:

```bash
# Old structure (if you kept it)
cd build_old
ctest --output-on-failure > old_results.txt

# New structure
cd build
ctest --output-on-failure > new_results.txt

# Compare
diff old_results.txt new_results.txt
```

## Running Specific Test Categories

### By Test Executable

```bash
# Run all concepts tests
ctest -R concepts_test --verbose

# Run all Raft tests
ctest -R "raft_.*_test" --verbose

# Run all CoAP tests
ctest -R "coap_.*_test" --verbose

# Run all HTTP tests
ctest -R http_transport_test --verbose
```

### By Label

```bash
# Run all Raft-related tests
ctest -L raft

# Run all integration tests
ctest -L integration

# Run all property tests
ctest -L property

# Run all unit tests
ctest -L unit

# Run all CoAP tests
ctest -L coap
```

### Parallel Execution

```bash
# Run all tests in parallel
ctest -j$(nproc)

# Run specific category in parallel
ctest -L raft -j4
```

## CI/CD Integration

### Example GitHub Actions Workflow

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        test_group:
          - concepts_test
          - network_simulator_test
          - raft_core_test
          - raft_integration_test
          - http_transport_test
          - coap_transport_test
          - coap_integration_test
    
    steps:
      - uses: actions/checkout@v2
      
      - name: Build
        run: |
          cmake -B build
          cmake --build build
      
      - name: Run ${{ matrix.test_group }}
        run: |
          cd build
          ctest -R ${{ matrix.test_group }} --output-on-failure
```

### Example GitLab CI

```yaml
test:concepts:
  script:
    - cmake -B build
    - cmake --build build
    - cd build && ctest -R concepts_test --output-on-failure

test:raft:
  script:
    - cmake -B build
    - cmake --build build
    - cd build && ctest -R "raft_.*_test" --output-on-failure

test:coap:
  script:
    - cmake -B build
    - cmake --build build
    - cd build && ctest -R "coap_.*_test" --output-on-failure
```

## Troubleshooting

### Build Failures

If you encounter build failures:

1. **Check for missing dependencies**:
   ```bash
   cmake -B build -DCMAKE_VERBOSE_MAKEFILE=ON
   ```

2. **Verify all source files exist**:
   ```bash
   cd tests
   for file in *.cpp; do
       if [ ! -f "$file" ]; then
           echo "Missing: $file"
       fi
   done
   ```

3. **Check for duplicate symbols**:
   - Ensure test files don't define conflicting symbols
   - Each test file should have unique test case names

### Test Failures

If tests fail after migration:

1. **Run tests individually**:
   ```bash
   cd build
   ./tests/concepts_test --log_level=all
   ```

2. **Compare with old results**:
   - Check if failures existed in old structure
   - Verify test logic wasn't affected by grouping

3. **Check for initialization order issues**:
   - Some tests may have implicit dependencies
   - Review global fixtures and initialization

## Rollback Plan

If you need to rollback:

```bash
cd tests
mv CMakeLists.txt CMakeLists_new.txt.failed
mv CMakeLists.txt.backup CMakeLists.txt

cd ..
cmake --build build --clean-first
```

## Future Improvements

1. **Add more granular labels** for better filtering
2. **Create test suites** for common scenarios
3. **Add performance tracking** for test execution times
4. **Implement test sharding** for even faster CI/CD
5. **Add test coverage reporting** per test group

## Questions?

For questions or issues with the new test organization, please:

1. Check this document first
2. Review the CMakeLists.txt comments
3. Run tests with `--verbose` flag for detailed output
4. Compare with the backup CMakeLists.txt.old

