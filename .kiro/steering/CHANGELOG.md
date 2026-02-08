---
title: Steering Documents Changelog
inclusion: manual
---

# Steering Documents Changelog

This document tracks changes to the steering documents in this directory.

## 2026-02-04: Added Efficient Test Execution with Output Storage

### test-execution-standards.md

Added new section: **Efficient Test Execution with Output Storage**

**Key Points:**
- When running large test suites (>10 tests), ALWAYS store output in a file first
- Analyze stored output multiple times instead of re-running tests
- Use `tee` to view output while storing it
- Use timestamped filenames to preserve test history
- Prevents wasting time and resources on redundant test runs

**The Golden Rule:**
```bash
# ✅ CORRECT - Store once, analyze many times
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee test_results.txt

# Now analyze as needed without re-running
grep "Failed" test_results.txt
tail -50 test_results.txt
head -100 test_results.txt

# ❌ INCORRECT - Running tests multiple times
ctest ... | head -100    # First run
ctest ... | tail -50     # Second run (wastes time!)
ctest ... | grep Failed  # Third run (wastes time!)
```

**Benefits:**
- Time efficiency - Large test suites can take minutes to hours
- Resource conservation - Avoid unnecessary CPU and memory usage
- Consistency - Analyze the same test run results multiple times
- Debugging - Preserve test output for later investigation
- CI/CD optimization - Reduce pipeline execution time

### QUICK_REFERENCE.md

Added new section: **Test Execution**

**Key Points:**
- Quick reference for CTest usage
- Store output workflow for large test suites
- Test filtering examples
- Added test execution items to checklist

## 2024-12-15: Updated to Two-Argument BOOST_AUTO_TEST_CASE Requirement

### All Steering Documents

**Updated Requirement**: Changed from `BOOST_TEST_TIMEOUT` function calls to two-argument `BOOST_AUTO_TEST_CASE` with timeout.

**Key Changes:**
- Replaced `BOOST_TEST_TIMEOUT(seconds)` function approach with `BOOST_AUTO_TEST_CASE(name, * boost::unit_test::timeout(seconds))`
- Updated all examples to use the two-argument form
- Marked the `BOOST_TEST_TIMEOUT` function approach as deprecated
- Provides better per-test-case timeout control
- More consistent with modern Boost.Test practices

**New Syntax:**
```cpp
// ✅ NEW CORRECT APPROACH
BOOST_AUTO_TEST_CASE(my_test, * boost::unit_test::timeout(30)) {
    // Test implementation
}

// ❌ OLD DEPRECATED APPROACH
BOOST_AUTO_TEST_CASE(my_test) {
    BOOST_TEST_TIMEOUT(30);
    // Test implementation
}
```

## 2024-12-14: Added BOOST_TEST_TIMEOUT Requirement

### test-execution-standards.md

Added new section: **BOOST_TEST_TIMEOUT Requirement**

**Key Points:**
- ALL Boost.Test test cases MUST include `BOOST_TEST_TIMEOUT`
- Prevents tests from hanging indefinitely in CI/CD pipelines
- Provides faster feedback when tests encounter infinite loops or deadlocks
- Complements CTest timeout management with test-case-level granularity

**Timeout Guidelines:**
- Unit Tests: 10-30 seconds
- Integration Tests: 30-60 seconds
- Property Tests: 60-120 seconds
- Performance Tests: 120-300 seconds
- Network Tests: 60-180 seconds

**Best Practices:**
- Place `BOOST_TEST_TIMEOUT` as first line in test cases
- Be generous but reasonable with timeout values
- Consider CI environment performance differences
- Document long timeouts with explanations
- Use consistent timeouts for similar test types

### cpp-coding-standards.md

Added new section: **BOOST_TEST_TIMEOUT Requirement**

**Key Points:**
- Mandatory timeout specification for all Boost.Test test cases
- Prevents hanging tests in CI/CD environments
- Provides consistent timeout behavior across environments
- Includes comprehensive examples and guidelines

**Implementation Requirements:**
- Timeout must be first line in test case
- Appropriate timeout values for different test categories
- Clear examples of correct and incorrect usage

### example-programs.md

Added new section: **Examples Using Boost.Test**

**Key Points:**
- Example programs using Boost.Test MUST include `BOOST_TEST_TIMEOUT`
- Examples may need longer timeouts due to demonstration complexity
- Provides specific timeout guidelines for example scenarios

**Timeout Guidelines for Examples:**
- Simple demonstrations: 60-120 seconds
- Complex integration scenarios: 120-300 seconds
- Performance demonstrations: 300+ seconds (with justification)

### QUICK_REFERENCE.md

Added new section: **Test Requirements**

**Key Points:**
- Quick reference for BOOST_TEST_TIMEOUT requirement
- Includes timeout guidelines table
- Added to checklist for code review

**Checklist Updates:**
- Added "All Boost.Test cases include `BOOST_TEST_TIMEOUT`"
- Added "Test timeouts are appropriate for test type"

## Rationale

These additions were made to:

1. **Prevent hanging tests** - Eliminate indefinite test execution in CI/CD
2. **Improve CI/CD reliability** - Faster feedback when tests encounter problems
3. **Standardize timeout behavior** - Consistent approach across all test code
4. **Complement existing timeout management** - Work with CTest timeout features
5. **Enforce through code review** - Make timeout requirement mandatory

## Impact

### On Existing Code

Existing Boost.Test code should be updated to include `BOOST_TEST_TIMEOUT`:
- New test code MUST include timeout specification
- Existing tests SHOULD be updated when modified
- Code reviews MUST check for timeout compliance

### On New Development

All new Boost.Test code must:
- Include `BOOST_TEST_TIMEOUT` as first line in test cases
- Use appropriate timeout values for test complexity
- Follow the guidelines provided in steering documents
- Pass code review timeout compliance checks

## Examples

### Before (Non-Compliant)

```cpp
BOOST_AUTO_TEST_CASE(network_communication_test) {
    // Missing BOOST_TEST_TIMEOUT - could hang indefinitely
    auto client = create_client();
    auto result = client.send_request("test_data");
    BOOST_CHECK(!result.empty());
}
```

### After (Compliant)

```cpp
BOOST_AUTO_TEST_CASE(network_communication_test) {
    BOOST_TEST_TIMEOUT(45);  // 45 second timeout
    
    auto client = create_client();
    auto result = client.send_request("test_data");
    BOOST_CHECK(!result.empty());
}
```

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
