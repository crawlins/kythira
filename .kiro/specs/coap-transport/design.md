# CoAP Transport Design Document

## Overview

The CoAP transport implementation provides concrete implementations of the `network_client` and `network_server` concepts for the Raft consensus algorithm using the CoAP (Constrained Application Protocol) protocol. This design enables Raft clusters to communicate over CoAP/UDP with optional DTLS encryption, making it suitable for deployment in IoT environments, constrained networks, and scenarios requiring lightweight, efficient communication.

The implementation consists of two main components:
- **coap_client**: Implements the `network_client` concept to send Raft RPCs over CoAP
- **coap_server**: Implements the `network_server` concept to receive and handle Raft RPCs over CoAP

Both components are templated on an RPC serializer that conforms to the `rpc_serializer` concept, allowing flexible serialization formats (CBOR, JSON, etc.) to be used independently of the transport layer.

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
│     coap_client        │      │      coap_server           │
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
│   (e.g., CBOR)         │      │   (e.g., CBOR)             │
│                        │      │                            │
│   Implements:          │      │   Implements:              │
│   - rpc_serializer     │      │   - rpc_serializer         │
│     concept            │      │     concept                │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         │ uses                          │ uses
         ▼                               ▼
┌────────────────────────────────────────────────────────────┐
│                    libcoap / CoAP Library                   │
│  (CoAP protocol implementation with DTLS support)          │
└────────────────────────────────────────────────────────────┘
```

### Request Flow (Client Side)

1. Raft node calls `send_request_vote()`, `send_append_entries()`, or `send_install_snapshot()` on coap_client
2. coap_client serializes the request using the RPC_Serializer
3. coap_client constructs CoAP POST request with appropriate options and token
4. libcoap sends the CoAP request to the target server (confirmable or non-confirmable)
5. libcoap receives CoAP response or handles retransmission/timeout
6. coap_client deserializes the response using the RPC_Serializer
7. coap_client satisfies the folly::Future with the response

### Request Flow (Server Side)

1. libcoap receives CoAP POST request at a resource endpoint
2. coap_server extracts the request payload
3. coap_server deserializes the request using the RPC_Serializer
4. coap_server invokes the registered handler function
5. Handler function returns a response
6. coap_server serializes the response using the RPC_Serializer
7. coap_server sends CoAP 2.05 Content response with serialized payload

## Components and Interfaces

### coap_client

**Template Parameters:**
- `RPC_Serializer`: Type that conforms to the `rpc_serializer` concept

**Constructor Parameters:**
- `node_id_to_endpoint_map`: Mapping from node IDs to CoAP endpoints (e.g., `{1: "coap://node1:5683", 2: "coaps://node2:5684"}`)
- `config`: Client configuration (timeouts, retransmission parameters, DTLS settings, etc.)
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
- `_node_id_to_endpoint`: Map from node IDs to CoAP endpoints
- `_coap_context`: libcoap context for client operations
- `_config`: Client configuration
- `_metrics`: Metrics recorder instance
- `_pending_requests`: Map from message token to pending future
- `_mutex`: Mutex for thread-safe access

**Private Methods:**
```cpp
template<typename Request, typename Response>
auto send_rpc(
    std::uint64_t target,
    const std::string& resource_path,
    const Request& request,
    std::chrono::milliseconds timeout
) -> folly::Future<Response>;

auto get_endpoint_uri(std::uint64_t node_id) const -> std::string;
auto generate_message_token() -> std::vector<std::byte>;
auto setup_dtls_context() -> void;
auto handle_response(coap_pdu_t* response, const std::vector<std::byte>& token) -> void;
```

### coap_server

**Template Parameters:**
- `RPC_Serializer`: Type that conforms to the `rpc_serializer` concept

**Constructor Parameters:**
- `bind_address`: Address to bind to (e.g., "0.0.0.0")
- `bind_port`: Port to bind to (e.g., 5683 for CoAP, 5684 for CoAPS)
- `config`: Server configuration (max connections, DTLS settings, etc.)
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
- `_coap_context`: libcoap context for server operations
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
auto setup_resources() -> void;
auto setup_dtls_context() -> void;

template<typename Request, typename Response>
auto handle_rpc_resource(
    coap_resource_t* resource,
    coap_session_t* session,
    const coap_pdu_t* request,
    const coap_string_t* query,
    coap_pdu_t* response,
    std::function<Response(const Request&)> handler
) -> void;

auto send_error_response(coap_pdu_t* response, coap_pdu_code_t code, const std::string& message) -> void;
```

## Data Models

### Client Configuration

