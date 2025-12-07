# Network Simulator

A C++23 network simulator library that models network communication between nodes using concepts and template metaprogramming.

## Features

- **Type-safe design** using C++23 concepts
- **Flexible addressing** supporting strings, integers, IPv4, and IPv6
- **Connectionless communication** (datagram-style like UDP)
- **Connection-oriented communication** (stream-style like TCP)
- **Configurable network characteristics** including latency and reliability
- **Asynchronous operations** using folly::Future
- **Thread-safe** implementation

## Requirements

- C++23 compatible compiler (GCC 13+, Clang 16+, or MSVC 2022+)
- CMake 3.20 or higher
- folly library
- Boost (system, thread, unit_test_framework)

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

```cpp
#include <network_simulator/network_simulator.hpp>

// Example usage will be added as implementation progresses
```

## Documentation

See the design document at `.kiro/specs/network-simulator/design.md` for detailed architecture and design decisions.

## License

TBD
