# Generic Future Architecture

## Overview

The Kythira Raft implementation has been converted to use a generic future architecture that provides flexibility and consistency across all asynchronous operations. This document describes the new architecture and how to use it.

## Architecture Components

### 1. Future Concept (`include/concepts/future.hpp`)

The system defines a generic `future` concept that specifies the interface requirements for future types:

```cpp
namespace kythira {

template<typename F, typename T>
concept future = requires(F f) {
    // Get value (blocking)
    { f.get() } -> std::same_as<T>;
    
    // Check if ready
    { f.isReady() } -> std::convertible_to<bool>;
    
    // Wait with timeout
    { f.wait(std::chrono::milliseconds{}) } -> std::convertible_to<bool>;
    
    // Chain continuation and error handling (with void specialization)
    // ...
};

} // namespace kythira
```

### 2. Kythira Future Implementation (`include/raft/future.hpp`)

The `kythira::Future<T>` class provides the default implementation that satisfies the future concept:

```cpp
namespace kythira {

template<typename T>
class Future {
public:
    // Construct from value
    explicit Future(T value);
    
    // Construct from exception
    explicit Future(folly::exception_wrapper ex);
    
    // Future concept interface
    auto get() -> T;
    auto isReady() const -> bool;
    auto wait(std::chrono::milliseconds timeout) -> bool;
    
    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F, T>>;
    
    template<typename F>
    auto onError(F&& func) -> Future<T>;
};

} // namespace kythira
```

### 3. Generic Core Implementations

All core implementations are now templated on future types and placed in the `kythira` namespace:

#### Network Concepts
```cpp
namespace kythira {

template<typename C, typename FutureType>
concept network_client = requires(
    C client,
    std::uint64_t target,
    const raft::request_vote_request<>& rvr,
    std::chrono::milliseconds timeout
) {
    { client.send_request_vote(target, rvr, timeout) } -> std::same_as<FutureType>;
    // ... other RPC methods
};

} // namespace kythira
```

#### Transport Implementations
```cpp
namespace kythira {

template<typename FutureType, typename RPC_Serializer, typename Metrics>
class cpp_httplib_client {
public:
    auto send_request_vote(...) -> FutureType;
    auto send_append_entries(...) -> FutureType;
    auto send_install_snapshot(...) -> FutureType;
};

template<typename FutureType, typename RPC_Serializer, typename Metrics, typename Logger>
class coap_client {
public:
    auto send_request_vote(...) -> FutureType;
    auto send_append_entries(...) -> FutureType;
    auto send_install_snapshot(...) -> FutureType;
    auto send_multicast_message(...) -> FutureType;
};

} // namespace kythira
```

#### Network Simulator Components
```cpp
namespace kythira {

template<address Addr, port Port, typename FutureType>
class Connection {
public:
    auto read() -> FutureType;
    auto read(std::chrono::milliseconds timeout) -> FutureType;
    auto write(std::vector<std::byte> data) -> FutureType;
    auto write(std::vector<std::byte> data, std::chrono::milliseconds timeout) -> FutureType;
};

template<address Addr, port Port, typename FutureType>
class Listener {
public:
    auto accept() -> FutureType;
    auto accept(std::chrono::milliseconds timeout) -> FutureType;
};

} // namespace kythira
```

## Usage Examples

### Basic Future Usage

```cpp
#include <raft/future.hpp>

// Create a future from a value
auto future1 = kythira::Future<int>(42);

// Create a future from an exception
auto future2 = kythira::Future<int>(std::make_exception_ptr(std::runtime_error("error")));

// Chain operations
auto result = future1.then([](int value) {
    return value * 2;
}).then([](int doubled) {
    return std::to_string(doubled);
});

// Handle errors
auto safe_result = result.onError([](std::exception_ptr ex) {
    return std::string("default");
});

// Get the final result
std::string final_value = safe_result.get();
```

### Using Generic Transport Clients

```cpp
#include <raft/http_transport.hpp>
#include <raft/future.hpp>

// Define the future type to use
using MyFutureType = kythira::Future<raft::request_vote_response<>>;

// Create HTTP client with generic future type
kythira::cpp_httplib_client<
    MyFutureType,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics
> http_client(node_map, config, metrics);

// Send RPC request
auto future = http_client.send_request_vote(target_id, request, timeout);

// Process response with chaining
auto processed_result = future.then([](const raft::request_vote_response<>& response) {
    if (response.vote_granted) {
        std::cout << "Vote granted for term " << response.term << std::endl;
        return std::string("vote_granted");
    } else {
        std::cout << "Vote denied for term " << response.term << std::endl;
        return std::string("vote_denied");
    }
}).onError([](std::exception_ptr ex) {
    try {
        std::rethrow_exception(ex);
    } catch (const std::exception& e) {
        std::cerr << "RPC failed: " << e.what() << std::endl;
        return std::string("rpc_failed");
    }
});

// Get the final result
std::string result = processed_result.get();
```

### Using Network Simulator

