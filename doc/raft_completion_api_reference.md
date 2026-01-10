# Raft Completion Components API Reference

This document provides comprehensive API documentation for the Raft completion components that enable production-ready asynchronous operation handling, commit waiting, configuration synchronization, and error handling.

## Overview

The Raft completion components extend the basic Raft consensus implementation with:

- **Commit Waiting**: Ensures client operations wait for actual durability before completing
- **Future Collection**: Handles multiple async operations efficiently (heartbeats, elections, replication)
- **Configuration Synchronization**: Manages safe two-phase configuration changes
- **Comprehensive Error Handling**: Provides robust retry and recovery mechanisms

All components use the kythira future wrapper classes for type-safe async operations while maintaining compatibility with the existing Raft architecture.

## Core Components

### CommitWaiter

The `commit_waiter` class manages pending client operations waiting for commit and state machine application.

#### Template Parameters

```cpp
template<typename LogIndex = std::uint64_t>
requires raft::log_index<LogIndex>
class commit_waiter;
```

- `LogIndex`: The log index type (must satisfy `log_index` concept)

#### Key Methods

##### register_operation()

```cpp
auto register_operation(
    log_index_t index,
    std::function<void(std::vector<std::byte>)> fulfill_callback,
    std::function<void(std::exception_ptr)> reject_callback,
    std::optional<std::chrono::milliseconds> timeout = std::nullopt
) -> void;
```

Registers a new operation that waits for commit and state machine application.

**Parameters:**
- `index`: The log index of the entry to wait for
- `fulfill_callback`: Callback to call when the operation is fulfilled
- `reject_callback`: Callback to call when the operation is rejected
- `timeout`: Optional timeout duration (nullopt means no timeout)

##### notify_committed_and_applied()

```cpp
template<typename ResultFunction>
auto notify_committed_and_applied(
    log_index_t commit_index,
    ResultFunction&& get_result
) -> void;
```

Notifies that entries up to `commit_index` are committed and applied to state machine.

**Parameters:**
- `commit_index`: The highest log index that has been committed and applied
- `get_result`: Function to get the state machine result for a given log index

##### cancel_all_operations()

```cpp
auto cancel_all_operations(const std::string& reason) -> void;
```

Cancels all pending operations with the given reason. Typically called when leadership is lost or the node shuts down.

##### cancel_timed_out_operations()

```cpp
auto cancel_timed_out_operations() -> std::size_t;
```

Cancels operations that have timed out. Should be called periodically to clean up timed-out operations.

**Returns:** Number of operations that were cancelled due to timeout.

#### Usage Example

```cpp
// Create commit waiter
raft::commit_waiter<std::uint64_t> waiter;

// Register operation
waiter.register_operation(
    42,  // log index
    [](std::vector<std::byte> result) {
        // Handle successful completion
        std::cout << "Operation completed with result size: " << result.size() << std::endl;
    },
    [](std::exception_ptr ex) {
        // Handle failure
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::cerr << "Operation failed: " << e.what() << std::endl;
        }
    },
    std::chrono::seconds(30)  // 30 second timeout
);

// Later, when entries are committed and applied
waiter.notify_committed_and_applied(42, [](std::uint64_t index) {
    return std::vector<std::byte>{0x01, 0x02, 0x03};  // State machine result
});
```

### RaftFutureCollector

The `raft_future_collector` class provides specialized future collection operations for Raft consensus.

#### Template Parameters

```cpp
template<typename T>
class raft_future_collector;
```

- `T`: The value type of the futures being collected

#### Key Static Methods

##### collect_majority()

```cpp
static auto collect_majority(
    std::vector<kythira::Future<T>> futures,
    std::chrono::milliseconds timeout
) -> kythira::Future<std::vector<T>>;
```

Collects futures and waits for majority response. A majority is defined as `(futures.size() / 2) + 1`.

**Parameters:**
- `futures`: Vector of futures to collect from
- `timeout`: Maximum time to wait for majority response

**Returns:** Future containing vector of results from majority

**Throws:** `future_collection_exception` if majority cannot be achieved

##### collect_all_with_timeout()

```cpp
static auto collect_all_with_timeout(
    std::vector<kythira::Future<T>> futures,
    std::chrono::milliseconds timeout
) -> kythira::Future<std::vector<kythira::Try<T>>>;
```