```cpp
struct coap_client_config {
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

### Server Configuration

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

### Endpoint Mapping

The client maintains a mapping from Raft node IDs to CoAP endpoints:
```cpp
std::unordered_map<std::uint64_t, std::string> node_id_to_endpoint;
// Example: {1: "coap://192.168.1.10:5683", 2: "coaps://192.168.1.11:5684"}
```

### CoAP Resources

- **RequestVote**: `POST /raft/request_vote`
- **AppendEntries**: `POST /raft/append_entries`
- **InstallSnapshot**: `POST /raft/install_snapshot`

### CoAP Message Options

**Request Options:**
- `Content-Format`: Serialization format (e.g., 60 for CBOR, 50 for JSON)
- `Accept`: Expected response format
- `Block1`: For large request payloads (block-wise transfer)
- `Block2`: For large response payloads

**Response Options:**
- `Content-Format`: Serialization format of response
- `Block1`: Acknowledgment of received blocks
- `Block2`: Response payload blocks

## Error Handling

### Client-Side Error Handling

**Network Errors:**
- Connection timeout → Future set to error with `coap_timeout_error`
- DTLS handshake failure → Future set to error with `coap_security_error`
- DNS resolution failure → Future set to error with `coap_network_error`

**CoAP Protocol Errors:**
- 4.xx response codes → Future set to error with `coap_client_error` exception
- 5.xx response codes → Future set to error with `coap_server_error` exception
- Message format error → Future set to error with `coap_protocol_error` exception

**Serialization Errors:**
- Deserialization failure → Future set to error with `serialization_error` exception

### Server-Side Error Handling

**Request Errors:**
- Malformed CoAP request → Return 4.00 Bad Request
- Deserialization failure → Return 4.00 Bad Request with diagnostic payload
- Missing required options → Return 4.00 Bad Request

**Handler Errors:**
- Handler throws exception → Return 5.00 Internal Server Error
- Handler returns invalid response → Return 5.00 Internal Server Error

**Resource Errors:**
- Request payload too large → Return 4.13 Request Entity Too Large
- Too many concurrent sessions → Return 5.03 Service Unavailable

### Exception Types

```cpp
class coap_transport_error : public std::runtime_error {
public:
    explicit coap_transport_error(const std::string& message);
};

class coap_client_error : public coap_transport_error {
public:
    coap_client_error(coap_pdu_code_t code, const std::string& message);
    auto response_code() const -> coap_pdu_code_t;
private:
    coap_pdu_code_t _response_code;
};

class coap_server_error : public coap_transport_error {
public:
    coap_server_error(coap_pdu_code_t code, const std::string& message);
    auto response_code() const -> coap_pdu_code_t;
private:
    coap_pdu_code_t _response_code;
};

class coap_timeout_error : public coap_transport_error {
public:
    explicit coap_timeout_error(const std::string& message);
};

class coap_security_error : public coap_transport_error {
public:
    explicit coap_security_error(const std::string& message);
};

class coap_protocol_error : public coap_transport_error {
public:
    explicit coap_protocol_error(const std::string& message);
};

