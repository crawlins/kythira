# Network Simulator Design Document

## Overview

The Network Simulator is a C++ library that models network communication between nodes using modern C++23 concepts and template metaprogramming. The design provides both connectionless (datagram) and connection-oriented (stream) communication patterns with configurable network characteristics including latency and reliability.

The simulator uses a directed graph topology where nodes are vertices and communication paths are edges weighted by latency and reliability parameters. This allows realistic simulation of network conditions including delays, packet loss, and network partitions.

The design emphasizes type safety through C++ concepts, asynchronous operations using folly::Future-like semantics, and flexibility through template parameterization of address and port types.

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
│   - send()                 │  │  - connect()               │
│   - receive()              │  │  - bind()                  │
│                            │  │  - listen()                │
│                            │  │  - read() / write()        │
└────────────┬───────────────┘  └────────────┬───────────────┘
             │                               │
             └───────────────┬───────────────┘
                             ▼
                ┌────────────────────────────┐
                │      Network Node          │
                │  (Template parameterized)  │
                └────────────┬───────────────┘
                             │
                             ▼
                ┌────────────────────────────┐
                │   Network Simulator Core   │
                │  - Topology Management     │
                │  - Message Routing         │
                │  - Latency Simulation      │
                │  - Reliability Simulation  │
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

1. **Concept Layer**: Defines compile-time contracts for types
2. **Core Simulator Layer**: Implements network topology and message routing
3. **Node Layer**: Provides network node abstraction with send/receive/connect/bind
4. **Connection Layer**: Implements connection-oriented communication
5. **Future Layer**: Provides asynchronous operation results

## Components and Interfaces

### 1. Concepts (Type Constraints)

#### 1.1 Address Concept

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

#### 1.2 Port Concept

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

#### 1.3 Try Concept

```cpp
template<typename T, typename V>
concept try_type = requires(T t) {
    // Must provide value access
    { t.value() } -> std::same_as<V>;
    
    // Must provide exception access
    { t.exception() } -> std::same_as<std::exception_ptr>;
    
    // Must be able to check if contains value
    { t.has_value() } -> std::convertible_to<bool>;
    
    // Must be able to check if contains exception
    { t.has_exception() } -> std::convertible_to<bool>;
};
```

#### 1.4 Future Concept

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

// Collective future operations (modeled after folly::collectAny and folly::collectAll)

// Wait for any future to complete (modeled after folly::collectAny)
// Returns a future of tuple containing the index and Try<T> of the first completed future
template<std::ranges::range R>
    requires future<std::ranges::range_value_t<R>, typename std::ranges::range_value_t<R>::value_type>
auto wait_for_any(R&& futures) -> future<std::tuple<std::size_t, Try<typename std::ranges::range_value_t<R>::value_type>>>;

// Wait for all futures to complete (modeled after folly::collectAll)
// Returns a future of range containing Try<T> for each future (preserving order)
template<std::ranges::range R>
    requires future<std::ranges::range_value_t<R>, typename std::ranges::range_value_t<R>::value_type>
