# Design Document

## Overview

This document describes the design of a complete, production-ready C++ implementation of the Raft consensus algorithm. The implementation uses C++20/23 concepts to define pluggable interfaces for all major components, enabling flexible deployment across different environments. The design emphasizes type safety through concepts, dependency injection through template parameters, and clear separation of concerns between the consensus algorithm core and its supporting infrastructure.

The Raft implementation follows the algorithm as described in the extended Raft paper by Diego Ongaro and John Ousterhout. It provides strong consistency guarantees through leader election, log replication, and safety mechanisms while maintaining understandability and correctness. Additionally, this design includes completion components that make the implementation production-ready by ensuring proper async coordination, commit waiting, configuration change synchronization, and comprehensive error handling.

**Kythira Integration:** This design leverages generic future concepts as template parameters throughout the implementation, allowing flexibility in future implementations while defaulting to the kythira future wrapper classes from `include/raft/future.hpp`. The kythira wrappers (`kythira::Future<T>`, `kythira::Promise<T>`, `kythira::Try<T>`, `kythira::FutureCollector`, `kythira::FutureFactory`) wrap Folly futures and provide a unified, type-safe interface for asynchronous programming while satisfying the kythira concept system requirements. Core implementations are placed in the `kythira` namespace to reflect their foundational role in the system architecture.

## Architecture

### High-Level Architecture

The Raft implementation is structured in layers:

1. **Concept Layer**: Defines C++ concepts for all pluggable components
2. **Core Algorithm Layer**: Implements the Raft consensus protocol
3. **Completion Layer**: Provides async coordination, commit waiting, and error handling
4. **Component Implementation Layer**: Provides concrete implementations of concepts
5. **Application Layer**: User-defined state machines and client applications

### Enhanced Component Dependencies

```
Node<Types = default_raft_types>
    ├── Types::network_client_type<Types::future_type, RpcSerializer, Data>
    │   └── RpcSerializer<Data> (concept: rpc_serializer, Data: serialized_data)
    ├── Types::network_server_type<Types::future_type, RpcSerializer, Data>
    │   └── RpcSerializer<Data> (concept: rpc_serializer, Data: serialized_data)
    ├── Types::persistence_engine_type (concept: persistence_engine)
    ├── Types::logger_type (concept: diagnostic_logger)
    ├── Types::metrics_type (concept: metrics)
    ├── Types::membership_manager_type (concept: membership_manager)
    └── Completion Components (using Types::future_type)
        ├── CommitWaiter<Types::future_type>
        ├── FutureCollector<Types::future_type, T> (for heartbeats, elections, replication)
        ├── ConfigurationSynchronizer<Types::future_type>
        └── ErrorHandler<Types::future_type, Result> (with retry policies)
```

### Enhanced Architecture Components

The completion adds several new components to the existing architecture:

```
Enhanced Raft Node
├── Existing Core Components (unchanged)
│   ├── NetworkClient, NetworkServer
│   ├── PersistenceEngine, Logger, Metrics
│   └── MembershipManager
├── New Async Coordination Layer
│   ├── CommitWaiter - manages client operation futures
│   ├── FutureCollector - handles multiple async operations
│   ├── ConfigurationSynchronizer - coordinates config changes
│   └── ErrorHandler - manages retry and recovery logic
└── Enhanced State Management
    ├── PendingOperations - tracks uncommitted client ops
    ├── CommitNotifier - signals when entries are committed
    └── ApplicationQueue - manages state machine application
```

### Template-Based Dependency Injection with Unified Types Parameter

All concrete classes use a unified types template parameter to specify their dependencies, providing a clean interface:

```cpp
// Unified types concept that encapsulates all required types
namespace kythira {

template<typename T>
concept raft_types = requires {
    // Future types
    typename T::future_type;
    typename T::promise_type;
    typename T::try_type;
    
    // Component types
    typename T::network_client_type;
    typename T::network_server_type;
    typename T::persistence_engine_type;
    typename T::logger_type;
    typename T::metrics_type;
    typename T::membership_manager_type;
    
    // Data types
    typename T::node_id_type;
    typename T::term_id_type;
    typename T::log_index_type;
    
    // Concept validation
    requires future<typename T::future_type, std::vector<std::byte>>;
    requires network_client<typename T::network_client_type, typename T::future_type>;
    requires network_server<typename T::network_server_type, typename T::future_type>;
    requires persistence_engine<typename T::persistence_engine_type>;
    requires diagnostic_logger<typename T::logger_type>;
    requires metrics<typename T::metrics_type>;
    requires membership_manager<typename T::membership_manager_type>;
};

// Default types implementation
struct default_raft_types {
    using future_type = kythira::Future<std::vector<std::byte>>;
    using promise_type = kythira::Promise<std::vector<std::byte>>;
    using try_type = kythira::Try<std::vector<std::byte>>;
    
    using network_client_type = kythira::simulator_network_client<future_type, json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>;
    using network_server_type = kythira::simulator_network_server<future_type, json_rpc_serializer<std::vector<std::byte>>, std::vector<std::byte>>;
    using persistence_engine_type = memory_persistence_engine;
    using logger_type = console_logger;
    using metrics_type = noop_metrics;
    using membership_manager_type = default_membership_manager;
    
    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
};

// Simplified Raft node with single template parameter
template<raft_types Types = default_raft_types>
class node {
    // All types automatically deduced from Types parameter
    using future_type = typename Types::future_type;
    using network_client_type = typename Types::network_client_type;
    using network_server_type = typename Types::network_server_type;
    // ... other type aliases
    
private:
    network_client_type _network_client;
    network_server_type _network_server;
    // ... other components
    
public:
    // Clean interface using deduced types
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout) -> future_type;
    auto read_state(std::chrono::milliseconds timeout) -> future_type;
    // ... other methods
};

} // namespace kythira
```

## Components and Interfaces

### 0. Future Concept Definition

The `future` concept defines the interface requirements for future types used throughout the Raft implementation:

```cpp
// Future concept defined in include/concepts/future.hpp
namespace kythira {

template<typename F, typename T>
concept future = requires(F f) {
    // Get value (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
    
    // Chain continuation
    { f.then(std::declval<std::function<void(T)>>()) };
    
    // Error handling
    { f.onError(std::declval<std::function<T(std::exception_ptr)>>()) };
};

} // namespace kythira
```

### 1. RPC Serializer Concept

The `rpc_serializer` concept defines the interface for serializing and deserializing Raft RPC messages. The serialized data type must be a range of `std::byte`.

```cpp
// Concept for serialized data - must be a range of std::byte
template<typename T>
concept serialized_data = std::ranges::range<T> && 
    std::same_as<std::ranges::range_value_t<T>, std::byte>;

template<typename S, typename Data, typename NodeId,
         typename RequestVoteRequest, typename RequestVoteResponse,
         typename AppendEntriesRequest, typename AppendEntriesResponse,
         typename InstallSnapshotRequest, typename InstallSnapshotResponse>
concept rpc_serializer = requires(
    S serializer,
    const RequestVoteRequest& request_vote_req,
    const RequestVoteResponse& request_vote_resp,
    const AppendEntriesRequest& append_entries_req,
    const AppendEntriesResponse& append_entries_resp,
    const InstallSnapshotRequest& install_snapshot_req,
    const InstallSnapshotResponse& install_snapshot_resp,
    const Data& data
) {
    // Data must satisfy serialized_data concept
    requires serialized_data<Data>;
    
    // NodeId must satisfy node_id concept
    requires node_id<NodeId>;
    
    // RPC message types must satisfy their respective concepts
    requires request_vote_request_type<RequestVoteRequest, NodeId>;
    requires request_vote_response_type<RequestVoteResponse>;
    requires append_entries_request_type<AppendEntriesRequest, NodeId>;
    requires append_entries_response_type<AppendEntriesResponse>;
    requires install_snapshot_request_type<InstallSnapshotRequest, NodeId>;
    requires install_snapshot_response_type<InstallSnapshotResponse>;
    
    // Serialize RequestVote RPC
    { serializer.serialize(request_vote_req) } -> std::same_as<Data>;
    { serializer.deserialize_request_vote_request(data) } -> std::same_as<RequestVoteRequest>;
    { serializer.serialize(request_vote_resp) } -> std::same_as<Data>;
    { serializer.deserialize_request_vote_response(data) } -> std::same_as<RequestVoteResponse>;
    
    // Serialize AppendEntries RPC
    { serializer.serialize(append_entries_req) } -> std::same_as<Data>;
    { serializer.deserialize_append_entries_request(data) } -> std::same_as<AppendEntriesRequest>;
    { serializer.serialize(append_entries_resp) } -> std::same_as<Data>;
    { serializer.deserialize_append_entries_response(data) } -> std::same_as<AppendEntriesResponse>;
    
    // Serialize InstallSnapshot RPC
    { serializer.serialize(install_snapshot_req) } -> std::same_as<Data>;
    { serializer.deserialize_install_snapshot_request(data) } -> std::same_as<InstallSnapshotRequest>;
    { serializer.serialize(install_snapshot_resp) } -> std::same_as<Data>;
    { serializer.deserialize_install_snapshot_response(data) } -> std::same_as<InstallSnapshotResponse>;
};
```

