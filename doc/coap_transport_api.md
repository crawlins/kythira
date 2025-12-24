# CoAP Transport API Documentation

## Overview

The CoAP transport implementation provides concrete implementations of the `network_client` and `network_server` concepts for the Raft consensus algorithm using the CoAP (Constrained Application Protocol) protocol with generic future support. This enables Raft clusters to communicate over CoAP/UDP with optional DTLS encryption, making it suitable for deployment in IoT environments, constrained networks, and scenarios requiring lightweight, efficient communication.

The transport layer has been updated to use the generic future architecture, allowing it to work with different future implementations while maintaining the same API structure.

## Quick Start

### Basic Usage with Generic Futures

```cpp
#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/future.hpp>
#include <concepts/future.hpp>

// Define future types for CoAP operations
using RequestVoteFuture = kythira::Future<raft::request_vote_response<>>;
using AppendEntriesFuture = kythira::Future<raft::append_entries_response<>>;
using InstallSnapshotFuture = kythira::Future<raft::install_snapshot_response<>>;

// Create client configuration
coap_client_config client_config;
client_config.ack_timeout = std::chrono::milliseconds{2000};
client_config.max_retransmit = 4;

// Create endpoint mapping
std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coap://192.168.1.10:5683"},
    {2, "coap://192.168.1.11:5683"},
    {3, "coap://192.168.1.12:5683"}
};

// Create CoAP client with generic future types
auto client = kythira::coap_client<
    RequestVoteFuture,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics,
    raft::console_logger
>(endpoints, client_config, metrics_instance, logger_instance);

// Send RequestVote RPC
auto request = raft::request_vote_request<>{
    .term = 5,
    .candidate_id = 1,
    .last_log_index = 100,
    .last_log_term = 4
};

auto future = client.send_request_vote(
    2,  // target node
    request,
    std::chrono::milliseconds{5000}  // timeout
);

// Handle response using generic future interface
future.then([](const raft::request_vote_response<>& response) {
    if (response.vote_granted) {
        std::cout << "Vote granted from term " << response.term << std::endl;
    }
}).onError([](std::exception_ptr ex) {
    try {
        std::rethrow_exception(ex);
    } catch (const std::exception& e) {
        std::cerr << "RPC failed: " << e.what() << std::endl;
    }
});
```

### Server Setup with Generic Futures

```cpp
#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/future.hpp>

// Create server configuration
coap_server_config server_config;
server_config.max_concurrent_sessions = 200;
server_config.max_request_size = 64 * 1024;

// Create CoAP server (server implementation is being updated for generic futures)
// This example shows the intended API structure
auto server = kythira::coap_server<
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics,
    raft::console_logger
>(
    "0.0.0.0",  // bind address
    5683,       // bind port
    server_config,
    metrics_instance,
    logger_instance
);

// Register RequestVote handler
server.register_request_vote_handler([](const raft::request_vote_request<>& request) {
    // Process vote request
    return raft::request_vote_response<>{
        .term = request.term,
        .vote_granted = true
    };
});

// Start server
server.start();

// Server is now listening for CoAP requests
std::cout << "CoAP server running on port 5683" << std::endl;

// Stop server when done
server.stop();
```

## API Reference

### coap_client<FutureType, RPC_Serializer, Metrics, Logger>

Template class implementing the `network_client` concept for CoAP communication with generic future support.

#### Template Parameters

- `FutureType`: The future type to use for async operations (must satisfy `kythira::future` concept)
- `RPC_Serializer`: Serializer for RPC messages (must satisfy `rpc_serializer` concept)
- `Metrics`: Metrics recorder type (must satisfy `metrics` concept)
- `Logger`: Logger type (must satisfy `diagnostic_logger` concept)

#### Constructor

```cpp
coap_client(
    const std::unordered_map<std::uint64_t, std::string>& node_id_to_endpoint_map,
    const coap_client_config& config,
    Metrics& metrics,
    Logger& logger
);
```

**Parameters:**
- `node_id_to_endpoint_map`: Mapping from Raft node IDs to CoAP endpoints
- `config`: Client configuration settings
- `metrics`: Metrics recorder for monitoring
- `logger`: Logger for diagnostic information

**Example endpoint formats:**
- `"coap://192.168.1.10:5683"` - Plain CoAP
- `"coaps://192.168.1.10:5684"` - CoAP over DTLS
- `"coap://[::1]:5683"` - IPv6 address

#### Methods

##### send_request_vote

```cpp
auto send_request_vote(
    std::uint64_t target,
    const raft::request_vote_request<>& request,
    std::chrono::milliseconds timeout
) -> FutureType;
```

Sends a RequestVote RPC to the specified target node using the generic future type.

**Parameters:**
- `target`: Target node ID
- `request`: RequestVote request message
- `timeout`: Request timeout