auto wait_for_all(R&& futures) -> future<std::ranges::range<try_type<typename std::ranges::range_value_t<R>::value_type>> auto>;
```

#### 1.5 Message Concept

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

#### 1.6 Connection Concept

```cpp
template<typename C>
concept connection = requires(C conn, std::vector<std::byte> data) {
    // Must support reading data
    { conn.read() } -> future<std::vector<std::byte>>;
    { conn.read(std::chrono::milliseconds{100}) } -> future<std::vector<std::byte>>;
    
    // Must support writing data
    { conn.write(data) } -> future<bool>;
    { conn.write(data, std::chrono::milliseconds{100}) } -> future<bool>;
    
    // Must be closeable
    { conn.close() } -> std::same_as<void>;
    
    // Must provide connection state
    { conn.is_open() } -> std::convertible_to<bool>;
};
```

#### 1.7 Listener Concept

```cpp
template<typename L, typename Conn>
concept listener = requires(L lstn) {
    // Must support accepting connections
    { lstn.accept() } -> future<Conn>;
    { lstn.accept(std::chrono::milliseconds{100}) } -> future<Conn>;
    
    // Must be closeable
    { lstn.close() } -> std::same_as<void>;
    
    // Must provide listener state
    { lstn.is_listening() } -> std::convertible_to<bool>;
};
```

#### 1.8 Endpoint Concept

```cpp
template<typename E, typename Addr, typename Port>
concept endpoint = requires(E ep) {
    // Must provide address access
    { ep.address() } -> std::same_as<Addr>;
    
    // Must provide port access
    { ep.port() } -> std::same_as<Port>;
    
    // Must support equality comparison
    { ep == ep } -> std::convertible_to<bool>;
    { ep != ep } -> std::convertible_to<bool>;
};
```

#### 1.9 Network Edge Concept

```cpp
template<typename E>
concept network_edge = requires(E edge) {
    // Must provide latency
    { edge.latency() } -> std::same_as<std::chrono::milliseconds>;
    
    // Must provide reliability (0.0 to 1.0)
    { edge.reliability() } -> std::convertible_to<double>;
};
```

#### 1.10 Network Node Concept

```cpp
template<typename N, typename Addr, typename Port, typename Msg, typename Conn, typename Lstn>
concept network_node = requires(N node, Msg msg, Addr addr, Port port) {
    // Connectionless operations
    { node.send(msg) } -> future<bool>;
    { node.send(msg, std::chrono::milliseconds{100}) } -> future<bool>;
    { node.receive() } -> future<Msg>;
    { node.receive(std::chrono::milliseconds{100}) } -> future<Msg>;
    
    // Connection-oriented client operations
    { node.connect(addr, port) } -> future<Conn>;
    { node.connect(addr, port, port) } -> future<Conn>;  // with source port
    { node.connect(addr, port, std::chrono::milliseconds{100}) } -> future<Conn>;
    
    // Connection-oriented server operations
    { node.bind() } -> future<Lstn>;  // bind to random port
    { node.bind(port) } -> future<Lstn>;  // bind to specific port
    { node.bind(port, std::chrono::milliseconds{100}) } -> future<Lstn>;
    
    // Node identity
    { node.address() } -> std::same_as<Addr>;
};
```

#### 1.11 Network Simulator Concept

```cpp
template<typename S, typename Addr, typename Port, typename Node, typename Edge>
concept network_simulator = requires(S sim, Addr addr, Edge edge) {
    // Edge must satisfy network_edge concept
    requires network_edge<Edge>;
    
    // Topology configuration
    { sim.add_node(addr) } -> std::same_as<void>;
    { sim.remove_node(addr) } -> std::same_as<void>;
    { sim.add_edge(addr, addr, edge) } -> std::same_as<void>;
    { sim.remove_edge(addr, addr) } -> std::same_as<void>;
    
    // Node creation
    { sim.create_node(addr) } -> std::same_as<std::shared_ptr<Node>>;
    
    // Simulation control
    { sim.start() } -> std::same_as<void>;
    { sim.stop() } -> std::same_as<void>;
    { sim.reset() } -> std::same_as<void>;
};
```

### 2. Core Data Structures

#### 2.1 Message Structure

```cpp
template<address Addr, port Port>
class Message {
public:
    Message(Addr src_addr, Port src_port, 
            Addr dst_addr, Port dst_port,
            std::vector<std::byte> payload = {});
    
    auto source_address() const -> Addr;
    auto source_port() const -> Port;
    auto destination_address() const -> Addr;
    auto destination_port() const -> Port;
    auto payload() const -> const std::vector<std::byte>&;
    
private:
    Addr _source_address;
    Port _source_port;
    Addr _destination_address;
    Port _destination_port;
    std::vector<std::byte> _payload;
};
```

#### 2.2 Network Edge

```cpp
struct NetworkEdge {
    std::chrono::milliseconds latency;
    double reliability;  // 0.0 to 1.0
    
