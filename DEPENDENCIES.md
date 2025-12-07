# Dependencies

This document lists the dependencies required to build and use the network simulator library.

## Required Dependencies

### C++ Compiler
- **GCC 13+**, **Clang 16+**, or **MSVC 2022+**
- Must support C++23 standard
- Concepts support required

### Build System
- **CMake 3.20 or higher**

### Libraries

#### folly (Facebook Open-source Library)
- **Status**: Required for full implementation
- **Purpose**: Provides Future/Promise implementation and executor framework
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install libfolly-dev
  
  # macOS (Homebrew)
  brew install folly
  
  # From source
  git clone https://github.com/facebook/folly.git
  cd folly
  mkdir build && cd build
  cmake ..
  make
  sudo make install
  ```

#### Boost
- **Status**: Required
- **Components**: system, thread, unit_test_framework
- **Minimum Version**: 1.70+
- **Installation**:
  ```bash
  # Ubuntu/Debian
  sudo apt-get install libboost-all-dev
  
  # macOS (Homebrew)
  brew install boost
  ```

## Optional Dependencies

### Property-Based Testing Library
- **RapidCheck** or similar C++ property-based testing framework
- Required for property-based tests (tasks 4.5+)
- Installation instructions will be added when implementing property tests

## Current Build Status

The project currently builds with:
- ✅ CMake 3.28.3
- ✅ GCC 13.3.0
- ✅ Boost 1.84.0
- ⚠️  folly (not yet installed, but build system is configured)

## Verifying Dependencies

To check if dependencies are installed:

```bash
# Check compiler version
g++ --version

# Check CMake version
cmake --version

# Check Boost
dpkg -l | grep libboost  # Ubuntu/Debian
brew list boost          # macOS

# Check folly
pkg-config --exists folly && echo "folly found" || echo "folly not found"
```

## Building Without folly

The current implementation can be built without folly installed. The build system will issue a warning but will continue. However, the full implementation of async operations will require folly to be installed.

## Next Steps

1. Install folly library for async operations support
2. Install property-based testing framework for comprehensive testing
3. Verify all dependencies are correctly linked
