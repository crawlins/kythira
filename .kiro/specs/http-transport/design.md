# Design Document

## Overview

The HTTP transport implementation provides concrete implementations of the `network_client` and `network_server` concepts for the Raft consensus algorithm using the cpp-httplib library. This design enables Raft clusters to communicate over standard HTTP/1.1 protocol with optional TLS encryption, making it suitable for deployment in web-based infrastructure and cloud environments.

The implementation consists of two main components:
- **cpp_httplib_client**: Implements the `network_client` concept to send Raft RPCs over HTTP
- **cpp_httplib_server**: Implements the `network_server` concept to receive and handle Raft RPCs over HTTP

Both components are templated on an RPC serializer that conforms to the `rpc_serializer` concept, allowing flexible serialization formats (JSON, Protocol Buffers, etc.) to be used independently of the transport layer.

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                        Raft Node                             │
│  (Template parameters: NetworkClient, NetworkServer, ...)    │
└────────────┬──────────────────────────────┬─────────────────┘
             │                              │
             │ uses                         │ uses
             ▼                              ▼
┌────────────────────────┐      ┌────────────────────────────┐
│  cpp_httplib_client    │      │   cpp_httplib_server       │
│  <RPC_Serializer>      │      │   <RPC_Serializer>         │
│                        │      │                            │
│  Implements:           │      │   Implements:              │
│  - network_client      │      │   - network_server         │
│    concept             │      │     concept                │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         │ uses                          │ uses
         ▼                               ▼
┌────────────────────────┐      ┌────────────────────────────┐
│   RPC_Serializer       │      │   RPC_Serializer           │
│   (e.g., JSON)         │      │   (e.g., JSON)             │
│                        │      │                            │
│   Implements:          │      │   Implements:              │
│   - rpc_serializer     │      │   - rpc_serializer         │
│     concept            │      │     concept                │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         │ uses                          │ uses
         ▼                               ▼
┌────────────────────────────────────────────────────────────┐
│                      cpp-httplib                            │
│  (HTTP/1.1 client and server implementation)               │
└────────────────────────────────────────────────────────────┘
```

### Request Flow (Client Side)

1. Raft node calls `send_request_vote()`, `send_append_entries()`, or `send_install_snapshot()` on cpp_httplib_client
2. cpp_httplib_client serializes the request using the RPC_Serializer
3. cpp_httplib_client constructs HTTP POST request with appropriate headers
4. cpp-httplib sends the HTTP request to the target server
5. cpp-httplib receives HTTP response
6. cpp_httplib_client deserializes the response using the RPC_Serializer
7. cpp_httplib_client satisfies the folly::Future with the response

### Request Flow (Server Side)

1. cpp-httplib receives HTTP POST request at an endpoint
2. cpp_httplib_server extracts the request body
3. cpp_httplib_server deserializes the request using the RPC_Serializer
4. cpp_httplib_server invokes the registered handler function
5. Handler function returns a response
6. cpp_httplib_server serializes the response using the RPC_Serializer
7. cpp_httplib_server sends HTTP 200 response with serialized body

## Components and Interfaces

### cpp_httplib_client

**Template Parameters:**
- `RPC_Serializer`: Type that conforms to the `rpc_serializer` concept

**Constructor Parameters:**
- `node_id_to_url_map`: Mapping from node IDs to base URLs (e.g., `{1: "http://node1:8080", 2: "http://node2:8080"}`)
- `config`: Client configuration (connection pool size, timeouts, etc.)
- `metrics`: Metrics recorder instance for monitoring

**Public Methods:**
```cpp
// Implements network_client concept
auto send_request_vote(
    std::uint64_t target,
    const request_vote_request<>& request,
    std::chrono::milliseconds timeout
) -> folly::Future<request_vote_response<>>;

auto send_append_entries(
    std::uint64_t target,
    const append_entries_request<>& request,
    std::chrono::milliseconds timeout
) -> folly::Future<append_entries_response<>>;

auto send_install_snapshot(
    std::uint64_t target,
    const install_snapshot_request<>& request,
    std::chrono::milliseconds timeout
) -> folly::Future<install_snapshot_response<>>;
```

**Private Members:**
- `_serializer`: Instance of RPC_Serializer
- `_node_id_to_url`: Map from node IDs to base URLs
- `_http_client`: cpp-httplib client instance (with connection pooling)
- `_config`: Client configuration
- `_metrics`: Metrics recorder instance
- `_mutex`: Mutex for thread-safe access

**Private Methods:**
```cpp
template<typename Request, typename Response>
auto send_rpc(
    std::uint64_t target,
    const std::string& endpoint,
    const Request& request,
    std::chrono::milliseconds timeout
) -> folly::Future<Response>;

