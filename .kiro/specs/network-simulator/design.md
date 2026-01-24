# Network Simulator Design Document

## Overview

The Network Simulator is a C++ library that models network communication between nodes using modern C++23 concepts and template metaprogramming. The design provides both connectionless (datagram) and connection-oriented (stream) communication patterns with configurable network characteristics including latency and reliability.

The simulator uses a directed graph topology where nodes are vertices and communication paths are edges weighted by latency and reliability parameters. This allows realistic simulation of network conditions including delays, packet loss, and network partitions.

**Key Design Innovation**: The design uses a single Types template parameter that provides all necessary type definitions through the `network_simulator_types` concept. This eliminates template complexity issues while maintaining type safety and clear interfaces. Each operation returns a specific, strongly-typed future (e.g., `typename Types::future_bool_type`, `typename Types::future_message_type`) rather than using a single generic future template parameter.

## Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     User Application                         │
│  (Client/Server code using network simulator concepts)       │
└────────────────┬────────────────────────────┬────────────────┘
                 │                            │
                 ▼                            ▼
┌────────────────────────────┐  ┌────────────────────────────┐
│   Connectionless API       │  │  Connection-Oriented API   │
│   - send() -> future_bool  │  │  - connect() -> future_conn│
│   - receive() -> future_msg│  │  - bind() -> future_listen │
│                            │  │  - accept() -> future_conn │
│                            │  │  - read() -> future_bytes  │
│                            │  │  - write() -> future_bool  │
└────────────┬───────────────┘  └────────────┬───────────────┘
             │                               │
             └───────────────┬───────────────┘
                             ▼
                ┌────────────────────────────┐
                │      Network Node          │
                │   (Types parameterized)    │
                └────────────┬───────────────┘
                             │
                             ▼
                ┌────────────────────────────┐
                │   Network Simulator Core   │
                │  - Topology Management     │
                │  - Message Routing         │
                │  - Latency Simulation      │
                │  - Reliability Simulation  │
                │  - Lifecycle Control       │
                └────────────┬───────────────┘
                             │
                             ▼
                ┌────────────────────────────┐
                │    Directed Graph Model    │
                │  - Nodes (vertices)        │
                │  - Edges (latency/reliab.) │
                └────────────────────────────┘