    NetworkEdge(std::chrono::milliseconds lat, double rel)
        : latency(lat), reliability(rel) {}
};
```

#### 2.3 Endpoint

```cpp
template<address Addr, port Port>
struct Endpoint {
    Addr address;
    Port port;
    
    auto operator==(const Endpoint&) const -> bool = default;
};

// Hash specialization for use in unordered containers
template<address Addr, port Port>
struct std::hash<Endpoint<Addr, Port>> {
    auto operator()(const Endpoint<Addr, Port>& ep) const -> std::size_t {
        std::size_t h1 = std::hash<Addr>{}(ep.address);
        std::size_t h2 = std::hash<Port>{}(ep.port);
        return h1 ^ (h2 << 1);
    }
};
```

### 3. Network Simulator Core

#### 3.1 NetworkSimulator Class

```cpp
template<address Addr, port Port>
class NetworkSimulator {
public:
    // Topology configuration
    auto add_node(Addr address) -> void;
    auto remove_node(Addr address) -> void;
    auto add_edge(Addr from, Addr to, NetworkEdge edge) -> void;
    auto remove_edge(Addr from, Addr to) -> void;
    
    // Node creation
    auto create_node(Addr address) -> std::shared_ptr<NetworkNode<Addr, Port>>;
    
    // Simulation control
    auto start() -> void;
    auto stop() -> void;
    auto reset() -> void;
    
private:
    // Directed graph: adjacency list representation
    std::unordered_map<Addr, std::unordered_map<Addr, NetworkEdge>> _topology;
    
    // Active nodes
    std::unordered_map<Addr, std::shared_ptr<NetworkNode<Addr, Port>>> _nodes;
    
    // Message queues per node
    std::unordered_map<Addr, std::queue<Message<Addr, Port>>> _message_queues;
    
    // Connection state
    std::unordered_map<Endpoint<Addr, Port>, std::shared_ptr<Connection<Addr, Port>>> _connections;
    
    // Listeners
    std::unordered_map<Endpoint<Addr, Port>, std::shared_ptr<Listener<Addr, Port>>> _listeners;
    
    // Thread pool for async operations
    folly::Executor* _executor;
    
    // Random number generator for reliability simulation
    std::mt19937 _rng;
    
    // Mutex for thread safety
    mutable std::shared_mutex _mutex;
    
    // Internal routing and delivery
    auto route_message(Message<Addr, Port> msg) -> folly::Future<bool>;
    auto deliver_message(Message<Addr, Port> msg) -> void;
    auto apply_latency(Addr from, Addr to) -> std::chrono::milliseconds;
    auto check_reliability(Addr from, Addr to) -> bool;
};
```

### 4. Network Node Implementation

#### 4.1 NetworkNode Class

```cpp
template<address Addr, port Port>
class NetworkNode {
public:
    explicit NetworkNode(Addr addr, NetworkSimulator<Addr, Port>* simulator);
    
    // Connectionless operations
    auto send(Message<Addr, Port> msg) -> folly::Future<bool>;
    auto send(Message<Addr, Port> msg, std::chrono::milliseconds timeout) -> folly::Future<bool>;
    auto receive() -> folly::Future<Message<Addr, Port>>;
    auto receive(std::chrono::milliseconds timeout) -> folly::Future<Message<Addr, Port>>;
    
    // Connection-oriented client operations
    auto connect(Addr dst_addr, Port dst_port) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    auto connect(Addr dst_addr, Port dst_port, Port src_port) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    auto connect(Addr dst_addr, Port dst_port, std::chrono::milliseconds timeout) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    
    // Connection-oriented server operations
    auto bind() -> folly::Future<std::shared_ptr<Listener<Addr, Port>>>;  // bind to random port
    auto bind(Port port) -> folly::Future<std::shared_ptr<Listener<Addr, Port>>>;  // bind to specific port
    auto bind(Port port, std::chrono::milliseconds timeout) -> folly::Future<std::shared_ptr<Listener<Addr, Port>>>;
    
    // Node identity
    auto address() const -> Addr { return _address; }
    
private:
    Addr _address;
    NetworkSimulator<Addr, Port>* _simulator;
    
