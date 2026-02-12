# State Machine Examples

This directory contains example state machine implementations demonstrating various patterns and use cases for the Kythira Raft consensus library.

## Available Examples

### 1. Counter State Machine (`counter_state_machine.hpp`)

**Use Case**: Simple atomic counter with increment/decrement operations.

**Operations**:
- `INC` - Increment counter by 1
- `DEC` - Decrement counter by 1
- `RESET` - Reset counter to 0
- `GET` - Get current value

**Example**:
```cpp
kythira::examples::counter_state_machine sm;

std::string cmd = "INC";
std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                             reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
auto result = sm.apply(bytes, 1);

std::cout << "Counter value: " << sm.get_value() << "\n";
```

**Best For**: Learning the basics, simple distributed counters, sequence generators.

### 2. Register State Machine (`register_state_machine.hpp`)

**Use Case**: Single-value register with versioning for optimistic concurrency control.

**Operations**:
- `WRITE <value>` - Write new value
- `READ` - Read current value
- `CAS <expected> <new>` - Compare-and-swap

**Example**:
```cpp
kythira::examples::register_state_machine sm;

std::string write_cmd = "WRITE 42";
std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(write_cmd.data()),
                             reinterpret_cast<const std::byte*>(write_cmd.data() + write_cmd.size()));
sm.apply(bytes, 1);

std::string read_cmd = "READ";
std::vector<std::byte> read_bytes(reinterpret_cast<const std::byte*>(read_cmd.data()),
                                  reinterpret_cast<const std::byte*>(read_cmd.data() + read_cmd.size()));
auto result = sm.apply(read_bytes, 2);
```

**Best For**: Linearizable read/write semantics, configuration storage, leader election.

### 3. Replicated Log State Machine (`replicated_log_state_machine.hpp`)

**Use Case**: Append-only log with efficient snapshot strategy.

**Operations**:
- `APPEND <data>` - Append entry to log

**Example**:
```cpp
kythira::examples::replicated_log_state_machine sm;

std::string cmd = "APPEND log entry data";
std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                             reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
sm.apply(bytes, 1);

std::cout << "Total entries: " << sm.entry_count() << "\n";
```

**Best For**: Event sourcing, audit logs, write-ahead logs, message queues.

### 4. Distributed Lock State Machine (`distributed_lock_state_machine.hpp`)

**Use Case**: Distributed locking with timeout-based expiration.

**Operations**:
- `ACQUIRE <lock_id> <owner> <timeout_ms>` - Acquire lock
- `RELEASE <lock_id> <owner>` - Release lock
- `QUERY <lock_id>` - Check lock status

**Example**:
```cpp
kythira::examples::distributed_lock_state_machine sm;

std::string cmd = "ACQUIRE mylock client1 5000";
std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(cmd.data()),
                             reinterpret_cast<const std::byte*>(cmd.data() + cmd.size()));
auto result = sm.apply(bytes, 1);

std::string result_str(reinterpret_cast<const char*>(result.data()), result.size());
if (result_str == "OK") {
    std::cout << "Lock acquired\n";
}
```

**Best For**: Distributed coordination, resource management, mutual exclusion.

## Comparison

| Feature | Counter | Register | Replicated Log | Distributed Lock |
|---------|---------|----------|----------------|------------------|
| **Complexity** | Minimal | Low | Medium | High |
| **State Size** | 8 bytes | ~100 bytes | Growing | Variable |
| **Snapshot Strategy** | Simple | Simple | Incremental | Expiration-based |
| **Use Case** | Counters | Config | Event logs | Coordination |
| **Performance** | Highest | High | Medium | Medium |

## Implementation Guidelines

### Command Format

All state machines use string-based commands for simplicity. For production:

1. **Use binary protocols** for efficiency
2. **Version your commands** for backward compatibility
3. **Validate inputs** thoroughly
4. **Return meaningful results** to clients

### Snapshot Strategy

Choose based on your data:

- **Full snapshot**: Simple, works for small state (Counter, Register)
- **Incremental**: Efficient for append-only data (Replicated Log)
- **Expiration-based**: Remove old data automatically (Distributed Lock)

### Error Handling

All examples throw exceptions for invalid commands:

```cpp
try {
    auto result = sm.apply(command, index);
} catch (const std::invalid_argument& e) {
    // Handle invalid command format
} catch (const std::runtime_error& e) {
    // Handle operation failure
}
```

## Testing

Each example includes:

- **Unit tests**: Basic operation validation
- **Property tests**: Snapshot round-trip, idempotency, determinism
- **Performance tests**: Latency and throughput benchmarks

Run tests:
```bash
cd build
ctest -R state_machine
```

## Creating Your Own State Machine

See [State Machine Implementation Guide](../../doc/state_machine_implementation_guide.md) for detailed instructions.

Quick checklist:
- [ ] Implement `apply(command, index)` method
- [ ] Implement `get_state()` for snapshots
- [ ] Implement `restore_from_snapshot(state, last_index)`
- [ ] Make operations deterministic
- [ ] Handle errors gracefully
- [ ] Write tests for your state machine
- [ ] Benchmark performance

## See Also

- [State Machine Interface Documentation](../../doc/state_machine_interface.md)
- [State Machine Implementation Guide](../../doc/state_machine_implementation_guide.md)
- [Raft Integration Examples](../../examples/raft/)
