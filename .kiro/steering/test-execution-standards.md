---
title: Test Execution Standards
inclusion: always
---

# Test Execution Standards

This document defines the mandatory standards for executing tests in this project. These standards ensure consistency, reliability, and proper integration with CI/CD pipelines.

## Core Principle

**ALL tests MUST be executed through CTest.** Direct execution of test binaries should only be used for debugging purposes.

## Mandatory CTest Usage

### Rule: Always Use CTest

**✅ CORRECT - Use CTest for all test execution:**
```bash
# Run all tests
ctest

# Run tests in parallel
ctest -j$(nproc)

# Run specific test pattern
ctest -R "coap.*test"

# Run tests with verbose output
ctest --verbose

# Run tests and show output on failure
ctest --output-on-failure
```

**❌ INCORRECT - Direct test execution:**
```bash
# Wrong - bypasses CTest infrastructure
./build/tests/my_test
./build/examples/my_example
```

### Rationale for CTest-Only Execution

1. **Consistency** - Same execution method across all environments
2. **Parallel Execution** - Faster test runs with `-j$(nproc)`
3. **Timeout Management** - Prevents hanging tests
4. **Result Reporting** - Standardized output for CI/CD
5. **Test Discovery** - Automatic detection of all registered tests
6. **Filtering** - Easy selection of test subsets
7. **Integration** - Works with CDash and other reporting tools

## Tests with Additional Arguments

### The Golden Rule

**When a test program needs additional command-line arguments, you MUST modify the `add_test` call in CMakeLists.txt to include those arguments, then run via `ctest`.**

### Step-by-Step Process

1. **Identify the need for arguments**
   - Test needs configuration files
   - Test requires specific parameters
   - Test needs environment variables
   - Test requires specific working directory

2. **Modify CMakeLists.txt**
   ```cmake
   # BEFORE - Basic test registration
   add_test(NAME my_test COMMAND my_test)
   
   # AFTER - Test with arguments
   add_test(NAME my_test COMMAND my_test --config=test.json --verbose)
   set_tests_properties(my_test PROPERTIES
       TIMEOUT 60
       LABELS "integration"
       WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test_data
       ENVIRONMENT "TEST_MODE=integration;LOG_LEVEL=debug"
   )
   ```

3. **Run via CTest**
   ```bash
   # Run the configured test
   ctest -R my_test --verbose
   ```

### Common Argument Patterns

#### Configuration Files
```cmake
add_test(NAME network_test COMMAND network_test 
    --config=${CMAKE_SOURCE_DIR}/test_configs/network.json)
```

#### Multiple Parameters
```cmake
add_test(NAME performance_test COMMAND performance_test 
    --threads=4 
    --duration=30s 
    --output=${CMAKE_BINARY_DIR}/perf_results.txt)
```

#### Environment Variables
```cmake
add_test(NAME integration_test COMMAND integration_test --verbose)
set_tests_properties(integration_test PROPERTIES
    ENVIRONMENT "TEST_DATA_DIR=${CMAKE_SOURCE_DIR}/test_data;DEBUG=1"
)
```

#### Working Directory
```cmake
add_test(NAME file_test COMMAND file_test --input=sample.txt)
set_tests_properties(file_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/test_data
)
```

#### Complex Example
```cmake
add_test(NAME coap_integration_test COMMAND coap_integration_test 
    --server-port=5683 
    --client-count=10 
    --duration=60s
    --config=${CMAKE_SOURCE_DIR}/test_configs/coap.json)
set_tests_properties(coap_integration_test PROPERTIES
    TIMEOUT 120
    LABELS "integration;coap;slow"
    ENVIRONMENT "COAP_LOG_LEVEL=debug"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/test_output
)
```

## Test Categories and Labels

### Organize Tests with Labels

Use consistent labels to categorize tests:

```cmake
# Unit tests
set_tests_properties(unit_test PROPERTIES LABELS "unit;fast")

# Integration tests  
set_tests_properties(integration_test PROPERTIES LABELS "integration;slow")

# Property-based tests
set_tests_properties(property_test PROPERTIES LABELS "property;unit")

# Example programs as tests
set_tests_properties(example_test PROPERTIES LABELS "example;integration")

# Performance tests
set_tests_properties(perf_test PROPERTIES LABELS "performance;slow")
```

### Running Test Categories

```bash
# Run only unit tests
ctest -L unit

# Run only fast tests
ctest -L fast

# Run integration tests
ctest -L integration

# Run everything except slow tests
ctest -LE slow

# Run CoAP-related tests
ctest -R "coap.*"
```

