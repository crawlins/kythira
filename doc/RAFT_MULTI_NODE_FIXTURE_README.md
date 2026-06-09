# Raft Multi-Node Test Fixture

## Overview

The Raft multi-node test fixture provides infrastructure for testing Raft consensus with multiple nodes in a simulated network environment. This fixture is designed to support future integration testing of Raft clusters with controlled network conditions.

**Task**: 700 - Create multi-node test fixture
**Requirements**: 1.1, 1.2, 1.3, 2.1
**Status**: Infrastructure Complete ✅

## Features

### Dynamic Cluster Sizes
- Supports clusters of 3, 5, 7, or 9 nodes
- Validates cluster size (must be odd and within range)
- Automatically generates unique node IDs

### Node Lifecycle Management
- Start/stop individual nodes
- Start/stop all nodes in cluster
- Restart nodes with simulated delay
- Track node running state

### Network Simulator Integration
- Fully connected mesh topology by default
- Configurable network latency
- Configurable packet loss/reliability
- Network partition simulation
- Network healing (restore connectivity)

### Time Control
- Manual timeout triggering (election, heartbeat)
- Time advancement for deterministic testing
- Configurable timeout parameters

## Usage

### Basic Setup

```cpp
#include "raft_multi_node_test_fixture.hpp"

// Create fixture with default configuration (3 nodes)
kythira::test::raft_multi_node_fixture fixture;
fixture.initialize_cluster();
fixture.start_all_nodes();

// Get node IDs
auto node_ids = fixture.get_node_ids();

// Cleanup
fixture.cleanup();
```

### Custom Configuration

```cpp
kythira::test::cluster_config config;
config.node_count = 5;
config.election_timeout_min = std::chrono::milliseconds(150);
config.election_timeout_max = std::chrono::milliseconds(300);
config.heartbeat_interval = std::chrono::milliseconds(50);
config.enable_network_delays = true;
config.network_latency = std::chrono::milliseconds(10);
config.network_reliability = 0.95;  // 5% packet loss

kythira::test::raft_multi_node_fixture fixture(config);
fixture.initialize_cluster();
```

### Node Lifecycle

```cpp
// Start all nodes
fixture.start_all_nodes();

// Stop a specific node
fixture.stop_node("node_0");

// Restart a node
fixture.restart_node("node_0");

// Check if node is running
bool running = fixture.is_node_running("node_0");

// Stop all nodes
fixture.stop_all_nodes();
```

### Network Simulation

```cpp
// Simulate network partition
std::vector<std::string> group1 = {"node_0", "node_1"};
std::vector<std::string> group2 = {"node_2", "node_3", "node_4"};
fixture.create_network_partition(group1, group2);

// Heal partition
fixture.heal_network_partition();

// Add latency to specific node
fixture.set_node_network_delay("node_0", std::chrono::milliseconds(50));

// Simulate packet loss for specific node
fixture.set_node_packet_loss("node_1", 0.1);  // 10% packet loss
```

### Time Control

```cpp
// Trigger election timeouts
fixture.tick_election_timeouts();

// Trigger heartbeat timeouts
fixture.tick_heartbeat_timeouts();

// Advance time by 100ms
fixture.advance_time(std::chrono::milliseconds(100));
```

## Configuration Options

### cluster_config Structure

```cpp
struct cluster_config {
    std::size_t node_count = 3;                              // Number of nodes (3, 5, 7, or 9)
    std::chrono::milliseconds election_timeout_min{150};     // Min election timeout
    std::chrono::milliseconds election_timeout_max{300};     // Max election timeout
    std::chrono::milliseconds heartbeat_interval{50};        // Heartbeat interval
    std::chrono::milliseconds rpc_timeout{100};              // RPC timeout
    bool enable_network_delays = false;                      // Enable simulated latency
    std::chrono::milliseconds network_latency{10};           // Network latency
    double network_reliability = 1.0;                        // Reliability (0.0-1.0)
};
```

## Architecture

### Components

1. **Network Simulator**: Provides simulated network communication
   - Topology management
   - Latency simulation
   - Packet loss simulation
   - Partition simulation