Waits for all futures to complete, handling timeouts gracefully.

##### collect_with_strategy()

```cpp
enum class collection_strategy {
    all,        // Wait for all futures
    majority,   // Wait for majority of futures
    any,        // Wait for any single future
    count       // Wait for specific count of futures
};

static auto collect_with_strategy(
    std::vector<kythira::Future<T>> futures,
    collection_strategy strategy,
    std::chrono::milliseconds timeout,
    std::size_t count = 0
) -> kythira::Future<std::vector<T>>;
```

Collects futures using a custom strategy.

#### Usage Example

```cpp
// Collect heartbeat responses for majority
std::vector<kythira::Future<append_entries_response>> heartbeat_futures;
// ... populate futures ...

auto majority_future = raft_future_collector<append_entries_response>::collect_majority(
    std::move(heartbeat_futures),
    std::chrono::milliseconds(5000)
);

majority_future.thenValue([](std::vector<append_entries_response> responses) {
    std::size_t success_count = 0;
    for (const auto& response : responses) {
        if (response.success()) {
            success_count++;
        }
    }
    std::cout << "Received " << success_count << " successful heartbeat responses" << std::endl;
});
```

### ConfigurationSynchronizer

The `configuration_synchronizer` class manages safe two-phase configuration changes.

#### Template Parameters

```cpp
template<
    typename NodeId = std::uint64_t, 
    typename LogIndex = std::uint64_t, 
    typename FutureType = kythira::Future<bool>
>
requires raft::node_id<NodeId> && raft::log_index<LogIndex>
class configuration_synchronizer;
```

#### Key Methods

##### start_configuration_change()

```cpp
auto start_configuration_change(
    const cluster_configuration<NodeId>& new_config,
    std::chrono::milliseconds timeout = std::chrono::seconds(60)
) -> future_type;
```

Starts a configuration change with proper synchronization.

**Parameters:**
- `new_config`: The target configuration to transition to
- `timeout`: Maximum time to wait for the configuration change

**Returns:** Future that completes when the configuration change is done

##### notify_configuration_committed()

```cpp
auto notify_configuration_committed(
    const cluster_configuration<NodeId>& config,
    LogIndex committed_index
) -> void;
```

Notifies that a configuration entry has been committed. This advances the configuration change through its phases.

##### is_configuration_change_in_progress()

```cpp
auto is_configuration_change_in_progress() const -> bool;
```

Checks if a configuration change is currently in progress.

#### Usage Example

```cpp
// Create configuration synchronizer
raft::configuration_synchronizer<std::uint64_t, std::uint64_t> sync;

// Start configuration change
cluster_configuration<std::uint64_t> new_config;
new_config.add_node(123);  // Add new node

auto change_future = sync.start_configuration_change(new_config, std::chrono::seconds(60));

change_future.thenValue([](bool success) {
    if (success) {
        std::cout << "Configuration change completed successfully" << std::endl;
    }
}).thenError([](const std::exception& e) {
    std::cerr << "Configuration change failed: " << e.what() << std::endl;
});

// Later, when configuration entries are committed
sync.notify_configuration_committed(joint_config, 42);  // Joint consensus committed
sync.notify_configuration_committed(final_config, 43); // Final config committed
```

## Exception Hierarchy

### Base Exception

```cpp
class raft_completion_exception : public raft_exception;
```

Base exception for all completion-related errors.

### Specific Exceptions

#### commit_timeout_exception

```cpp
template<typename LogIndex = std::uint64_t>
class commit_timeout_exception : public raft_completion_exception;
```

Thrown when a commit operation times out.

**Methods:**
- `get_entry_index() -> LogIndex`: Returns the log index that timed out
- `get_timeout() -> std::chrono::milliseconds`: Returns the timeout duration

#### leadership_lost_exception

```cpp
template<typename TermId = std::uint64_t>
class leadership_lost_exception : public raft_completion_exception;
```

Thrown when leadership is lost during an operation.

**Methods:**
- `get_old_term() -> TermId`: Returns the previous term
- `get_new_term() -> TermId`: Returns the new term

#### future_collection_exception

```cpp
class future_collection_exception : public raft_completion_exception;
```

Thrown when future collection operations fail.

**Methods:**
- `get_operation() -> const std::string&`: Returns the operation name
- `get_failed_count() -> std::size_t`: Returns the number of failed futures

