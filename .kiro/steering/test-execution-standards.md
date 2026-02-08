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

## Efficient Test Execution with Output Storage

### The Golden Rule for Large Test Suites

**When running a large group of tests (more than 10 tests), ALWAYS store the output in a file first, then analyze the stored output. NEVER run the same test suite multiple times without making changes.**

### Rationale

1. **Time Efficiency** - Large test suites can take minutes to hours to run
2. **Resource Conservation** - Avoid unnecessary CPU and memory usage
3. **Consistency** - Analyze the same test run results multiple times
4. **Debugging** - Preserve test output for later investigation
5. **CI/CD Optimization** - Reduce pipeline execution time

### Correct Pattern: Store and Analyze

**✅ CORRECT - Store output once, analyze multiple times:**

```bash
# Step 1: Run tests ONCE and store output
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee test_results.txt

# Step 2: Analyze the stored output as needed
grep "Test.*Failed" test_results.txt
grep "Test.*Passed" test_results.txt
tail -50 test_results.txt
head -100 test_results.txt

# Step 3: Extract specific information
grep -E "Test #[0-9]+.*Failed" test_results.txt | wc -l  # Count failures
grep -E "Test #[0-9]+.*Passed" test_results.txt | wc -l  # Count passes
```

**❌ INCORRECT - Running tests multiple times:**

```bash
# WRONG - Running tests multiple times to get different views
ctest --test-dir build 2>&1 | head -100
ctest --test-dir build 2>&1 | tail -50
ctest --test-dir build 2>&1 | grep "Failed"
# This runs the entire test suite 3 times unnecessarily!
```

### Recommended Workflow

#### 1. Initial Test Run with Storage

```bash
# Create a timestamped test results file
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TEST_OUTPUT="test_results_${TIMESTAMP}.txt"

# Run tests and store output
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee "${TEST_OUTPUT}"

echo "Test results stored in: ${TEST_OUTPUT}"
```

#### 2. Analyze Stored Results

```bash
# Get test summary
tail -20 "${TEST_OUTPUT}"

# Count test results
echo "Total tests: $(grep -c "Test #" "${TEST_OUTPUT}")"
echo "Passed: $(grep -c "Passed" "${TEST_OUTPUT}")"
echo "Failed: $(grep -c "Failed" "${TEST_OUTPUT}")"

# List failed tests
echo "Failed tests:"
grep "Test.*Failed" "${TEST_OUTPUT}"

# Extract specific test output
grep -A 50 "test_name" "${TEST_OUTPUT}"
```

#### 3. Generate Test Report

```bash
# Create a summary report from stored output
cat > test_summary.md << EOF
# Test Execution Summary

**Date**: $(date)
**Test Output File**: ${TEST_OUTPUT}

## Results

$(tail -20 "${TEST_OUTPUT}")

## Failed Tests

$(grep "Test.*Failed" "${TEST_OUTPUT}" || echo "No failures")

## Statistics

- Total Tests: $(grep -c "Test #" "${TEST_OUTPUT}")
- Passed: $(grep -c "Passed" "${TEST_OUTPUT}")
- Failed: $(grep -c "Failed" "${TEST_OUTPUT}")
EOF

echo "Summary report created: test_summary.md"
```

### Advanced Analysis Patterns

#### Extract Test Timing Information

```bash
# Find slowest tests
grep "sec$" "${TEST_OUTPUT}" | sort -k4 -n -r | head -20
```

#### Filter by Test Category

```bash
# Analyze only Raft tests
grep "raft_.*test" "${TEST_OUTPUT}"

# Analyze only property tests
grep "property_test" "${TEST_OUTPUT}"
```

#### Compare Test Runs

```bash
# Store multiple test runs
ctest --test-dir build 2>&1 | tee test_run_1.txt
# Make changes...
ctest --test-dir build 2>&1 | tee test_run_2.txt

# Compare results
diff test_run_1.txt test_run_2.txt
```