```

### Component Layers

1. **Types Layer**: Defines all type contracts through the `network_simulator_types` concept
2. **Concept Layer**: Defines compile-time contracts for individual types
3. **Core Simulator Layer**: Implements network topology and message routing
4. **Node Layer**: Provides network node abstraction with send/receive/connect/bind
5. **Connection Layer**: Implements connection-oriented communication
6. **Exception Layer**: Provides structured error handling

## Components and Interfaces

### 1. Network Simulator Types Concept

The core innovation of this design is the `network_simulator_types` concept that provides all type definitions through a single template parameter:

```cpp
template<typename T>
concept network_simulator_types = requires {
    // Core types
    typename T::address_type;
    typename T::port_type;
    typename T::message_type;
    typename T::connection_type;
    typename T::listener_type;
    
    // Future types for specific operations
    typename T::future_bool_type;
    typename T::future_message_type;
    typename T::future_connection_type;
    typename T::future_listener_type;
    typename T::future_bytes_type;
    
    // Type constraints
    requires address<typename T::address_type>;
    requires port<typename T::port_type>;
    requires message<typename T::message_type, typename T::address_type, typename T::port_type>;
    requires connection<typename T::connection_type>;
    requires listener<typename T::listener_type, typename T::connection_type>;
    
    // Future constraints
    requires future<typename T::future_bool_type, bool>;
    requires future<typename T::future_message_type, typename T::message_type>;
    requires future<typename T::future_connection_type, std::shared_ptr<typename T::connection_type>>;
    requires future<typename T::future_listener_type, std::shared_ptr<typename T::listener_type>>;
    requires future<typename T::future_bytes_type, std::vector<std::byte>>;
};
```

### 2. Individual Type Concepts

#### 2.1 Address Concept

```cpp
template<typename T>
concept address = requires(T a, T b) {
    // Must be copyable and movable
    { T(a) } -> std::same_as<T>;
    { T(std::move(a)) } -> std::same_as<T>;
    
    // Must support equality comparison
    { a == b } -> std::convertible_to<bool>;
    { a != b } -> std::convertible_to<bool>;
    
    // Must be hashable for use in maps
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};
```

**Supported Types:**
- `std::string`
- `unsigned long`
- `in_addr` (IPv4)
- `in6_addr` (IPv6)

#### 2.2 Port Concept

```cpp
template<typename T>
concept port = requires(T p, T q) {
    // Must be copyable and movable
    { T(p) } -> std::same_as<T>;
    { T(std::move(p)) } -> std::same_as<T>;
    
    // Must support equality comparison
    { p == q } -> std::convertible_to<bool>;
    { p != q } -> std::convertible_to<bool>;
    
    // Must be hashable
    { std::hash<T>{}(p) } -> std::convertible_to<std::size_t>;
};
```

**Supported Types:**
- `unsigned short`
- `std::string`
#### 2.3 Future Concept

```cpp
template<typename F, typename T>
concept future = requires(F f) {
    // Must provide value access (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Must support then() for chaining
    { f.then([](T val) { return val; }) } -> future<T>;
    
    // Must support error handling
    { f.on_error([](std::exception_ptr) {}) } -> future<T>;
    
    // Must be able to check if ready
    { f.is_ready() } -> std::convertible_to<bool>;
    
    // Must support timeout
    { f.wait(std::chrono::milliseconds{100}) } -> std::convertible_to<bool>;
};
```

#### 2.4 Message Concept

```cpp
template<typename M, typename Addr, typename Port>
concept message = requires(M msg) {
    // Must provide source address
    { msg.source_address() } -> std::same_as<Addr>;
    
    // Must provide source port
    { msg.source_port() } -> std::same_as<Port>;
    
    // Must provide destination address
    { msg.destination_address() } -> std::same_as<Addr>;
    
    // Must provide destination port
    { msg.destination_port() } -> std::same_as<Port>;
    
    // Must provide payload access
    { msg.payload() } -> std::same_as<std::vector<std::byte>>;
};
```

#### 2.5 Connection Concept

```cpp
template<typename C, typename FutureBytes, typename FutureBool>
concept connection = requires(C conn, std::vector<std::byte> data) {
    // Must support reading data
    { conn.read() } -> std::same_as<FutureBytes>;
    { conn.read(std::chrono::milliseconds{100}) } -> std::same_as<FutureBytes>;
    
    // Must support writing data
    { conn.write(data) } -> std::same_as<FutureBool>;
    { conn.write(data, std::chrono::milliseconds{100}) } -> std::same_as<FutureBool>;
    
    // Must be closeable
    { conn.close() } -> std::same_as<void>;
    
    // Must provide connection state
    { conn.is_open() } -> std::convertible_to<bool>;
};
```

#### 2.6 Listener Concept

```cpp
template<typename L, typename FutureConn>
concept listener = requires(L lstn) {
    // Must support accepting connections
    { lstn.accept() } -> std::same_as<FutureConn>;
    { lstn.accept(std::chrono::milliseconds{100}) } -> std::same_as<FutureConn>;
    
    // Must be closeable
    { lstn.close() } -> std::same_as<void>;
    
    // Must provide listener state
    { lstn.is_listening() } -> std::convertible_to<bool>;
};
```

#### 2.7 Network Edge Concept

```cpp
template<typename E>
concept network_edge = requires(E edge) {
    // Must provide latency
    { edge.latency() } -> std::same_as<std::chrono::milliseconds>;
    
    // Must provide reliability (0.0 to 1.0)
    { edge.reliability() } -> std::convertible_to<double>;
};
```

#### 2.8 Network Node Concept

```cpp
template<typename N, typename Types>
concept network_node = requires(N node, typename Types::message_type msg, 
                                typename Types::address_type addr, typename Types::port_type port) {
    // Connectionless operations
    { node.send(msg) } -> std::same_as<typename Types::future_bool_type>;
    { node.send(msg, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_bool_type>;
    { node.receive() } -> std::same_as<typename Types::future_message_type>;
    { node.receive(std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_message_type>;
    
    // Connection-oriented client operations
    { node.connect(addr, port) } -> std::same_as<typename Types::future_connection_type>;
    { node.connect(addr, port, port) } -> std::same_as<typename Types::future_connection_type>;  // with source port
    { node.connect(addr, port, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_connection_type>;
    
    // Connection-oriented server operations
    { node.bind() } -> std::same_as<typename Types::future_listener_type>;  // bind to random port
    { node.bind(port) } -> std::same_as<typename Types::future_listener_type>;  // bind to specific port
    { node.bind(port, std::chrono::milliseconds{100}) } -> std::same_as<typename Types::future_listener_type>;
    
    // Node identity
    { node.address() } -> std::same_as<typename Types::address_type>;
};
```

#### 2.9 Network Simulator Concept

```cpp
template<typename S, typename Types>
concept network_simulator = requires(S sim, typename Types::address_type addr, NetworkEdge edge) {
    // Topology configuration
    { sim.add_node(addr) } -> std::same_as<void>;
    { sim.remove_node(addr) } -> std::same_as<void>;
    { sim.add_edge(addr, addr, edge) } -> std::same_as<void>;
    { sim.remove_edge(addr, addr) } -> std::same_as<void>;
    
    // Node creation
    { sim.create_node(addr) } -> std::same_as<std::shared_ptr<typename Types::node_type>>;
    
    // Simulation control
    { sim.start() } -> std::same_as<void>;
    { sim.stop() } -> std::same_as<void>;
    { sim.reset() } -> std::same_as<void>;
};
```
### 3. Core Data Structures

#### 3.1 Example Types Implementation

Here's an example of how to implement the `network_simulator_types` concept:

```cpp
struct DefaultNetworkTypes {
    // Core types
    using address_type = std::string;
    using port_type = unsigned short;
    using message_type = Message<address_type, port_type>;
    using connection_type = Connection<address_type, port_type>;
    using listener_type = Listener<address_type, port_type>;
    using node_type = NetworkNode<DefaultNetworkTypes>;
    
    // Future types (using folly::Future as base)
    using future_bool_type = folly::Future<bool>;
    using future_message_type = folly::Future<message_type>;
    using future_connection_type = folly::Future<std::shared_ptr<connection_type>>;
    using future_listener_type = folly::Future<std::shared_ptr<listener_type>>;
    using future_bytes_type = folly::Future<std::vector<std::byte>>;
};

// Verify the concept is satisfied
static_assert(network_simulator_types<DefaultNetworkTypes>);
```

#### 3.2 Message Structure

```cpp
template<typename Types>
class Message {
public:
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    
    Message(address_type src_addr, port_type src_port, 
            address_type dst_addr, port_type dst_port,
            std::vector<std::byte> payload = {});
    
    auto source_address() const -> address_type;
    auto source_port() const -> port_type;
    auto destination_address() const -> address_type;
    auto destination_port() const -> port_type;
    auto payload() const -> const std::vector<std::byte>&;
    
private:
    address_type _source_address;
    port_type _source_port;
    address_type _destination_address;
    port_type _destination_port;
    std::vector<std::byte> _payload;
};
```

#### 3.3 Network Edge

```cpp
struct NetworkEdge {
    std::chrono::milliseconds latency;
    double reliability;  // 0.0 to 1.0
    
    NetworkEdge(std::chrono::milliseconds lat, double rel)
        : latency(lat), reliability(rel) {}
        
    auto latency() const -> std::chrono::milliseconds { return latency; }
    auto reliability() const -> double { return reliability; }
};
```

#### 3.4 Endpoint

```cpp
template<typename Types>
struct Endpoint {
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    
    address_type address;
    port_type port;
    
    auto operator==(const Endpoint&) const -> bool = default;
};

// Hash specialization for use in unordered containers
template<typename Types>
struct std::hash<Endpoint<Types>> {
    auto operator()(const Endpoint<Types>& ep) const -> std::size_t {
        std::size_t h1 = std::hash<typename Types::address_type>{}(ep.address);
        std::size_t h2 = std::hash<typename Types::port_type>{}(ep.port);
        return h1 ^ (h2 << 1);
    }
};
```
### 4. Network Simulator Core

#### 4.1 NetworkSimulator Class

```cpp
template<network_simulator_types Types>
class NetworkSimulator {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using message_type = typename Types::message_type;
    using connection_type = typename Types::connection_type;
    using listener_type = typename Types::listener_type;
    using node_type = typename Types::node_type;
    using endpoint_type = Endpoint<Types>;
    
    // Future type aliases
    using future_bool_type = typename Types::future_bool_type;
    using future_message_type = typename Types::future_message_type;
    using future_connection_type = typename Types::future_connection_type;
    using future_listener_type = typename Types::future_listener_type;
    using future_bytes_type = typename Types::future_bytes_type;
    
    // Topology configuration
    auto add_node(address_type address) -> void;
    auto remove_node(address_type address) -> void;
    auto add_edge(address_type from, address_type to, NetworkEdge edge) -> void;
    auto remove_edge(address_type from, address_type to) -> void;
    
    // Node creation
    auto create_node(address_type address) -> std::shared_ptr<node_type>;
    
    // Simulation control
    auto start() -> void;
    auto stop() -> void;
    auto reset() -> void;
    
    // Query methods for testing
    auto has_node(address_type address) const -> bool;
    auto has_edge(address_type from, address_type to) const -> bool;
    auto get_edge(address_type from, address_type to) const -> NetworkEdge;
    
private:
    // Directed graph: adjacency list representation
    std::unordered_map<address_type, std::unordered_map<address_type, NetworkEdge>> _topology;
    
    // Active nodes
    std::unordered_map<address_type, std::shared_ptr<node_type>> _nodes;
    
    // Message queues per node
    std::unordered_map<address_type, std::queue<message_type>> _message_queues;
    
    // Connection state
    std::unordered_map<endpoint_type, std::shared_ptr<connection_type>> _connections;
    
    // Listeners
    std::unordered_map<endpoint_type, std::shared_ptr<listener_type>> _listeners;
    
    // Thread pool for async operations
    std::unique_ptr<folly::Executor> _executor;
    
    // Random number generator for reliability simulation
    std::mt19937 _rng;
    
    // Simulation state
    std::atomic<bool> _started{false};
    
    // Mutex for thread safety
    mutable std::shared_mutex _mutex;
    
    // Internal routing and delivery
    auto route_message(message_type msg) -> future_bool_type;
    auto deliver_message(message_type msg) -> void;
    auto apply_latency(address_type from, address_type to) -> std::chrono::milliseconds;
    auto check_reliability(address_type from, address_type to) -> bool;
    auto retrieve_message(address_type address) -> future_message_type;
    auto retrieve_message(address_type address, std::chrono::milliseconds timeout) -> future_message_type;
    
    // Connection establishment
    auto establish_connection(address_type src_addr, port_type src_port, 
                             address_type dst_addr, port_type dst_port) -> future_connection_type;
    
    // Listener establishment
    auto create_listener(address_type addr, port_type port) -> future_listener_type;
    auto create_listener(address_type addr) -> future_listener_type;  // Random port
    auto create_listener(address_type addr, port_type port, 
                        std::chrono::milliseconds timeout) -> future_listener_type;
};
```

### 5. Network Node Implementation

#### 5.1 NetworkNode Class

```cpp
template<network_simulator_types Types>
class NetworkNode {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using message_type = typename Types::message_type;
    using connection_type = typename Types::connection_type;
    using listener_type = typename Types::listener_type;
    using simulator_type = NetworkSimulator<Types>;
    
    // Future type aliases
    using future_bool_type = typename Types::future_bool_type;
    using future_message_type = typename Types::future_message_type;
    using future_connection_type = typename Types::future_connection_type;
    using future_listener_type = typename Types::future_listener_type;
    
    explicit NetworkNode(address_type addr, simulator_type* simulator);
    
    // Connectionless operations
    auto send(message_type msg) -> future_bool_type;
    auto send(message_type msg, std::chrono::milliseconds timeout) -> future_bool_type;
    auto receive() -> future_message_type;
    auto receive(std::chrono::milliseconds timeout) -> future_message_type;
    
    // Connection-oriented client operations
    auto connect(address_type dst_addr, port_type dst_port) -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, port_type src_port) -> future_connection_type;
    auto connect(address_type dst_addr, port_type dst_port, std::chrono::milliseconds timeout) -> future_connection_type;
    
    // Connection-oriented server operations
    auto bind() -> future_listener_type;  // bind to random port
    auto bind(port_type port) -> future_listener_type;  // bind to specific port
    auto bind(port_type port, std::chrono::milliseconds timeout) -> future_listener_type;
    
    // Node identity
    auto address() const -> address_type { return _address; }
    
private:
    address_type _address;
    simulator_type* _simulator;
    
    // Ephemeral port allocation
    auto allocate_ephemeral_port() -> port_type;
    std::unordered_set<port_type> _used_ports;
    mutable std::mutex _port_mutex;
};
```

### 6. Connection Implementation

#### 6.1 Connection Class

```cpp
template<network_simulator_types Types>
class Connection {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using endpoint_type = Endpoint<Types>;
    using simulator_type = NetworkSimulator<Types>;
    using future_bool_type = typename Types::future_bool_type;
    using future_bytes_type = typename Types::future_bytes_type;
    
    Connection(endpoint_type local, 
               endpoint_type remote,
               simulator_type* simulator);
    
    auto read() -> future_bytes_type;
    auto read(std::chrono::milliseconds timeout) -> future_bytes_type;
    
    auto write(std::vector<std::byte> data) -> future_bool_type;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> future_bool_type;
    
    auto close() -> void;
    auto is_open() const -> bool;
    
    auto local_endpoint() const -> endpoint_type { return _local; }
    auto remote_endpoint() const -> endpoint_type { return _remote; }
    
    // Internal method for simulator to deliver data
    auto deliver_data(std::vector<std::byte> data) -> void;
    
private:
    endpoint_type _local;
    endpoint_type _remote;
    simulator_type* _simulator;
    
    std::atomic<bool> _open{true};
    
    // Buffered data for this connection
    std::queue<std::vector<std::byte>> _read_buffer;
    mutable std::mutex _buffer_mutex;
    std::condition_variable _data_available;
};
```

#### 6.2 Listener Class

```cpp
template<network_simulator_types Types>
class Listener {
public:
    // Type aliases from Types template argument
    using address_type = typename Types::address_type;
    using port_type = typename Types::port_type;
    using endpoint_type = Endpoint<Types>;
    using connection_type = typename Types::connection_type;
    using simulator_type = NetworkSimulator<Types>;
    using future_connection_type = typename Types::future_connection_type;
    
    Listener(endpoint_type local_endpoint,
             simulator_type* simulator);
    
    auto accept() -> future_connection_type;
    auto accept(std::chrono::milliseconds timeout) -> future_connection_type;
    
    auto close() -> void;
    auto is_listening() const -> bool;
    
    auto local_endpoint() const -> endpoint_type { return _local; }
    
    // Internal method for simulator to queue pending connections
    auto queue_pending_connection(std::shared_ptr<connection_type> connection) -> void;
    
private:
    endpoint_type _local;
    simulator_type* _simulator;
    
    std::atomic<bool> _listening{true};
    
    // Queue of pending connections
    std::queue<std::shared_ptr<connection_type>> _pending_connections;
    mutable std::mutex _queue_mutex;
    std::condition_variable _connection_available;
## Data Models

### Topology Graph Model

The network topology is modeled as a directed graph where:
- **Vertices**: Network nodes identified by addresses from `typename Types::address_type`
- **Edges**: Communication paths with two weights:
  - **Latency weight**: Time delay for message transmission
  - **Reliability weight**: Probability (0.0-1.0) of successful transmission

```
Example topology:

    Node A ──(10ms, 0.99)──> Node B
      │                        │
      │                        │
   (5ms, 1.0)             (20ms, 0.95)
      │                        │
      ▼                        ▼
    Node C <──(15ms, 0.98)── Node D
```

### Message Flow Model

#### Connectionless (Datagram) Flow

```
1. Application calls node.send(message)
2. Node submits message to simulator
3. Simulator checks topology for route
4. Simulator applies latency delay
5. Simulator applies reliability check (probabilistic drop)
6. If successful, message queued at destination
7. Destination node.receive() retrieves message
```

#### Connection-Oriented (Stream) Flow

```
Client Side:
1. Application calls node.connect(addr, port)
2. Node allocates ephemeral source port (if not specified)
3. Connection request routed through simulator
4. Simulator creates connection object
5. Future resolves with connection

Server Side:
1. Application calls node.bind(port)
2. Simulator creates listener on port
3. Future resolves with listener
4. Application calls listener.accept()
5. When client connects, future resolves with connection

Data Transfer:
1. Application calls connection.write(data)
2. Data routed through simulator with latency/reliability
3. Data buffered at remote connection
4. Remote application calls connection.read()
5. Future resolves with buffered data
```

### State Models

#### Connection States

```
CLOSED ──connect()──> CONNECTING ──success──> OPEN ──close()──> CLOSED
                           │
                           └──timeout/error──> CLOSED
```

#### Listener States

```
CLOSED ──bind()──> LISTENING ──close()──> CLOSED
```

## Connection Management Architecture

### Connection Establishment with Timeout Handling

The network simulator implements robust connection establishment with comprehensive timeout handling to ensure reliable operation under various network conditions.

#### Connection Establishment Flow

```cpp
template<network_simulator_types Types>
class NetworkSimulator {
private:
    struct ConnectionRequest {
        endpoint_type source;
        endpoint_type destination;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::milliseconds timeout;
        folly::Promise<std::shared_ptr<connection_type>> promise;
        
        bool is_expired() const {
            return std::chrono::steady_clock::now() - start_time > timeout;
        }
    };
    
    // Pending connection requests with timeout tracking
    std::vector<ConnectionRequest> _pending_connections;
    std::mutex _connection_requests_mutex;
    
    // Timer for connection timeout processing
    std::unique_ptr<folly::TimerManager> _timer_manager;
    
    auto establish_connection_with_timeout(
        endpoint_type source, 
        endpoint_type destination,
        std::chrono::milliseconds timeout
    ) -> future_connection_type;
    
    auto process_connection_timeouts() -> void;
    auto cancel_expired_connections() -> void;
};
```

#### Timeout Handling Implementation

1. **Request Tracking**: Each connection request is tracked with timestamp and timeout
2. **Periodic Cleanup**: Timer-based cleanup of expired connection requests
3. **Cancellation Support**: Ability to cancel pending operations
4. **Error Propagation**: Detailed timeout errors with context information

### Connection Pooling and Reuse

The simulator implements connection pooling to improve performance by reusing existing connections when possible.

#### Connection Pool Architecture

```cpp
template<network_simulator_types Types>
class ConnectionPool {
public:
    struct PooledConnection {
        std::shared_ptr<connection_type> connection;
        std::chrono::steady_clock::time_point last_used;
        std::chrono::steady_clock::time_point created;
        bool is_healthy;
        
        bool is_stale(std::chrono::milliseconds max_age) const {
            return std::chrono::steady_clock::now() - last_used > max_age;
        }
    };
    
    struct PoolConfig {
        std::size_t max_connections_per_endpoint = 10;
        std::chrono::milliseconds max_idle_time{300000}; // 5 minutes
        std::chrono::milliseconds max_connection_age{3600000}; // 1 hour
        bool enable_health_checks = true;
    };
    
private:
    // Pool organized by destination endpoint
    std::unordered_map<endpoint_type, std::vector<PooledConnection>> _connection_pools;
    PoolConfig _config;
    mutable std::shared_mutex _pool_mutex;
    
    // Background cleanup timer
    std::unique_ptr<folly::TimerManager> _cleanup_timer;
    
public:
    auto get_or_create_connection(endpoint_type destination) -> future_connection_type;
    auto return_connection(std::shared_ptr<connection_type> conn) -> void;
    auto cleanup_stale_connections() -> void;
    auto configure_pool(PoolConfig config) -> void;
};
```

#### Pool Management Features

1. **LRU Eviction**: Least recently used connections are evicted when pool is full
2. **Health Checking**: Periodic validation of pooled connections
3. **Automatic Cleanup**: Background cleanup of stale and invalid connections
4. **Per-Endpoint Limits**: Separate pool limits for each destination

### Listener Management with Resource Cleanup

Comprehensive listener management ensures proper resource cleanup and prevents resource leaks.

#### Listener Resource Tracking

```cpp
template<network_simulator_types Types>
class ListenerManager {
public:
    struct ListenerResource {
        std::shared_ptr<listener_type> listener;
        endpoint_type bound_endpoint;
        std::chrono::steady_clock::time_point created;
        std::vector<std::shared_ptr<connection_type>> pending_connections;
        std::atomic<bool> is_active{true};
        
        // Resource tracking
        std::vector<std::unique_ptr<folly::TimerManager>> timers;
        std::vector<folly::Future<folly::Unit>> pending_operations;
    };
    
private:
    std::unordered_map<endpoint_type, ListenerResource> _active_listeners;
    mutable std::shared_mutex _listeners_mutex;
    
    // Port allocation tracking
    std::unordered_set<port_type> _allocated_ports;
    std::mutex _port_allocation_mutex;
    
public:
    auto create_listener(endpoint_type endpoint) -> future_listener_type;
    auto close_listener(endpoint_type endpoint) -> void;
    auto cleanup_all_listeners() -> void;
    auto release_port(port_type port) -> void;
    auto is_port_available(port_type port) const -> bool;
};
```

#### Resource Cleanup Features

1. **Immediate Cleanup**: Resources released immediately on explicit close
2. **Automatic Cleanup**: All resources cleaned up on simulator stop/reset
3. **Port Management**: Proper port allocation and release tracking
4. **Pending Operation Handling**: Graceful handling of pending accept operations

### Connection State Tracking and Lifecycle Management

Comprehensive connection state tracking provides visibility into connection health and lifecycle events.

#### Connection State Model

```cpp
enum class ConnectionState {
    CONNECTING,    // Connection establishment in progress
    CONNECTED,     // Connection established and ready
    CLOSING,       // Connection close initiated
    CLOSED,        // Connection closed
    ERROR          // Connection in error state
};

template<network_simulator_types Types>
class ConnectionTracker {
public:
    struct ConnectionStats {
        std::chrono::steady_clock::time_point established_time;
        std::chrono::steady_clock::time_point last_activity;
        std::size_t bytes_sent = 0;
        std::size_t bytes_received = 0;
        std::size_t messages_sent = 0;
        std::size_t messages_received = 0;
        std::optional<std::string> last_error;
    };
    
    struct ConnectionInfo {
        endpoint_type local_endpoint;
        endpoint_type remote_endpoint;
        ConnectionState state;
        ConnectionStats stats;
        std::weak_ptr<connection_type> connection_ref;
        
        // Optional observer callback
        std::function<void(ConnectionState, ConnectionState)> state_change_callback;
    };
    
private:
    std::unordered_map<endpoint_type, ConnectionInfo> _connection_info;
    mutable std::shared_mutex _info_mutex;
    
    // Keep-alive and idle timeout management
    std::chrono::milliseconds _keep_alive_interval{30000}; // 30 seconds
    std::chrono::milliseconds _idle_timeout{300000}; // 5 minutes
    std::unique_ptr<folly::TimerManager> _keep_alive_timer;
    
public:
    auto register_connection(std::shared_ptr<connection_type> conn) -> void;
    auto update_connection_state(endpoint_type endpoint, ConnectionState new_state) -> void;
    auto update_connection_stats(endpoint_type endpoint, std::size_t bytes_transferred, bool is_send) -> void;
    auto get_connection_info(endpoint_type endpoint) const -> std::optional<ConnectionInfo>;
    auto get_all_connections() const -> std::vector<ConnectionInfo>;
    auto cleanup_connection(endpoint_type endpoint) -> void;
    
    // Keep-alive and idle management
    auto configure_keep_alive(std::chrono::milliseconds interval) -> void;
    auto configure_idle_timeout(std::chrono::milliseconds timeout) -> void;
    auto process_keep_alive() -> void;
    auto process_idle_timeouts() -> void;
};
```

#### Lifecycle Management Features

1. **State Transitions**: Proper tracking of connection state changes
2. **Statistics Collection**: Comprehensive metrics on data transfer and activity
3. **Error Tracking**: Detailed error information and history
4. **Observer Pattern**: Optional callbacks for state change notifications
5. **Keep-Alive Support**: Configurable keep-alive mechanisms
6. **Idle Timeout**: Automatic cleanup of idle connections

### Integration with Core Simulator

The connection management components integrate seamlessly with the core NetworkSimulator:

```cpp
template<network_simulator_types Types>
class NetworkSimulator {
private:
    // Connection management components
    std::unique_ptr<ConnectionPool<Types>> _connection_pool;
    std::unique_ptr<ListenerManager<Types>> _listener_manager;
    std::unique_ptr<ConnectionTracker<Types>> _connection_tracker;
    
    // Configuration
    struct ConnectionConfig {
        std::chrono::milliseconds default_connect_timeout{30000}; // 30 seconds
        std::chrono::milliseconds default_accept_timeout{60000};  // 60 seconds
        bool enable_connection_pooling = true;
        bool enable_connection_tracking = true;
        bool enable_keep_alive = false;
    } _connection_config;
    
public:
    auto configure_connection_management(ConnectionConfig config) -> void;
    auto get_connection_pool() -> ConnectionPool<Types>&;
    auto get_listener_manager() -> ListenerManager<Types>&;
    auto get_connection_tracker() -> ConnectionTracker<Types>&;
};
```

## Error Handling

### Exception Types

```cpp
namespace network_simulator {

class NetworkException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TimeoutException : public NetworkException {
public:
    TimeoutException() : NetworkException("Operation timed out") {}
};

class ConnectionClosedException : public NetworkException {
public:
    ConnectionClosedException() : NetworkException("Connection is closed") {}
};

class PortInUseException : public NetworkException {
public:
    explicit PortInUseException(const std::string& port) 
        : NetworkException("Port already in use: " + port) {}
};

class NodeNotFoundException : public NetworkException {
public:
    explicit NodeNotFoundException(const std::string& address)
        : NetworkException("Node not found: " + address) {}
};

class NoRouteException : public NetworkException {
public:
    NoRouteException(const std::string& from, const std::string& to)
        : NetworkException("No route from " + from + " to " + to) {}
};

} // namespace network_simulator
```

### Error Handling Strategy

1. **Timeout Errors**: All operations with timeouts use `TimeoutException` when the timeout expires
2. **Connection Errors**: Operations on closed connections throw `ConnectionClosedException`
3. **Port Conflicts**: Binding to an in-use port throws `PortInUseException`
4. **Routing Errors**: Messages to unreachable nodes throw `NoRouteException`
5. **Future Error States**: Exceptions are propagated through the future mechanism, allowing async error handling

### Error Recovery

- **Timeouts**: Callers can retry operations or use longer timeouts
- **Connection Failures**: Callers can attempt reconnection with exponential backoff
- **Port Conflicts**: Callers can try different ports or wait for port release
## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

Based on the prework analysis, I've identified properties that can be combined to eliminate redundancy and provide comprehensive validation:

### Property 1: Topology Edge Latency Preservation

*For any* pair of nodes and configured latency value, when an edge is added to the topology with that latency, querying the topology SHALL return the same latency value.

**Validates: Requirements 1.1, 11.3, 11.6**

### Property 2: Topology Edge Reliability Preservation

*For any* pair of nodes and configured reliability value, when an edge is added to the topology with that reliability, querying the topology SHALL return the same reliability value.

**Validates: Requirements 1.2, 11.3, 11.6**

### Property 3: Latency Application

*For any* message sent between two nodes with a configured latency, the time between send and receive SHALL be at least the configured latency value (within measurement tolerance).

**Validates: Requirements 1.3**

### Property 4: Reliability Application

*For any* large set of messages sent between two nodes with configured reliability R, the proportion of successfully delivered messages SHALL approximate R within statistical bounds.

**Validates: Requirements 1.4**

### Property 5: Graph-Based Routing

*For any* message sent from source to destination, if a path exists in the directed graph, the message SHALL only traverse edges that exist in the topology.

**Validates: Requirements 1.5**

### Property 6: Send Success Result

*For any* message that is accepted by the network simulator for transmission, the send operation SHALL return a future that resolves to true.

**Validates: Requirements 4.2**

### Property 7: Send Timeout Result

*For any* send operation that cannot accept the message before the timeout expires, the send operation SHALL return a future that resolves to false.

**Validates: Requirements 4.3**

### Property 8: Send Does Not Guarantee Delivery

*For any* message where send returns true, the message MAY not appear at the destination (due to reliability < 1.0), demonstrating that send success does not guarantee delivery.

**Validates: Requirements 4.4**

### Property 9: Receive Returns Sent Message

*For any* message sent to a node that is successfully delivered, calling receive on that node SHALL return a future that resolves to a message with the same source, destination, and payload.

**Validates: Requirements 5.2**

### Property 10: Receive Timeout Exception

*For any* receive operation with a timeout where no message arrives before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 5.3**

### Property 11: Connect Uses Specified Source Port

*For any* connect operation with an explicitly specified source port, the resulting connection's local endpoint SHALL have that source port.

**Validates: Requirements 6.2**

### Property 12: Connect Assigns Unique Ephemeral Ports

*For any* sequence of connect operations without specified source ports from the same node, each resulting connection SHALL have a unique source port that was not previously in use.

**Validates: Requirements 6.3**

### Property 13: Successful Connection Returns Connection Object

*For any* connect operation that successfully establishes a connection, the future SHALL resolve to a valid connection object with is_open() returning true.

**Validates: Requirements 6.4**

### Property 14: Connect Timeout Exception

*For any* connect operation with a timeout where the connection cannot be established before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 6.5**

### Property 15: Successful Bind Returns Listener

*For any* bind operation that successfully binds to a port, the future SHALL resolve to a valid listener object with is_listening() returning true.

**Validates: Requirements 7.2, 7.4**

### Property 16: Bind Assigns Unique Ports

*For any* bind operation without a specified port, the resulting listener SHALL have a unique port that was not previously in use.

**Validates: Requirements 7.3**

### Property 17: Accept Returns Connection on Client Connect

*For any* listener with a pending accept operation, when a client connects to the bound port, the accept future SHALL resolve to a valid connection object.

**Validates: Requirements 7.7**

### Property 18: Accept Timeout Exception

*For any* accept operation with a timeout where no client connects before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 7.8**

### Property 19: Connection Read-Write Round Trip

*For any* data written to one end of a connection, reading from the other end SHALL return the same data (subject to network reliability and latency).

**Validates: Requirements 8.2, 8.5**

### Property 20: Read Timeout Exception

*For any* read operation with a timeout where no data is available before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 8.3**

### Property 21: Write Timeout Exception

*For any* write operation with a timeout where the write cannot complete before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 8.6**

### Property 22: Topology Management Operations

*For any* node or edge added to the topology, the topology query methods SHALL reflect the addition, and for any node or edge removed, the query methods SHALL reflect the removal.

**Validates: Requirements 11.1, 11.2, 11.4, 11.5**

### Property 23: Simulation Lifecycle Control

*For any* simulator that is started, network operations SHALL be processed, and for any simulator that is stopped, new network operations SHALL be rejected with appropriate errors.

**Validates: Requirements 12.1, 12.2, 12.4**

### Property 24: Simulation Reset

*For any* simulator with existing state, calling reset SHALL clear all topology, nodes, connections, and listeners, returning the simulator to initial conditions.

**Validates: Requirements 12.3**

### Property 25: Connection Establishment Timeout Handling

*For any* connection establishment request with a specified timeout, if the connection cannot be established within the timeout period, the operation SHALL fail with a timeout exception and cancel any pending connection attempts.

**Validates: Requirements 15.1, 15.2, 15.3**

### Property 26: Connection Establishment Cancellation

*For any* pending connection establishment operation, when cancellation is requested, the operation SHALL be cancelled and any associated resources SHALL be cleaned up immediately.

**Validates: Requirements 15.5**

### Property 27: Connection Pool Reuse

*For any* connection request to a destination where a healthy pooled connection exists, the connection pool SHALL return the existing connection rather than creating a new one.

**Validates: Requirements 16.2**

### Property 28: Connection Pool Eviction

*For any* connection pool that reaches its capacity limit, adding a new connection SHALL evict the least recently used connection from the pool.

**Validates: Requirements 16.3**

### Property 29: Connection Pool Cleanup

*For any* pooled connection that becomes stale or invalid, the connection pool SHALL automatically remove it from the pool during cleanup operations.

**Validates: Requirements 16.4**

### Property 30: Listener Resource Cleanup

*For any* listener that is closed or when the simulator is stopped, all associated resources including ports, pending connections, and timers SHALL be immediately released and made available for reuse.

**Validates: Requirements 17.2, 17.3, 17.4**

### Property 31: Listener Port Release

*For any* listener that is closed, the bound port SHALL be immediately released and made available for new listeners to bind to.

**Validates: Requirements 17.6**

### Property 32: Connection State Tracking

*For any* connection that is established, the connection tracker SHALL maintain accurate state information including current status, establishment time, and data transfer statistics.

**Validates: Requirements 18.1, 18.2**

### Property 33: Connection State Updates

*For any* connection state change event (connecting, connected, closing, closed, error), the connection tracker SHALL update the connection state appropriately and notify any registered observers.

**Validates: Requirements 18.2, 18.4**

### Property 34: Connection Resource Cleanup

*For any* connection that is closed or enters an error state, all associated resources including buffers, timers, and network handles SHALL be properly deallocated to prevent resource leaks.

**Validates: Requirements 18.6**

## Testing Strategy

### Unit Testing

Unit tests will verify specific behaviors and edge cases:

1. **Concept Satisfaction Tests**: Verify that expected types satisfy the concepts
   - Test that `std::string`, `unsigned long`, `in_addr`, `in6_addr` satisfy `address` concept
   - Test that `unsigned short`, `std::string` satisfy `port` concept
   - Test that example Types implementations satisfy `network_simulator_types` concept

2. **Message Construction Tests**: Verify message creation with various address/port combinations
   - Test message with empty payload
   - Test message with large payload
   - Test message accessor methods

3. **Topology Configuration Tests**: Verify graph operations
   - Add/remove nodes
   - Add/remove edges
   - Query edge properties

4. **Connection State Tests**: Verify connection lifecycle
   - Connection open/close transitions
   - Operations on closed connections throw exceptions

5. **Listener State Tests**: Verify listener lifecycle
   - Listener start/stop transitions
   - Operations on closed listeners throw exceptions

6. **Ephemeral Port Allocation Tests**: Verify unique port assignment
   - Multiple connections get unique ports
   - Port reuse after connection close

### Property-Based Testing

Property-based tests will verify universal properties across many randomly generated inputs using a C++ property-based testing library (e.g., RapidCheck or similar).

**Configuration**: Each property test will run a minimum of 100 iterations with randomly generated inputs.

**Property Test Implementations**:

Each correctness property from the design document will be implemented as a separate property-based test, tagged with comments referencing the specific property number and requirements.

### Integration Testing

Integration tests will verify end-to-end scenarios:

1. **Client-Server Communication**: Full connection establishment, data transfer, and teardown
2. **Multi-Node Topology**: Messages routed through intermediate nodes
3. **Network Partition Simulation**: Verify behavior when edges are removed
4. **Concurrent Operations**: Multiple nodes sending/receiving simultaneously
5. **Stress Testing**: High message volume with varying latency/reliability

### Test Infrastructure

- **Test Fixtures**: Reusable network topologies for common scenarios
- **Mock Time**: Ability to advance simulated time for latency testing
- **Deterministic RNG**: Seeded random number generator for reproducible reliability tests
- **Assertion Helpers**: Custom assertions for future results and exception types

## Implementation Notes

### Thread Safety

- All public methods of `NetworkSimulator`, `NetworkNode`, `Connection`, and `Listener` are thread-safe
- Internal state protected by `std::shared_mutex` for read-write locking
- Message queues and connection buffers use mutexes and condition variables

### Performance Considerations

- **Lock Granularity**: Fine-grained locking to minimize contention
- **Lock-Free Queues**: Consider lock-free data structures for message queues
- **Thread Pool**: Use folly::Executor for async operations to avoid thread creation overhead
- **Memory Management**: Use `std::shared_ptr` for shared ownership, avoid unnecessary copies

### Type System Benefits

The new Types template parameter approach provides several advantages:

1. **Simplified Template Complexity**: Single template parameter instead of multiple
2. **Type Safety**: All types are defined consistently through the concept
3. **Flexibility**: Easy to swap different future implementations or address types
4. **Maintainability**: Clear separation of type definitions from implementation
5. **Extensibility**: Easy to add new types to the Types concept

### Future Implementation

The design uses the Types template argument to provide future implementations, which can be:
- folly::Future for production use
- Custom test futures for unit testing
- Mock futures for simulation scenarios

## Dependencies

- **folly**: For Future/Promise implementation and executor framework (when using folly-based Types)
- **C++23 compiler**: For concepts, ranges, and modern language features
- **Boost.Graph** (optional): For advanced graph algorithms if needed
- **RapidCheck or similar**: For property-based testing framework

## Build System

- **CMake**: Build system configuration
- **C++23 standard**: Required language standard
- **Compiler support**: GCC 13+, Clang 16+, or MSVC 2022+

## Future Enhancements

1. **Advanced Routing**: Implement shortest-path routing algorithms
2. **Bandwidth Simulation**: Add bandwidth limits to edges
3. **Packet Reordering**: Simulate out-of-order delivery
4. **Network Partitions**: Dynamic topology changes during simulation
5. **Statistics Collection**: Detailed metrics on message delivery, latency distribution, etc.
6. **Visualization**: Tools to visualize topology and message flow
7. **Protocol Implementations**: Built-in implementations of common protocols (TCP, UDP, etc.)
8. **Failure Injection**: Programmatic injection of various failure modes

- **Timeouts**: Callers can retry operations or use longer timeouts
- **Connection Failures**: Callers can attempt reconnection with exponential backoff
- **Port Conflicts**: Callers can try different ports or wait for port release
- **Routing Failures**: Callers can reconfigure topology or use different routes

## Testing Strategy

### Unit Testing

Unit tests will verify specific behaviors and edge cases:

1. **Concept Satisfaction Tests**: Verify that expected types satisfy the concepts
   - Test that `std::string`, `unsigned long`, `in_addr`, `in6_addr` satisfy `address` concept
   - Test that `unsigned short`, `std::string` satisfy `port` concept

2. **Message Construction Tests**: Verify message creation with various address/port combinations
   - Test message with empty payload
   - Test message with large payload
   - Test message accessor methods

3. **Topology Configuration Tests**: Verify graph operations
   - Add/remove nodes
   - Add/remove edges
   - Query edge properties

4. **Connection State Tests**: Verify connection lifecycle
   - Connection open/close transitions
   - Operations on closed connections throw exceptions

5. **Listener State Tests**: Verify listener lifecycle
   - Listener start/stop transitions
   - Operations on closed listeners throw exceptions

6. **Ephemeral Port Allocation Tests**: Verify unique port assignment
   - Multiple connections get unique ports
   - Port reuse after connection close

### Property-Based Testing

Property-based tests will verify universal properties across many randomly generated inputs using a C++ property-based testing library (e.g., RapidCheck or similar).

**Configuration**: Each property test will run a minimum of 100 iterations with randomly generated inputs.

**Property Test Implementations**:

1. **Property 1: Topology Edge Latency Preservation**
   - Generate random node addresses and latency values
   - Add edges with those latencies
   - Verify querying returns the same latency
   - **Feature: network-simulator, Property 1: Topology Edge Latency Preservation**

2. **Property 2: Topology Edge Reliability Preservation**
   - Generate random node addresses and reliability values (0.0-1.0)
   - Add edges with those reliabilities
   - Verify querying returns the same reliability
   - **Feature: network-simulator, Property 2: Topology Edge Reliability Preservation**

3. **Property 3: Latency Application**
   - Generate random topology with known latencies
   - Send messages and measure delivery time
   - Verify delivery time >= configured latency
   - **Feature: network-simulator, Property 3: Latency Application**

4. **Property 4: Reliability Application**
   - Generate random reliability values
   - Send large number of messages (e.g., 1000)
   - Verify delivery rate approximates reliability (within statistical bounds)
   - **Feature: network-simulator, Property 4: Reliability Application**

5. **Property 5: Graph-Based Routing**
   - Generate random directed graph topologies
   - Send messages between nodes
   - Verify messages only traverse existing edges
   - **Feature: network-simulator, Property 5: Graph-Based Routing**

6. **Property 6: Send Success Result**
   - Generate random messages
   - Send messages that can be accepted
   - Verify future resolves to true
   - **Feature: network-simulator, Property 6: Send Success Result**

7. **Property 7: Send Timeout Result**
   - Generate random messages
   - Create congestion scenarios
   - Use short timeouts
   - Verify future resolves to false on timeout
   - **Feature: network-simulator, Property 7: Send Timeout Result**

8. **Property 8: Send Does Not Guarantee Delivery**
   - Configure edges with reliability < 1.0
   - Send messages that return true
   - Verify some messages don't arrive at destination
   - **Feature: network-simulator, Property 8: Send Does Not Guarantee Delivery**

9. **Property 9: Receive Returns Sent Message**
   - Generate random messages
   - Send messages with reliability = 1.0
   - Receive at destination
   - Verify received message matches sent message
   - **Feature: network-simulator, Property 9: Receive Returns Sent Message**

10. **Property 10: Receive Timeout Exception**
    - Call receive with short timeout
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 10: Receive Timeout Exception**

11. **Property 11: Connect Uses Specified Source Port**
    - Generate random source ports
    - Connect with specified source port
    - Verify connection local endpoint has that port
    - **Feature: network-simulator, Property 11: Connect Uses Specified Source Port**

12. **Property 12: Connect Assigns Unique Ephemeral Ports**
    - Create multiple connections without specifying source port
    - Verify each connection has unique source port
    - **Feature: network-simulator, Property 12: Connect Assigns Unique Ephemeral Ports**

13. **Property 13: Successful Connection Returns Connection Object**
    - Generate random destination addresses/ports
    - Connect to reachable destinations
    - Verify connection object is valid and open
    - **Feature: network-simulator, Property 13: Successful Connection Returns Connection Object**

14. **Property 14: Connect Timeout Exception**
    - Connect to unreachable destinations with short timeout
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 14: Connect Timeout Exception**

15. **Property 15: Successful Bind Returns Listener**
    - Generate random ports
    - Bind to available ports
    - Verify listener object is valid and listening
    - **Feature: network-simulator, Property 15: Successful Bind Returns Listener**

16. **Property 16: Bind Timeout Exception**
    - Attempt bind with conflicts and short timeout
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 16: Bind Timeout Exception**

17. **Property 17: Accept Returns Connection on Client Connect**
    - Server binds and calls accept
    - Client connects
    - Verify server gets valid connection
    - **Feature: network-simulator, Property 17: Accept Returns Connection on Client Connect**

18. **Property 18: Accept Timeout Exception**
    - Call accept with short timeout and no connecting clients
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 18: Accept Timeout Exception**

19. **Property 19: Connection Read-Write Round Trip**
    - Generate random data
    - Write data to one end of connection
    - Read from other end
    - Verify data matches (with reliability = 1.0)
    - **Feature: network-simulator, Property 19: Connection Read-Write Round Trip**

20. **Property 20: Read Timeout Exception**
    - Call read with short timeout and no available data
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 20: Read Timeout Exception**

21. **Property 21: Successful Write Returns True**
    - Generate random data
    - Write to open connection
    - Verify future resolves to true
    - **Feature: network-simulator, Property 21: Successful Write Returns True**

22. **Property 22: Write Timeout Exception**
    - Create congestion scenario
    - Write with short timeout
    - Verify timeout exception is thrown
    - **Feature: network-simulator, Property 22: Write Timeout Exception**

### Integration Testing

Integration tests will verify end-to-end scenarios:

1. **Client-Server Communication**: Full connection establishment, data transfer, and teardown
2. **Multi-Node Topology**: Messages routed through intermediate nodes
3. **Network Partition Simulation**: Verify behavior when edges are removed
4. **Concurrent Operations**: Multiple nodes sending/receiving simultaneously
5. **Stress Testing**: High message volume with varying latency/reliability

### Test Infrastructure

- **Test Fixtures**: Reusable network topologies for common scenarios
- **Mock Time**: Ability to advance simulated time for latency testing
- **Deterministic RNG**: Seeded random number generator for reproducible reliability tests
- **Assertion Helpers**: Custom assertions for future results and exception types

## Implementation Notes

### Thread Safety

- All public methods of `NetworkSimulator`, `NetworkNode`, `Connection`, and `Listener` are thread-safe
- Internal state protected by `std::shared_mutex` for read-write locking
- Message queues and connection buffers use mutexes and condition variables

### Performance Considerations

- **Lock Granularity**: Fine-grained locking to minimize contention
- **Lock-Free Queues**: Consider lock-free data structures for message queues
- **Thread Pool**: Use folly::Executor for async operations to avoid thread creation overhead
- **Memory Management**: Use `std::shared_ptr` for shared ownership, avoid unnecessary copies

### Latency Simulation

- Use `std::this_thread::sleep_for()` or timer-based scheduling for latency delays
- Consider using a priority queue ordered by delivery time for efficient scheduling
- Option for "fast-forward" mode that skips actual time delays for testing

### Reliability Simulation

- Use `std::bernoulli_distribution` with configured reliability as probability
- Seed RNG for reproducible tests
- Option for deterministic mode that disables random drops

### Future Implementation

The design uses folly::Future as the concrete future implementation, which provides:
- Composable async operations via `then()`, `onError()`, `ensure()`
- Timeout support via `within()`
- Thread pool integration via executors
- Exception propagation through the future chain

## Dependencies

- **folly**: For Future/Promise implementation and executor framework
- **C++23 compiler**: For concepts, ranges, and modern language features
- **Boost.Graph** (optional): For advanced graph algorithms if needed
- **RapidCheck or similar**: For property-based testing framework

## Build System

- **CMake**: Build system configuration
- **C++23 standard**: Required language standard
- **Compiler support**: GCC 13+, Clang 16+, or MSVC 2022+

## Future Enhancements

1. **Advanced Routing**: Implement shortest-path routing algorithms
2. **Bandwidth Simulation**: Add bandwidth limits to edges
3. **Packet Reordering**: Simulate out-of-order delivery
4. **Network Partitions**: Dynamic topology changes during simulation
5. **Statistics Collection**: Detailed metrics on message delivery, latency distribution, etc.
6. **Visualization**: Tools to visualize topology and message flow
7. **Protocol Implementations**: Built-in implementations of common protocols (TCP, UDP, etc.)
8. **Failure Injection**: Programmatic injection of various failure modes
