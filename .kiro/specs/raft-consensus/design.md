# Design Document

## Overview

This document describes the design of a C++ implementation of the Raft consensus algorithm. The implementation uses C++20/23 concepts to define pluggable interfaces for all major components, enabling flexible deployment across different environments. The design emphasizes type safety through concepts, dependency injection through template parameters, and clear separation of concerns between the consensus algorithm core and its supporting infrastructure.

The Raft implementation follows the algorithm as described in the extended Raft paper by Diego Ongaro and John Ousterhout. It provides strong consistency guarantees through leader election, log replication, and safety mechanisms while maintaining understandability and correctness.

## Architecture

### High-Level Architecture

The Raft implementation is structured in layers:

1. **Concept Layer**: Defines C++ concepts for all pluggable components
2. **Core Algorithm Layer**: Implements the Raft consensus protocol
3. **Component Implementation Layer**: Provides concrete implementations of concepts
4. **Application Layer**: User-defined state machines and client applications

### Component Dependencies

```
Node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager>
    ├── NetworkClient<RpcSerializer, Data>
    │   └── RpcSerializer<Data> (concept: rpc_serializer, Data: serialized_data)
    ├── NetworkServer<RpcSerializer, Data>
    │   └── RpcSerializer<Data> (concept: rpc_serializer, Data: serialized_data)
    ├── PersistenceEngine (concept: persistence_engine)
    ├── Logger (concept: diagnostic_logger)
    ├── Metrics (concept: metrics)
    └── MembershipManager (concept: membership_manager)
```

### Template-Based Dependency Injection

All concrete classes use template parameters to specify their dependencies:

```cpp
// Network client depends on RPC serializer and data type
template<typename Serializer, serialized_data Data>
requires rpc_serializer<Serializer, Data>
class http_network_client;

// Network server depends on RPC serializer and data type
template<typename Serializer, serialized_data Data>
requires rpc_serializer<Serializer, Data>
class http_network_server;

// Raft node depends on all pluggable components
template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager
>
requires 
    network_client<NetworkClient> &&
    network_server<NetworkServer> &&
    persistence_engine<PersistenceEngine> &&
    diagnostic_logger<Logger> &&
    metrics<Metrics> &&
    membership_manager<MembershipManager>
class node;
```

## Components and Interfaces

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

The `network_client` concept defines the interface for sending RPC requests to remote Raft nodes. Network client implementations are templated on an RPC serializer type.

```cpp
template<typename C, typename Future, typename NodeId>
concept network_client = requires(
    C client,
    const NodeId& target,
    const request_vote_request& rvr,
    const append_entries_request& aer,
    const install_snapshot_request& isr,
    std::chrono::milliseconds timeout
) {
    requires node_id<NodeId>;
    
    // Send RequestVote RPC - returns a future
    { client.send_request_vote(target, rvr, timeout) } 
        -> std::same_as<Future<request_vote_response>>;
    
    // Send AppendEntries RPC - returns a future
    { client.send_append_entries(target, aer, timeout) }
        -> std::same_as<Future<append_entries_response>>;
    
    // Send InstallSnapshot RPC - returns a future
    { client.send_install_snapshot(target, isr, timeout) }
        -> std::same_as<Future<install_snapshot_response>>;
};
```

**Concrete Implementations:**
- `http_network_client<Serializer, Data>`: HTTP/1.1 or HTTP/2 transport
- `https_network_client<Serializer, Data>`: HTTPS with TLS
- `coap_network_client<Serializer, Data>`: CoAP over UDP
- `coaps_network_client<Serializer, Data>`: CoAP over DTLS (Datagram TLS)
- `simulator_network_client<Serializer, Data>`: Network simulator transport

### 3. Network Server Concept

The `network_server` concept defines the interface for receiving RPC requests from remote Raft nodes. Network server implementations are templated on an RPC serializer type.