#### configuration_change_exception

```cpp
class configuration_change_exception : public raft_completion_exception;
```

Thrown when configuration changes fail.

**Methods:**
- `get_phase() -> const std::string&`: Returns the phase that failed
- `get_reason() -> const std::string&`: Returns the failure reason

## Integration with Raft Node

### Enhanced submit_command()

The enhanced `submit_command()` method now properly waits for commit and state machine application:

```cpp
auto submit_command(
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) -> kythira::Future<std::vector<std::byte>>;
```

**Behavior:**
1. Creates and appends log entry
2. Registers operation with CommitWaiter BEFORE replication
3. Replicates to followers
4. Returns future that completes only after commit and state machine application

### Enhanced read_state()

The enhanced `read_state()` method uses future collection for heartbeat verification:

```cpp
auto read_state(std::chrono::milliseconds timeout) 
    -> kythira::Future<std::vector<std::byte>>;
```

**Behavior:**
1. Sends heartbeats to all followers
2. Uses `collect_majority()` to wait for majority response
3. Verifies leader status before returning state
4. Returns current state machine state if successful

### Enhanced Configuration Operations

Configuration change operations now use proper synchronization:

```cpp
auto add_server(NodeId new_node) -> kythira::Future<bool>;
auto remove_server(NodeId old_node) -> kythira::Future<bool>;
```

**Behavior:**
1. Checks if configuration change is already in progress
2. Uses ConfigurationSynchronizer for two-phase commit
3. Waits for joint consensus commit before proceeding to final configuration
4. Returns future that completes when configuration change is done

## Thread Safety

All completion components are thread-safe:

- **CommitWaiter**: Uses mutex to protect internal state
- **RaftFutureCollector**: Stateless utility class (thread-safe by design)
- **ConfigurationSynchronizer**: Uses mutex to protect internal state

## Performance Considerations

### Memory Usage

- CommitWaiter stores callbacks for pending operations (bounded by client request rate)
- FutureCollector is stateless (no memory overhead)
- ConfigurationSynchronizer stores minimal state for active configuration changes

### Timeout Handling

- All operations support configurable timeouts
- Periodic cleanup prevents resource leaks from timed-out operations
- Exponential backoff for retry operations (where applicable)

### Async Efficiency

- All operations are fully asynchronous (no blocking)
- Future chaining minimizes callback overhead
- Parallel execution for independent operations

## Error Recovery

### Automatic Recovery

- Network failures: Automatic retry with exponential backoff
- Timeout handling: Graceful cleanup and error propagation
- Leadership changes: Automatic cancellation of pending operations

### Manual Recovery

- `cancel_all_operations()`: Clean shutdown of pending operations
- `handle_timeout()`: Periodic cleanup of timed-out operations
- Configuration rollback: Automatic rollback on configuration change failures

## Best Practices

### Timeout Configuration

```cpp
// Recommended timeout values
constexpr auto CLIENT_OPERATION_TIMEOUT = std::chrono::seconds(30);
constexpr auto HEARTBEAT_TIMEOUT = std::chrono::milliseconds(5000);
constexpr auto CONFIGURATION_CHANGE_TIMEOUT = std::chrono::seconds(60);
```

### Error Handling

```cpp
// Always handle both success and failure cases
future.thenValue([](auto result) {
    // Handle success
}).thenError([](const std::exception& e) {
    // Handle failure - check exception type for specific handling
    if (auto timeout_ex = dynamic_cast<const commit_timeout_exception<>*>(&e)) {
        // Handle timeout specifically
    } else if (auto leadership_ex = dynamic_cast<const leadership_lost_exception<>*>(&e)) {
        // Handle leadership loss specifically
    }
});
```

### Resource Management

```cpp
// Periodic cleanup to prevent resource leaks
void periodic_maintenance() {
    // Clean up timed-out operations
    auto cancelled_count = commit_waiter.cancel_timed_out_operations();
    
    // Handle configuration change timeouts
    config_synchronizer.handle_timeout();
    
    // Log cleanup statistics
    if (cancelled_count > 0) {
        logger.info("Cleaned up {} timed-out operations", cancelled_count);
    }
}
```

## Migration Guide

See [Raft Completion Migration Guide](raft_completion_migration_guide.md) for detailed information on migrating from the old immediate-return behavior to the new completion-based approach.