---
title: Coding Standards Quick Reference
inclusion: manual
---

# Coding Standards Quick Reference

Quick reference for the most commonly needed coding standards.

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Concepts | `snake_case` | `network_fixture`, `virtual_node` |
| Classes/Structs | `PascalCase` | `LocalNetworkFixture`, `NodeConfig` |
| Member Functions | `snake_case` | `send_message()`, `create_node()` |
| Member Variables | `_snake_case` | `_topology`, `_latency_simulator` |
| Local Variables | `snake_case` | `connection_id`, `start_time` |
| Function Parameters | `snake_case` | `from_id`, `to_id`, `msg` |
| Enum Classes | `PascalCase` | `ConnectionType`, `FailureType` |
| Enum Values | `snake_case` | `reliable`, `connection_drop` |
| Constants | `snake_case` | `default_timeout`, `max_nodes` |
| Namespaces | `snake_case` | `network_fixture`, `raft` |
| Template Types | `PascalCase` | `NodeType`, `MessageType` |
| Template Values | `snake_case` | `buffer_size`, `max_count` |

## Constants

### ✅ DO: Define as Named Constants

```cpp
namespace {
    constexpr const char* client_id = "client";
    constexpr std::size_t max_retries = 3;
    constexpr std::chrono::milliseconds timeout{5000};
}

auto connect() -> void {
    for (std::size_t i = 0; i < max_retries; ++i) {
        if (try_connect(client_id, timeout)) {
            return;
        }
    }
}
```

### ❌ DON'T: Use Magic Numbers/Strings

```cpp
auto connect() -> void {
    for (std::size_t i = 0; i < 3; ++i) {  // What is 3?
        if (try_connect("client", std::chrono::milliseconds{5000})) {
            return;
        }
    }
}
```

### Acceptable Without Constants

- `true`, `false`
- `nullptr`, `std::nullopt`
- `0`, `1` in obvious contexts (indexing, initialization)
- `""` when semantically clear
- Single-use format strings

## Constant Scoping

```cpp
// File-local (anonymous namespace)
namespace {
    constexpr std::size_t buffer_size = 4096;
}

// Class constant
class Fixture {
    static constexpr std::size_t max_nodes = 1000;
};

// Shared constant (header)
namespace network_fixture {
    inline constexpr std::size_t default_capacity = 1000;
}
```

## Function Signatures

```cpp
// ✅ Preferred: Trailing return type
auto send_message(Message msg) -> folly::Future<folly::Unit>;
auto is_connected() const noexcept -> bool;

// ✅ Acceptable: Simple types
void reset();
bool is_valid() const;
```

## Const Correctness

```cpp
class NetworkFixture {
public:
    // Const member function
    [[nodiscard]] auto stats() const -> NetworkStats;
    
    // Noexcept when guaranteed
    [[nodiscard]] auto is_started() const noexcept -> bool;
    
    // Const reference parameter
    auto configure(const LatencyConfig& config) -> void;
};
```

## Example Programs

### Structure

```cpp
int main() {
    int failed_scenarios = 0;
    
    // Run all scenarios
    if (!test_scenario_1()) failed_scenarios++;
    if (!test_scenario_2()) failed_scenarios++;
    if (!test_scenario_3()) failed_scenarios++;
    
    // Report results
    if (failed_scenarios > 0) {
        std::cerr << "❌ " << failed_scenarios << " scenario(s) failed\n";
        return 1;
    }
    
    std::cout << "✅ All scenarios passed!\n";
    return 0;
}
```

### Build System Integration

```cmake
# Helper function
function(add_network_example example_name source_file output_dir)
    add_executable(${example_name} ${source_file})
    target_link_libraries(${example_name} PRIVATE
        network_fixture
        Folly::folly
    )
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${output_dir}"
    )
endfunction()

# Add example
add_network_example(my_example 
    network-test-fixture/my_example.cpp
    network-test-fixture)
```

### Test Scenario

```cpp
namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
}

auto test_basic_communication() -> bool {
    try {
        auto node_a = fixture.create(test_node_a);
        auto node_b = fixture.create(test_node_b);
        
        // Test logic here
        
        std::cout << "  ✓ Test passed\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Test failed: " << e.what() << "\n";
        return false;
    }
}
```

## Common Patterns

### Error Handling

```cpp
if (!_running) {
    return folly::makeFuture<folly::Unit>(
        std::runtime_error("Transport not running"));
}
```

### Member Variable Access

```cpp
auto send_message(Message msg) -> void {
    if (!_started) {  // Clear member variable access
        throw std::runtime_error("Not started");
    }
    _topology.add(std::move(msg));
}
```

### Range-Based Loops

```cpp
for (const auto& node : _topology.nodes()) {
    process_node(node);
}
```

## File Organization

```cpp
#pragma once

#include <system_headers>
#include <third_party_headers>
#include "project_headers.hpp"

namespace project_name {

// Forward declarations
class ClassName;

// Constants
inline constexpr std::size_t default_value = 100;

// Class definition
class ClassName {
public:
    // Public interface
    
private:
    // Private members
};

} // namespace project_name
```

## Quick Checklist

Before committing code, verify:

- [ ] All names follow conventions (snake_case, PascalCase, _snake_case)
- [ ] String literals defined as constants
- [ ] Numeric values defined as constants (except acceptable cases)
- [ ] Member variables prefixed with underscore
- [ ] Const correctness applied
- [ ] Noexcept used where appropriate
- [ ] [[nodiscard]] on functions returning values
- [ ] Example programs run all scenarios
- [ ] Example programs return correct exit codes
- [ ] Clear pass/fail indication in output
- [ ] Examples added to CMakeLists.txt
- [ ] Examples build successfully

## See Also

- [Full C++ Coding Standards](cpp-coding-standards.md)
- [Example Program Guidelines](example-programs.md)
- [Changelog](CHANGELOG.md)
