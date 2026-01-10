# Raft Completion Migration Guide

This guide helps developers migrate from the old Raft implementation with immediate-return behavior to the new completion-based implementation that provides proper durability guarantees.

## Overview of Changes

The Raft completion implementation introduces several breaking changes to ensure production-ready behavior:

### Before (Immediate Return)
- `submit_command()` returned immediately without waiting for commit
- `read_state()` used temporary fixes instead of proper leader validation
- Configuration changes didn't wait for proper synchronization
- No comprehensive error handling or retry mechanisms

### After (Completion-Based)
- `submit_command()` waits for commit AND state machine application
- `read_state()` validates leadership through majority heartbeat collection
- Configuration changes use proper two-phase synchronization
- Comprehensive error handling with retry and recovery mechanisms

## Breaking Changes

### 1. submit_command() Behavior

#### Old Behavior
```cpp
// Returned immediately - no durability guarantee
auto future = node.submit_command(command, timeout);
// Future completed immediately, before replication
```

#### New Behavior
```cpp
// Returns future that completes only after commit AND state machine application
auto future = node.submit_command(command, timeout);
future.thenValue([](std::vector<std::byte> result) {
    // This callback is called ONLY after:
    // 1. Entry is replicated to majority
    // 2. Entry is committed
    // 3. Entry is applied to state machine
    // 4. State machine result is available
});
```

#### Migration Steps

1. **Update Expectations**: Client code should expect longer completion times
2. **Handle New Exceptions**: Add error handling for new exception types
3. **Adjust Timeouts**: Increase timeout values to account for full commit cycle

```cpp
// Before
auto future = node.submit_command(command, std::chrono::milliseconds(100));

// After - increase timeout for full commit cycle
auto future = node.submit_command(command, std::chrono::seconds(30));
future.thenValue([](std::vector<std::byte> result) {
    // Handle successful completion with state machine result
}).thenError([](const std::exception& e) {
    // Handle specific error types
    if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
        // Handle commit timeout
        std::cerr << "Commit timed out for entry " << timeout_ex->get_entry_index() << std::endl;
    } else if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
        // Handle leadership loss
        std::cerr << "Leadership lost: " << leadership_ex->get_old_term() 
                  << " -> " << leadership_ex->get_new_term() << std::endl;
    }
});
```

### 2. read_state() Behavior

#### Old Behavior
```cpp
// Used temporary fix - returned immediately without proper validation
auto future = node.read_state(timeout);
// No guarantee of linearizable read consistency
```

#### New Behavior
```cpp
// Validates leadership through majority heartbeat collection
auto future = node.read_state(timeout);
future.thenValue([](std::vector<std::byte> state) {
    // This callback is called ONLY after:
    // 1. Heartbeats sent to all followers
    // 2. Majority of followers respond successfully
    // 3. Leadership is confirmed
    // 4. Current state machine state is returned
});
```

#### Migration Steps

1. **Expect Longer Latency**: Read operations now require network round-trip
2. **Handle Leadership Errors**: Reads can fail if leadership is lost
3. **Adjust Read Patterns**: Consider caching for frequently accessed data

```cpp
// Before
auto future = node.read_state(std::chrono::milliseconds(10));

// After - account for network round-trip
auto future = node.read_state(std::chrono::seconds(5));
future.thenValue([](std::vector<std::byte> state) {
    // Handle linearizable read result
}).thenError([](const std::exception& e) {
    // Handle read failures (leadership loss, network issues, etc.)
});
```

### 3. Configuration Changes

#### Old Behavior
```cpp
// Configuration changes didn't wait for proper synchronization
auto future = node.add_server(new_node_id);
// Completed immediately without waiting for commit
```

#### New Behavior
```cpp
// Uses proper two-phase configuration change protocol
auto future = node.add_server(new_node_id);
future.thenValue([](bool success) {
    // This callback is called ONLY after:
    // 1. Joint consensus configuration is committed
    // 2. Final configuration is committed
    // 3. Configuration change is complete
});
```