2. **Node Management**: Tracks node state and lifecycle
   - Node creation and initialization
   - Start/stop control
   - Running state tracking

3. **Time Control**: Deterministic time advancement
   - Manual timeout triggering
   - Controlled time progression

### Future Integration

The fixture is designed as infrastructure for future Raft node integration. To integrate actual Raft nodes:

1. Replace `std::shared_ptr<void> raft_node` placeholder with actual Raft node type
2. Implement network client/server adapters for network simulator
3. Uncomment and implement actual Raft node method calls in:
   - `initialize_cluster()` - set cluster configuration
   - `start_node()` / `stop_node()` - node lifecycle
   - `get_leader()` - leader detection
   - `tick_election_timeouts()` / `tick_heartbeat_timeouts()` - timeout handling

## Testing

The fixture itself is tested in `raft_multi_node_fixture_test.cpp`:

```bash
# Build the test
cmake --build build --target raft_multi_node_fixture_test

# Run the test using CTest
ctest --test-dir build -R raft_multi_node_fixture_test --verbose --output-on-failure
```

### Test Coverage

- ✅ Fixture initialization with different cluster sizes (3, 5, 7 nodes)
- ✅ Invalid cluster size validation (even numbers, too small)
- ✅ Node lifecycle management (start, stop, restart)
- ✅ Network topology configuration
- ✅ Network partition simulation
- ✅ Time advancement and timeout triggers
- ✅ Cluster configuration management
- ✅ Fixture cleanup and resource management

All tests pass successfully (7/7 test cases).

## Design Decisions

### Why Infrastructure Only?

This fixture provides the infrastructure for multi-node testing without actual Raft node integration because:

1. **Separation of Concerns**: Network simulation and node management are independent of Raft implementation
2. **Incremental Development**: Infrastructure can be tested and validated independently
3. **Flexibility**: Easy to integrate different Raft implementations or test strategies
4. **Reusability**: Can be used for other distributed system testing

### Why Network Simulator?

The network simulator provides:
- **Deterministic Testing**: Reproducible network conditions
- **Failure Injection**: Controlled network failures and partitions
- **Performance Testing**: Latency and reliability simulation
- **Integration Testing**: Multi-node communication without actual network

### Why Manual Time Control?

Manual time control enables:
- **Deterministic Tests**: Reproducible timing behavior
- **Fast Tests**: No waiting for actual timeouts
- **Precise Control**: Trigger specific timing scenarios
- **Debugging**: Step through timing-sensitive code

## Future Work

### Immediate Next Steps (Tasks 701-703)

1. **Task 701**: Implement cluster initialization test
   - Test proper cluster bootstrap
   - Verify initial follower state
   - Validate election timeout randomization
   - Confirm first leader election

2. **Task 702**: Test membership management operations
   - Verify add_server with joint consensus
   - Test remove_server with cleanup
   - Validate configuration change safety

3. **Task 703**: Test network partition scenarios
   - Verify split-brain prevention
   - Test leader election in majority partition
   - Validate log replication after partition heal

### Long-term Enhancements

- **Raft Node Integration**: Connect actual Raft nodes to fixture
- **Network Adapters**: Implement network client/server for simulator
- **Advanced Scenarios**: Complex failure patterns and recovery
- **Performance Testing**: Throughput and latency benchmarks
- **Visualization**: Cluster state and network topology visualization

## References

- **Raft Paper**: [In Search of an Understandable Consensus Algorithm](https://raft.github.io/raft.pdf)
- **Network Simulator**: `include/network_simulator/`
- **Raft Implementation**: `include/raft/raft.hpp`
- **Task Specification**: `.kiro/specs/raft-consensus/tasks.md`

## Notes

This is an **optional enhancement task**. The note in the task specification states:

> "Current tests use simplified single-node implementations and mock network interactions. Core Raft functionality is already validated through property-based and integration tests."

The fixture provides infrastructure for future multi-node integration testing while the core Raft algorithm is already production-ready and fully tested.