**Concrete Implementations:**
- `json_rpc_serializer<std::vector<std::byte>>`: Uses JSON for human-readable serialization
- `protobuf_rpc_serializer<std::vector<std::byte>>`: Uses Protocol Buffers for efficient binary serialization
- `json_rpc_serializer<std::span<std::byte>>`: Zero-copy JSON serialization using spans

### 2. Network Client Concept

The `network_client` concept defines the interface for sending RPC requests to remote Raft nodes. Network client implementations are templated on future type and RPC serializer type, and are placed in the `kythira` namespace.

```cpp
namespace kythira {

template<typename C, typename FutureType, typename NodeId>
concept network_client = requires(
    C client,
    const NodeId& target,
    const request_vote_request& rvr,
    const append_entries_request& aer,
    const install_snapshot_request& isr,
    std::chrono::milliseconds timeout
) {
    requires node_id<NodeId>;
    requires future<FutureType, request_vote_response>;
    
    // Send RequestVote RPC - returns a generic future
    { client.send_request_vote(target, rvr, timeout) } 
        -> std::same_as<FutureType>;
    
    // Send AppendEntries RPC - returns a generic future
    { client.send_append_entries(target, aer, timeout) }
        -> std::same_as<FutureType>;
    
    // Send InstallSnapshot RPC - returns a generic future
    { client.send_install_snapshot(target, isr, timeout) }
        -> std::same_as<FutureType>;
};

} // namespace kythira
```

**Concrete Implementations:**
- `kythira::http_network_client<FutureType, Serializer, Data>`: HTTP/1.1 or HTTP/2 transport
- `kythira::https_network_client<FutureType, Serializer, Data>`: HTTPS with TLS
- `kythira::coap_network_client<FutureType, Serializer, Data>`: CoAP over UDP
- `kythira::coaps_network_client<FutureType, Serializer, Data>`: CoAP over DTLS (Datagram TLS)
- `kythira::simulator_network_client<FutureType, Serializer, Data>`: Network simulator transport

### 3. Network Server Concept

The `network_server` concept defines the interface for receiving RPC requests from remote Raft nodes. Network server implementations are templated on future type and RPC serializer type, and are placed in the `kythira` namespace.

```cpp
namespace kythira {

template<typename S, typename FutureType>
concept network_server = requires(
    S server,
    std::function<request_vote_response(const request_vote_request&)> rv_handler,
    std::function<append_entries_response(const append_entries_request&)> ae_handler,
    std::function<install_snapshot_response(const install_snapshot_request&)> is_handler
) {
    requires future<FutureType, void>;
    
    // Register RPC handlers
    { server.register_request_vote_handler(rv_handler) } -> std::same_as<void>;
    { server.register_append_entries_handler(ae_handler) } -> std::same_as<void>;
    { server.register_install_snapshot_handler(is_handler) } -> std::same_as<void>;
    
    // Server lifecycle
    { server.start() } -> std::same_as<void>;
    { server.stop() } -> std::same_as<void>;
    { server.is_running() } -> std::convertible_to<bool>;
};

} // namespace kythira
```

**Concrete Implementations:**
- `kythira::http_network_server<FutureType, Serializer, Data>`: HTTP/1.1 or HTTP/2 transport
- `kythira::https_network_server<FutureType, Serializer, Data>`: HTTPS with TLS
- `kythira::coap_network_server<FutureType, Serializer, Data>`: CoAP over UDP
- `kythira::coaps_network_server<FutureType, Serializer, Data>`: CoAP over DTLS (Datagram TLS)
- `kythira::simulator_network_server<FutureType, Serializer, Data>`: Network simulator transport

### 4. Persistence Engine Concept

The `persistence_engine` concept defines the interface for durable storage of Raft state.

```cpp
template<typename P>
concept persistence_engine = requires(
    P engine,
    term_id term,
    const node_id& node,
    const log_entry& entry,
    log_index index,
    const snapshot& snap
) {
    // Persistent state operations
    { engine.save_current_term(term) } -> std::same_as<void>;
    { engine.load_current_term() } -> std::same_as<term_id>;
    
    { engine.save_voted_for(node) } -> std::same_as<void>;
    { engine.load_voted_for() } -> std::same_as<std::optional<node_id>>;
    
    // Log operations
    { engine.append_log_entry(entry) } -> std::same_as<void>;
    { engine.get_log_entry(index) } -> std::same_as<std::optional<log_entry>>;
    { engine.get_log_entries(index, index) } -> std::same_as<std::vector<log_entry>>;
    { engine.get_last_log_index() } -> std::same_as<log_index>;
    { engine.truncate_log(index) } -> std::same_as<void>;
    
    // Snapshot operations
    { engine.save_snapshot(snap) } -> std::same_as<void>;
    { engine.load_snapshot() } -> std::same_as<std::optional<snapshot>>;
    { engine.delete_log_entries_before(index) } -> std::same_as<void>;
};
```

**Concrete Implementations:**
- `rocksdb_persistence_engine`: RocksDB-based durable storage
- `memory_persistence_engine`: In-memory storage for testing

### 5. Diagnostic Logger Concept

The `diagnostic_logger` concept defines the interface for structured logging.

```cpp
enum class log_level {
    trace,
    debug,
    info,
    warning,
    error,
    critical
};

template<typename L>
concept diagnostic_logger = requires(
    L logger,
    log_level level,
    std::string_view message,
    std::string_view key,
    std::string_view value
) {
    // Basic logging
    { logger.log(level, message) } -> std::same_as<void>;
    
    // Structured logging with key-value pairs
    { logger.log(level, message, 
        std::vector<std::pair<std::string_view, std::string_view>>{}) } 
        -> std::same_as<void>;
    
    // Convenience methods
    { logger.trace(message) } -> std::same_as<void>;
    { logger.debug(message) } -> std::same_as<void>;
    { logger.info(message) } -> std::same_as<void>;
    { logger.warning(message) } -> std::same_as<void>;
    { logger.error(message) } -> std::same_as<void>;
    { logger.critical(message) } -> std::same_as<void>;
};
```

**Concrete Implementations:**
- `log4cpp_logger`: log4cpp-based logging
- `spdlog_logger`: spdlog-based logging
- `boost_log_logger`: Boost.Log-based logging

### 6. Membership Manager Concept

The `membership_manager` concept defines the interface for managing cluster membership changes.

```cpp
template<typename M>
concept membership_manager = requires(
    M manager,
    const node_id& node,
    const cluster_configuration& config
) {
    // Node validation
    { manager.validate_new_node(node) } -> std::convertible_to<bool>;
    { manager.authenticate_node(node) } -> std::convertible_to<bool>;
    
    // Configuration management
    { manager.create_joint_configuration(config, config) } 
        -> std::same_as<cluster_configuration>;
    { manager.is_node_in_configuration(node, config) } 
        -> std::convertible_to<bool>;
    
    // Cleanup
    { manager.handle_node_removal(node) } -> std::same_as<void>;
};
```

**Concrete Implementations:**
- `default_membership_manager`: Basic membership management
- `authenticated_membership_manager`: Adds authentication requirements

### 7. Node Concept

The `node` concept defines the interface for a Raft node that participates in the consensus protocol. A node is templated on all its pluggable components.

```cpp
template<typename M>
concept membership_manager = requires(
    M manager,
    const node_id& node,
    const cluster_configuration& config
) {
    // Node validation
    { manager.validate_new_node(node) } -> std::convertible_to<bool>;
    { manager.authenticate_node(node) } -> std::convertible_to<bool>;
    
    // Configuration management
    { manager.create_joint_configuration(config, config) } 
        -> std::same_as<cluster_configuration>;
    { manager.is_node_in_configuration(node, config) } 
        -> std::convertible_to<bool>;
    
    // Cleanup
    { manager.handle_node_removal(node) } -> std::same_as<void>;
};
```