#### Migration Steps

1. **Increase Timeouts**: Configuration changes take longer due to two-phase protocol
2. **Handle Serialization**: Only one configuration change can be active at a time
3. **Add Proper Error Handling**: Configuration changes can fail in multiple ways

```cpp
// Before
auto future = node.add_server(new_node_id);

// After - proper synchronization with error handling
auto future = node.add_server(new_node_id);
future.thenValue([](bool success) {
    std::cout << "Server added successfully" << std::endl;
}).thenError([](const std::exception& e) {
    if (auto config_ex = dynamic_cast<const raft::configuration_change_exception*>(&e)) {
        std::cerr << "Configuration change failed in phase " << config_ex->get_phase()
                  << ": " << config_ex->get_reason() << std::endl;
    }
});
```

## New Exception Types

### Commit Timeout Exception

```cpp
try {
    // Handle commit timeout
} catch (const raft::commit_timeout_exception<>& e) {
    std::cerr << "Entry " << e.get_entry_index() 
              << " timed out after " << e.get_timeout().count() << "ms" << std::endl;
    // Retry logic or user notification
}
```

### Leadership Lost Exception

```cpp
try {
    // Handle leadership loss
} catch (const raft::leadership_lost_exception<>& e) {
    std::cerr << "Leadership lost: term " << e.get_old_term() 
              << " -> " << e.get_new_term() << std::endl;
    // Redirect to new leader or retry
}
```

### Future Collection Exception

```cpp
try {
    // Handle collection failures
} catch (const raft::future_collection_exception& e) {
    std::cerr << "Operation " << e.get_operation() 
              << " failed: " << e.get_failed_count() << " futures failed" << std::endl;
    // Network issue handling
}
```

### Configuration Change Exception

```cpp
try {
    // Handle configuration change failures
} catch (const raft::configuration_change_exception& e) {
    std::cerr << "Configuration change failed in " << e.get_phase() 
              << ": " << e.get_reason() << std::endl;
    // Rollback or retry logic
}
```

## Timeout Configuration

### Recommended Timeout Values

```cpp
namespace raft_timeouts {
    // Client operations - account for full commit cycle
    constexpr auto CLIENT_COMMAND_TIMEOUT = std::chrono::seconds(30);
    
    // Read operations - account for heartbeat round-trip
    constexpr auto CLIENT_READ_TIMEOUT = std::chrono::seconds(5);
    
    // Configuration changes - account for two-phase protocol
    constexpr auto CONFIGURATION_CHANGE_TIMEOUT = std::chrono::seconds(60);
    
    // Internal RPC operations
    constexpr auto RPC_TIMEOUT = std::chrono::seconds(5);
    constexpr auto HEARTBEAT_TIMEOUT = std::chrono::milliseconds(1000);
    constexpr auto ELECTION_TIMEOUT = std::chrono::milliseconds(5000);
}
```

### Timeout Configuration Example

```cpp
// Configure timeouts based on network characteristics
raft::raft_configuration config;
config.rpc_timeout = std::chrono::seconds(5);
config.heartbeat_interval = std::chrono::milliseconds(500);
config.election_timeout_min = std::chrono::milliseconds(2000);
config.election_timeout_max = std::chrono::milliseconds(4000);

// Create node with configuration
auto node = raft::node(
    node_id, network_client, network_server, 
    persistence, logger, metrics, membership, config
);
```

## Error Handling Patterns

### Retry Logic

