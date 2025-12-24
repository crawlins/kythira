# Network Simulator

A C++23 network simulator library that models network communication between nodes using concepts and template metaprogramming.

## Features

- **Type-safe design** using C++23 concepts
- **Flexible addressing** supporting strings, integers, IPv4, and IPv6
- **Connectionless communication** (datagram-style like UDP)
- **Connection-oriented communication** (stream-style like TCP)
- **Configurable network characteristics** including latency and reliability
- **Asynchronous operations** using generic future concepts with kythira::Future
- **Thread-safe** implementation
- **HTTP/HTTPS transport** for Raft consensus with pluggable serialization
- **Production-ready networking** with connection pooling and TLS support

## Requirements

- C++23 compatible compiler (GCC 13+, Clang 16+, or MSVC 2022+)
- CMake 3.20 or higher
- folly library
- Boost (system, thread, unit_test_framework)
- cpp-httplib (for HTTP transport)
- OpenSSL (optional, for HTTPS support)

## Building

```bash
# Create build directory
mkdir build
cd build

# Configure
cmake ..

# Build
cmake --build .

# Run tests
ctest
```

## Project Structure

```
.
├── include/
│   └── network_simulator/
│       ├── network_simulator.hpp  # Main header
│       ├── concepts.hpp           # C++23 concepts
│       ├── types.hpp              # Core data types
│       └── exceptions.hpp         # Exception types
├── src/                           # Implementation files (to be added)
├── tests/                         # Unit and property-based tests
├── examples/                      # Example programs
└── CMakeLists.txt                 # Build configuration
```

## Usage

### Network Simulator

```cpp
#include <network_simulator/network_simulator.hpp>

// Example usage will be added as implementation progresses
```

### HTTP Transport for Raft

The HTTP transport provides a production-ready implementation of the Raft network layer using standard HTTP/1.1 protocol with generic future support.

```cpp
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/future.hpp>

// Define the future type to use
using MyFutureType = kythira::Future<raft::request_vote_response<>>;

// Server setup with generic future type
raft::cpp_httplib_server_config server_config;
server_config.max_concurrent_connections = 100;
server_config.request_timeout = std::chrono::seconds{30};

raft::noop_metrics metrics;

// Note: Server implementation is being updated to support generic futures
// This example shows the intended API structure

// Client setup with generic future type
raft::cpp_httplib_client_config client_config;
client_config.connection_pool_size = 10;
client_config.connection_timeout = std::chrono::milliseconds{3000};
client_config.request_timeout = std::chrono::milliseconds{5000};

std::unordered_map<std::uint64_t, std::string> node_urls;
node_urls[1] = "http://node1:8080";
node_urls[2] = "http://node2:8080";

// Create HTTP client with generic future type
kythira::cpp_httplib_client<
    MyFutureType,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics
> client(std::move(node_urls), client_config, metrics);

// Send RPC with generic future
raft::request_vote_request<> request;
request.term = 5;
request.candidate_id = 1;

auto future = client.send_request_vote(2, request, std::chrono::milliseconds{5000});
// Handle future result using kythira::Future interface
auto response = future.get();
```
```

### HTTPS/TLS Configuration

For production deployments, enable TLS encryption:

```cpp
// Server with HTTPS
raft::cpp_httplib_server_config server_config;
server_config.enable_ssl = true;
server_config.ssl_cert_path = "/path/to/server.crt";
server_config.ssl_key_path = "/path/to/server.key";

// Client with HTTPS URLs
std::unordered_map<std::uint64_t, std::string> node_urls;
node_urls[1] = "https://node1:8443";
node_urls[2] = "https://node2:8443";

raft::cpp_httplib_client_config client_config;
client_config.enable_ssl_verification = true;
client_config.ca_cert_path = "/path/to/ca.crt";
```

### Configuration Options

#### Server Configuration

- `max_concurrent_connections`: Maximum number of simultaneous connections (default: 100)
- `max_request_body_size`: Maximum request body size in bytes (default: 10 MB)
- `request_timeout`: Timeout for processing requests (default: 30 seconds)
- `enable_ssl`: Enable HTTPS/TLS (default: false)
- `ssl_cert_path`: Path to SSL certificate file
- `ssl_key_path`: Path to SSL private key file

#### Client Configuration

- `connection_pool_size`: Number of connections to pool per target (default: 10)
- `connection_timeout`: Timeout for establishing connections (default: 5 seconds)
- `request_timeout`: Timeout for complete request/response cycle (default: 10 seconds)
- `keep_alive_timeout`: How long to keep idle connections (default: 60 seconds)
- `enable_ssl_verification`: Verify server certificates (default: true)
- `ca_cert_path`: Path to CA certificate bundle
- `user_agent`: User-Agent header value (default: "raft-cpp-httplib/1.0")

## Documentation

See the design document at `.kiro/specs/network-simulator/design.md` for detailed architecture and design decisions.

## License

TBD