**Concrete Implementations:**
- `default_membership_manager`: Basic membership management
- `authenticated_membership_manager`: Adds authentication requirements

### 7. Metrics Concept

The `metrics` concept defines the interface for collecting and reporting performance metrics.

```cpp
template<typename M>
concept metrics = requires(
    M metric,
    std::string_view name,
    std::string_view dimension_name,
    std::string_view dimension_value,
    std::int64_t count,
    std::chrono::nanoseconds duration,
    double value
) {
    // Metric configuration
    { metric.set_metric_name(name) } -> std::same_as<void>;
    { metric.add_dimension(dimension_name, dimension_value) } -> std::same_as<void>;
    
    // Recording methods
    { metric.add_one() } -> std::same_as<void>;
    { metric.add_count(count) } -> std::same_as<void>;
    { metric.add_duration(duration) } -> std::same_as<void>;
    { metric.add_value(value) } -> std::same_as<void>;
    
    // Metric emission
    { metric.emit() } -> std::same_as<void>;
};
```

**Concrete Implementations:**
- `cloudwatch_metrics`: Amazon CloudWatch metrics
- `prometheus_metrics`: Prometheus metrics
- `statsd_metrics`: StatsD metrics
- `noop_metrics`: No-op metrics for testing

### 8. Node Concept

The `node` concept defines the interface for a Raft node that participates in the consensus protocol, placed in the `kythira` namespace.

```cpp
namespace kythira {

template<typename N, typename FutureType>
concept raft_node = requires(
    N node,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) {
    requires future<FutureType, std::vector<std::byte>>;
    
    // Client operations - return generic futures
    { node.submit_command(command, timeout) } 
        -> std::same_as<FutureType>;
    { node.read_state(timeout) } 
        -> std::same_as<FutureType>;
    
    // Node lifecycle
    { node.start() } -> std::same_as<void>;
    { node.stop() } -> std::same_as<void>;
    { node.is_running() } -> std::convertible_to<bool>;
    
    // Node state queries
    { node.get_node_id() } -> std::same_as<node_id>;
    { node.get_current_term() } -> std::same_as<term_id>;
    { node.get_state() } -> std::same_as<server_state>;
    { node.is_leader() } -> std::convertible_to<bool>;
    
    // Cluster operations - return generic futures
    { node.add_server(node_id{}) } -> std::same_as<FutureType>;
    { node.remove_server(node_id{}) } -> std::same_as<FutureType>;
};

} // namespace kythira
```

**Concrete Implementation:**
- `kythira::node<FutureType, NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager>`: The main Raft node implementation templated on future type and all pluggable components

### 9. Commit Waiting Mechanism

The commit waiting mechanism ensures that client operations wait for actual durability before completing, using generic future concepts.

#### CommitWaiter Component

```cpp
namespace kythira {

template<typename FutureType>
requires future<FutureType, std::vector<std::byte>>
class commit_waiter {
private:
    struct pending_operation {
        log_index entry_index;
        // Use generic promise concept with kythira::Promise as default
        typename FutureType::promise_type promise;
        std::chrono::steady_clock::time_point submitted_at;
        std::optional<std::chrono::milliseconds> timeout;
    };
    
    std::unordered_map<log_index, std::vector<pending_operation>> _pending_operations;
    std::mutex _mutex;
    
public:
    // Register a new operation that waits for commit
    auto register_operation(log_index index, std::chrono::milliseconds timeout) 
        -> FutureType;
    
    // Notify that entries up to commit_index are committed and applied
    auto notify_committed_and_applied(log_index commit_index) -> void;
    
    // Cancel all pending operations (e.g., on leadership loss)
    auto cancel_all_operations(const std::string& reason) -> void;
    
    // Cancel operations that have timed out
    auto cancel_timed_out_operations() -> void;
};

} // namespace kythira
```

### 10. Future Collection Mechanism

The future collection mechanism handles multiple async operations efficiently using generic future concepts.

#### FutureCollector Component

```cpp
namespace kythira {

template<typename FutureType, typename T>
requires future<FutureType, T>
class future_collector {
public:
    // Collect futures and wait for majority response
    auto collect_majority(
        std::vector<FutureType> futures,
        std::chrono::milliseconds timeout
    ) -> FutureType;
    
    // Collect all futures with timeout handling
    auto collect_all_with_timeout(
        std::vector<FutureType> futures,
        std::chrono::milliseconds timeout
    ) -> FutureType;
    
    // Cancel all futures in a collection
    auto cancel_collection(std::vector<FutureType>& futures) -> void;
};

} // namespace kythira
```

### 11. Configuration Change Synchronization

The configuration synchronization mechanism ensures safe transitions between cluster configurations using generic future concepts.

#### ConfigurationSynchronizer Component

```cpp
namespace kythira {

template<typename FutureType>
requires future<FutureType, bool>
class configuration_synchronizer {
private:
    enum class config_change_phase {
        none,
        joint_consensus,
        final_configuration
    };
    
    config_change_phase _current_phase{config_change_phase::none};
    std::optional<cluster_configuration> _target_configuration;
    std::optional<typename FutureType::promise_type> _change_promise;
    
public:
    // Start a configuration change with proper synchronization
    auto start_configuration_change(
        const cluster_configuration& new_config,
        std::chrono::milliseconds timeout
    ) -> FutureType;
    
    // Notify that a configuration entry has been committed
    auto notify_configuration_committed(
        const cluster_configuration& config,
        log_index committed_index
    ) -> void;
    
    // Cancel ongoing configuration change
    auto cancel_configuration_change(const std::string& reason) -> void;
    
    // Check if a configuration change is in progress
    auto is_configuration_change_in_progress() const -> bool;
};

} // namespace kythira
```

### 12. Comprehensive Error Handling

The error handling system provides robust retry and recovery mechanisms for all network operations using generic future concepts.

#### ErrorHandler Component

```cpp
namespace kythira {

template<typename FutureType, typename Result>
requires future<FutureType, Result>
class error_handler {
private:
    struct retry_policy {
        std::chrono::milliseconds initial_delay{100};
        std::chrono::milliseconds max_delay{5000};
        double backoff_multiplier{2.0};
        std::size_t max_attempts{5};
    };
    
    std::unordered_map<std::string, retry_policy> _retry_policies;
    
public:
    // Execute operation with retry and error handling
    template<typename Operation>
    auto execute_with_retry(
        const std::string& operation_name,
        Operation&& op,
        const retry_policy& policy = {}
    ) -> FutureType;
    
    // Handle specific error types
    auto handle_network_error(const std::exception& e) -> bool;
    auto handle_timeout_error(const std::exception& e) -> bool;
    auto handle_serialization_error(const std::exception& e) -> bool;
    
    // Configure retry policies for different operations
    auto set_retry_policy(const std::string& operation, const retry_policy& policy) -> void;
};

} // namespace kythira
```

## Data Models

### Core Raft Types

```cpp
// Node identifier concept - can be any unsigned integer or string
template<typename T>
concept node_id = std::unsigned_integral<T> || std::same_as<T, std::string>;

// Term number concept (monotonically increasing unsigned integer)
template<typename T>
concept term_id = std::unsigned_integral<T>;

// Log index concept (1-based, monotonically increasing unsigned integer)
template<typename T>
concept log_index = std::unsigned_integral<T>;

// Server states
enum class server_state {
    follower,
    candidate,
    leader
};

// Log entry concept
template<typename T, typename TermId, typename LogIndex>
concept log_entry_type = requires(T entry) {
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { entry.term() } -> std::same_as<TermId>;
    { entry.index() } -> std::same_as<LogIndex>;
    { entry.command() } -> std::same_as<const std::vector<std::byte>&>;
};

// Cluster configuration concept
template<typename T, typename NodeId>
concept cluster_configuration_type = requires(T config) {
    requires node_id<NodeId>;
    { config.nodes() } -> std::same_as<const std::vector<NodeId>&>;
    { config.is_joint_consensus() } -> std::convertible_to<bool>;
    { config.old_nodes() } -> std::same_as<std::optional<std::vector<NodeId>>>;
};

// Snapshot concept
template<typename T, typename NodeId, typename TermId, typename LogIndex>
concept snapshot_type = requires(T snap) {
    requires node_id<NodeId>;
    requires term_id<TermId>;
    requires log_index<LogIndex>;
    { snap.last_included_index() } -> std::same_as<LogIndex>;
    { snap.last_included_term() } -> std::same_as<TermId>;
    { snap.configuration() }; // Returns cluster_configuration_type
    { snap.state_machine_state() } -> std::same_as<const std::vector<std::byte>&>;
};
```