**Returns:** Future of the specified type that resolves to the response or error

**Throws:**
- `coap_timeout_error`: Request timed out
- `coap_client_error`: 4.xx response from server
- `coap_server_error`: 5.xx response from server
- `coap_network_error`: Network connectivity issues
- `serialization_error`: Message serialization/deserialization failed

##### send_append_entries

```cpp
auto send_append_entries(
    std::uint64_t target,
    const raft::append_entries_request<>& request,
    std::chrono::milliseconds timeout
) -> FutureType;
```

Sends an AppendEntries RPC to the specified target node using the generic future type.

**Parameters:**
- `target`: Target node ID
- `request`: AppendEntries request message
- `timeout`: Request timeout

**Returns:** Future of the specified type that resolves to the response or error

**Note:** Large requests automatically use CoAP block-wise transfer when enabled.

##### send_install_snapshot

```cpp
auto send_install_snapshot(
    std::uint64_t target,
    const raft::install_snapshot_request<>& request,
    std::chrono::milliseconds timeout
) -> FutureType;
```

Sends an InstallSnapshot RPC to the specified target node using the generic future type.

**Parameters:**
- `target`: Target node ID
- `request`: InstallSnapshot request message
- `timeout`: Request timeout

**Returns:** Future of the specified type that resolves to the response or error

**Note:** Snapshot data automatically uses CoAP block-wise transfer for large payloads.

### coap_server<RPC_Serializer, Metrics, Logger>

Template class implementing the `network_server` concept for CoAP communication with generic future support.

#### Template Parameters

- `RPC_Serializer`: Serializer for RPC messages (must satisfy `rpc_serializer` concept)
- `Metrics`: Metrics recorder type (must satisfy `metrics` concept)
- `Logger`: Logger type (must satisfy `diagnostic_logger` concept)

#### Constructor

```cpp
coap_server(
    const std::string& bind_address,
    std::uint16_t bind_port,
    const coap_server_config& config,
    Metrics& metrics,
    Logger& logger
);
```

**Parameters:**
- `bind_address`: Address to bind to (e.g., "0.0.0.0", "::1")
- `bind_port`: Port to bind to (typically 5683 for CoAP, 5684 for CoAPS)
- `config`: Server configuration settings
- `metrics`: Metrics recorder for monitoring
- `logger`: Logger for diagnostic information

#### Methods

##### register_request_vote_handler

```cpp
auto register_request_vote_handler(
    std::function<raft::request_vote_response<>(const raft::request_vote_request<>&)> handler
) -> void;
```

Registers a handler function for RequestVote RPCs.

**Parameters:**
- `handler`: Function to handle RequestVote requests

**Handler Requirements:**
- Must be thread-safe (may be called concurrently)
- Should not block for extended periods
- Should handle exceptions gracefully

##### register_append_entries_handler

```cpp
auto register_append_entries_handler(
    std::function<raft::append_entries_response<>(const raft::append_entries_request<>&)> handler
) -> void;
```

Registers a handler function for AppendEntries RPCs.

##### register_install_snapshot_handler

```cpp
auto register_install_snapshot_handler(
    std::function<raft::install_snapshot_response<>(const raft::install_snapshot_request<>&)> handler
) -> void;
```

Registers a handler function for InstallSnapshot RPCs.

##### start

```cpp
auto start() -> void;
```

Starts the CoAP server and begins listening for requests.

**Throws:**
- `coap_network_error`: Failed to bind to address/port
- `coap_security_error`: DTLS setup failed
- `std::runtime_error`: Server already running

##### stop

```cpp
auto stop() -> void;
```

Stops the CoAP server and closes all connections.

**Note:** Gracefully completes in-flight requests before shutting down.

##### is_running

```cpp
auto is_running() const -> bool;
```

Returns whether the server is currently running.

## Configuration

### coap_client_config

Configuration structure for CoAP client behavior.

```cpp
struct coap_client_config {
    // CoAP Protocol Parameters (RFC 7252)
    std::chrono::milliseconds ack_timeout{2000};
    std::chrono::milliseconds ack_random_factor_ms{1000};
    std::size_t max_retransmit{4};
    std::chrono::milliseconds nstart{1};
    std::chrono::milliseconds default_leisure{5000};
    std::chrono::milliseconds probing_rate{1000};
    
    // DTLS Configuration
    bool enable_dtls{false};
    std::string psk_identity{};
    std::vector<std::byte> psk_key{};
    std::string cert_file{};
    std::string key_file{};
    std::string ca_file{};
    bool verify_peer_cert{true};
    
    // Block Transfer
    std::size_t max_block_size{1024};
    bool enable_block_transfer{true};
    
    // Connection Management
    std::size_t max_sessions{100};
    std::chrono::seconds session_timeout{300};
};
```