```cpp
template<typename S>
concept network_server = requires(
    S server,
    std::function<request_vote_response(const request_vote_request&)> rv_handler,
    std::function<append_entries_response(const append_entries_request&)> ae_handler,
    std::function<install_snapshot_response(const install_snapshot_request&)> is_handler
) {
    // Register RPC handlers
    { server.register_request_vote_handler(rv_handler) } -> std::same_as<void>;
    { server.register_append_entries_handler(ae_handler) } -> std::same_as<void>;
    { server.register_install_snapshot_handler(is_handler) } -> std::same_as<void>;
    
    // Server lifecycle
    { server.start() } -> std::same_as<void>;
    { server.stop() } -> std::same_as<void>;
    { server.is_running() } -> std::convertible_to<bool>;
};
```

**Concrete Implementations:**
- `http_network_server<Serializer, Data>`: HTTP/1.1 or HTTP/2 transport
- `https_network_server<Serializer, Data>`: HTTPS with TLS
- `coap_network_server<Serializer, Data>`: CoAP over UDP
- `coaps_network_server<Serializer, Data>`: CoAP over DTLS (Datagram TLS)
- `simulator_network_server<Serializer, Data>`: Network simulator transport

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

The `node` concept defines the interface for a Raft node that participates in the consensus protocol.

```cpp
template<typename N, typename Future>
concept raft_node = requires(
    N node,
    const std::vector<std::byte>& command,
    std::chrono::milliseconds timeout
) {
    // Client operations - return futures
    { node.submit_command(command, timeout) } 
        -> std::same_as<Future<std::vector<std::byte>>>;
    { node.read_state(timeout) } 
        -> std::same_as<Future<std::vector<std::byte>>>;
    
    // Node lifecycle
    { node.start() } -> std::same_as<void>;
    { node.stop() } -> std::same_as<void>;
    { node.is_running() } -> std::convertible_to<bool>;
    
    // Node state queries
    { node.get_node_id() } -> std::same_as<node_id>;
    { node.get_current_term() } -> std::same_as<term_id>;
    { node.get_state() } -> std::same_as<server_state>;
    { node.is_leader() } -> std::convertible_to<bool>;
    
    // Cluster operations - return futures
    { node.add_server(node_id{}) } -> std::same_as<Future<bool>>;
    { node.remove_server(node_id{}) } -> std::same_as<Future<bool>>;
};
```

**Concrete Implementation:**
- `node<NetworkClient, NetworkServer, PersistenceEngine, Logger, Metrics, MembershipManager>`: The main Raft node implementation templated on all pluggable components

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

### Raft Node State

```cpp
template<
    typename NetworkClient,
    typename NetworkServer,
    typename PersistenceEngine,
    typename Logger,
    typename Metrics,
    typename MembershipManager
>
class node {
private:
    // Persistent state (stored before responding to RPCs)
    term_id _current_term;
    std::optional<node_id> _voted_for;
    std::vector<log_entry> _log;
    
    // Volatile state (all servers)
    log_index _commit_index;
    log_index _last_applied;
    server_state _state;
    
    // Volatile state (leaders only)
    std::unordered_map<node_id, log_index> _next_index;
    std::unordered_map<node_id, log_index> _match_index;
    
    // Components
    NetworkClient _network_client;
    NetworkServer _network_server;
    PersistenceEngine _persistence;
    Logger _logger;
    Metrics _metrics;
    MembershipManager _membership;
    
    // Timing
    std::chrono::milliseconds _election_timeout;
    std::chrono::milliseconds _heartbeat_interval;
    std::chrono::steady_clock::time_point _last_heartbeat;
    
    // Configuration
    cluster_configuration _configuration;
    node_id _node_id;
};
```

## Error Handling

### Exception Hierarchy

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
```

### Error Handling Strategy

1. **Network Errors**: Retry with exponential backoff up to configured limits
2. **Persistence Errors**: Fatal - log and terminate to prevent data corruption
3. **Serialization Errors**: Reject malformed messages and log warnings
4. **Election Timeouts**: Normal operation - trigger new election
5. **RPC Failures**: Retry according to Raft protocol requirements

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