### RPC Message Type Concepts

The RPC message types are defined as concepts to allow for flexible implementations while ensuring required fields are present.

```cpp
// RequestVote RPC Request
template<typename T, typename NodeId>
concept request_vote_request_type = requires(T req) {
    requires node_id<NodeId>;
    { req.term() } -> std::same_as<term_id>;
    { req.candidate_id() } -> std::same_as<NodeId>;
    { req.last_log_index() } -> std::same_as<log_index>;
    { req.last_log_term() } -> std::same_as<term_id>;
};

// RequestVote RPC Response
template<typename T>
concept request_vote_response_type = requires(T resp) {
    { resp.term() } -> std::same_as<term_id>;
    { resp.vote_granted() } -> std::convertible_to<bool>;
};

// AppendEntries RPC Request
template<typename T, typename NodeId>
concept append_entries_request_type = requires(T req) {
    requires node_id<NodeId>;
    { req.term() } -> std::same_as<term_id>;
    { req.leader_id() } -> std::same_as<NodeId>;
    { req.prev_log_index() } -> std::same_as<log_index>;
    { req.prev_log_term() } -> std::same_as<term_id>;
    { req.entries() } -> std::same_as<const std::vector<log_entry>&>;
    { req.leader_commit() } -> std::same_as<log_index>;
};

// AppendEntries RPC Response
template<typename T>
concept append_entries_response_type = requires(T resp) {
    { resp.term() } -> std::same_as<term_id>;
    { resp.success() } -> std::convertible_to<bool>;
    { resp.conflict_index() } -> std::same_as<std::optional<log_index>>;
    { resp.conflict_term() } -> std::same_as<std::optional<term_id>>;
};

// InstallSnapshot RPC Request
template<typename T, typename NodeId>
concept install_snapshot_request_type = requires(T req) {
    requires node_id<NodeId>;
    { req.term() } -> std::same_as<term_id>;
    { req.leader_id() } -> std::same_as<NodeId>;
    { req.last_included_index() } -> std::same_as<log_index>;
    { req.last_included_term() } -> std::same_as<term_id>;
    { req.offset() } -> std::same_as<std::size_t>;
    { req.data() } -> std::same_as<const std::vector<std::byte>&>;
    { req.done() } -> std::convertible_to<bool>;
};

// InstallSnapshot RPC Response
template<typename T>
concept install_snapshot_response_type = requires(T resp) {
    { resp.term() } -> std::same_as<term_id>;
};
```

### Default RPC Message Implementations

Concrete implementations of the RPC message concepts:

```cpp
// RequestVote RPC
struct request_vote_request {
    term_id _term;
    node_id _candidate_id;
    log_index _last_log_index;
    term_id _last_log_term;
    
    auto term() const -> term_id { return _term; }
    auto candidate_id() const -> node_id { return _candidate_id; }
    auto last_log_index() const -> log_index { return _last_log_index; }
    auto last_log_term() const -> term_id { return _last_log_term; }
};

struct request_vote_response {
    term_id _term;
    bool _vote_granted;
    
    auto term() const -> term_id { return _term; }
    auto vote_granted() const -> bool { return _vote_granted; }
};

// AppendEntries RPC
struct append_entries_request {
    term_id _term;
    node_id _leader_id;
    log_index _prev_log_index;
    term_id _prev_log_term;
    std::vector<log_entry> _entries;
    log_index _leader_commit;
    
    auto term() const -> term_id { return _term; }
    auto leader_id() const -> node_id { return _leader_id; }
    auto prev_log_index() const -> log_index { return _prev_log_index; }
    auto prev_log_term() const -> term_id { return _prev_log_term; }
    auto entries() const -> const std::vector<log_entry>& { return _entries; }
    auto leader_commit() const -> log_index { return _leader_commit; }
};

struct append_entries_response {
    term_id _term;
    bool _success;
    std::optional<log_index> _conflict_index;  // Optimization
    std::optional<term_id> _conflict_term;     // Optimization
    
    auto term() const -> term_id { return _term; }
    auto success() const -> bool { return _success; }
    auto conflict_index() const -> std::optional<log_index> { return _conflict_index; }
    auto conflict_term() const -> std::optional<term_id> { return _conflict_term; }
};

// InstallSnapshot RPC
struct install_snapshot_request {
    term_id _term;
    node_id _leader_id;
    log_index _last_included_index;
    term_id _last_included_term;
    std::size_t _offset;
    std::vector<std::byte> _data;
    bool _done;
    
    auto term() const -> term_id { return _term; }
    auto leader_id() const -> node_id { return _leader_id; }
    auto last_included_index() const -> log_index { return _last_included_index; }
    auto last_included_term() const -> term_id { return _last_included_term; }
    auto offset() const -> std::size_t { return _offset; }
    auto data() const -> const std::vector<std::byte>& { return _data; }
    auto done() const -> bool { return _done; }
};

struct install_snapshot_response {
    term_id _term;
    
    auto term() const -> term_id { return _term; }
};
```

### Enhanced Raft Node State

The existing node state is extended with new components for async coordination using the unified types parameter:

```cpp
namespace kythira {

template<raft_types Types = default_raft_types>
class node {
    // Type aliases from unified Types parameter
    using future_type = typename Types::future_type;
    using network_client_type = typename Types::network_client_type;
    using network_server_type = typename Types::network_server_type;
    using persistence_engine_type = typename Types::persistence_engine_type;
    using logger_type = typename Types::logger_type;
    using metrics_type = typename Types::metrics_type;
    using membership_manager_type = typename Types::membership_manager_type;
    using node_id_type = typename Types::node_id_type;
    using term_id_type = typename Types::term_id_type;
    using log_index_type = typename Types::log_index_type;
    
private:
    // Existing persistent state (stored before responding to RPCs)
    term_id_type _current_term;
    std::optional<node_id_type> _voted_for;
    std::vector<log_entry> _log;
    
    // Existing volatile state (all servers)
    log_index_type _commit_index;
    log_index_type _last_applied;
    server_state _state;
    
    // Existing volatile state (leaders only)
    std::unordered_map<node_id_type, log_index_type> _next_index;
    std::unordered_map<node_id_type, log_index_type> _match_index;
    
    // Components using types from unified Types parameter
    network_client_type _network_client;
    network_server_type _network_server;
    persistence_engine_type _persistence;
    logger_type _logger;
    metrics_type _metrics;
    membership_manager_type _membership;
    
    // New async coordination components using unified types
    commit_waiter<future_type> _commit_waiter;
    future_collector<future_type, append_entries_response> _heartbeat_collector;
    future_collector<future_type, request_vote_response> _election_collector;
    configuration_synchronizer<future_type> _config_synchronizer;
    error_handler<future_type, append_entries_response> _append_entries_error_handler;
    error_handler<future_type, request_vote_response> _vote_error_handler;
    error_handler<future_type, install_snapshot_response> _snapshot_error_handler;
    
    // Enhanced state tracking
    std::unordered_map<log_index_type, std::chrono::steady_clock::time_point> _entry_timestamps;
    std::unordered_set<node_id_type> _unresponsive_followers;
    
    // Existing timing
    std::chrono::milliseconds _election_timeout;
    std::chrono::milliseconds _heartbeat_interval;
    std::chrono::steady_clock::time_point _last_heartbeat;
    
    // Enhanced configuration
    cluster_configuration _configuration;
    node_id_type _node_id;
    std::chrono::milliseconds _commit_timeout{30000};
    std::chrono::milliseconds _rpc_timeout{5000};
    std::chrono::milliseconds _heartbeat_lease_duration{1000};
    
public:
    // Clean interface using unified types
    auto submit_command(const std::vector<std::byte>& command, std::chrono::milliseconds timeout) -> future_type;
    auto read_state(std::chrono::milliseconds timeout) -> future_type;
    auto add_server(node_id_type server_id) -> future_type;
    auto remove_server(node_id_type server_id) -> future_type;
    // ... other methods
};

} // namespace kythira
```ollector;
    future_collector<request_vote_response> _election_collector;
    configuration_synchronizer<kythira::Future<bool>> _config_synchronizer;
    error_handler<append_entries_response> _append_entries_error_handler;
    error_handler<request_vote_response> _vote_error_handler;
    error_handler<install_snapshot_response> _snapshot_error_handler;
    
    // Enhanced state tracking
    std::unordered_map<log_index, std::chrono::steady_clock::time_point> _entry_timestamps;
    std::unordered_set<NodeId> _unresponsive_followers;
    
    // Existing timing
    std::chrono::milliseconds _election_timeout;
    std::chrono::milliseconds _heartbeat_interval;
    std::chrono::steady_clock::time_point _last_heartbeat;
    
    // Enhanced configuration
    cluster_configuration _configuration;
    node_id _node_id;
    std::chrono::milliseconds _commit_timeout{30000};
    std::chrono::milliseconds _rpc_timeout{5000};
    std::chrono::milliseconds _heartbeat_lease_duration{1000};
};
```

### Completion-Related Data Models

#### Pending Operation Tracking

```cpp
struct pending_client_operation {
    log_index entry_index;
    std::vector<std::byte> command;
    std::chrono::steady_clock::time_point submitted_at;
    std::chrono::milliseconds timeout;
    kythira::Promise<std::vector<std::byte>> promise;
    
