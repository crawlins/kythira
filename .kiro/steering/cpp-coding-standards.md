---
title: C++ Coding Standards
inclusion: always
---

# C++ Coding Standards

This document defines the coding standards and naming conventions for C++ code in this project.

## Naming Conventions

### Concepts (C++20/C++23)

**Rule**: Concept names MUST use snake_case.

```cpp
// ✅ CORRECT
template<typename T>
concept network_fixture = requires(T fixture) { /* ... */ };

template<typename T>
concept virtual_node = requires(T node) { /* ... */ };

template<typename T>
concept message_filter = requires(T filter) { /* ... */ };

// ❌ INCORRECT
template<typename T>
concept NetworkFixture = requires(T fixture) { /* ... */ };  // PascalCase

template<typename T>
concept virtualNode = requires(T node) { /* ... */ };  // camelCase
```

**Rationale**: 
- Concepts are type constraints, similar to type traits
- snake_case distinguishes them from class names
- Consistent with standard library conventions (e.g., `std::integral`, `std::movable`)

### Classes and Structs

**Rule**: Class and struct names MUST use PascalCase.

```cpp
// ✅ CORRECT
class LocalNetworkFixture { /* ... */ };
class VirtualNode { /* ... */ };
struct NodeConfig { /* ... */ };
struct MessageEvent { /* ... */ };

// ❌ INCORRECT
class local_network_fixture { /* ... */ };  // snake_case
class virtualNode { /* ... */ };  // camelCase
```

### Member Functions

**Rule**: Member function names MUST use snake_case.

```cpp
// ✅ CORRECT
class NetworkFixture {
public:
    auto create_node(std::string id) -> void;
    auto send_message(Message msg) -> folly::Future<folly::Unit>;
    auto configure_latency(LatencyConfig config) -> void;
    auto is_connected(std::string from, std::string to) const -> bool;
};

// ❌ INCORRECT
class NetworkFixture {
public:
    auto CreateNode(std::string id) -> void;  // PascalCase
    auto sendMessage(Message msg) -> void;  // camelCase
};
```

**Rationale**:
- Consistent with standard library (e.g., `std::vector::push_back()`)
- Clear distinction from class names
- Improves readability

### Member Variables

**Rule**: Member variables MUST use snake_case prefixed with an underscore.

```cpp
// ✅ CORRECT
class NetworkFixture {
private:
    NetworkTopology _topology;
    LatencySimulator _latency_simulator;
    FailureInjector _failure_injector;
    std::atomic<bool> _started{false};
    mutable std::shared_mutex _mutex;
};

// ❌ INCORRECT
class NetworkFixture {
private:
    NetworkTopology topology;  // No underscore prefix
    NetworkTopology m_topology;  // Hungarian notation
    NetworkTopology mTopology;  // camelCase with prefix
    NetworkTopology Topology;  // PascalCase
};
```

**Rationale**:
- Underscore prefix clearly distinguishes member variables from local variables
- Prevents naming conflicts with function parameters
- Makes member access obvious in code
- Avoids Hungarian notation (m_, p_, etc.)

### Local Variables and Parameters

**Rule**: Local variables and function parameters MUST use snake_case (no prefix).

```cpp
// ✅ CORRECT
auto send_message(std::string from_id, std::string to_id, Message msg) -> void {
    auto connection_id = generate_id(from_id, to_id);
    auto future = deliver_message(msg);
    auto result = future.get();
}

// ❌ INCORRECT
auto send_message(std::string FromId, std::string toId) -> void {  // Mixed case
    auto ConnectionId = generate_id(FromId, toId);  // PascalCase
}
```

### Constants and Enumerations

**Rule**: 
- Enum class names use PascalCase
- Enum values use snake_case
- Constants use snake_case

```cpp
// ✅ CORRECT
enum class ConnectionType : std::uint8_t {
    reliable,
    unreliable,
    broadcast,
    multicast
};

enum class FailureType : std::uint8_t {
    none,
    connection_drop,
    network_partition,
    message_loss
};

constexpr std::size_t default_queue_size = 1000;
constexpr std::chrono::milliseconds default_timeout{5000};

// ❌ INCORRECT
enum class ConnectionType {
    Reliable,  // PascalCase
    UNRELIABLE,  // UPPER_CASE
    broadCast  // camelCase
};

constexpr std::size_t DEFAULT_QUEUE_SIZE = 1000;  // UPPER_CASE
constexpr std::size_t DefaultQueueSize = 1000;  // PascalCase
```

### Namespaces

**Rule**: Namespace names MUST use snake_case.