## Timeout Management

### Set Appropriate Timeouts

```cmake
# Fast unit tests - short timeout
set_tests_properties(unit_test PROPERTIES TIMEOUT 10)

# Integration tests - medium timeout
set_tests_properties(integration_test PROPERTIES TIMEOUT 60)

# Performance tests - long timeout
set_tests_properties(performance_test PROPERTIES TIMEOUT 300)

# Example programs - medium timeout
set_tests_properties(example_test PROPERTIES TIMEOUT 120)
```

### BOOST_TEST_TIMEOUT Requirement

**Rule**: ALL Boost.Test test cases MUST use the two-argument version of `BOOST_AUTO_TEST_CASE` with timeout to prevent hanging tests.

**Rationale**:
- Prevents tests from hanging indefinitely in CI/CD pipelines
- Provides faster feedback when tests encounter infinite loops or deadlocks
- Ensures consistent timeout behavior across different test environments
- Complements CTest timeout management with test-case-level granularity
- Provides per-test-case timeout control for better granularity

**Implementation**:

```cpp
// ✅ CORRECT - Two-argument version with timeout
BOOST_AUTO_TEST_CASE(my_test_case, * boost::unit_test::timeout(30)) {
    // Test implementation - 30 second timeout
    // ...
}

// ✅ CORRECT - Property-based tests with longer timeout
BOOST_AUTO_TEST_CASE(property_based_test, * boost::unit_test::timeout(60)) {
    // Property test with many iterations - 60 second timeout
    // ...
}

// ✅ CORRECT - Integration tests with extended timeout
BOOST_AUTO_TEST_CASE(integration_test, * boost::unit_test::timeout(120)) {
    // Integration test - 120 second timeout
    // ...
}

// ❌ INCORRECT - Single argument without timeout
BOOST_AUTO_TEST_CASE(my_test_case) {
    // No timeout - test could hang indefinitely
    // ...
}

// ❌ INCORRECT - Using deprecated BOOST_TEST_TIMEOUT function
BOOST_AUTO_TEST_CASE(my_test_case) {
    BOOST_TEST_TIMEOUT(30);  // Deprecated approach
    // ...
}
```

**Timeout Guidelines for BOOST_AUTO_TEST_CASE**:

| Test Type | Recommended Timeout | Rationale |
|-----------|-------------------|-----------|
| Unit Tests | 10-30 seconds | Fast, focused tests |
| Integration Tests | 30-60 seconds | May involve I/O operations |
| Property Tests | 60-120 seconds | Many iterations with random data |
| Performance Tests | 120-300 seconds | Measurement and benchmarking |
| Network Tests | 60-180 seconds | Network operations can be slow |

**Best Practices**:

1. **Always use two-argument form**: Use `BOOST_AUTO_TEST_CASE(name, * boost::unit_test::timeout(seconds))`
2. **Be generous but reasonable**: Allow enough time for legitimate test execution
3. **Consider CI environment**: CI systems may be slower than development machines
4. **Document long timeouts**: Explain why tests need more than 60 seconds with comments
5. **Use consistent timeouts**: Similar test types should have similar timeouts

**Example with Different Test Types**:

```cpp
// Fast unit test
BOOST_AUTO_TEST_CASE(unit_test_example, * boost::unit_test::timeout(15)) {
    // Quick validation test
    BOOST_CHECK_EQUAL(add(2, 3), 5);
}

// Integration test with I/O
BOOST_AUTO_TEST_CASE(integration_test_example, * boost::unit_test::timeout(45)) {
    // Test that involves file I/O or network operations
    auto result = process_file("test_data.json");
    BOOST_CHECK(!result.empty());
}

// Property-based test
BOOST_AUTO_TEST_CASE(property_test_example, * boost::unit_test::timeout(90)) {
    // Property test with many iterations
    for (int i = 0; i < 1000; ++i) {
        auto input = generate_random_input();
        auto result = process(input);
        BOOST_CHECK(validate_property(input, result));
    }
}
```

### Timeout Guidelines

| Test Type | Recommended Timeout | Rationale |
|-----------|-------------------|-----------|
| Unit Tests | 10-30 seconds | Should be fast |
| Integration Tests | 60-120 seconds | May involve network/IO |
| Property Tests | 30-60 seconds | Many iterations |
| Performance Tests | 300+ seconds | Measurement takes time |
| Example Programs | 60-180 seconds | Comprehensive scenarios |

## CI/CD Integration

### Standard CI/CD Test Execution