auto get_base_url(std::uint64_t node_id) const -> std::string;
auto set_request_headers(httplib::Headers& headers) const -> void;
```

### cpp_httplib_server

**Template Parameters:**
- `RPC_Serializer`: Type that conforms to the `rpc_serializer` concept

**Constructor Parameters:**
- `bind_address`: Address to bind to (e.g., "0.0.0.0")
- `bind_port`: Port to bind to (e.g., 8080)
- `config`: Server configuration (max connections, request size limits, etc.)
- `metrics`: Metrics recorder instance for monitoring

**Public Methods:**
```cpp
// Implements network_server concept
auto register_request_vote_handler(
    std::function<request_vote_response<>(const request_vote_request<>&)> handler
) -> void;

auto register_append_entries_handler(
    std::function<append_entries_response<>(const append_entries_request<>&)> handler
) -> void;

auto register_install_snapshot_handler(
    std::function<install_snapshot_response<>(const install_snapshot_request<>&)> handler
) -> void;

auto start() -> void;
auto stop() -> void;
auto is_running() const -> bool;
```

**Private Members:**
- `_serializer`: Instance of RPC_Serializer
- `_http_server`: cpp-httplib server instance
- `_request_vote_handler`: Registered handler for RequestVote RPCs
- `_append_entries_handler`: Registered handler for AppendEntries RPCs
- `_install_snapshot_handler`: Registered handler for InstallSnapshot RPCs
- `_bind_address`: Server bind address
- `_bind_port`: Server bind port
- `_config`: Server configuration
- `_metrics`: Metrics recorder instance
- `_running`: Atomic flag for server state
- `_mutex`: Mutex for thread-safe access

**Private Methods:**
```cpp
auto setup_endpoints() -> void;

template<typename Request, typename Response>
auto handle_rpc_endpoint(
    const httplib::Request& http_req,
    httplib::Response& http_resp,
    std::function<Response(const Request&)> handler
) -> void;

auto set_response_headers(httplib::Response& response) const -> void;
auto handle_error(httplib::Response& response, int status_code, const std::string& message) const -> void;
```

## Data Models

### Client Configuration

```cpp
struct cpp_httplib_client_config {
    std::size_t connection_pool_size{10};
    std::chrono::milliseconds connection_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};
    std::chrono::milliseconds keep_alive_timeout{60000};
    bool enable_ssl_verification{true};
    std::string ca_cert_path{};
    std::string user_agent{"raft-cpp-httplib/1.0"};
};
```

### Server Configuration

```cpp
struct cpp_httplib_server_config {
    std::size_t max_concurrent_connections{100};
    std::size_t max_request_body_size{10 * 1024 * 1024};  // 10 MB
    std::chrono::seconds request_timeout{30};
    bool enable_ssl{false};
    std::string ssl_cert_path{};
    std::string ssl_key_path{};
};
```

### URL Mapping

The client maintains a mapping from Raft node IDs to HTTP base URLs:
```cpp
std::unordered_map<std::uint64_t, std::string> node_id_to_url;
// Example: {1: "http://192.168.1.10:8080", 2: "http://192.168.1.11:8080"}
```

### HTTP Endpoints

- **RequestVote**: `POST /v1/raft/request_vote`
- **AppendEntries**: `POST /v1/raft/append_entries`
- **InstallSnapshot**: `POST /v1/raft/install_snapshot`

### HTTP Headers

**Request Headers:**
- `Content-Type`: Serialization format (e.g., "application/json")
- `Content-Length`: Size of request body
- `User-Agent`: Raft implementation identifier

**Response Headers:**
- `Content-Type`: Serialization format (e.g., "application/json")
- `Content-Length`: Size of response body

## Error Handling

### Client-Side Error Handling

**Network Errors:**
- Connection refused → Future set to error with `std::runtime_error`
- Connection timeout → Future set to error with `std::runtime_error`
- DNS resolution failure → Future set to error with `std::runtime_error`

**HTTP Errors:**
- 4xx status codes → Future set to error with `http_client_error` exception
- 5xx status codes → Future set to error with `http_server_error` exception
- Timeout → Future set to error with `http_timeout_error` exception

**Serialization Errors:**
- Deserialization failure → Future set to error with `serialization_error` exception

### Server-Side Error Handling

**Request Errors:**
- Malformed HTTP request → Return 400 Bad Request
- Deserialization failure → Return 400 Bad Request with error details
- Missing required headers → Return 400 Bad Request

**Handler Errors:**
- Handler throws exception → Return 500 Internal Server Error
- Handler returns invalid response → Return 500 Internal Server Error

**Resource Errors:**
- Request body too large → Return 413 Payload Too Large
- Too many concurrent connections → Return 503 Service Unavailable

### Exception Types

```cpp
class http_transport_error : public std::runtime_error {
public:
    explicit http_transport_error(const std::string& message);
};