```cpp
// ✅ CORRECT
namespace network_fixture {
    // ...
}

namespace network_fixture::detail {
    // ...
}

// ❌ INCORRECT
namespace NetworkFixture {  // PascalCase
    // ...
}

namespace networkFixture {  // camelCase
    // ...
}
```

### Template Parameters

**Rule**: Template type parameters use PascalCase, template value parameters use snake_case.

```cpp
// ✅ CORRECT
template<typename T, typename Allocator, std::size_t buffer_size>
class Container { /* ... */ };

template<typename NodeType, typename MessageType>
auto send(NodeType node, MessageType msg) -> void;

// ❌ INCORRECT
template<typename t, typename allocator>  // lowercase
class Container { /* ... */ };

template<typename node_type>  // snake_case for types
auto send(node_type node) -> void;
```

## Complete Example

```cpp
// Concept definition (snake_case)
template<typename T>
concept network_fixture = requires(T fixture) {
    { fixture.create_node("id") } -> std::same_as<void>;
    { fixture.send_message(Message{}) } -> std::same_as<folly::Future<folly::Unit>>;
};

// Enum class (PascalCase) with values (snake_case)
enum class ConnectionState : std::uint8_t {
    connecting,
    connected,
    disconnected,
    failed
};

// Class definition (PascalCase)
class LocalNetworkFixture {
public:
    // Member functions (snake_case)
    auto create_node(std::string node_id) -> void;
    auto send_message(std::string from, std::string to, Message msg) -> folly::Future<folly::Unit>;
    auto configure_latency(LatencyConfig config) -> void;
    [[nodiscard]] auto is_started() const noexcept -> bool;
    
private:
    // Member variables (snake_case with underscore prefix)
    NetworkTopology _topology;
    LatencySimulator _latency_simulator;
    FailureInjector _failure_injector;
    std::atomic<bool> _started{false};
    mutable std::shared_mutex _mutex;
    
    // Private member functions (snake_case)
    auto deliver_message(Message msg) -> folly::Future<folly::Unit>;
    auto update_statistics() -> void;
};

// Namespace (snake_case)
namespace network_fixture {

// Function implementation
auto LocalNetworkFixture::send_message(
    std::string from_id,  // Parameters (snake_case)
    std::string to_id,
    Message msg
) -> folly::Future<folly::Unit> {
    // Local variables (snake_case)
    auto connection_id = generate_id(from_id, to_id);
    auto start_time = std::chrono::steady_clock::now();
    
    // Member variable access (with underscore)
    if (!_started) {
        return folly::makeFuture<folly::Unit>(
            std::runtime_error("Fixture not started"));
    }
    
    return deliver_message(std::move(msg));
}

} // namespace network_fixture
```

## Additional Conventions

### File Names

- Header files: snake_case with `.hpp` extension
  - `local_network_fixture.hpp`
  - `virtual_node.hpp`
  - `config_concepts.hpp`

- Source files: snake_case with `.cpp` extension
  - `local_network_fixture.cpp`
  - `virtual_node.cpp`

### Include Guards

Use `#pragma once` instead of traditional include guards.

```cpp
// ✅ CORRECT
#pragma once

namespace network_fixture {
    // ...
}

// ❌ AVOID (but acceptable)
#ifndef NETWORK_FIXTURE_LOCAL_NETWORK_FIXTURE_HPP
#define NETWORK_FIXTURE_LOCAL_NETWORK_FIXTURE_HPP
// ...
#endif
```

### Function Return Types

Use trailing return type syntax for consistency:

```cpp
// ✅ PREFERRED
auto send_message(Message msg) -> folly::Future<folly::Unit>;
auto is_connected() const noexcept -> bool;

// ✅ ACCEPTABLE (for simple types)
void reset();
bool is_valid() const;
```

### Const Correctness

- Mark member functions `const` when they don't modify state
- Use `const&` for parameters that won't be modified
- Use `noexcept` when functions guarantee no exceptions

```cpp
class NetworkFixture {
public:
    [[nodiscard]] auto stats() const -> NetworkStats;
    [[nodiscard]] auto is_started() const noexcept -> bool;
    auto configure(const LatencyConfig& config) -> void;
};
```

### Magic Numbers and String Literals

**Rule**: String and numeric literals MUST be defined as named constants in both regular and test code.

**Rationale**:
- Improves code readability and maintainability
- Makes values self-documenting
- Enables easy modification in one place
- Reduces errors from typos in repeated literals
- Facilitates testing with consistent values