    auto is_timed_out() const -> bool {
        auto elapsed = std::chrono::steady_clock::now() - submitted_at;
        return elapsed > timeout;
    }
};
```

#### Configuration Change State

```cpp
struct configuration_change_state {
    enum class phase {
        none,
        joint_consensus_pending,
        joint_consensus_committed,
        final_configuration_pending,
        final_configuration_committed
    };
    
    phase current_phase{phase::none};
    cluster_configuration old_configuration;
    cluster_configuration new_configuration;
    log_index joint_config_index{0};
    log_index final_config_index{0};
    std::chrono::steady_clock::time_point started_at;
    kythira::Promise<bool> completion_promise;
};
```

## Error Handling

### Enhanced Exception Hierarchy

```cpp
class raft_exception : public std::runtime_error {
public:
    explicit raft_exception(const std::string& message);
};

class network_exception : public raft_exception {
public:
    explicit network_exception(const std::string& message);
};

class persistence_exception : public raft_exception {
public:
    explicit persistence_exception(const std::string& message);
};

class serialization_exception : public raft_exception {
public:
    explicit serialization_exception(const std::string& message);
};

class election_exception : public raft_exception {
public:
    explicit election_exception(const std::string& message);
};

// Completion-specific exceptions
class raft_completion_exception : public raft_exception {
public:
    explicit raft_completion_exception(const std::string& message);
};

class commit_timeout_exception : public raft_completion_exception {
public:
    commit_timeout_exception(log_index index, std::chrono::milliseconds timeout);
    auto get_entry_index() const -> log_index { return _entry_index; }
    auto get_timeout() const -> std::chrono::milliseconds { return _timeout; }
    
private:
    log_index _entry_index;
    std::chrono::milliseconds _timeout;
};

class leadership_lost_exception : public raft_completion_exception {
public:
    leadership_lost_exception(term_id old_term, term_id new_term);
    auto get_old_term() const -> term_id { return _old_term; }
    auto get_new_term() const -> term_id { return _new_term; }
    
private:
    term_id _old_term;
    term_id _new_term;
};

class future_collection_exception : public raft_completion_exception {
public:
    future_collection_exception(const std::string& operation, std::size_t failed_count);
    auto get_operation() const -> const std::string& { return _operation; }
    auto get_failed_count() const -> std::size_t { return _failed_count; }
    
private:
    std::string _operation;
    std::size_t _failed_count;
};

class configuration_change_exception : public raft_completion_exception {
public:
    configuration_change_exception(const std::string& phase, const std::string& reason);
    auto get_phase() const -> const std::string& { return _phase; }
    auto get_reason() const -> const std::string& { return _reason; }
    
private:
    std::string _phase;
    std::string _reason;
};
```

### Enhanced Error Handling Strategy

1. **Network Errors**: Retry with exponential backoff up to configured limits using ErrorHandler
2. **Persistence Errors**: Fatal - log and terminate to prevent data corruption
3. **Serialization Errors**: Reject malformed messages and log warnings
4. **Election Timeouts**: Normal operation - trigger new election
5. **RPC Failures**: Retry according to Raft protocol requirements with comprehensive error handling
6. **Commit Timeout Recovery**: Cancel timed-out operations and notify clients
7. **Leadership Loss Recovery**: Cancel all pending operations and clean up state
8. **Configuration Change Failure Recovery**: Rollback to previous stable configuration
9. **Future Collection Failures**: Handle individual future failures without blocking collections

## Testing Strategy

### Unit Testing

Unit tests will verify individual components in isolation:

- RPC serializer round-trip tests for all message types
- Network client/server mock-based tests
- Persistence engine CRUD operations
- Logger output verification
- Membership manager validation logic

### Property-Based Testing

Property-based tests will verify Raft safety and liveness properties using a property testing framework (e.g., RapidCheck for C++):

- **Election Safety**: At most one leader per term
- **Leader Append-Only**: Leaders never overwrite or delete log entries
- **Log Matching**: Logs with same index/term are identical up to that point
- **Leader Completeness**: Committed entries appear in all future leader logs
- **State Machine Safety**: No two servers apply different commands at same index

### Integration Testing

Integration tests will verify end-to-end scenarios:

- Leader election with various failure patterns
- Log replication with network partitions
- Cluster membership changes
- Snapshot creation and installation
- Client request processing

### Network Simulator Testing

The network simulator transport will enable controlled testing of:

- Network partitions and healing
- Message delays and reordering
- Packet loss scenarios
- Byzantine failure detection

## Implementation Notes

### Thread Safety

The Raft node implementation will use:
- Mutex protection for shared state
- Lock-free data structures where appropriate
- Separate threads for election timer, heartbeat timer, and RPC handling

### Performance Optimizations

1. **Batch Log Replication**: Send multiple entries in single AppendEntries RPC
2. **Pipeline RPCs**: Don't wait for responses before sending next batch
3. **Fast Log Matching**: Include conflict term/index in AppendEntries response
4. **Read-Only Optimization**: Lease-based reads without log entries
5. **Snapshot Streaming**: Transfer snapshots in chunks

### Configuration Parameters

```cpp
// Raft configuration concept
template<typename T>
concept raft_configuration_type = requires(T config) {
    { config.election_timeout_min() } -> std::same_as<std::chrono::milliseconds>;
    { config.election_timeout_max() } -> std::same_as<std::chrono::milliseconds>;
    { config.heartbeat_interval() } -> std::same_as<std::chrono::milliseconds>;
    { config.rpc_timeout() } -> std::same_as<std::chrono::milliseconds>;
    { config.max_entries_per_append() } -> std::same_as<std::size_t>;
    { config.snapshot_threshold_bytes() } -> std::same_as<std::size_t>;
    { config.snapshot_chunk_size() } -> std::same_as<std::size_t>;
};

// Default raft configuration implementation
struct raft_configuration {
    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{100};
    std::size_t _max_entries_per_append{100};
    std::size_t _snapshot_threshold_bytes{10'000'000};
    std::size_t _snapshot_chunk_size{1'000'000};
    