class http_client_error : public http_transport_error {
public:
    http_client_error(int status_code, const std::string& message);
    auto status_code() const -> int;
private:
    int _status_code;
};

class http_server_error : public http_transport_error {
public:
    http_server_error(int status_code, const std::string& message);
    auto status_code() const -> int;
private:
    int _status_code;
};

class http_timeout_error : public http_transport_error {
public:
    explicit http_timeout_error(const std::string& message);
};

class serialization_error : public http_transport_error {
public:
    explicit serialization_error(const std::string& message);
};
```

## Metrics Collection

The HTTP transport emits metrics for monitoring and observability. Both the client and server accept a metrics recorder instance that conforms to the `metrics` concept.

### Client Metrics

**Request Metrics:**
- `http.client.request.sent` - Counter for total requests sent
  - Dimensions: `rpc_type` (request_vote, append_entries, install_snapshot), `target_node_id`
- `http.client.request.latency` - Timing for request round-trip time
  - Dimensions: `rpc_type`, `target_node_id`, `status` (success, error, timeout)
- `http.client.request.size` - Gauge for request body size in bytes
  - Dimensions: `rpc_type`, `target_node_id`
- `http.client.response.size` - Gauge for response body size in bytes
  - Dimensions: `rpc_type`, `target_node_id`

**Error Metrics:**
- `http.client.error` - Counter for client errors
  - Dimensions: `error_type` (connection_failed, timeout, 4xx, 5xx, deserialization_failed), `target_node_id`

**Connection Pool Metrics:**
- `http.client.connection.created` - Counter for new connections created
  - Dimensions: `target_node_id`
- `http.client.connection.reused` - Counter for connections reused from pool
  - Dimensions: `target_node_id`
- `http.client.connection.closed` - Counter for connections closed
  - Dimensions: `target_node_id`, `reason` (idle_timeout, error, pool_full)
- `http.client.connection.pool_size` - Gauge for current connection pool size
  - Dimensions: `target_node_id`

### Server Metrics

**Request Metrics:**
- `http.server.request.received` - Counter for total requests received
  - Dimensions: `rpc_type` (request_vote, append_entries, install_snapshot), `endpoint`
- `http.server.request.latency` - Timing for request processing time
  - Dimensions: `rpc_type`, `endpoint`, `status_code`
- `http.server.request.size` - Gauge for request body size in bytes
  - Dimensions: `rpc_type`, `endpoint`
- `http.server.response.size` - Gauge for response body size in bytes
  - Dimensions: `rpc_type`, `endpoint`, `status_code`

**Error Metrics:**
- `http.server.error` - Counter for server errors
  - Dimensions: `error_type` (malformed_request, deserialization_failed, handler_exception), `endpoint`

**Server Lifecycle Metrics:**
- `http.server.started` - Counter for server start events
- `http.server.stopped` - Counter for server stop events
- `http.server.active_connections` - Gauge for current number of active connections

### Metrics Emission Points

**Client:**
- Before sending request: Record request size, increment request counter
- After receiving response: Record latency, response size, status
- On error: Increment error counter with appropriate error type
- On connection pool operations: Record connection lifecycle events

**Server:**
- On request received: Increment request counter, record request size
- After handler execution: Record latency, response size, status code
- On error: Increment error counter with appropriate error type
- On lifecycle events: Record start/stop events

## Testing Strategy

### Unit Tests

1. **Client Construction Tests**
   - Test client creation with valid configuration
   - Test client creation with invalid configuration
   - Test URL mapping initialization

2. **Server Construction Tests**
   - Test server creation with valid configuration
   - Test server creation with invalid configuration
   - Test handler registration

3. **Serialization Integration Tests**
   - Test request serialization with JSON serializer
   - Test response deserialization with JSON serializer
   - Test serialization error handling

4. **HTTP Header Tests**
   - Test correct Content-Type header setting
   - Test correct Content-Length header setting
   - Test User-Agent header setting

5. **Error Handling Tests**
   - Test 4xx status code handling
   - Test 5xx status code handling
   - Test timeout handling
   - Test connection failure handling

### Property-Based Tests

The property-based tests will use a C++ property-based testing library (e.g., RapidCheck) to verify correctness properties across randomly generated inputs.


## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: POST method for all RPCs

*For any* Raft RPC request (RequestVote, AppendEntries, or InstallSnapshot), the HTTP client should use the POST method.
**Validates: Requirements 1.6**

### Property 2: Serialization round-trip preserves content

*For any* valid Raft RPC message (request or response), serializing then deserializing should produce an equivalent message.
**Validates: Requirements 16.2, 2.5, 2.6, 2.7, 2.8**

### Property 3: Content-Type header matches serializer format

*For any* HTTP request or response, the Content-Type header should match the serialization format of the configured RPC_Serializer.
**Validates: Requirements 2.9, 15.1, 15.4**

### Property 4: Content-Length header for requests

*For any* HTTP request sent by the client, the Content-Length header should equal the size of the serialized request body.
**Validates: Requirements 15.2**

### Property 5: User-Agent header for requests

*For any* HTTP request sent by the client, the User-Agent header should identify the Raft implementation.
**Validates: Requirements 15.3**

### Property 6: Content-Length header for responses

*For any* HTTP response sent by the server, the Content-Length header should equal the size of the serialized response body.
**Validates: Requirements 15.5**

### Property 7: Handler invocation for all RPCs

*For any* valid RPC request received by the server, the corresponding registered handler should be invoked with the deserialized request.
**Validates: Requirements 6.3, 7.3, 8.3**

### Property 8: Connection reuse for same target

*For any* sequence of requests to the same target node, the HTTP client should reuse existing connections from the connection pool when available.
**Validates: Requirements 11.2**

### Property 9: 4xx status codes produce client errors

*For any* HTTP response with a 4xx status code, the client should set the future to error state with an http_client_error exception.
**Validates: Requirements 13.4**

### Property 10: 5xx status codes produce server errors

*For any* HTTP response with a 5xx status code, the client should set the future to error state with an http_server_error exception.
**Validates: Requirements 13.5**


### Property-Based Testing Configuration

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs
- Be tagged with a comment referencing the correctness property: `**Feature: http-transport, Property {number}: {property_text}**`
- Use RapidCheck or a similar C++ property-based testing library

### Unit Test Coverage

Unit tests should cover:
- Concept conformance (cpp_httplib_client satisfies network_client, cpp_httplib_server satisfies network_server)
- Specific endpoint paths (/v1/raft/request_vote, /v1/raft/append_entries, /v1/raft/install_snapshot)
- HTTP/1.1 protocol version
- Successful request/response handling (status 200)
- Error handling (timeouts, connection failures, malformed requests)
- Handler registration
- Server lifecycle (start, stop, is_running)
- TLS/HTTPS support (certificate validation, TLS version)
- Connection pool initialization
- Keep-alive headers
- Timeout enforcement
- Error status codes (400, 500, 413, 503)
- Configuration acceptance

## Implementation Notes

### Thread Safety

Both cpp_httplib_client and cpp_httplib_server must be thread-safe:
- Use mutexes to protect shared state (connection pool, handlers, configuration)
- cpp-httplib provides thread-safe client and server implementations
- folly::Future provides thread-safe asynchronous operations

### Connection Pooling

The client should maintain a connection pool per target node:
- Pool size configurable via `cpp_httplib_client_config::connection_pool_size`
- Connections reused for multiple requests to the same target
- Idle connections closed after `keep_alive_timeout`
- Pool cleanup on client destruction

### Timeout Handling

Timeouts are enforced at multiple levels:
- Connection timeout: Time to establish TCP connection
- Request timeout: Total time for request/response cycle
- cpp-httplib provides timeout support via `set_connection_timeout()` and `set_read_timeout()`

### TLS/HTTPS Support

TLS support is optional and configured via:
- Client: HTTPS URLs in node_id_to_url_map, `enable_ssl_verification`, `ca_cert_path`
- Server: `enable_ssl`, `ssl_cert_path`, `ssl_key_path`
- cpp-httplib provides TLS support via OpenSSL

### Serializer Integration

The RPC_Serializer is used for all serialization/deserialization:
- Client: `serialize()` for requests, `deserialize_*_response()` for responses
- Server: `deserialize_*_request()` for requests, `serialize()` for responses
- Content-Type header set based on serializer format (e.g., "application/json" for JSON serializer)

### Error Recovery

The HTTP transport does not implement automatic retry logic:
- Raft algorithm handles RPC failures and retries at a higher level
- Transport layer reports failures via folly::Future error states
- Raft node decides when and how to retry based on algorithm requirements

## Dependencies

### External Libraries

- **cpp-httplib**: Header-only HTTP/HTTPS library
  - Version: 0.14.0 or higher
  - License: MIT
  - Repository: https://github.com/yhirose/cpp-httplib

- **folly**: Facebook's C++ library for futures and async operations
  - Version: 2023.01.01 or higher
  - License: Apache 2.0
  - Repository: https://github.com/facebook/folly

- **OpenSSL**: For TLS/HTTPS support (optional)
  - Version: 1.1.1 or higher
  - License: Apache 2.0

### Internal Dependencies

- **raft/types.hpp**: Raft RPC message types and concepts
- **raft/network.hpp**: network_client and network_server concepts
- **raft/json_serializer.hpp**: JSON serializer implementation (for testing)

## Build Integration

### CMakeLists.txt

```cmake
# Add cpp-httplib dependency
find_package(httplib REQUIRED)