#### Key Parameters

**ack_timeout**: Base timeout for confirmable messages (default: 2000ms)
- Shorter values: Faster failure detection, more network traffic
- Longer values: Better tolerance for slow networks, slower failure detection

**max_retransmit**: Maximum number of retransmission attempts (default: 4)
- Higher values: Better reliability, longer failure detection time
- Lower values: Faster failure detection, less reliable delivery

**max_block_size**: Maximum block size for block-wise transfer (default: 1024 bytes)
- Larger blocks: Better throughput, higher memory usage
- Smaller blocks: Lower memory usage, more protocol overhead

### coap_server_config

Configuration structure for CoAP server behavior.

```cpp
struct coap_server_config {
    std::size_t max_concurrent_sessions{200};
    std::size_t max_request_size{64 * 1024};  // 64 KB
    std::chrono::seconds session_timeout{300};
    
    // DTLS Configuration
    bool enable_dtls{false};
    std::string psk_identity{};
    std::vector<std::byte> psk_key{};
    std::string cert_file{};
    std::string key_file{};
    std::string ca_file{};
    bool verify_peer_cert{true};
    
    // Block Transfer
    std::size_t max_block_size{1024};
    bool enable_block_transfer{true};
    
    // Multicast Support
    bool enable_multicast{false};
    std::string multicast_address{"224.0.1.187"};
    std::uint16_t multicast_port{5683};
};
```

#### Key Parameters

**max_concurrent_sessions**: Maximum number of concurrent DTLS sessions (default: 200)
- Higher values: Support more concurrent clients, higher memory usage
- Lower values: Lower memory usage, may reject clients under load

**max_request_size**: Maximum size of incoming requests (default: 64KB)
- Larger values: Support larger Raft messages, higher memory usage
- Smaller values: Lower memory usage, may reject large snapshots

## Error Handling

### Exception Hierarchy

```cpp
coap_transport_error
├── coap_client_error        // 4.xx response codes
├── coap_server_error        // 5.xx response codes
├── coap_timeout_error       // Request timeout
├── coap_security_error      // DTLS/security issues
├── coap_protocol_error      // CoAP protocol violations
└── coap_network_error       // Network connectivity issues
```

### Common Error Scenarios

#### Network Connectivity

```cpp
try {
    auto response = client.send_request_vote(target, request, timeout).get();
} catch (const coap_network_error& e) {
    // Handle network issues: DNS resolution, unreachable host, etc.
    std::cerr << "Network error: " << e.what() << std::endl;
    // Consider: retry with different endpoint, mark node as unavailable
}
```

#### Request Timeout

```cpp
try {
    auto response = client.send_request_vote(target, request, timeout).get();
} catch (const coap_timeout_error& e) {
    // Handle timeout: slow network, overloaded server, etc.
    std::cerr << "Request timeout: " << e.what() << std::endl;
    // Consider: retry with longer timeout, mark node as slow
}
```

#### Server Errors

```cpp
try {
    auto response = client.send_request_vote(target, request, timeout).get();
} catch (const coap_server_error& e) {
    // Handle server errors: internal server error, service unavailable, etc.
    std::cerr << "Server error " << e.response_code() << ": " << e.what() << std::endl;
    // Consider: retry later, mark server as temporarily unavailable
}
```

#### DTLS Security Errors

```cpp
try {
    auto response = client.send_request_vote(target, request, timeout).get();
} catch (const coap_security_error& e) {
    // Handle security issues: certificate validation, handshake failure, etc.
    std::cerr << "Security error: " << e.what() << std::endl;
    // Consider: check certificates, verify PSK configuration
}
```

## Advanced Usage

### Custom Serializers

The CoAP transport supports any serializer implementing the `rpc_serializer` concept:

```cpp
// Using CBOR serializer for compact binary encoding
#include <raft/cbor_serializer.hpp>
auto client = coap_client<cbor_serializer>(endpoints, config, metrics);

// Using custom serializer
class my_serializer {
public:
    template<typename T>
    auto serialize(const T& obj) const -> std::vector<std::byte>;
    
    template<typename T>
    auto deserialize(const std::vector<std::byte>& data) const -> T;
    
    auto content_format() const -> std::uint16_t;
};

auto client = coap_client<my_serializer>(endpoints, config, metrics);
```

### Metrics Integration

The CoAP transport provides detailed metrics for monitoring:

```cpp
class my_metrics : public metrics_recorder {
public:
    void record_request_sent(const std::string& rpc_type, std::uint64_t target) override {
        // Record outgoing request metrics
    }
    
    void record_request_completed(const std::string& rpc_type, 
                                 std::chrono::milliseconds duration,
                                 bool success) override {
        // Record request completion metrics
    }
    
    void record_request_received(const std::string& rpc_type) override {
        // Record incoming request metrics
    }
};

auto metrics = std::make_shared<my_metrics>();
auto client = coap_client<json_serializer>(endpoints, config, metrics);
```