```cpp
// ✅ CORRECT - Named constants
namespace {
    constexpr std::size_t default_queue_capacity = 1000;
    constexpr std::chrono::milliseconds default_timeout{5000};
    constexpr std::chrono::milliseconds min_latency{10};
    constexpr std::chrono::milliseconds max_latency{50};
    constexpr double default_reliability = 0.99;
    constexpr const char* default_node_prefix = "node";
}

auto configure_network() -> void {
    fixture.configure_latency(LatencyConfig::uniform(min_latency, max_latency));
    fixture.configure_reliability(default_reliability);
}

// ❌ INCORRECT - Magic numbers and strings
auto configure_network() -> void {
    fixture.configure_latency(LatencyConfig::uniform(
        std::chrono::milliseconds{10},  // What does 10 mean?
        std::chrono::milliseconds{50}   // What does 50 mean?
    ));
    fixture.configure_reliability(0.99);  // Magic number
}
```

**In Test Code:**

```cpp
// ✅ CORRECT - Test constants
namespace {
    constexpr const char* test_client_id = "test_client";
    constexpr const char* test_server_id = "test_server";
    constexpr const char* test_message_payload = "Hello, World!";
    constexpr std::size_t test_message_count = 100;
    constexpr std::chrono::seconds test_timeout{30};
}

BOOST_AUTO_TEST_CASE(test_message_delivery) {
    auto client = fixture.create(test_client_id);
    auto server = fixture.create(test_server_id);
    
    for (std::size_t i = 0; i < test_message_count; ++i) {
        Message msg(
            std::format("msg_{}", i),
            test_client_id,
            test_server_id,
            test_message_payload
        );
        fixture.send(test_client_id, test_server_id, std::move(msg));
    }
}

// ❌ INCORRECT - Repeated literals
BOOST_AUTO_TEST_CASE(test_message_delivery) {
    auto client = fixture.create("test_client");  // Repeated literal
    auto server = fixture.create("test_server");  // Repeated literal
    
    for (std::size_t i = 0; i < 100; ++i) {  // Magic number
        Message msg(
            std::format("msg_{}", i),
            "test_client",   // Repeated literal (typo risk)
            "test_server",   // Repeated literal (typo risk)
            "Hello, World!"  // Repeated literal
        );
        fixture.send("test_client", "test_server", std::move(msg));
    }
}
```

**Acceptable Exceptions:**

The following are acceptable without named constants:
- Boolean literals: `true`, `false`
- Null values: `nullptr`, `std::nullopt`
- Zero/one in obvious contexts: `0`, `1` (e.g., array indexing, loop initialization)
- Empty strings when semantically clear: `""`
- Format strings that are used only once and are self-documenting

```cpp
// ✅ ACCEPTABLE - Obvious values
if (count == 0) { /* ... */ }
for (std::size_t i = 0; i < items.size(); ++i) { /* ... */ }
auto msg = Message("", from, to, payload);  // Empty ID is clear
```

**Scope of Constants:**

- **File-local constants**: Use anonymous namespace
- **Class constants**: Use `static constexpr` members
- **Shared constants**: Define in header with `inline constexpr`

```cpp
// File-local constants
namespace {
    constexpr std::size_t buffer_size = 4096;
}

// Class constants
class NetworkFixture {
public:
    static constexpr std::size_t max_nodes = 1000;
    static constexpr std::chrono::milliseconds default_timeout{5000};
};

// Shared constants (in header)
namespace network_fixture {
    inline constexpr std::size_t default_queue_capacity = 1000;
    inline constexpr const char* default_node_id_prefix = "node";
}
```

## Rationale Summary

| Element | Convention | Rationale |
|---------|-----------|-----------|
| Concepts | snake_case | Distinguishes from classes, matches std library |
| Classes | PascalCase | Standard C++ convention, clear type names |
| Member Functions | snake_case | Matches std library, clear distinction from classes |
| Member Variables | _snake_case | Clear distinction from locals, no naming conflicts |
| Local Variables | snake_case | Clean, readable, matches std library |
| Enum Classes | PascalCase | Type names like classes |
| Enum Values | snake_case | Values like variables/constants |
| Namespaces | snake_case | Matches std library convention |

## Enforcement

- Code reviews MUST verify naming conventions
- Linters/formatters SHOULD be configured to check conventions
- New code MUST follow these conventions
- Existing code SHOULD be updated when modified

## Exceptions

The only acceptable exceptions are:
1. Third-party library interfaces (match their conventions)
2. Platform-specific code (match platform conventions)
3. Legacy code being gradually migrated (document the plan)

## See Also

- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/)
- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)
