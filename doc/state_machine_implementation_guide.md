# State Machine Implementation Guide

## Overview

This guide provides step-by-step instructions for implementing custom state machines that integrate with the Raft consensus algorithm. A state machine is the application-specific component that executes committed log entries and maintains the replicated state.

## Table of Contents

1. [State Machine Concept](#state-machine-concept)
2. [Basic Implementation Steps](#basic-implementation-steps)
3. [Command Format Design](#command-format-design)
4. [Snapshot Strategy](#snapshot-strategy)
5. [Error Handling](#error-handling)
6. [Performance Optimization](#performance-optimization)
7. [Testing Your State Machine](#testing-your-state-machine)
8. [Integration with Raft](#integration-with-raft)
9. [Examples](#examples)

## State Machine Concept

The state machine must satisfy the `state_machine` concept defined in `include/raft/types.hpp`:

```cpp
template<typename SM, typename LogIndex>
concept state_machine = requires(
    SM sm,
    const std::vector<std::byte>& command,
    const std::vector<std::byte>& snapshot_data,
    LogIndex index
) {
    requires log_index<LogIndex>;
    
    // Apply a committed log entry to the state machine
    { sm.apply(command, index) } -> std::same_as<std::vector<std::byte>>;
    
    // Get the current state for snapshot creation
    { sm.get_state() } -> std::same_as<std::vector<std::byte>>;
    
    // Restore state from a snapshot
    { sm.restore_from_snapshot(snapshot_data, index) } -> std::same_as<void>;
};
```

### Required Methods

1. **`apply(command, index)`**: Apply a committed log entry to the state machine
   - **Parameters**: 
     - `command`: The command to execute (as bytes)
     - `index`: The log index of this command
   - **Returns**: Result of the command execution (as bytes)
   - **Guarantees**: Must be deterministic - same command produces same result

2. **`get_state()`**: Capture the current state for snapshot creation
   - **Returns**: Complete state machine state (as bytes)
   - **Guarantees**: Must capture all state needed to restore the state machine

3. **`restore_from_snapshot(snapshot_data, index)`**: Restore state from a snapshot
   - **Parameters**:
     - `snapshot_data`: The snapshot data to restore from
     - `index`: The log index of the snapshot
   - **Guarantees**: Must restore state machine to exact state at given index

## Basic Implementation Steps

### Step 1: Define Your State Machine Class

```cpp
#include <raft/types.hpp>

namespace myapp {

template<typename LogIndex = std::uint64_t>
requires kythira::log_index<LogIndex>
class my_state_machine {
public:
    my_state_machine() = default;
    
    auto apply(const std::vector<std::byte>& command, LogIndex index) 
        -> std::vector<std::byte>;
    
    auto get_state() const -> std::vector<std::byte>;
    
    auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, 
                               LogIndex index) -> void;

private:
    // Your state machine's internal state
    LogIndex _last_applied_index{0};
    // ... other state variables
};

// Validate concept compliance
static_assert(kythira::state_machine<my_state_machine<std::uint64_t>, std::uint64_t>,
              "my_state_machine must satisfy state_machine concept");

} // namespace myapp
```

### Step 2: Implement the `apply` Method

The `apply` method is called for each committed log entry in order:

```cpp
auto my_state_machine::apply(const std::vector<std::byte>& command, LogIndex index) 
    -> std::vector<std::byte> 
{
    // 1. Validate command format
    if (command.empty()) {
        throw std::invalid_argument("Empty command");
    }
    
    // 2. Update last applied index
    _last_applied_index = index;
    
    // 3. Parse command type and parameters
    auto cmd_type = parse_command_type(command);
    
    // 4. Execute command based on type
    switch (cmd_type) {
        case command_type::operation1:
            return handle_operation1(command);
        case command_type::operation2:
            return handle_operation2(command);
        // ... other operations
        default:
            throw std::invalid_argument("Unknown command type");
    }
}
```

**Key Points**:
- Must be **deterministic**: Same command always produces same result
- Must be **sequential**: Commands are applied in log index order
- Must **update state**: Modify internal state based on command
- Must **return result**: Return command result to client
- Should **track index**: Store last_applied_index for debugging

### Step 3: Implement the `get_state` Method

The `get_state` method captures the complete state for snapshot creation:

```cpp
auto my_state_machine::get_state() const -> std::vector<std::byte> {
    std::vector<std::byte> state;
    
    // Serialize all state machine state
    // Example: Serialize a map
    serialize_uint64(state, _my_map.size());
    for (const auto& [key, value] : _my_map) {
        serialize_string(state, key);
        serialize_string(state, value);
    }
    
    return state;
}
```

**Key Points**:
- Must capture **all state**: Include everything needed to restore
- Should be **efficient**: Minimize snapshot size when possible
- Must be **consistent**: Snapshot represents a consistent point in time
- Should **include metadata**: Consider versioning for future compatibility

### Step 4: Implement the `restore_from_snapshot` Method

The `restore_from_snapshot` method restores state from a snapshot:

```cpp
auto my_state_machine::restore_from_snapshot(
    const std::vector<std::byte>& snapshot_data, 
    LogIndex index) -> void 
{
    // 1. Clear current state
    _my_map.clear();
    _last_applied_index = index;
    
    // 2. Handle empty snapshot
    if (snapshot_data.empty()) {
        return; // Empty state is valid
    }
    
    // 3. Deserialize state
    std::size_t offset = 0;
    auto map_size = deserialize_uint64(snapshot_data, offset);
    
    for (std::uint64_t i = 0; i < map_size; ++i) {
        auto key = deserialize_string(snapshot_data, offset);
        auto value = deserialize_string(snapshot_data, offset);
        _my_map[key] = value;
    }
    
    // 4. Validate restored state
    if (offset != snapshot_data.size()) {
        throw std::invalid_argument("Invalid snapshot: extra data");
    }
}
```

**Key Points**:
- Must **clear existing state**: Start fresh from snapshot
- Must **handle empty snapshots**: Empty state is valid
- Must **validate format**: Detect corrupted snapshots
- Should **be atomic**: Either fully restore or fail cleanly

## Command Format Design

### Binary Command Format

Design a compact binary format for commands:

```cpp
// Example: Key-Value Store Command Format
// [command_type (1 byte)][key_length (4 bytes)][key][value_length (4 bytes)][value]

enum class command_type : std::uint8_t {
    put = 1,
    get = 2,
    del = 3
};

// Helper to create PUT command
static auto make_put_command(const std::string& key, const std::string& value) 
    -> std::vector<std::byte> 
{
    std::vector<std::byte> command;
    
    // Command type
    command.push_back(static_cast<std::byte>(command_type::put));
    
    // Key length and key
    std::uint32_t key_length = static_cast<std::uint32_t>(key.size());
    append_uint32(command, key_length);
    append_bytes(command, key.data(), key.size());
    
    // Value length and value
    std::uint32_t value_length = static_cast<std::uint32_t>(value.size());
    append_uint32(command, value_length);
    append_bytes(command, value.data(), value.size());
    
    return command;
}
```

### Command Format Guidelines

1. **Use Fixed-Size Headers**: Makes parsing easier and faster
2. **Include Length Fields**: For variable-length data
3. **Version Your Format**: Add version byte for future compatibility
4. **Keep It Simple**: Simpler formats are easier to debug
5. **Document Format**: Write clear documentation for command structure

### Command Parsing Best Practices

```cpp
auto parse_command(const std::vector<std::byte>& command) -> parsed_command {
    if (command.empty()) {
        throw std::invalid_argument("Empty command");
    }
    
    std::size_t offset = 0;
    
    // Parse command type
    if (offset + 1 > command.size()) {
        throw std::invalid_argument("Missing command type");
    }
    auto cmd_type = static_cast<command_type>(command[offset++]);
    
    // Parse parameters based on type
    // ... validate sizes before reading
    
    return parsed_command{cmd_type, /* ... */};
}
```

## Snapshot Strategy

### When to Create Snapshots

Snapshots reduce log size and speed up recovery. Create snapshots when:

1. **Log Size Threshold**: Log exceeds configured size (e.g., 10,000 entries)
2. **Time-Based**: Periodically (e.g., every hour)
3. **On Demand**: Manually triggered by operator

### Snapshot Format Design

```cpp
// Example: Snapshot Format
// [version (1 byte)][num_entries (8 bytes)][entry1][entry2]...

auto get_state() const -> std::vector<std::byte> {
    std::vector<std::byte> state;
    
    // Version for future compatibility
    state.push_back(std::byte{1});
    
    // Number of entries
    append_uint64(state, _store.size());
    
    // Serialize each entry
    for (const auto& [key, value] : _store) {
        append_string(state, key);
        append_string(state, value);
    }
    
    return state;
}
```

### Snapshot Optimization Strategies

1. **Incremental Snapshots**: Only store changes since last snapshot
2. **Compression**: Compress snapshot data to reduce size
3. **Lazy Snapshots**: Create snapshots in background thread
4. **Chunked Transfer**: Split large snapshots into chunks for transfer

### Snapshot Validation

```cpp
auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, 
                          LogIndex index) -> void 
{
    if (snapshot_data.empty()) {
        return; // Empty snapshot is valid
    }
    
    std::size_t offset = 0;
    
    // Validate version
    if (offset + 1 > snapshot_data.size()) {
        throw std::invalid_argument("Invalid snapshot: missing version");
    }
    auto version = static_cast<std::uint8_t>(snapshot_data[offset++]);
    if (version != 1) {
        throw std::invalid_argument("Unsupported snapshot version");
    }
    
    // ... continue parsing with validation
}
```

## Error Handling

### Command Validation

```cpp
auto apply(const std::vector<std::byte>& command, LogIndex index) 
    -> std::vector<std::byte> 
{
    try {
        // Validate command format
        validate_command_format(command);
        
        // Execute command
        return execute_command(command, index);
        
    } catch (const std::invalid_argument& e) {
        // Log error with context
        log_error("Invalid command at index {}: {}", index, e.what());
        
        // Return error result to client
        return make_error_result(e.what());
    }
}
```

### Error Handling Guidelines

1. **Validate Early**: Check command format before execution
2. **Log Errors**: Include context (index, command type, error message)
3. **Return Errors**: Don't throw exceptions for invalid commands
4. **Be Consistent**: Same invalid command always produces same error
5. **Fail Fast**: Detect corruption early in snapshot restoration

### Exception Safety

```cpp
auto restore_from_snapshot(const std::vector<std::byte>& snapshot_data, 
                          LogIndex index) -> void 
{
    // Use RAII for exception safety
    auto temp_state = parse_snapshot(snapshot_data);
    
    // Only update state if parsing succeeds
    _store = std::move(temp_state);
    _last_applied_index = index;
}
```

## Performance Optimization

### Apply Method Optimization

1. **Minimize Allocations**: Reuse buffers when possible
2. **Batch Operations**: Group multiple operations when safe
3. **Use Efficient Data Structures**: Choose appropriate containers
4. **Avoid Copies**: Use move semantics and references

```cpp
// Good: Efficient apply implementation
auto apply(const std::vector<std::byte>& command, LogIndex index) 
    -> std::vector<std::byte> 
{
    _last_applied_index = index;
    
    // Parse command (no allocations)
    auto [cmd_type, key, value] = parse_command_view(command);
    
    // Execute command efficiently
    switch (cmd_type) {
        case command_type::put:
            _store[std::string(key)] = std::string(value);
            return {}; // Empty result
        // ...
    }
}
```

### Snapshot Optimization

1. **Lazy Serialization**: Serialize on-demand rather than storing serialized form
2. **Compression**: Compress large snapshots
3. **Incremental Updates**: Track changes since last snapshot
4. **Parallel Serialization**: Serialize independent parts in parallel

### Memory Management

```cpp
class my_state_machine {
private:
    // Use memory pool for frequent allocations
    std::pmr::monotonic_buffer_resource _buffer;
    std::pmr::unordered_map<std::string, std::string> _store{&_buffer};
    
    // Periodically reset buffer to reclaim memory
    auto compact_memory() -> void {
        if (_buffer.bytes_allocated() > threshold) {
            auto temp_store = std::unordered_map<std::string, std::string>(
                _store.begin(), _store.end());
            _buffer.release();
            _store = std::pmr::unordered_map<std::string, std::string>(
                temp_store.begin(), temp_store.end(), &_buffer);
        }
    }
};
```

## Testing Your State Machine

### Unit Tests

```cpp
BOOST_AUTO_TEST_CASE(test_apply_put_command, * boost::unit_test::timeout(10)) {
    my_state_machine sm;
    
    auto cmd = my_state_machine::make_put_command("key1", "value1");
    auto result = sm.apply(cmd, 1);
    
    BOOST_CHECK(sm.contains("key1"));
    BOOST_CHECK_EQUAL(sm.get_value("key1"), "value1");
}
```

### Snapshot Round-Trip Tests

```cpp
BOOST_AUTO_TEST_CASE(test_snapshot_round_trip, * boost::unit_test::timeout(10)) {
    my_state_machine sm1;
    
    // Populate state machine
    sm1.apply(make_put_command("key1", "value1"), 1);
    sm1.apply(make_put_command("key2", "value2"), 2);
    
    // Create snapshot
    auto snapshot = sm1.get_state();
    
    // Restore to new state machine
    my_state_machine sm2;
    sm2.restore_from_snapshot(snapshot, 2);
    
    // Verify state matches
    BOOST_CHECK_EQUAL(sm2.get_value("key1"), "value1");
    BOOST_CHECK_EQUAL(sm2.get_value("key2"), "value2");
}
```

### Property-Based Tests

```cpp
BOOST_AUTO_TEST_CASE(test_determinism_property, * boost::unit_test::timeout(60)) {
    // Generate random command sequence
    auto commands = generate_random_commands(100);
    
    // Apply to two state machines
    my_state_machine sm1, sm2;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        sm1.apply(commands[i], i + 1);
        sm2.apply(commands[i], i + 1);
    }
    
    // Verify same final state
    BOOST_CHECK_EQUAL(sm1.get_state(), sm2.get_state());
}
```

## Integration with Raft

### Creating a Raft Node with Custom State Machine

```cpp
#include <raft/raft.hpp>
#include "my_state_machine.hpp"

// Define Raft types with your state machine
struct my_raft_types {
    using state_machine_type = myapp::my_state_machine<std::uint64_t>;
    using future_type = kythira::Future<std::vector<std::byte>>;
    // ... other type definitions
};

// Create Raft node
auto create_raft_node(const std::string& node_id) {
    my_raft_types::state_machine_type state_machine;
    
    kythira::node<my_raft_types> raft_node(
        node_id,
        std::move(state_machine),
        // ... other components
    );
    
    return raft_node;
}
```

### Submitting Commands

```cpp
// Submit command to Raft cluster
auto submit_put_command(kythira::node<my_raft_types>& node,
                       const std::string& key,
                       const std::string& value) 
{
    auto command = my_state_machine::make_put_command(key, value);
    
    auto future = node.submit_command(command, std::chrono::seconds(5));
    
    return future.then([](const std::vector<std::byte>& result) {
        // Handle result
        return parse_result(result);
    });
}
```

## Examples

### Example 1: Counter State Machine

See `include/raft/examples/counter_state_machine.hpp` for a minimal example:
- Simple integer counter
- Increment, decrement, reset, get operations
- Demonstrates basic state machine structure

### Example 2: Key-Value Store

See `include/raft/test_state_machine.hpp` for a complete example:
- In-memory key-value store
- PUT, GET, DELETE operations
- Demonstrates command parsing and snapshot serialization

### Example 3: Register State Machine

See `include/raft/examples/register_state_machine.hpp` for linearizability example:
- Single-value register
- Read and write operations
- Demonstrates linearizable semantics

## Best Practices Summary

1. **Keep It Simple**: Start with simple state machine, add complexity as needed
2. **Test Thoroughly**: Unit tests, property tests, integration tests
3. **Document Format**: Clear documentation for command and snapshot formats
4. **Handle Errors Gracefully**: Validate inputs, return errors, don't crash
5. **Optimize Carefully**: Profile before optimizing, focus on hot paths
6. **Version Everything**: Add version fields for future compatibility
7. **Be Deterministic**: Same command always produces same result
8. **Track Applied Index**: Store last_applied_index for debugging

## Additional Resources

- **State Machine Concept**: `include/raft/types.hpp`
- **Test State Machine**: `include/raft/test_state_machine.hpp`
- **Counter Example**: `include/raft/examples/counter_state_machine.hpp`
- **State Machine Interface**: `doc/state_machine_interface.md`
- **Raft Documentation**: `doc/raft.md`

## Getting Help

If you encounter issues implementing your state machine:

1. Review the example implementations
2. Check the test cases for usage patterns
3. Verify your state machine satisfies the concept with `static_assert`
4. Test snapshot round-trip thoroughly
5. Validate determinism with property-based tests

## Conclusion

Implementing a custom state machine for Raft requires careful attention to:
- **Determinism**: Same commands produce same results
- **Completeness**: Snapshots capture all necessary state
- **Correctness**: Proper error handling and validation
- **Performance**: Efficient apply and snapshot operations

Follow this guide, study the examples, and test thoroughly to create a robust state machine for your application.