```bash
# Build the project
cmake --build build

# Run all tests with proper reporting
cd build && ctest --output-on-failure -j$(nproc)

# Generate test report (if using CDash)
ctest -D Experimental
```

### Parallel Execution

```bash
# Use all available cores
ctest -j$(nproc)

# Limit parallel jobs (for resource-constrained environments)
ctest -j4
```

### Handling Test Failures

```bash
# Show output only for failed tests
ctest --output-on-failure

# Rerun only failed tests
ctest --rerun-failed

# Stop on first failure (for debugging)
ctest --stop-on-failure
```

## Debugging Tests

### When Direct Execution is Acceptable

Direct execution is ONLY acceptable for:

1. **Interactive Debugging**
   ```bash
   gdb ./build/tests/my_test
   lldb ./build/tests/my_test
   ```

2. **Profiling**
   ```bash
   valgrind ./build/tests/my_test
   perf record ./build/tests/my_test
   ```

3. **Manual Investigation**
   ```bash
   # When you need to see raw output without CTest formatting
   ./build/tests/my_test --log_level=trace
   ```

### Debugging Through CTest

Prefer debugging through CTest when possible:

```bash
# Run single test with verbose output
ctest -R my_test --verbose

# Run test and show all output
ctest -R my_test --output-on-failure --verbose

# Set environment for debugging
CTEST_OUTPUT_ON_FAILURE=1 ctest -R my_test
```

## Common Anti-Patterns

### ❌ Direct Execution in Scripts

```bash
# WRONG - Don't do this in scripts or documentation
./build/tests/network_test --config=test.json
./build/examples/coap_example --verbose
```

### ❌ Bypassing CTest for Arguments

```bash
# WRONG - Don't work around CTest
if [ "$VERBOSE" = "1" ]; then
    ./build/tests/my_test --verbose
else
    ./build/tests/my_test
fi
```

### ❌ Manual Test Discovery

```bash
# WRONG - Don't manually find and run tests
for test in build/tests/*_test; do
    if [ -x "$test" ]; then
        "$test"
    fi
done
```

## Correct Patterns

### ✅ Proper Script Integration

```bash
# CORRECT - Use CTest for all test execution
cmake --build build
cd build && ctest --output-on-failure -j$(nproc)
```

### ✅ Conditional Test Execution

```bash
# CORRECT - Use CTest labels and filters
if [ "$QUICK_TESTS_ONLY" = "1" ]; then
    ctest -L "unit" -j$(nproc)
else
    ctest --output-on-failure -j$(nproc)
fi
```

### ✅ Test Result Handling

```bash
# CORRECT - Check CTest exit code
if ! ctest --output-on-failure -j$(nproc); then
    echo "Tests failed!"
    exit 1
fi
```

## Enforcement

### Code Review Requirements

- All test modifications MUST use CTest
- Direct test execution in scripts MUST be justified
- New tests MUST be registered with `add_test`
- Test arguments MUST be configured in CMakeLists.txt
- All Boost.Test test cases MUST use two-argument `BOOST_AUTO_TEST_CASE` with timeout
- Test timeouts MUST be appropriate for the test type and complexity

### CI/CD Requirements

- All CI/CD pipelines MUST use CTest
- Test execution MUST use parallel execution (`-j$(nproc)`)
- Test failures MUST be reported with `--output-on-failure`

### Documentation Requirements

- All documentation MUST show CTest usage
- Direct execution examples MUST include disclaimer
- Test setup instructions MUST include CMakeLists.txt configuration

## Migration Guide

### Updating Existing Scripts

If you have scripts that directly execute tests:

1. **Identify direct test execution**
   ```bash
   # Find patterns like this
   ./build/tests/my_test
   ./build/examples/my_example
   ```

2. **Replace with CTest**
   ```bash
   # Replace with
   ctest -R my_test
   ctest -R my_example
   ```

3. **Configure arguments in CMakeLists.txt**
   ```cmake
   # Add any required arguments
   add_test(NAME my_test COMMAND my_test --required-arg)
   ```

### Updating Documentation

1. **Find direct execution examples**
2. **Replace with CTest equivalents**
3. **Add CMakeLists.txt configuration examples**
4. **Include proper timeout and label configuration**

## Summary

- **ALWAYS use CTest** for test execution
- **NEVER directly execute** test binaries in scripts or CI/CD
- **MODIFY CMakeLists.txt** to add test arguments
- **USE proper labels** to categorize tests
- **SET appropriate timeouts** for different test types
- **LEVERAGE parallel execution** for faster feedback

Following these standards ensures consistent, reliable, and maintainable test execution across all environments.