```cpp
class RaftClient {
private:
    static constexpr int MAX_RETRIES = 3;
    static constexpr auto RETRY_DELAY = std::chrono::milliseconds(100);
    
public:
    auto submit_command_with_retry(
        const std::vector<std::byte>& command
    ) -> kythira::Future<std::vector<std::byte>> {
        return submit_command_with_retry_impl(command, 0);
    }
    
private:
    auto submit_command_with_retry_impl(
        const std::vector<std::byte>& command,
        int attempt
    ) -> kythira::Future<std::vector<std::byte>> {
        
        return node.submit_command(command, raft_timeouts::CLIENT_COMMAND_TIMEOUT)
            .thenError([this, command, attempt](const std::exception& e) 
                -> kythira::Future<std::vector<std::byte>> {
                
                // Check if we should retry
                bool should_retry = false;
                if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    should_retry = true;  // Retry on leadership loss
                } else if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
                    should_retry = (attempt < MAX_RETRIES);  // Retry timeouts with limit
                }
                
                if (should_retry && attempt < MAX_RETRIES) {
                    // Wait before retry
                    return kythira::FutureFactory::makeFuture()
                        .delayed(RETRY_DELAY * (attempt + 1))  // Exponential backoff
                        .thenValue([this, command, attempt](auto) {
                            return submit_command_with_retry_impl(command, attempt + 1);
                        });
                } else {
                    // Give up and propagate error
                    return kythira::FutureFactory::makeExceptionalFuture<std::vector<std::byte>>(
                        std::current_exception());
                }
            });
    }
};
```

### Leader Discovery

```cpp
class RaftClusterClient {
private:
    std::vector<NodeId> cluster_nodes;
    std::optional<NodeId> current_leader;
    
public:
    auto submit_command(const std::vector<std::byte>& command) 
        -> kythira::Future<std::vector<std::byte>> {
        
        if (current_leader) {
            return try_submit_to_leader(*current_leader, command);
        } else {
            return discover_leader_and_submit(command);
        }
    }
    
private:
    auto try_submit_to_leader(NodeId leader_id, const std::vector<std::byte>& command)
        -> kythira::Future<std::vector<std::byte>> {
        
        auto node = get_node_connection(leader_id);
        return node->submit_command(command, raft_timeouts::CLIENT_COMMAND_TIMEOUT)
            .thenError([this, command](const std::exception& e) 
                -> kythira::Future<std::vector<std::byte>> {
                
                if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    // Leader changed, clear cached leader and retry
                    current_leader.reset();
                    return discover_leader_and_submit(command);
                } else {
                    // Other error, propagate
                    return kythira::FutureFactory::makeExceptionalFuture<std::vector<std::byte>>(
                        std::current_exception());
                }
            });
    }
    
    auto discover_leader_and_submit(const std::vector<std::byte>& command)
        -> kythira::Future<std::vector<std::byte>> {
        
        // Try each node until we find the leader
        return try_nodes_sequentially(cluster_nodes.begin(), command);
    }
};
```

## Performance Considerations

### Latency Impact

| Operation | Old Latency | New Latency | Reason |
|-----------|-------------|-------------|---------|
| submit_command | ~1ms | ~50-200ms | Full commit cycle |
| read_state | ~1ms | ~10-50ms | Heartbeat validation |
| add_server | ~1ms | ~1-5s | Two-phase protocol |
| remove_server | ~1ms | ~1-5s | Two-phase protocol |

### Throughput Impact

- **Write Throughput**: Slightly reduced due to proper commit waiting
- **Read Throughput**: Reduced due to heartbeat validation (consider read-only replicas)
- **Configuration Changes**: Serialized (only one at a time)

### Optimization Strategies

1. **Batching**: Batch multiple commands to amortize commit overhead
2. **Read Replicas**: Use follower nodes for read-only queries (with eventual consistency)
3. **Caching**: Cache frequently accessed state to reduce read operations
4. **Connection Pooling**: Reuse network connections to reduce setup overhead

## Testing Migration

### Unit Tests

```cpp
// Before - test immediate return
TEST_CASE("submit_command returns immediately") {
    auto future = node.submit_command(command, std::chrono::milliseconds(10));
    REQUIRE(future.isReady());  // Completed immediately
}

// After - test proper completion
TEST_CASE("submit_command waits for commit and application") {
    auto future = node.submit_command(command, std::chrono::seconds(30));
    
    // Should not be ready immediately
    REQUIRE_FALSE(future.isReady());
    
    // Simulate commit and application
    simulate_replication_and_commit();
    
    // Now should be ready
    auto result = future.get();
    REQUIRE_FALSE(result.empty());
}
```

