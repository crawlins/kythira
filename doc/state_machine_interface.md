# State Machine Interface

## Overview

The state machine interface defines the contract between the Raft consensus algorithm and application-specific state machines. This interface enables Raft to replicate arbitrary application state across a cluster while maintaining strong consistency guarantees.

## Concept Definition

The `state_machine` concept is defined in `include/raft/types.hpp`:

```cpp
template<typename SM, typename LogIndex>
concept state_machine = requires(
    SM& sm,
    const SM& const_sm,
    const std::vector<std::byte>& command,
    const std::vector<std::byte>& snapshot_data,
    LogIndex index
) {
    requires log_index<LogIndex>;
    
    // Apply a committed log entry to the state machine
    { sm.apply(command, index) } -> std::same_as<std::vector<std::byte>>;
    
    // Get the current state of the state machine for snapshot creation
    { const_sm.get_state() } -> std::same_as<std::vector<std::byte>>;
    
    // Restore the state machine from a snapshot
    { sm.restore_from_snapshot(snapshot_data, index) } -> std::same_as<void>;
};
```

## Requirements

The state machine interface satisfies the following requirements from the Raft specification:

- **Requirements 1.1**: Integration with Raft consensus
- **Requirements 7.4**: Log entry application
- **Requirements 10.1-10.4**: Snapshot creation and restoration
- **Requirements 15.2**: State machine synchronization with commit waiting
- **Requirements 19.1-19.5**: Sequential application of committed entries
- **Requirements 31.1-31.2**: Complete snapshot and log compaction support

## Interface Methods

### apply

```cpp
auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte>;
```

Applies a committed log entry to the state machine.

**Parameters:**
- `command`: The serialized command to apply (application-specific format)
- `index`: The log index of the command being applied

**Returns:**
- A serialized result of applying the command (may be empty for some commands)

**Behavior:**
- The state machine MUST apply the command atomically
- The state machine MUST update its internal state based on the command
- The state machine MUST track the last applied index
- The state machine MAY throw exceptions if the command cannot be applied
- Commands MUST be applied in log index order (enforced by Raft)

**Thread Safety:**
- Raft guarantees that `apply` is called sequentially (never concurrently)
- State machine implementations do not need internal synchronization for `apply`

**Example:**
```cpp
// Apply a PUT command to a key-value store
auto result = state_machine.apply(put_command, 42);
```

### get_state

```cpp
auto get_state() const -> std::vector<std::byte>;
```

Gets the current state of the state machine for snapshot creation.

**Returns:**
- A serialized representation of the entire state machine state

**Behavior:**
- The state machine MUST return a complete snapshot of its current state
- The returned data MUST be sufficient to restore the state machine via `restore_from_snapshot`
- The method MUST be const (read-only)
- The method SHOULD be efficient (avoid unnecessary copies)

**Thread Safety:**
- May be called concurrently with `apply` in some implementations
- State machine implementations SHOULD use appropriate synchronization if needed

**Example:**
```cpp
// Create a snapshot of the current state
auto snapshot_data = state_machine.get_state();
```

### restore_from_snapshot

```cpp
auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index) -> void;
```

Restores the state machine from a snapshot.

**Parameters:**
- `snapshot_data`: The serialized state machine state (from a previous `get_state` call)
- `index`: The log index corresponding to this snapshot

**Behavior:**
- The state machine MUST replace its entire state with the snapshot data
- The state machine MUST clear any existing state before restoring
- The state machine MUST update its last applied index to the provided index
- The state machine MAY throw exceptions if the snapshot data is invalid or corrupted

**Thread Safety:**
- Raft guarantees that `restore_from_snapshot` is not called concurrently with `apply`
- State machine implementations do not need internal synchronization

**Example:**
```cpp
// Restore state machine from a snapshot
state_machine.restore_from_snapshot(snapshot_data, 100);
```

## Implementation Guidelines

### Command Format

State machines are responsible for defining their own command format. Common approaches include:

1. **Binary Protocol**: Fixed-size headers with type tags and length fields
2. **JSON**: Human-readable but less efficient
3. **Protocol Buffers**: Efficient and schema-based
4. **Custom Serialization**: Application-specific formats