    auto election_timeout_min() const -> std::chrono::milliseconds { return _election_timeout_min; }
    auto election_timeout_max() const -> std::chrono::milliseconds { return _election_timeout_max; }
    auto heartbeat_interval() const -> std::chrono::milliseconds { return _heartbeat_interval; }
    auto rpc_timeout() const -> std::chrono::milliseconds { return _rpc_timeout; }
    auto max_entries_per_append() const -> std::size_t { return _max_entries_per_append; }
    auto snapshot_threshold_bytes() const -> std::size_t { return _snapshot_threshold_bytes; }
    auto snapshot_chunk_size() const -> std::size_t { return _snapshot_chunk_size; }
};
```


## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

Based on the prework analysis, we have identified the following correctness properties that must hold for the Raft implementation:

### Property 1: Election Safety

*For any* term, at most one leader can be elected in that term.

**Validates: Requirements 6.5**

This is the fundamental safety property for leader election. No matter what sequence of elections, timeouts, and failures occur, the system must never allow two leaders to exist in the same term.

### Property 2: Leader Append-Only

*For any* leader and any log entry in that leader's log, the leader never overwrites or deletes that entry.

**Validates: Requirements 8.1**

Leaders only append new entries to their logs. This property ensures that once a leader has an entry, it remains in the log, which is critical for maintaining log consistency.

### Property 3: Log Matching

*For any* two logs, if they contain entries with the same index and term, then all entries up through that index are identical in both logs.

**Validates: Requirements 7.5**

This property ensures that logs remain consistent across the cluster. If two servers agree on an entry at a particular index, they must agree on all preceding entries.

### Property 4: Leader Completeness

*For any* committed log entry from term T, all leaders elected in terms greater than T contain that entry in their logs.

**Validates: Requirements 8.1, 8.5**

This property ensures that committed entries are never lost. Once an entry is committed, it will appear in the logs of all future leaders, preventing data loss during leader changes.

### Property 5: State Machine Safety

*For any* log index, no two servers apply different commands at that index to their state machines.

**Validates: Requirements 8.4**

This is the ultimate safety property for the replicated state machine. It ensures that all servers execute the same sequence of commands, maintaining consistency across the cluster.

### Property 6: RPC Serialization Round-Trip

*For any* valid Raft RPC message (RequestVote, AppendEntries, or InstallSnapshot), serializing then deserializing the message produces an equivalent message with all fields preserved.

**Validates: Requirements 2.5**

This property ensures that serialization implementations correctly preserve all message data. It's critical for maintaining protocol correctness across network boundaries.

### Property 7: Malformed Message Rejection

*For any* byte sequence that does not represent a valid Raft RPC message, the deserializer rejects it with an appropriate error.

**Validates: Requirements 2.6**

This property ensures that the system handles invalid input gracefully and doesn't process corrupted or malicious messages.

### Property 8: Network Retry Convergence

*For any* RPC that fails due to network issues, the system retries according to Raft timeout requirements and eventually either succeeds or determines the target is unreachable.

**Validates: Requirements 3.12**

This property ensures that transient network failures don't prevent the system from making progress, while permanent failures are detected appropriately.

### Property 9: Persistence Before Response

*For any* RPC that requires persistent state changes (RequestVote, AppendEntries), the system durably stores the state before sending the response.

**Validates: Requirements 5.5**

This property ensures that the system can recover correctly after crashes. State must be persisted before acknowledging operations to prevent inconsistencies.

### Property 10: Persistence Round-Trip

*For any* Raft state (currentTerm, votedFor, log entries), saving then loading the state produces equivalent state.

**Validates: Requirements 5.6**

This property ensures that persistence implementations correctly preserve all state data across save/load cycles, which is critical for crash recovery.

### Property 11: Log Convergence

*For any* two servers with divergent logs, when one becomes leader, the follower's log eventually converges to match the leader's log.

**Validates: Requirements 7.3**

This property ensures that log inconsistencies are resolved. The leader's log is authoritative, and all followers eventually match it.

### Property 12: Commit Implies Replication

*For any* log entry that is committed, that entry has been replicated to a majority of servers in the cluster.

**Validates: Requirements 7.4**

This property ensures that committed entries are durable. Even if the leader crashes, a majority of servers have the entry, so it will be preserved.

### Property 13: Joint Consensus Majority

*For any* decision made during joint consensus (elections, commits), the decision requires majorities from both the old and new configurations.

**Validates: Requirements 9.4**

This property ensures safety during configuration changes. No unilateral decisions can be made by either configuration alone during the transition.

### Property 14: Snapshot Preserves State

*For any* state machine state, creating a snapshot and then restoring from that snapshot produces equivalent state.

**Validates: Requirements 10.5**

This property ensures that log compaction doesn't lose information. The state machine can be reconstructed from snapshots.

### Property 15: Linearizable Operations

*For any* sequence of client operations, the system ensures linearizable semantics where each operation appears to execute instantaneously at some point between invocation and response.

**Validates: Requirements 1.4**

This property ensures strong consistency for client operations. Operations appear to execute in a total order consistent with real-time ordering.

### Property 16: Majority Availability

*For any* cluster configuration where a majority of servers are operational and can communicate, the cluster remains available for client requests.

**Validates: Requirements 1.6**

This property ensures liveness. As long as a majority is available, the system can make progress.

### Property 17: Crash Recovery

*For any* server that crashes and restarts, the server recovers its state from persistent storage and successfully rejoins the cluster.

**Validates: Requirements 1.7**

This property ensures that servers can recover from crashes without manual intervention or data loss.

### Property 18: Safety Under Partitions

*For any* network partition or failure scenario, the system maintains all safety properties (Election Safety, Log Matching, State Machine Safety).

**Validates: Requirements 1.5**

This property ensures that safety is never violated, even during network failures. The system may become unavailable but never inconsistent.

### Property 19: Duplicate Detection

*For any* client operation with a serial number, if the operation is retried, the system detects the duplicate and returns the cached response without re-executing.

**Validates: Requirements 11.4**

This property ensures exactly-once semantics for client operations, preventing duplicate execution during retries.

### Property 20: Liveness After Partition Healing

*For any* network partition that heals, the system restores liveness and resumes normal operation.

**Validates: Requirements 13.5**

This property ensures that the system recovers from partitions. Once connectivity is restored, the cluster returns to normal operation.

### Property 21: State Transition Logging

*For any* Raft state transition (follower→candidate, candidate→leader, etc.), the system logs the transition with appropriate severity and context.

**Validates: Requirements 4.6**

This property ensures observability. All important state changes are logged for debugging and monitoring.

### Property 22: Higher Term Causes Follower Transition

*For any* server (candidate or leader) that discovers a higher term, the server immediately transitions to follower state.

**Validates: Requirements 6.4**

This property ensures that servers respect term numbers. Discovering a higher term means the server's information is stale, so it must step down.

### Completion Properties

The following properties ensure the correctness of the completion components:

### Property 23: Commit Waiting Completion

*For any* client command submission, the returned future completes only after the command is both committed (replicated to majority) and applied to the state machine.

**Validates: Requirements 15.1, 15.2**

### Property 24: Application Before Future Fulfillment

*For any* committed log entry with associated client futures, state machine application occurs before any client future is fulfilled.

**Validates: Requirements 15.2**

### Property 25: Error Propagation on Application Failure

*For any* state machine application failure, the error is propagated to all associated client futures.

**Validates: Requirements 15.3**

### Property 26: Leadership Loss Rejection

*For any* client operation submitted to a leader that loses leadership before commit, the associated future is rejected with an appropriate error.

**Validates: Requirements 15.4**

### Property 27: Sequential Application Order

*For any* set of concurrently submitted commands, they are applied to the state machine in log order regardless of submission timing.

**Validates: Requirements 15.5**

### Property 28: Heartbeat Majority Collection

*For any* heartbeat operation, the system waits for majority response before completing the operation.

**Validates: Requirements 16.1**

### Property 29: Election Vote Collection

*For any* leader election, vote collection determines outcome based on majority votes received.

**Validates: Requirements 16.2**

### Property 30: Replication Majority Acknowledgment

*For any* log entry replication, commit index advances only when majority acknowledgment is received.

**Validates: Requirements 16.3**

### Property 31: Timeout Handling in Collections

*For any* future collection with timeouts, individual timeouts are handled without blocking other operations.

**Validates: Requirements 16.4**

### Property 32: Collection Cancellation Cleanup

*For any* cancelled future collection operation, all pending futures are properly cleaned up.

**Validates: Requirements 16.5**

### Property 33: Joint Consensus Synchronization

*For any* server addition, the system waits for joint consensus configuration commit before proceeding to final configuration.

**Validates: Requirements 17.1**

### Property 34: Configuration Phase Synchronization

*For any* server removal, each configuration phase waits for commit before proceeding to the next phase.

**Validates: Requirements 17.2**

### Property 35: Configuration Change Serialization

*For any* configuration change attempt while another is in progress, the new change is prevented until the current change completes.

**Validates: Requirements 17.3**

### Property 36: Configuration Rollback on Failure

*For any* configuration change failure during any phase, the system rolls back to the previous stable configuration.

**Validates: Requirements 17.4**

### Property 37: Leadership Change During Configuration

*For any* leadership change during configuration change, the system properly handles the transition and continues or aborts appropriately.

**Validates: Requirements 17.5**

### Property 38: Heartbeat Retry with Backoff

*For any* heartbeat RPC failure, the system retries with exponential backoff up to configured limits.

**Validates: Requirements 18.1**

### Property 39: AppendEntries Retry Handling

*For any* AppendEntries RPC failure, the system retries and handles different failure modes appropriately.

**Validates: Requirements 18.2**

### Property 40: Snapshot Transfer Retry

*For any* InstallSnapshot RPC failure, the system retries snapshot transfer with proper error recovery.

**Validates: Requirements 18.3**

### Property 41: Vote Request Failure Handling

*For any* RequestVote RPC failure during election, the system handles the failure and continues the election process.

**Validates: Requirements 18.4**

### Property 42: Partition Detection and Handling

*For any* network partition, the system detects the partition and handles it according to Raft safety requirements.

**Validates: Requirements 18.5**

### Property 43: Timeout Classification

*For any* RPC timeout, the system distinguishes between network delays and actual failures.

**Validates: Requirements 18.6**

### Property 44: Batch Entry Application

*For any* commit index advance, all entries between old and new commit index are applied to the state machine.

**Validates: Requirements 19.1**

### Property 45: Sequential Application Ordering

*For any* state machine application operation, entries are applied in increasing log index order.

**Validates: Requirements 19.2**

### Property 46: Application Success Handling

*For any* successful state machine application, the applied index is updated and waiting client futures are fulfilled.

**Validates: Requirements 19.3**

### Property 47: Application Failure Handling

*For any* state machine application failure, further application is halted and the error is reported.

**Validates: Requirements 19.4**

### Property 48: Applied Index Catch-up

*For any* scenario where applied index lags behind commit index, the system catches up by applying pending entries.

**Validates: Requirements 19.5**

### Property 49: Follower Acknowledgment Tracking

*For any* entry replication to followers, the system tracks which followers have acknowledged each entry.

**Validates: Requirements 20.1**

### Property 50: Majority Commit Index Advancement

*For any* entry acknowledged by majority of followers, the commit index advances to include that entry.

**Validates: Requirements 20.2**

### Property 51: Non-blocking Slow Followers

*For any* slow follower responses, the system continues replication without blocking other operations.

**Validates: Requirements 20.3**

### Property 52: Unresponsive Follower Handling

*For any* consistently unresponsive follower, the system marks it unavailable but continues with majority.

**Validates: Requirements 20.4**

### Property 53: Leader Self-acknowledgment

*For any* commit decision, the leader includes itself in majority calculations.

**Validates: Requirements 20.5**

### Property 54: Read Linearizability Verification

*For any* read_state operation, leader status is verified by collecting heartbeat responses from majority.

**Validates: Requirements 21.1**

### Property 55: Successful Read State Return

*For any* successful heartbeat collection during read, the current state machine state is returned.

**Validates: Requirements 21.2**

### Property 56: Failed Read Rejection

*For any* failed heartbeat collection during read, the read request is rejected with leadership error.

**Validates: Requirements 21.3**

### Property 57: Read Abortion on Leadership Loss

*For any* leadership loss during read operation, the read is aborted and error is returned.

**Validates: Requirements 21.4**

### Property 58: Concurrent Read Efficiency

*For any* concurrent read operations, they are handled efficiently without unnecessary heartbeat overhead.

**Validates: Requirements 21.5**

### Property 59: Shutdown Cleanup

*For any* node shutdown, all pending futures are cancelled and resources are cleaned up.

**Validates: Requirements 22.1**

### Property 60: Step-down Operation Cancellation

*For any* leader step-down, all pending client operations are cancelled with appropriate errors.

**Validates: Requirements 22.2**

### Property 61: Timeout Cancellation Cleanup

*For any* operation timeout, the associated future is cancelled and related state is cleaned up.

**Validates: Requirements 22.3**

### Property 62: Callback Safety After Cancellation

*For any* cancelled future, no callbacks are invoked after cancellation.

**Validates: Requirements 22.4**

### Property 63: Resource Leak Prevention

*For any* future cleanup operation, memory leaks and resource exhaustion are prevented.

**Validates: Requirements 22.5**

### Property 64: RPC Timeout Configuration

*For any* RPC timeout configuration, separate timeout values for different RPC types are supported.

**Validates: Requirements 23.1**

### Property 65: Retry Policy Configuration

*For any* retry policy configuration, exponential backoff with configurable parameters is supported.

**Validates: Requirements 23.2**

### Property 66: Heartbeat Interval Compatibility

*For any* heartbeat interval configuration, compatibility with election timeouts is ensured.

**Validates: Requirements 23.3**

### Property 67: Adaptive Timeout Behavior

*For any* network condition change, timeout and retry behavior adapts within configured bounds.

**Validates: Requirements 23.4**

### Property 68: Configuration Validation

*For any* invalid timeout configuration, the system rejects it with clear error messages.

**Validates: Requirements 23.5**

### Property 69: RPC Error Logging

*For any* RPC operation failure, detailed error information including failure type, target node, and retry attempts is logged.

**Validates: Requirements 24.1**

### Property 70: Commit Timeout Logging

*For any* commit waiting timeout, the timeout is logged with context about pending operations.

**Validates: Requirements 24.2**

### Property 71: Configuration Failure Logging

*For any* configuration change failure, the failure reason and current cluster state are logged.

**Validates: Requirements 24.3**

### Property 72: Collection Error Logging

*For any* future collection error, which futures failed and why is logged.

**Validates: Requirements 24.4**

### Property 73: Application Failure Logging

*For any* state machine application failure, the failing entry and error details are logged.

**Validates: Requirements 24.5**

### Property 74: Kythira Future Type Safety

*For any* async operation, kythira::Future<T> wrapper classes maintain type safety and concept compliance.

**Validates: Requirements 25.1, 25.2, 25.3, 25.4, 25.5**

};
```