### Integration Tests

```cpp
TEST_CASE("end-to-end client operation") {
    // Create cluster
    auto cluster = create_test_cluster(3);
    
    // Submit command to leader
    auto leader = cluster.get_leader();
    auto future = leader->submit_command(test_command, std::chrono::seconds(30));
    
    // Verify completion after commit
    auto result = future.get();
    
    // Verify state machine application
    auto state = leader->read_state(std::chrono::seconds(5)).get();
    REQUIRE(state_contains_command_result(state, result));
}
```

## Deployment Considerations

### Rolling Updates

1. **Backward Compatibility**: Old and new implementations cannot coexist
2. **Full Cluster Restart**: Required for migration
3. **Data Preservation**: Persistent state format is unchanged

### Monitoring

Add monitoring for new metrics:

```cpp
// Monitor completion times
metrics.histogram("raft.command.completion_time", completion_time_ms);

// Monitor timeout rates
metrics.counter("raft.command.timeouts").increment();

// Monitor leadership changes
metrics.counter("raft.leadership.changes").increment();

// Monitor configuration change success/failure
metrics.counter("raft.config.changes.success").increment();
metrics.counter("raft.config.changes.failure").increment();
```

### Alerting

Set up alerts for:
- High commit timeout rates
- Frequent leadership changes
- Configuration change failures
- Excessive retry attempts

## Troubleshooting

### Common Issues

#### High Timeout Rates

**Symptoms**: Many `commit_timeout_exception` errors

**Causes**:
- Network latency too high for configured timeouts
- Slow state machine application
- Insufficient cluster resources

**Solutions**:
- Increase timeout values
- Optimize state machine performance
- Scale cluster resources

#### Frequent Leadership Changes

**Symptoms**: Many `leadership_lost_exception` errors

**Causes**:
- Network instability
- Node resource constraints
- Incorrect timeout configuration

**Solutions**:
- Stabilize network connectivity
- Increase node resources
- Tune election timeout parameters

#### Configuration Change Failures

**Symptoms**: `configuration_change_exception` errors

**Causes**:
- Concurrent configuration change attempts
- Network partitions during configuration change
- Node failures during two-phase protocol

**Solutions**:
- Serialize configuration changes
- Ensure network stability before changes
- Wait for cluster health before changes

### Debug Logging

Enable detailed logging for troubleshooting:

```cpp
// Enable debug logging for completion components
logger.set_level(raft::log_level::debug);

// Log commit waiter operations
logger.debug("Registered operation for index {}", entry_index);
logger.debug("Fulfilled {} operations for commit index {}", count, commit_index);

// Log future collection operations
logger.debug("Collecting majority from {} futures", futures.size());
logger.debug("Majority collection completed: {} successes, {} failures", success_count, failure_count);

// Log configuration synchronization
logger.debug("Starting configuration change to {} nodes", new_config.size());
logger.debug("Joint consensus committed at index {}", joint_index);
logger.debug("Final configuration committed at index {}", final_index);
```

## Summary

The migration to completion-based Raft implementation provides:

✅ **Proper Durability**: Operations complete only after commit and state machine application
✅ **Linearizable Reads**: Read operations validate leadership before returning state
✅ **Safe Configuration Changes**: Two-phase protocol ensures cluster safety
✅ **Comprehensive Error Handling**: Detailed error types for proper error handling
✅ **Production Readiness**: Robust retry and recovery mechanisms

The migration requires:
- Updating client code to handle longer completion times
- Adding proper error handling for new exception types
- Adjusting timeout values for new behavior
- Testing with realistic network conditions

For additional help, see:
- [Raft Completion API Reference](raft_completion_api_reference.md)
- [Raft Configuration Guide](raft_configuration_guide.md)
- [Raft Troubleshooting Guide](raft_troubleshooting_guide.md)