### Integration with Development Workflow

#### Before Making Changes

```bash
# Establish baseline
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee baseline_tests.txt
```

#### After Making Changes

```bash
# Run tests again and compare
ctest --test-dir build --output-on-failure -j$(nproc) 2>&1 | tee after_changes_tests.txt

# Quick comparison
diff <(grep "Test.*Failed" baseline_tests.txt) <(grep "Test.*Failed" after_changes_tests.txt)
```

#### Focused Re-testing

```bash
# Only re-run tests that failed in the stored output
FAILED_TESTS=$(grep "Test.*Failed" "${TEST_OUTPUT}" | sed 's/.*Test #\([0-9]*\).*/\1/')
for test_num in $FAILED_TESTS; do
    ctest --test-dir build -I $test_num,$test_num --output-on-failure
done
```

### Best Practices

1. **Always use `tee`** - Allows viewing output while storing it
   ```bash
   ctest ... 2>&1 | tee output.txt
   ```

2. **Use timestamped filenames** - Prevents overwriting previous results
   ```bash
   TEST_OUTPUT="test_results_$(date +%Y%m%d_%H%M%S).txt"
   ```

3. **Store both stdout and stderr** - Use `2>&1` to capture all output
   ```bash
   ctest ... 2>&1 | tee output.txt
   ```

4. **Document test runs** - Add metadata to stored output
   ```bash
   {
       echo "=== Test Run Metadata ==="
       echo "Date: $(date)"
       echo "Git Commit: $(git rev-parse HEAD)"
       echo "Git Branch: $(git branch --show-current)"
       echo "========================"
       ctest --test-dir build --output-on-failure -j$(nproc)
   } 2>&1 | tee test_results.txt
   ```

5. **Clean up old test outputs** - Prevent disk space issues
   ```bash
   # Keep only last 10 test result files
   ls -t test_results_*.txt | tail -n +11 | xargs rm -f
   ```

### Anti-Patterns to Avoid

#### ❌ Running Tests Multiple Times for Different Views

```bash
# WRONG - Wastes time and resources
ctest ... | head -100    # First run
ctest ... | tail -50     # Second run (unnecessary!)
ctest ... | grep Failed  # Third run (unnecessary!)
```

#### ❌ Not Storing Output Before Analysis

```bash
# WRONG - Can't analyze later
ctest ... | grep "something"
# If you need to check something else, you have to run tests again!
```

#### ❌ Overwriting Previous Results

```bash
# WRONG - Loses historical data
ctest ... > test_results.txt  # Overwrites every time
```

### Correct Patterns

#### ✅ Store Once, Analyze Many Times

```bash
# CORRECT - Single test run, multiple analyses
ctest ... 2>&1 | tee test_results.txt

# Now analyze as much as needed
grep "Failed" test_results.txt
grep "Passed" test_results.txt
tail -50 test_results.txt
head -100 test_results.txt
# No additional test runs needed!
```

#### ✅ Preserve Test History

```bash
# CORRECT - Keep historical results
ctest ... 2>&1 | tee "test_results_$(date +%Y%m%d_%H%M%S).txt"
```

#### ✅ Efficient Re-testing

```bash
# CORRECT - Only re-run what's needed
ctest ... 2>&1 | tee full_results.txt

# Identify failures
FAILED=$(grep "Test.*Failed" full_results.txt)

# Re-run only failed tests
ctest --rerun-failed --output-on-failure
```

## Summary

- **ALWAYS use CTest** for test execution
- **NEVER directly execute** test binaries in scripts or CI/CD
- **MODIFY CMakeLists.txt** to add test arguments
- **USE proper labels** to categorize tests
- **SET appropriate timeouts** for different test types
- **LEVERAGE parallel execution** for faster feedback
- **STORE test output** when running large test suites
- **ANALYZE stored output** instead of re-running tests
- **USE timestamped filenames** to preserve test history

Following these standards ensures consistent, reliable, and maintainable test execution across all environments.