### Default Implementations of Core Types

```cpp
// Default log entry implementation
template<term_id TermId, log_index LogIndex>
struct log_entry {
    TermId _term;
    LogIndex _index;
    std::vector<std::byte> _command;
    
    auto term() const -> TermId { return _term; }
    auto index() const -> LogIndex { return _index; }
    auto command() const -> const std::vector<std::byte>& { return _command; }
};

// Default cluster configuration implementation
template<node_id NodeId>
struct cluster_configuration {
    std::vector<NodeId> _nodes;
    bool _is_joint_consensus;
    std::optional<std::vector<NodeId>> _old_nodes;
    
    auto nodes() const -> const std::vector<NodeId>& { return _nodes; }
    auto is_joint_consensus() const -> bool { return _is_joint_consensus; }
    auto old_nodes() const -> std::optional<std::vector<NodeId>> { return _old_nodes; }
};

// Default snapshot implementation
template<node_id NodeId, term_id TermId, log_index LogIndex>
struct snapshot {
    LogIndex _last_included_index;
    TermId _last_included_term;
    cluster_configuration<NodeId> _configuration;
    std::vector<std::byte> _state_machine_state;
    
    auto last_included_index() const -> LogIndex { return _last_included_index; }
    auto last_included_term() const -> TermId { return _last_included_term; }
    auto configuration() const -> const cluster_configuration<NodeId>& { return _configuration; }
    auto state_machine_state() const -> const std::vector<std::byte>& { return _state_machine_state; }
};
```


## Raft Implementation Completion Details

This section provides detailed design information for the completion items identified in the TODO document's "Raft Implementation Completion" section.

### Complete Future Collection Mechanisms

The future collection mechanisms enable efficient coordination of multiple asynchronous operations without blocking:

#### Heartbeat Response Collection
- **Purpose**: Collect heartbeat responses from followers to verify leader validity for linearizable reads
- **Implementation**: Use `FutureCollector<FutureType, append_entries_response>` to collect responses
- **Majority Logic**: Wait for responses from majority of followers before confirming leader status
- **Timeout Handling**: Individual follower timeouts don't block the collection; proceed when majority responds
- **Optimization**: Batch concurrent read requests to share heartbeat collection overhead

#### Election Vote Collection
- **Purpose**: Collect vote responses during leader election to determine election outcome
- **Implementation**: Use `FutureCollector<FutureType, request_vote_response>` to collect votes
- **Majority Logic**: Transition to leader when majority of votes are received
- **Timeout Handling**: Continue election even if some followers don't respond
- **Split Vote Prevention**: Randomized election timeouts prevent repeated split votes