### Error Handling

State machines SHOULD throw exceptions for:
- Invalid command format
- Corrupted snapshot data
- Application-specific validation failures

Raft will catch these exceptions and:
- Log the error
- Halt further application (for `apply` failures)
- Propagate the error to clients (for `apply` failures)

### Snapshot Strategy

State machines SHOULD consider:
- **Snapshot Size**: Keep snapshots compact by only including essential state
- **Snapshot Frequency**: Balance between log size and snapshot overhead
- **Incremental Snapshots**: Consider incremental approaches for large state machines

### Performance Considerations

- **Apply Performance**: Keep `apply` operations fast (< 1ms typical)
- **Snapshot Performance**: `get_state` may be called infrequently but should complete in reasonable time
- **Memory Usage**: Be mindful of memory usage for large state machines

## Example Implementation

See `include/raft/test_state_machine.hpp` for a complete example implementation of a key-value store state machine.

### Basic Structure

```cpp
template<typename LogIndex = std::uint64_t>
requires log_index<LogIndex>
class my_state_machine {
public:
    auto apply(const std::vector<std::byte>& command, LogIndex index) 
        -> std::vector<std::byte> {
        // Parse command
        // Update internal state
        // Return result
    }
    
    auto get_state() const -> std::vector<std::byte> {
        // Serialize internal state
        // Return snapshot data
    }
    
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, LogIndex index) 
        -> void {
        // Clear existing state
        // Deserialize snapshot data
        // Update internal state
    }

private:
    // Application-specific state
    LogIndex _last_applied_index{0};
};
```

## Testing

The state machine concept is validated by:
- **Concept Validation Tests**: Verify that implementations satisfy the concept
- **Functional Tests**: Test apply, get_state, and restore_from_snapshot operations
- **Snapshot Round-Trip Tests**: Verify snapshot creation and restoration preserve state
- **Error Handling Tests**: Verify proper exception handling

See `tests/raft_state_machine_concept_test.cpp` for comprehensive test examples.

## Integration with Raft

The Raft node uses the state machine interface in the following scenarios:

### Log Application

When the commit index advances, Raft calls `apply` for each committed entry:

```cpp
for (auto i = _last_applied + 1; i <= _commit_index; ++i) {
    auto entry = get_log_entry(i);
    try {
        auto result = _state_machine.apply(entry.command(), i);
        // Fulfill client futures with result
    } catch (const std::exception& e) {
        // Log error and halt application
    }
}
```

### Snapshot Creation

When log compaction is needed, Raft calls `get_state`:

```cpp
auto state_data = _state_machine.get_state();
auto snap = snapshot{
    ._last_included_index = _last_applied,
    ._last_included_term = get_log_entry(_last_applied).term(),
    ._configuration = _configuration,
    ._state_machine_state = state_data
};
```

### Snapshot Installation

When receiving a snapshot from the leader, Raft calls `restore_from_snapshot`:

```cpp
try {
    _state_machine.restore_from_snapshot(
        snapshot.state_machine_state(),
        snapshot.last_included_index()
    );
    _last_applied = snapshot.last_included_index();
} catch (const std::exception& e) {
    // Log error and reject snapshot
}
```

## Best Practices

1. **Keep Commands Idempotent**: Design commands to be safely retried
2. **Version Snapshots**: Include version information in snapshots for compatibility
3. **Validate Inputs**: Thoroughly validate command and snapshot formats
4. **Log Errors**: Provide detailed error messages for debugging
5. **Test Thoroughly**: Use property-based testing to verify correctness
6. **Document Format**: Clearly document command and snapshot formats
7. **Consider Compatibility**: Plan for schema evolution and backward compatibility

## See Also

- [Raft Consensus Algorithm](https://raft.github.io/)
- [Requirements Document](.kiro/specs/raft-consensus/requirements.md)
- [Design Document](.kiro/specs/raft-consensus/design.md)
- [Test State Machine Implementation](../include/raft/test_state_machine.hpp)
- [State Machine Concept Tests](../tests/raft_state_machine_concept_test.cpp)