class coap_network_error : public coap_transport_error {
public:
    explicit coap_network_error(const std::string& message);
};
```

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system-essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Message serialization round-trip consistency

*For any* valid Raft RPC message (request or response), serializing then deserializing should produce an equivalent message.
**Validates: Requirements 1.2, 1.3, 7.2**

### Property 2: CoAP POST method for all RPCs

*For any* Raft RPC request (RequestVote, AppendEntries, or InstallSnapshot), the CoAP client should use the POST method.
**Validates: Requirements 1.2**

### Property 3: Content-Format option matches serializer

*For any* CoAP request or response, the Content-Format option should match the serialization format of the configured RPC_Serializer.
**Validates: Requirements 1.2, 1.3**

### Property 4: Confirmable message acknowledgment handling

*For any* confirmable CoAP message sent by the client, the transport should wait for acknowledgment and handle retransmission according to RFC 7252.
**Validates: Requirements 3.1, 3.3**

### Property 5: Duplicate message detection

*For any* CoAP message with the same Message ID received multiple times, only the first occurrence should be processed.
**Validates: Requirements 3.2**

### Property 6: Non-confirmable message delivery

*For any* non-confirmable CoAP message sent by the client, the transport should not wait for acknowledgment.
**Validates: Requirements 3.5**

### Property 7: Exponential backoff retransmission

*For any* failed message transmission, retransmission intervals should follow exponential backoff as specified in RFC 7252.
**Validates: Requirements 2.4, 3.3, 8.4**

### Property 8: Block transfer for large messages

*For any* message larger than the configured block size, the transport should use CoAP block-wise transfer.
**Validates: Requirements 2.3, 7.5**

### Property 9: DTLS connection establishment

*For any* CoAPS endpoint, the transport should establish DTLS connections with proper certificate or PSK validation.
**Validates: Requirements 1.4, 6.1, 6.3**

### Property 10: Certificate validation failure handling

*For any* invalid certificate presented during DTLS handshake, the transport should reject the connection.
**Validates: Requirements 6.2**

### Property 11: Multicast message delivery

*For any* multicast-enabled configuration, messages sent to multicast addresses should be delivered to all listening nodes.
**Validates: Requirements 2.5**

### Property 12: Concurrent request processing

*For any* set of concurrent requests, the server should process them in parallel without blocking.
**Validates: Requirements 7.3**

### Property 13: Connection reuse optimization

*For any* sequence of requests to the same target node, the client should reuse existing sessions when available.
**Validates: Requirements 7.4**

### Property 14: Malformed message rejection

*For any* malformed CoAP message received by the server, it should be rejected without affecting other message processing.
**Validates: Requirements 8.2**

### Property 15: Resource exhaustion handling

*For any* resource exhaustion condition (memory, connections), the transport should handle it gracefully without crashing.
**Validates: Requirements 8.3**

### Property 16: Network partition recovery

*For any* network partition scenario, the transport should detect the condition and attempt reconnection.
**Validates: Requirements 8.1**

### Property 17: Connection limit enforcement

*For any* configuration with connection limits, the transport should enforce limits and handle excess connections appropriately.
**Validates: Requirements 8.5**

### Property 18: Future resolution on completion

*For any* RPC request sent via the client, the returned future should resolve when the operation completes (success or failure).
**Validates: Requirements 4.2**

### Property 19: Exception throwing on errors

*For any* error condition encountered during transport operations, appropriate exceptions should be thrown with descriptive messages.
**Validates: Requirements 4.3**

### Property 20: Logging of significant events

*For any* significant transport operation (message send/receive, connection events, errors), appropriate log entries should be generated.
**Validates: Requirements 5.1, 5.2, 5.3**

## Testing Strategy

### Unit Tests

1. **Client Construction Tests**
   - Test client creation with valid configuration
   - Test client creation with invalid configuration
   - Test endpoint mapping initialization

2. **Server Construction Tests**
   - Test server creation with valid configuration
   - Test server creation with invalid configuration
   - Test handler registration

3. **Serialization Integration Tests**
   - Test request serialization with CBOR serializer
   - Test response deserialization with CBOR serializer
   - Test serialization error handling

4. **CoAP Option Tests**
   - Test correct Content-Format option setting
   - Test Accept option setting
   - Test Block1/Block2 option handling

5. **Error Handling Tests**
   - Test 4.xx response code handling
   - Test 5.xx response code handling
   - Test timeout handling
   - Test DTLS handshake failure handling

### Property-Based Tests

The property-based tests will use a C++ property-based testing library (e.g., RapidCheck) to verify correctness properties across randomly generated inputs.

Each property-based test should:
- Run a minimum of 100 iterations with randomly generated inputs
- Be tagged with a comment referencing the correctness property: `**Feature: coap-transport, Property {number}: {property_text}**`
- Use RapidCheck or a similar C++ property-based testing library

### Unit Test Coverage

Unit tests should cover:
- Concept conformance (coap_client satisfies network_client, coap_server satisfies network_server)
- Specific resource paths (/raft/request_vote, /raft/append_entries, /raft/install_snapshot)
- CoAP protocol compliance (RFC 7252)
- Successful request/response handling (2.05 Content)
- Error handling (timeouts, connection failures, malformed requests)
- Handler registration
- Server lifecycle (start, stop, is_running)
- DTLS/CoAPS support (certificate validation, PSK authentication)
- Session management
- Block-wise transfer
- Multicast support
- Retransmission and acknowledgment handling
- Error response codes (4.00, 4.13, 5.00, 5.03)
- Configuration acceptance

## Implementation Notes

### Thread Safety

Both coap_client and coap_server must be thread-safe:
- Use mutexes to protect shared state (session management, handlers, configuration)
- libcoap provides thread-safe context operations when properly configured
- folly::Future provides thread-safe asynchronous operations

### Session Management

The client should maintain CoAP sessions per target node:
- Session reuse for multiple requests to the same target
- Session timeout and cleanup
- DTLS session resumption for encrypted connections

### Message Token Management

CoAP uses tokens to correlate requests and responses:
- Generate unique tokens for each request
- Maintain mapping from token to pending future
- Handle token collision and cleanup

### Block-wise Transfer

For large messages exceeding the configured block size:
- Implement RFC 7959 block-wise transfer
- Handle Block1 (request payload) and Block2 (response payload)
- Support early negotiation of block size

### DTLS Integration

DTLS support for secure communication:
- Certificate-based authentication
- Pre-shared key (PSK) authentication
- Cipher suite configuration
- Certificate validation and revocation checking

### Multicast Support

CoAP multicast for efficient group communication:
- IPv4 and IPv6 multicast addresses
- Multicast-specific message handling
- Response suppression and aggregation

### Error Recovery

The CoAP transport implements automatic retry logic:
- Exponential backoff for retransmissions
- Maximum retry limits
- Connection recovery after network partitions
- DNS resolution retry

## Dependencies

### External Libraries

- **libcoap**: C library for CoAP protocol implementation
  - Version: 4.3.0 or higher
  - License: BSD 2-Clause
  - Repository: https://github.com/obgm/libcoap

- **folly**: Facebook's C++ library for futures and async operations
  - Version: 2023.01.01 or higher
  - License: Apache 2.0
  - Repository: https://github.com/facebook/folly

- **OpenSSL**: For DTLS/TLS support
  - Version: 1.1.1 or higher
  - License: Apache 2.0

### Internal Dependencies

- **raft/types.hpp**: Raft RPC message types and concepts
- **raft/network.hpp**: network_client and network_server concepts
- **raft/json_serializer.hpp**: JSON serializer implementation (for testing)

## Build Integration

### CMakeLists.txt

```cmake
# Find libcoap dependency
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCOAP REQUIRED libcoap-3)