### Asynchronous Operations

All client operations return `folly::Future` for asynchronous handling:

```cpp
// Chain multiple operations
client.send_request_vote(1, vote_request, timeout)
    .then([&](const request_vote_response<>& response) {
        if (response.vote_granted) {
            return client.send_append_entries(1, append_request, timeout);
        }
        return folly::makeFuture<append_entries_response<>>(
            std::runtime_error("Vote not granted"));
    })
    .then([](const append_entries_response<>& response) {
        std::cout << "AppendEntries successful" << std::endl;
    })
    .onError([](const std::exception& e) {
        std::cerr << "Operation failed: " << e.what() << std::endl;
    });

// Parallel operations
std::vector<folly::Future<request_vote_response<>>> futures;
for (auto target : {2, 3, 4}) {
    futures.push_back(client.send_request_vote(target, request, timeout));
}

folly::collectAll(futures)
    .then([](const std::vector<folly::Try<request_vote_response<>>>& results) {
        int votes = 0;
        for (const auto& result : results) {
            if (result.hasValue() && result.value().vote_granted) {
                votes++;
            }
        }
        std::cout << "Received " << votes << " votes" << std::endl;
    });
```

## CoAP Protocol Details

### Resource Paths

The CoAP transport uses the following resource paths:

- **RequestVote**: `POST /raft/request_vote`
- **AppendEntries**: `POST /raft/append_entries`
- **InstallSnapshot**: `POST /raft/install_snapshot`

### CoAP Options

**Request Options:**
- `Content-Format`: Serialization format (e.g., 60 for CBOR, 50 for JSON)
- `Accept`: Expected response format
- `Block1`: For large request payloads (block-wise transfer)

**Response Options:**
- `Content-Format`: Serialization format of response
- `Block2`: For large response payloads (block-wise transfer)

### Message Types

- **Confirmable (CON)**: Requires acknowledgment, used for reliable delivery
- **Non-confirmable (NON)**: No acknowledgment required, used for best-effort delivery
- **Acknowledgment (ACK)**: Acknowledges a confirmable message
- **Reset (RST)**: Indicates inability to process a message

### Response Codes

**Success:**
- `2.05 Content`: Successful response with payload

**Client Errors:**
- `4.00 Bad Request`: Malformed request or invalid parameters
- `4.04 Not Found`: Resource not found
- `4.13 Request Entity Too Large`: Request payload exceeds limits

**Server Errors:**
- `5.00 Internal Server Error`: Handler threw exception
- `5.03 Service Unavailable`: Server overloaded or shutting down

## Thread Safety

Both `coap_client` and `coap_server` are thread-safe:

- Multiple threads can safely call client methods concurrently
- Server handlers may be invoked concurrently from multiple threads
- Internal synchronization protects shared state
- folly::Future provides thread-safe asynchronous operations

## Memory Management

The CoAP transport is designed for efficient memory usage:

- Zero-copy message handling where possible
- Automatic cleanup of completed requests
- Session pooling to reduce allocation overhead
- Block-wise transfer for large messages to limit memory usage

## Integration with Raft

### Complete Example

```cpp
#include <raft/raft.hpp>
#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>

int main() {
    // Configure CoAP transport
    coap_client_config client_config;
    client_config.ack_timeout = std::chrono::milliseconds{1000};
    client_config.max_retransmit = 3;
    
    coap_server_config server_config;
    server_config.max_concurrent_sessions = 100;
    
    std::unordered_map<std::uint64_t, std::string> endpoints = {
        {1, "coap://node1:5683"},
        {2, "coap://node2:5683"},
        {3, "coap://node3:5683"}
    };
    
    auto metrics = std::make_shared<console_metrics>();
    
    // Create transport components
    auto client = std::make_shared<coap_client<json_serializer>>(
        endpoints, client_config, metrics);
    auto server = std::make_shared<coap_server<json_serializer>>(
        "0.0.0.0", 5683, server_config, metrics);
    
    // Create Raft node
    auto node = raft_node<
        coap_client<json_serializer>,
        coap_server<json_serializer>,
        memory_persistence,
        console_logger,
        console_metrics
    >(
        1,  // node_id
        client,
        server,
        std::make_shared<memory_persistence>(),
        std::make_shared<console_logger>(),
        metrics
    );
    
    // Start the node
    node.start();
    
    // Node is now participating in Raft consensus over CoAP
    std::cout << "Raft node started with CoAP transport" << std::endl;
    
    // Keep running
    std::this_thread::sleep_for(std::chrono::hours{1});
    
    return 0;
}
```

This example demonstrates a complete Raft node using CoAP transport for all communication.