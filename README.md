# Network Simulator

A C++23 network simulator library that models network communication between nodes using concepts and template metaprogramming.

## Features

- **Type-safe design** using C++23 concepts
- **Enhanced C++20 concepts** for Folly-compatible types (futures, promises, executors)
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
│   ├── concepts/
│   │   └── future.hpp             # Enhanced C++20 concepts for Folly types
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

### Enhanced C++20 Concepts

The library provides enhanced C++20 concepts that work seamlessly with Folly types while enabling generic programming:

```cpp
#include <concepts/future.hpp>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

// Generic function that works with any Future-like type
template<kythira::future<int> FutureType>
auto process_async_result(FutureType future) -> int {
    if (future.isReady()) {
        return std::move(future).get();
    }
    
    return std::move(future)
        .thenValue([](int value) { return value * 2; })
        .get();
}

// Works with folly::Future
folly::Future<int> folly_future = folly::makeFuture(21);
auto result = process_async_result(std::move(folly_future)); // Returns 42

// Generic promise handling
template<kythira::promise<std::string> PromiseType>
auto create_greeting(PromiseType promise, const std::string& name) -> auto {
    auto future = promise.getFuture();
    promise.setValue("Hello, " + name + "!");
    return future;
}

// Static assertions verify Folly compatibility
static_assert(kythira::future<folly::Future<int>, int>);
static_assert(kythira::promise<folly::Promise<int>, int>);
static_assert(kythira::try_type<folly::Try<int>, int>);
```

See [Concepts Documentation](doc/concepts_documentation.md) for comprehensive usage examples and [Concepts Migration Guide](doc/concepts_migration_guide.md) for migration from older concept versions.

### Network Simulator

```cpp
#include <network_simulator/network_simulator.hpp>

// Example usage will be added as implementation progresses
```

### HTTP Transport for Raft

The HTTP transport provides a production-ready implementation of the Raft network layer using standard HTTP/1.1 protocol with the transport_types concept for clean type parameterization.

#### Basic Usage with transport_types

```cpp
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>

// Define transport types bundle
struct http_transport_types {
    using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
    template<typename T> using future_template = folly::Future<T>;
    using executor_type = folly::CPUThreadPoolExecutor;
    using metrics_type = raft::noop_metrics;
};

// Server setup
raft::cpp_httplib_server_config server_config;
server_config.max_concurrent_connections = 100;
server_config.request_timeout = std::chrono::seconds{30};

raft::noop_metrics metrics;

raft::cpp_httplib_server<http_transport_types> server(
    "0.0.0.0",  // bind address
    8080,       // bind port
    server_config,
    metrics
);

// Register handlers
server.register_request_vote_handler([](const auto& request) {
    raft::request_vote_response<> response;
    response.term = request.term;
    response.vote_granted = true;
    return response;
});

server.start();

// Client setup
raft::cpp_httplib_client_config client_config;
client_config.connection_pool_size = 10;
client_config.connection_timeout = std::chrono::milliseconds{3000};
client_config.request_timeout = std::chrono::milliseconds{5000};

std::unordered_map<std::uint64_t, std::string> node_urls;
node_urls[1] = "http://node1:8080";
node_urls[2] = "http://node2:8080";

raft::cpp_httplib_client<http_transport_types> client(
    std::move(node_urls), 
    client_config, 
    metrics
);

// Send RPC - returns correctly typed future
raft::request_vote_request<> request;
request.term = 5;
request.candidate_id = 1;

auto future = client.send_request_vote(2, request, std::chrono::milliseconds{5000});
// future is folly::Future<request_vote_response<>>
auto response = std::move(future).get();
```

#### Alternative Future Types

The transport_types concept supports different future implementations:

```cpp
// Using std::future
struct std_transport_types {
    using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
    template<typename T> using future_template = std::future<T>;
    using executor_type = std::thread;
    using metrics_type = raft::noop_metrics;
};

// Using custom future
template<typename T>
class custom_future { /* ... */ };

struct custom_transport_types {
    using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
    template<typename T> using future_template = custom_future<T>;
    using executor_type = custom_executor;
    using metrics_type = raft::noop_metrics;
};
```
```

### HTTPS/TLS Configuration

For production deployments, enable TLS encryption:

#### Server HTTPS Setup

```cpp
// Server with HTTPS
raft::cpp_httplib_server_config server_config;
server_config.enable_ssl = true;
server_config.ssl_cert_path = "/path/to/server.crt";
server_config.ssl_key_path = "/path/to/server.key";