# Create CoAP transport library
add_library(raft_coap_transport INTERFACE)
target_include_directories(raft_coap_transport INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBCOAP_INCLUDE_DIRS}
)
target_link_libraries(raft_coap_transport INTERFACE
    ${LIBCOAP_LIBRARIES}
    Folly::folly
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_compile_features(raft_coap_transport INTERFACE cxx_std_23)
target_compile_definitions(raft_coap_transport INTERFACE
    ${LIBCOAP_CFLAGS_OTHER}
)

# Install headers
install(FILES
    include/raft/coap_transport.hpp
    DESTINATION include/raft
)
```

### Header File Structure

```
include/raft/
├── coap_transport.hpp      # Main header with client and server classes
└── coap_exceptions.hpp     # Exception types for CoAP transport
```

## Performance Considerations

### UDP vs TCP

- CoAP uses UDP for reduced overhead and faster connection establishment
- No connection state maintenance reduces server memory usage
- Suitable for high-frequency, low-latency communication patterns

### Message Size Optimization

- CBOR serialization: ~20-50% smaller than JSON
- Block-wise transfer for large messages reduces memory usage
- Efficient binary encoding minimizes network bandwidth

### Session Reuse

- DTLS session resumption reduces handshake overhead
- Session caching improves performance for repeated communications
- Typical session establishment: 100-200ms, reuse: 1-10ms

### Multicast Efficiency

- Single message to multiple recipients
- Reduces network traffic for broadcast scenarios
- Suitable for cluster-wide announcements

## Security Considerations

### DTLS/CoAPS

- Always use CoAPS (DTLS) in production environments
- Support for TLS 1.2 and 1.3 (via DTLS 1.2 and 1.3)
- Certificate-based and PSK-based authentication
- Configurable cipher suites

### Message Authentication

- DTLS provides message authentication and integrity
- PSK mode suitable for closed networks
- Certificate mode for PKI-based deployments

### Denial of Service Protection

- Connection limits prevent resource exhaustion
- Message size limits prevent memory exhaustion
- Rate limiting can be implemented at application layer

### Replay Attack Prevention

- CoAP message IDs provide replay protection within the duplicate detection window
- DTLS provides additional replay protection

## Future Enhancements

### CoAP Observe Pattern

- Implement RFC 7641 for resource observation
- Suitable for leader election notifications
- Reduces polling overhead for status updates

### Group Communication

- CoAP group communication (RFC 7390)
- Efficient multicast with response aggregation
- Suitable for cluster-wide operations

### Caching Support

- CoAP caching mechanisms (RFC 7252)
- Reduce redundant message transmission
- Improve performance for read-heavy workloads

### HTTP/CoAP Proxy

- CoAP-HTTP proxy support (RFC 8075)
- Enable integration with HTTP-based systems
- Protocol translation capabilities