#### Replication Acknowledgment Collection
- **Purpose**: Track follower acknowledgments to determine when entries are committed
- **Implementation**: Use follower acknowledgment tracking with match_index updates
- **Majority Logic**: Advance commit index when majority of followers acknowledge replication
- **Slow Follower Handling**: Continue with majority; don't block on slow followers
- **Leader Self-Acknowledgment**: Include leader in majority calculations

### Proper Heartbeat Response Collection for Linearizable Reads

The read_state operation ensures linearizable reads by verifying leader validity:

```cpp
namespace kythira {

template<raft_types Types>
auto node<Types>::read_state(std::chrono::milliseconds timeout) -> future_type {
    // Verify we are the leader
    if (_state != server_state::leader) {
        return future_factory::make_exception_future<std::vector<std::byte>>(
            leadership_lost_exception(_current_term, _current_term));
    }
    
    // Send heartbeats to all followers
    std::vector<future_type> heartbeat_futures;
    for (const auto& follower : _configuration.nodes()) {
        if (follower == _node_id) continue;  // Skip self
        
        auto heartbeat_future = _network_client.send_append_entries(
            follower,
            create_heartbeat_request(),
            _rpc_timeout
        );
        heartbeat_futures.push_back(std::move(heartbeat_future));
    }
    
    // Collect majority of heartbeat responses
    return _heartbeat_collector.collect_majority(
        std::move(heartbeat_futures),
        timeout
    ).then([this](auto responses) -> std::vector<std::byte> {
        // Check for higher term in responses
        for (const auto& response : responses) {
            if (response.term() > _current_term) {
                step_down_to_follower(response.term());
                throw leadership_lost_exception(_current_term, response.term());
            }
        }
        
        // Leader validity confirmed, return current state
        return get_state_machine_state();
    });
}

} // namespace kythira
```

### Complete Configuration Change Synchronization with Two-Phase Protocol

Configuration changes use a two-phase protocol to ensure safety:

#### Phase 1: Joint Consensus
1. Leader creates joint consensus configuration (C_old,new) containing both old and new configurations
2. Leader appends joint configuration to log and replicates to followers
3. System waits for joint configuration to be committed (replicated to majority of both old and new)
4. During joint consensus, all decisions require majorities from both configurations

#### Phase 2: Final Configuration
1. Once joint configuration is committed, leader creates final configuration (C_new)
2. Leader appends final configuration to log and replicates to followers
3. System waits for final configuration to be committed
4. Once committed, configuration change is complete

#### Implementation Details

```cpp
namespace kythira {

template<raft_types Types>
auto node<Types>::add_server(node_id_type server_id) -> future_type {
    // Validate we are the leader
    if (_state != server_state::leader) {
        return future_factory::make_exception_future<bool>(
            leadership_lost_exception(_current_term, _current_term));
    }
    
    // Check if configuration change is already in progress
    if (_config_synchronizer.is_configuration_change_in_progress()) {
        return future_factory::make_exception_future<bool>(
            configuration_change_exception("add_server", "Configuration change already in progress"));
    }
    
    // Create new configuration with added server
    cluster_configuration new_config = _configuration;
    new_config.add_node(server_id);
    
    // Start configuration change with synchronization
    return _config_synchronizer.start_configuration_change(
        new_config,
        _commit_timeout
    );
}

} // namespace kythira
```

### Proper Timeout Handling for RPC Operations

All RPC operations have configurable timeouts with exponential backoff retry:

#### RequestVote RPC Timeout Handling
- **Initial Timeout**: Configurable per RPC (default: 100ms)
- **Retry Policy**: Exponential backoff with jitter
- **Max Attempts**: Configurable (default: 5 attempts)
- **Failure Handling**: Continue election even if some votes fail

#### AppendEntries RPC Timeout Handling
- **Initial Timeout**: Configurable per RPC (default: 100ms)
- **Retry Policy**: Exponential backoff for transient failures
- **Failure Modes**: Distinguish between network delays and actual failures
- **Slow Follower Handling**: Mark as unresponsive but continue with majority

#### InstallSnapshot RPC Timeout Handling
- **Initial Timeout**: Longer timeout for large transfers (default: 5000ms)
- **Retry Policy**: Resume interrupted transfers from last successful chunk
- **Chunk-based Transfer**: Transfer snapshots in chunks to handle timeouts gracefully
- **Progress Tracking**: Track transfer progress to resume efficiently

#### Timeout Configuration Validation

```cpp
struct rpc_timeout_configuration {
    std::chrono::milliseconds request_vote_timeout{100};
    std::chrono::milliseconds append_entries_timeout{100};
    std::chrono::milliseconds install_snapshot_timeout{5000};
    std::chrono::milliseconds heartbeat_timeout{50};
    
    // Validation: ensure timeouts are compatible with election timeout
    auto validate(std::chrono::milliseconds election_timeout_min) const -> bool {
        return heartbeat_timeout * 3 < election_timeout_min &&
               request_vote_timeout < election_timeout_min &&
               append_entries_timeout < election_timeout_min;
    }
};
```

### Complete Snapshot Installation and Log Compaction

Snapshot installation and log compaction manage storage efficiently:

#### Snapshot Creation
- **Trigger**: Log size exceeds configured threshold (default: 10MB)
- **Metadata**: Includes last_included_index, last_included_term, and cluster configuration
- **State Capture**: Captures complete state machine state at snapshot point
- **Atomic Operation**: Snapshot creation is atomic to ensure consistency

#### Snapshot Transfer
- **Chunked Transfer**: Snapshots are transferred in configurable chunks (default: 1MB)
- **Resume Capability**: Failed transfers can resume from last successful chunk
- **Progress Tracking**: Track transfer progress for monitoring and debugging
- **Retry Logic**: Exponential backoff retry for failed chunk transfers

#### Log Compaction
- **Safe Deletion**: Only delete log entries covered by committed snapshot
- **Metadata Preservation**: Keep snapshot metadata for recovery
- **Atomic Operation**: Log deletion is atomic to prevent inconsistencies
- **Validation**: Verify snapshot integrity before deleting log entries

#### Implementation Flow

```cpp
namespace kythira {

template<raft_types Types>
auto node<Types>::create_snapshot() -> void {
    // Check if snapshot is needed
    auto log_size = calculate_log_size();
    if (log_size < _config.snapshot_threshold_bytes()) {
        return;
    }
    
    // Capture state machine state
    auto state = _state_machine.capture_state();
    
    // Create snapshot with metadata
    snapshot snap{
        .last_included_index = _last_applied,
        .last_included_term = _log[_last_applied].term(),
        .configuration = _configuration,
        .state_machine_state = std::move(state)
    };
    
    // Persist snapshot
    _persistence.save_snapshot(snap);
    
    // Compact log (delete entries covered by snapshot)
    _persistence.delete_log_entries_before(_last_applied);
    
    _logger.info("Snapshot created", {
        {"last_included_index", std::to_string(_last_applied)},
        {"log_size_before", std::to_string(log_size)},
        {"log_size_after", std::to_string(calculate_log_size())}
    });
}

template<raft_types Types>
auto node<Types>::install_snapshot(const snapshot& snap) -> void {
    // Validate snapshot
    if (snap.last_included_index() <= _last_applied) {
        _logger.warning("Ignoring stale snapshot", {
            {"snapshot_index", std::to_string(snap.last_included_index())},
            {"last_applied", std::to_string(_last_applied)}
        });
        return;
    }
    
    // Apply snapshot to state machine
    _state_machine.restore_state(snap.state_machine_state());
    
    // Update Raft state
    _last_applied = snap.last_included_index();
    _commit_index = std::max(_commit_index, snap.last_included_index());
    _configuration = snap.configuration();
    
    // Compact log
    _persistence.delete_log_entries_before(snap.last_included_index());
    
    _logger.info("Snapshot installed", {
        {"last_included_index", std::to_string(snap.last_included_index())},
        {"last_included_term", std::to_string(snap.last_included_term())}
    });
}

} // namespace kythira
```

### Integration with Existing Implementation

All completion components integrate seamlessly with the existing Raft implementation:

1. **CommitWaiter**: Integrated into submit_command to provide proper async waiting
2. **FutureCollector**: Used in read_state, start_election, and replication tracking
3. **ConfigurationSynchronizer**: Used in add_server and remove_server operations
4. **ErrorHandler**: Wraps all RPC operations for consistent retry and error handling
5. **Unified Types Parameter**: All components use the raft_types template parameter for clean integration

The completion ensures that the Raft implementation is production-ready with proper async coordination, comprehensive error handling, and efficient resource management.