# Create HTTP transport library
add_library(raft_http_transport INTERFACE)
target_include_directories(raft_http_transport INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(raft_http_transport INTERFACE
    httplib::httplib
    Folly::folly
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_features(raft_http_transport INTERFACE cxx_std_23)

# Install headers
install(FILES
    include/raft/cpp_httplib_transport.hpp
    DESTINATION include/raft
)
```

### Header File Structure

```
include/raft/
├── cpp_httplib_transport.hpp  # Main header with client and server classes
└── cpp_httplib_exceptions.hpp # Exception types for HTTP transport
```

## Performance Considerations

### Connection Pooling Benefits

- Reduces TCP connection establishment overhead
- Reuses TLS sessions for HTTPS
- Typical connection establishment: 50-100ms
- Typical request with pooled connection: 1-10ms

### Serialization Overhead

- JSON serialization: ~1-5ms per message
- Protocol Buffers: ~0.1-1ms per message (if implemented)
- Serialization is CPU-bound, not I/O-bound

### Network Latency

- Local network (same datacenter): 1-5ms
- Cross-datacenter: 50-200ms
- Internet: 100-500ms

### Throughput

- HTTP/1.1 with keep-alive: ~1000-5000 requests/second per connection
- Limited by serialization and network latency
- Multiple connections can be used for higher throughput

## Security Considerations

### TLS/HTTPS

- Always use HTTPS in production environments
- Validate server certificates against trusted CAs
- Use TLS 1.2 or higher (TLS 1.3 preferred)
- Disable weak cipher suites

### Authentication

- HTTP transport does not implement authentication
- Authentication should be handled at a higher layer (e.g., mutual TLS, API keys)
- Consider adding authentication headers in future versions

### Request Size Limits

- Enforce maximum request body size to prevent DoS attacks
- Default: 10 MB (configurable via `max_request_body_size`)
- Return 413 Payload Too Large for oversized requests

### Rate Limiting

- HTTP transport does not implement rate limiting
- Rate limiting should be handled by infrastructure (e.g., load balancer, API gateway)
- Consider adding rate limiting in future versions

## Future Enhancements

### HTTP/2 Support

- Upgrade to a library that supports HTTP/2 (e.g., nghttp2)
- Benefits: multiplexing, header compression, server push
- Requires significant refactoring of cpp-httplib dependency

### Compression

- Add support for gzip/deflate compression
- Reduce network bandwidth for large messages
- Trade-off: CPU overhead vs. network savings

### Circuit Breaker

- Implement circuit breaker pattern for failing nodes
- Prevent cascading failures
- Automatically recover when nodes become healthy

### Retry Logic

- Add configurable retry logic with exponential backoff
- Coordinate with Raft algorithm retry logic
- Avoid duplicate retries at multiple layers
