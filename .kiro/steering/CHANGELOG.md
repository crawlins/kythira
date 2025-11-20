---
title: Steering Documents Changelog
inclusion: manual
---

# Steering Documents Changelog

This document tracks changes to the steering documents in this directory.

## 2024-11-19: Added Build System Integration Guidelines

### example-programs.md

Added new section: **Build System Integration**

**Key Points:**
- Example programs MUST be integrated into the build system
- All examples must be added to CMakeLists.txt
- Provides CMakeLists.txt template and helper function pattern
- Specifies directory structure and organization
- Documents build and CI/CD integration requirements

**Requirements Added:**
- Executable target for each example
- Library linking specifications
- C++23 standard requirement
- Output directory organization
- Subdirectory structure for components

**Benefits Highlighted:**
- Automatic compilation with project
- Dependency management via CMake
- Regression detection through build failures
- CI/CD testing automation
- Documentation validation
- Easy discovery and execution

**Checklist Updated:**
- Added "Added to CMakeLists.txt"
- Added "Builds successfully with cmake"
- Added "Linked against required libraries"
- Added "Placed in appropriate output directory"

## 2024-11-19: Added Constants Guidelines

### cpp-coding-standards.md

Added new section: **Magic Numbers and String Literals**

**Key Points:**
- String and numeric literals MUST be defined as named constants
- Applies to both regular code and test code
- Improves readability, maintainability, and reduces errors
- Provides clear examples of correct and incorrect usage

**Acceptable Exceptions:**
- Boolean literals (`true`, `false`)
- Null values (`nullptr`, `std::nullopt`)
- Zero/one in obvious contexts (array indexing, loop initialization)
- Empty strings when semantically clear
- Format strings used only once

**Scope Guidelines:**
- File-local constants: Use anonymous namespace
- Class constants: Use `static constexpr` members
- Shared constants: Define in header with `inline constexpr`

### example-programs.md

Added new best practice: **Use Named Constants**

**Key Points:**
- Example programs should define string and numeric literals as constants
- Demonstrates good coding practices to users
- Makes examples more maintainable
- Reduces risk of typos in repeated literals
- Shows consistent values across test scenarios

**Benefits Highlighted:**
- Self-documenting code
- Easy to modify values in one place
- Reduces typo errors
- Consistent values across test scenarios
- Improves maintainability

## Rationale

These additions were made to:

1. **Standardize code quality** - Ensure consistent practices across the codebase
2. **Improve maintainability** - Make it easier to update values and understand code
3. **Reduce errors** - Minimize typos from repeated string literals
4. **Educate developers** - Show best practices through examples
5. **Align with industry standards** - Follow common C++ best practices

## Impact

### On Existing Code

Existing code is not required to be updated immediately, but:
- New code MUST follow these guidelines
- Code being modified SHOULD be updated to follow guidelines
- Code reviews MUST check for compliance

### On New Development

All new code and examples must:
- Define string literals as named constants
- Define numeric values as named constants (except acceptable exceptions)
- Use appropriate scoping for constants (namespace, class, or inline)
- Follow the examples provided in the steering documents

## Examples

### Before (Non-Compliant)

```cpp
auto test_network() -> void {
    auto client = fixture.create("client");
    auto server = fixture.create("server");
    
    for (int i = 0; i < 100; ++i) {
        fixture.send("client", "server", Message("msg", "client", "server", "data"));
    }
}
```

### After (Compliant)

```cpp
namespace {
    constexpr const char* client_id = "client";
    constexpr const char* server_id = "server";
    constexpr std::size_t message_count = 100;
    constexpr const char* test_payload = "data";
}

auto test_network() -> void {
    auto client = fixture.create(client_id);
    auto server = fixture.create(server_id);
    
    for (std::size_t i = 0; i < message_count; ++i) {
        fixture.send(
            client_id, 
            server_id, 
            Message("msg", client_id, server_id, test_payload)
        );
    }
}
```

## See Also

- [C++ Coding Standards](cpp-coding-standards.md)
- [Example Program Guidelines](example-programs.md)
- [C++ Core Guidelines - Con: Constants and Immutability](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#S-const)