```cpp
#include <network_simulator/connection.hpp>
#include <network_simulator/listener.hpp>
#include <raft/future.hpp>

// Define future type for network operations
using NetworkFuture = kythira::Future<std::vector<std::byte>>;
using ConnectionFuture = kythira::Future<std::shared_ptr<kythira::Connection<std::string, std::uint16_t, NetworkFuture>>>;

// Create connection with generic future type
kythira::Connection<std::string, std::uint16_t, NetworkFuture> connection(
    local_endpoint, remote_endpoint, simulator
);

// Read data asynchronously with chaining
auto read_chain = connection.read(std::chrono::milliseconds{1000})
    .then([](const std::vector<std::byte>& data) {
        std::cout << "Received " << data.size() << " bytes" << std::endl;
        return data;
    })
    .onError([](std::exception_ptr ex) {
        std::cerr << "Read failed, returning empty data" << std::endl;
        return std::vector<std::byte>{};
    });

auto data = read_chain.get();

// Write data asynchronously
auto write_future = connection.write(std::move(data));
write_future.get(); // Wait for completion

// Listener operations
kythira::Listener<std::string, std::uint16_t, ConnectionFuture> listener(
    local_endpoint, simulator
);

auto accept_future = listener.accept(std::chrono::milliseconds{5000});
auto new_connection = accept_future.get();
```

### Collective Operations

```cpp
#include <raft/future.hpp>

// Wait for any future to complete
std::vector<kythira::Future<int>> futures;
futures.emplace_back(kythira::Future<int>(1));
futures.emplace_back(kythira::Future<int>(2));

auto any_result = kythira::wait_for_any(std::move(futures));
auto [index, try_result] = any_result.get();

if (try_result.has_value()) {
    std::cout << "First completed future (index " << index << "): " 
              << try_result.value() << std::endl;
}

// Wait for all futures to complete
std::vector<kythira::Future<int>> more_futures;
more_futures.emplace_back(kythira::Future<int>(3));
more_futures.emplace_back(kythira::Future<int>(4));

auto all_results = kythira::wait_for_all(std::move(more_futures));
auto results = all_results.get();

std::cout << "All results: ";
for (const auto& result : results) {
    if (result.has_value()) {
        std::cout << result.value() << " ";
    }
}
std::cout << std::endl;

// Practical example: Collecting votes from multiple nodes
std::vector<kythira::Future<raft::request_vote_response<>>> vote_futures;

// Assume we have multiple HTTP clients
for (const auto& [node_id, client] : http_clients) {
    auto vote_future = client.send_request_vote(node_id, vote_request, timeout);
    vote_futures.push_back(std::move(vote_future));
}

// Wait for all votes
auto all_votes = kythira::wait_for_all(std::move(vote_futures));
auto vote_results = all_votes.get();

// Count granted votes
int granted_votes = 0;
for (const auto& result : vote_results) {
    if (result.has_value() && result.value().vote_granted) {
        granted_votes++;
    }
}

if (granted_votes > vote_results.size() / 2) {
    std::cout << "Majority achieved: " << granted_votes << "/" << vote_results.size() << std::endl;
}
```

## Migration Guide

### From std::future

**Before:**
```cpp
#include <future>

std::future<int> async_operation() {
    return std::async(std::launch::async, []() { return 42; });
}

auto future = async_operation();
int result = future.get();
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<int> async_operation() {
    return kythira::Future<int>(42);
}

auto future = async_operation();
int result = future.get();
```

### From folly::Future

**Before:**
```cpp
#include <folly/futures/Future.h>

folly::Future<int> async_operation() {
    return folly::makeFuture<int>(42);
}

auto future = async_operation();
int result = std::move(future).get();
```

**After:**
```cpp
#include <raft/future.hpp>

kythira::Future<int> async_operation() {
    return kythira::Future<int>(42);
}

auto future = async_operation();
int result = future.get();
```

### Promise/Future Patterns

**Before:**
```cpp
folly::Promise<int> promise;
auto future = promise.getFuture();
promise.setValue(42);
```

**After:**
```cpp
// Direct construction from value
auto future = kythira::Future<int>(42);

// Or from exception
auto error_future = kythira::Future<int>(
    std::make_exception_ptr(std::runtime_error("error"))
);
```

### Template Instantiation

When using the generic implementations, you typically instantiate them with `kythira::Future` as the default:

```cpp
// HTTP transport with kythira::Future
using HttpClient = kythira::cpp_httplib_client<
    kythira::Future<raft::request_vote_response<>>,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics
>;

// CoAP transport with kythira::Future
using CoapClient = kythira::coap_client<
    kythira::Future<raft::request_vote_response<>>,
    raft::json_rpc_serializer<std::vector<std::byte>>,
    raft::noop_metrics,
    raft::console_logger
>;
```

## Benefits

1. **Consistency**: All async operations use the same future interface
2. **Flexibility**: Core implementations can work with different future types
3. **Testability**: Easy to substitute mock future implementations for testing
4. **Performance**: Maintains the same performance characteristics as folly::Future
5. **Type Safety**: Compile-time concept checking ensures correct usage

## Concept Compliance

All implementations include static assertions to verify concept compliance:

```cpp
// Verify kythira::Future satisfies the future concept
static_assert(kythira::future<kythira::Future<int>, int>, 
              "kythira::Future<int> must satisfy future concept");

// Verify transport clients satisfy network_client concept
static_assert(kythira::network_client<HttpClient, kythira::Future<raft::request_vote_response<>>>, 
              "HttpClient must satisfy network_client concept");
```

## Error Handling

The generic future architecture preserves all existing error handling patterns:

- Exception propagation through `onError()` method
- Timeout handling with `wait()` method
- Resource management through RAII patterns
- Thread safety and synchronization behavior

## Performance Considerations

The conversion maintains equivalent performance characteristics:

- `kythira::Future` wraps `folly::Future` internally
- No additional overhead for async operations
- Same memory allocation patterns
- Equivalent thread safety guarantees

## Testing

The system includes comprehensive property-based tests that validate:

- Future concept compliance
- Generic implementation correctness
- Behavioral preservation
- Performance equivalence
- Complete conversion validation

Run the tests with:
```bash
ctest -R "future.*property_test|core_implementation.*property_test"
```