raft::cpp_httplib_server<http_transport_types> server(
    "0.0.0.0", 8443, server_config, metrics
);
```

#### Client HTTPS Setup

```cpp
// Client with HTTPS URLs
std::unordered_map<std::uint64_t, std::string> node_urls;
node_urls[1] = "https://node1:8443";
node_urls[2] = "https://node2:8443";

raft::cpp_httplib_client_config client_config;
client_config.enable_ssl_verification = true;
client_config.ca_cert_path = "/path/to/ca.crt";

raft::cpp_httplib_client<http_transport_types> client(
    std::move(node_urls), client_config, metrics
);
```

#### TLS Best Practices

- **Always use HTTPS in production** - HTTP should only be used for development
- **Validate certificates** - Keep `enable_ssl_verification = true` in production
- **Use TLS 1.2 or higher** - The transport automatically enforces modern TLS versions
- **Proper certificate management** - Use certificates from trusted CAs
- **Certificate rotation** - Plan for certificate renewal and rotation

#### Self-Signed Certificates (Development Only)

For development environments, you can generate self-signed certificates:

```bash
# Generate private key
openssl genrsa -out server.key 2048

# Generate certificate signing request
openssl req -new -key server.key -out server.csr

# Generate self-signed certificate
openssl x509 -req -days 365 -in server.csr -signkey server.key -out server.crt
```

**Warning**: Never use self-signed certificates in production. Disable certificate verification only for development:

```cpp
// Development only - disable certificate verification
client_config.enable_ssl_verification = false;
```

### Configuration Options

#### transport_types Concept

The HTTP transport uses a single template parameter that conforms to the `transport_types` concept:

```cpp
template<typename T>
concept transport_types = requires {
    // Required type members
    typename T::serializer_type;
    typename T::executor_type;
    typename T::metrics_type;
    
    // Required template template parameter for futures
    template<typename ResponseType> typename T::template future_template<ResponseType>;
    
    // Concept constraints
    requires rpc_serializer<typename T::serializer_type>;
    requires metrics<typename T::metrics_type>;
    
    // Validate future_template can be instantiated with Raft response types
    requires future<typename T::template future_template<request_vote_response<>>, request_vote_response<>>;
    requires future<typename T::template future_template<append_entries_response<>>, append_entries_response<>>;
    requires future<typename T::template future_template<install_snapshot_response<>>, install_snapshot_response<>>;
};
```

**Benefits of transport_types:**
- **Single template parameter** - Clean interface with all type dependencies
- **Type safety** - Compile-time validation of type compatibility
- **Flexibility** - Support for different future, serializer, and executor implementations
- **Concept-based** - Clear requirements and constraints

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

### HTTP Transport Documentation

- [HTTP Transport Design](`.kiro/specs/http-transport/design.md`) - HTTP transport architecture and implementation
- [HTTP Transport Requirements](`.kiro/specs/http-transport/requirements.md`) - Detailed requirements specification
- [HTTP Transport Troubleshooting](doc/http_transport_troubleshooting.md) - Common issues and solutions

### Core Documentation

- [Concepts Summary](doc/concepts_summary.md) - High-level overview of enhanced concepts
- [Concepts Documentation](doc/concepts_documentation.md) - Comprehensive guide to enhanced C++20 concepts
- [Concepts API Reference](doc/concepts_api_reference.md) - Complete API reference
- [Concepts Migration Guide](doc/concepts_migration_guide.md) - Migration from older concept versions
- [Future Migration Guide](doc/future_migration_guide.md) - Migrating from old future patterns
- [Generic Future Architecture](doc/generic_future_architecture.md) - Overall architecture design

### Design Documents

- [Network Simulator Design](`.kiro/specs/network-simulator/design.md`) - Network simulator architecture
- [Folly Concepts Enhancement](`.kiro/specs/folly-concepts-enhancement/design.md`) - Enhanced concepts design

### Examples

- [Concepts Usage Examples](examples/concepts_usage_examples.cpp) - Practical concept usage patterns
- [Network Examples](examples/) - Network simulator usage examples
- [Raft Examples](examples/raft/) - Raft consensus implementation examples
- [HTTP Transport Example](examples/raft/http_transport_example.cpp) - Complete HTTP transport usage example

## License

TBD