    // Ephemeral port allocation
    auto allocate_ephemeral_port() -> Port;
    std::unordered_set<Port> _used_ports;
    mutable std::mutex _port_mutex;
};
```

### 5. Connection Implementation

#### 5.1 Connection Class

```cpp
template<address Addr, port Port>
class Connection {
public:
    Connection(Endpoint<Addr, Port> local, 
               Endpoint<Addr, Port> remote,
               NetworkSimulator<Addr, Port>* simulator);
    
    auto read() -> folly::Future<std::vector<std::byte>>;
    auto read(std::chrono::milliseconds timeout) -> folly::Future<std::vector<std::byte>>;
    
    auto write(std::vector<std::byte> data) -> folly::Future<bool>;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> folly::Future<bool>;
    
    auto close() -> void;
    auto is_open() const -> bool;
    
    auto local_endpoint() const -> Endpoint<Addr, Port> { return _local; }
    auto remote_endpoint() const -> Endpoint<Addr, Port> { return _remote; }
    
private:
    Endpoint<Addr, Port> _local;
    Endpoint<Addr, Port> _remote;
    NetworkSimulator<Addr, Port>* _simulator;
    
    std::atomic<bool> _open{true};
    
    // Buffered data for this connection
    std::queue<std::vector<std::byte>> _read_buffer;
    mutable std::mutex _buffer_mutex;
    std::condition_variable _data_available;
};
```

#### 5.2 Listener Class

```cpp
template<address Addr, port Port>
class Listener {
public:
    Listener(Endpoint<Addr, Port> local_endpoint,
             NetworkSimulator<Addr, Port>* simulator);
    
    auto accept() -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    auto accept(std::chrono::milliseconds timeout) -> folly::Future<std::shared_ptr<Connection<Addr, Port>>>;
    
    auto close() -> void;
    auto is_listening() const -> bool;
    
    auto local_endpoint() const -> Endpoint<Addr, Port> { return _local; }
    
private:
    Endpoint<Addr, Port> _local;
    NetworkSimulator<Addr, Port>* _simulator;
    
    std::atomic<bool> _listening{true};
    
    // Queue of pending connections
    std::queue<std::shared_ptr<Connection<Addr, Port>>> _pending_connections;
    mutable std::mutex _queue_mutex;
    std::condition_variable _connection_available;
};
```

## Data Models

### Topology Graph Model

The network topology is modeled as a directed graph where:
- **Vertices**: Network nodes identified by addresses
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


## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Topology Edge Latency Preservation

*For any* pair of nodes and configured latency value, when an edge is added to the topology with that latency, querying the topology SHALL return the same latency value.

**Validates: Requirements 1.1**

### Property 2: Topology Edge Reliability Preservation

*For any* pair of nodes and configured reliability value, when an edge is added to the topology with that reliability, querying the topology SHALL return the same reliability value.

**Validates: Requirements 1.2**

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

**Validates: Requirements 7.2**

### Property 16: Bind Timeout Exception

*For any* bind operation with a timeout where the bind cannot complete before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 7.3**

### Property 17: Accept Returns Connection on Client Connect

*For any* listener with a pending accept operation, when a client connects to the bound port, the accept future SHALL resolve to a valid connection object.

**Validates: Requirements 7.5**

### Property 18: Accept Timeout Exception

*For any* accept operation with a timeout where no client connects before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 7.6**

### Property 19: Connection Read-Write Round Trip

*For any* data written to one end of a connection, reading from the other end SHALL return the same data (subject to network reliability and latency).

**Validates: Requirements 8.2**

### Property 20: Read Timeout Exception

*For any* read operation with a timeout where no data is available before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 8.3**

### Property 21: Successful Write Returns True

*For any* write operation that successfully queues data for transmission, the future SHALL resolve to true.

**Validates: Requirements 8.5**

### Property 22: Write Timeout Exception

*For any* write operation with a timeout where the write cannot complete before the timeout expires, the future SHALL enter an error state with a timeout exception.

**Validates: Requirements 8.6**

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
