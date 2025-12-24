---
title: Example Program Guidelines
inclusion: always
---

# Example Program Guidelines

This document defines the standards and requirements for example programs in this project.

## Purpose of Examples

Example programs serve multiple purposes:
1. **Demonstrate API usage** - Show developers how to use the library correctly
2. **Validate functionality** - Verify that features work as documented
3. **Serve as integration tests** - Catch regressions in real-world usage patterns
4. **Provide starting points** - Give developers working code to build upon

## Requirements for Example Programs

### 1. Comprehensive Scenario Coverage

**Rule**: Examples MUST run ALL scenarios they are designed to demonstrate.

- ✅ **DO**: Run every test scenario from start to finish
- ❌ **DON'T**: Skip scenarios or exit early on first failure
- ✅ **DO**: Continue testing even after individual scenario failures
- ❌ **DON'T**: Abort the entire example on first error

**Example Structure**:
```cpp
int main() {
    int failed_scenarios = 0;
    
    // Run all scenarios
    if (!test_scenario_1()) failed_scenarios++;
    if (!test_scenario_2()) failed_scenarios++;
    if (!test_scenario_3()) failed_scenarios++;
    
    // Report results
    if (failed_scenarios > 0) {
        std::cerr << failed_scenarios << " scenario(s) failed\n";
        return 1;  // Non-zero exit code
    }
    
    std::cout << "All scenarios passed!\n";
    return 0;  // Success
}
```

### 2. Exit Code Requirements

**Rule**: Examples MUST return non-zero exit codes when ANY scenario fails.

- **Return 0**: All scenarios passed successfully
- **Return 1**: One or more scenarios failed
- **Return 2**: Fatal error (couldn't run scenarios)

This allows examples to be used in automated testing:
```bash
./example_program || echo "Example failed!"
```

### 3. Clear Success/Failure Indication

**Rule**: Examples MUST clearly indicate which scenarios passed and which failed.

Use visual indicators:
- ✅ or ✓ for passed scenarios
- ❌ or ✗ for failed scenarios
- Clear summary at the end

**Example Output**:
```
Test 1: Basic Functionality
  ✓ Scenario passed

Test 2: Error Handling
  ✗ Scenario failed: Unexpected exception

Test 3: Performance
  ✓ Scenario passed

Summary: 2/3 scenarios passed
Exit code: 1
```

### 4. Error Handling

**Rule**: Examples MUST handle errors gracefully and continue testing.

```cpp
void test_scenario() {
    try {
        // Test code
        std::cout << "  ✓ Scenario passed\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}
```

### 5. Self-Contained Execution

**Rule**: Examples MUST be self-contained and not depend on external state.

- ✅ **DO**: Set up all required resources in the example
- ❌ **DON'T**: Assume pre-existing files, services, or configuration
- ✅ **DO**: Clean up resources after each scenario
- ❌ **DON'T**: Leave the system in a modified state

### 6. Documentation

**Rule**: Examples MUST include comments explaining what they demonstrate.

```cpp
// Example: Demonstrating failure injection in distributed systems
// This example shows how to:
// 1. Configure network failures (drops, partitions, latency)
// 2. Test system behavior under adverse conditions
// 3. Verify recovery mechanisms

int main() {
    // ... implementation
}
```

### 7. Build System Integration

**Rule**: Example programs MUST be integrated into the build system.

All example programs must be:
- Added to CMakeLists.txt
- Buildable with the standard build command
- Organized in appropriate subdirectories
- Linked against required libraries

**Directory Structure**:
```
examples/
├── CMakeLists.txt                    # Main examples CMake file
├── network-test-fixture/
│   ├── basic_usage.cpp
│   ├── failure_injection.cpp
│   └── request_response_correlation.cpp
└── raft/
    ├── CMakeLists.txt                # Raft examples CMake file
    └── rpc_correlation_example.cpp
```

**CMakeLists.txt Template**:

```cmake
# Helper function for creating example executables
function(add_network_example example_name source_file output_dir)
    add_executable(${example_name} ${source_file})
    target_link_libraries(${example_name} PRIVATE
        network_fixture
        Folly::folly
    )
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    
    # Set output directory for the executable
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/${output_dir}"
    )
endfunction()

# Add example programs
add_network_example(basic_usage_example 
    network-test-fixture/basic_usage.cpp
    network-test-fixture)

add_network_example(failure_injection_example 
    network-test-fixture/failure_injection.cpp
    network-test-fixture)
```

**Requirements**:

1. **Executable Target**: Each example must be a separate executable
   ```cmake
   add_executable(example_name source_file.cpp)
   ```

2. **Library Linking**: Link all required libraries
   ```cmake
   target_link_libraries(example_name PRIVATE
       required_library
       Folly::folly
   )
   ```

3. **C++ Standard**: Specify C++23 standard
   ```cmake
   target_compile_features(example_name PRIVATE cxx_std_23)
   ```

4. **Output Directory**: Place executables in organized subdirectories
   ```cmake
   set_target_properties(example_name PROPERTIES
       RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/category"
   )
   ```

5. **Subdirectory Organization**: Use subdirectories for different components
   ```cmake
   # In examples/CMakeLists.txt
   add_subdirectory(raft)
   ```

**Building Examples**:

```bash
# Build all examples
cmake --build build

# Build specific example
cmake --build build --target example_name

# Run example
./build/examples/category/example_name
```

**CI/CD Integration**:

Examples should be built and run in CI/CD:

```bash
# Build all examples
cmake --build build --target all

# Run all examples and check exit codes
for example in build/examples/*/*; do
    if [ -x "$example" ]; then
        echo "Running $example..."
        if ! "$example"; then
            echo "Example $example failed"
            exit 1
        fi
    fi
done
```

**Benefits of Build System Integration**:

1. **Automatic Compilation** - Examples are built with the project
2. **Dependency Management** - CMake handles library linking
3. **Regression Detection** - Build failures catch API changes
4. **CI/CD Testing** - Examples run automatically in pipelines
5. **Documentation Validation** - Ensures examples stay up-to-date
6. **Easy Discovery** - Developers can find and run examples easily

## Example Program Checklist

Before committing an example program, verify:

- [ ] Runs all designed scenarios
- [ ] Returns 0 on complete success
- [ ] Returns non-zero if any scenario fails
- [ ] Clearly indicates pass/fail for each scenario
- [ ] Handles errors gracefully (doesn't crash)
- [ ] Continues testing after individual failures
- [ ] Provides summary of results
- [ ] Is self-contained (no external dependencies)
- [ ] Includes explanatory comments
- [ ] Cleans up resources properly
- [ ] Added to CMakeLists.txt
- [ ] Builds successfully with cmake
- [ ] Linked against required libraries
- [ ] Placed in appropriate output directory
- [ ] Uses named constants for literals

## Testing Examples

### Using CTest for Examples

**Rule**: Examples SHOULD be executed using `ctest` when possible. Direct execution should only be used as a last resort.

**Rationale**:
- CTest provides consistent execution and reporting
- Enables parallel execution of multiple examples
- Integrates with CI/CD pipelines seamlessly
- Provides timeout management and failure handling
- Standardizes output formatting

**Preferred Approach - Register Examples with CTest**:

In CMakeLists.txt:
```cmake
# Register example as a test
add_test(NAME example_name_test COMMAND example_name)
set_tests_properties(example_name_test PROPERTIES
    TIMEOUT 60
    LABELS "example;integration"
)
```

**Running Examples via CTest**:

```bash
# ✅ CORRECT - Run all examples through CTest
cd build
ctest -L example

# ✅ CORRECT - Run examples in parallel
ctest -L example -j$(nproc)

# ✅ CORRECT - Run specific example
ctest -R example_name_test --verbose
```

### Examples Using Boost.Test

**Rule**: If example programs use Boost.Test framework, they MUST include `BOOST_TEST_TIMEOUT`.

**Rationale**:
- Example programs may demonstrate complex scenarios that could hang
- Timeout prevents CI/CD pipelines from hanging on problematic examples
- Provides consistent behavior with other test code in the project

**Implementation**:

```cpp
// ✅ CORRECT - Example with Boost.Test and timeout
BOOST_AUTO_TEST_CASE(example_network_communication, * boost::unit_test::timeout(120)) {
    // Demonstrate network communication - 120 second timeout for examples
    auto client = create_client();
    auto server = create_server();
    
    // Run example scenarios
    demonstrate_basic_communication(client, server);
    demonstrate_error_handling(client, server);
    demonstrate_performance_features(client, server);
}

// ❌ INCORRECT - Missing timeout in example
BOOST_AUTO_TEST_CASE(example_without_timeout) {
    // Missing timeout - could hang during demonstration
    demonstrate_complex_scenario();
}

// ❌ INCORRECT - Using deprecated BOOST_TEST_TIMEOUT function
BOOST_AUTO_TEST_CASE(example_with_deprecated_timeout) {
    BOOST_TEST_TIMEOUT(120);  // Deprecated approach
    demonstrate_complex_scenario();
}
```

**Timeout Guidelines for Examples**:
- **Simple demonstrations**: 60-120 seconds
- **Complex integration scenarios**: 120-300 seconds  
- **Performance demonstrations**: 300+ seconds (with justification)

**IMPORTANT: Examples with Additional Arguments**:

**Rule**: When an example program needs to be run with additional command-line arguments, you MUST modify the `add_test` call in CMakeLists.txt to include those arguments, then run the example via `ctest`.

**❌ NEVER DO THIS**:
```bash
# Wrong - bypasses CTest infrastructure
./build/examples/category/my_example --verbose --config=example.json
```

**✅ ALWAYS DO THIS**:
1. First, modify CMakeLists.txt:
```cmake
# Add arguments to the example test command
add_test(NAME my_example_test COMMAND my_example --verbose --config=example.json)
set_tests_properties(my_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;integration"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/examples/category
)
```

2. Then run via CTest:
```bash
# Correct - uses CTest with configured arguments
ctest -R my_example_test --verbose
```

**Examples of Proper Example Test Configuration**:

```cmake
# Example with configuration file
add_test(NAME network_example_test COMMAND network_example --config=${CMAKE_SOURCE_DIR}/examples/config.json)

# Example with multiple arguments
add_test(NAME performance_example_test COMMAND performance_example --threads=4 --duration=30s)

# Example with environment variables
add_test(NAME integration_example_test COMMAND integration_example --verbose)
set_tests_properties(integration_example_test PROPERTIES
    ENVIRONMENT "EXAMPLE_DATA_DIR=${CMAKE_SOURCE_DIR}/examples/data"
    TIMEOUT 180
    LABELS "example;integration;slow"
)
```

**Direct Execution (Last Resort)**:

```bash
# ❌ AVOID - Direct execution bypasses CTest benefits
./build/examples/category/example_name

# ⚠️ ACCEPTABLE FOR MANUAL TESTING - When exploring output interactively
./build/examples/category/example_name
```

**CI/CD Integration**:

```bash
# ✅ CORRECT - Build and test examples with CTest
cmake --build build
cd build && ctest -L example --output-on-failure -j$(nproc)

# ❌ AVOID - Manual loop over executables
for example in build/examples/*/*; do
    if [ -x "$example" ]; then
        echo "Running $example..."
        if ! "$example"; then
            echo "Example $example failed"
            exit 1
        fi
    fi
done
```

**Benefits of CTest for Examples**:
1. **Consistency** - Same execution method as unit tests
2. **Reporting** - Standardized pass/fail reporting
3. **Timeout Protection** - Prevents hanging examples
4. **Parallel Execution** - Faster validation
5. **Filtering** - Easy to run specific examples or categories

## Anti-Patterns to Avoid

### ❌ Exiting on First Failure
```cpp
// BAD: Stops at first failure
if (!test1()) return 1;  // Never runs test2 or test3
if (!test2()) return 1;
if (!test3()) return 1;
```

### ❌ Ignoring Failures
```cpp
// BAD: Always returns success
test1();
test2();
test3();
return 0;  // Even if tests failed!
```

### ❌ Unclear Output
```cpp
// BAD: No indication of what passed/failed
test1();
test2();
test3();
std::cout << "Done\n";  // Did it work?
```

## Best Practices

### ✅ Track All Results
```cpp
struct TestResults {
    int passed = 0;
    int failed = 0;
    
    void record(bool success) {
        if (success) passed++;
        else failed++;
    }
    
    int exit_code() const {
        return failed > 0 ? 1 : 0;
    }
};
```

### ✅ Descriptive Output
```cpp
void print_separator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}
```

### ✅ Exception Safety
```cpp
bool run_scenario(const std::string& name, std::function<void()> test) {
    std::cout << "Running: " << name << "\n";
    try {
        test();
        std::cout << "  ✓ Passed\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Failed: " << e.what() << "\n";
        return false;
    }
}
```

### ✅ Use Named Constants

**Rule**: Define string and numeric literals as named constants.

```cpp
// ✅ CORRECT - Named constants
namespace {
    constexpr const char* test_node_a = "node_a";
    constexpr const char* test_node_b = "node_b";
    constexpr std::size_t test_message_count = 100;
    constexpr std::chrono::milliseconds test_timeout{5000};
    constexpr const char* test_payload = "Test message";
}

auto test_message_delivery() -> bool {
    auto node_a = fixture.create(test_node_a);
    auto node_b = fixture.create(test_node_b);
    
    for (std::size_t i = 0; i < test_message_count; ++i) {
        Message msg(
            std::format("msg_{}", i),
            test_node_a,
            test_node_b,
            test_payload
        );
        fixture.send(test_node_a, test_node_b, std::move(msg));
    }
    
    return true;
}

// ❌ INCORRECT - Magic numbers and repeated literals
auto test_message_delivery() -> bool {
    auto node_a = fixture.create("node_a");  // Repeated literal
    auto node_b = fixture.create("node_b");  // Repeated literal
    
    for (std::size_t i = 0; i < 100; ++i) {  // Magic number
        Message msg(
            std::format("msg_{}", i),
            "node_a",        // Repeated literal (typo risk)
            "node_b",        // Repeated literal (typo risk)
            "Test message"   // Repeated literal
        );
        fixture.send("node_a", "node_b", std::move(msg));
    }
    
    return true;
}
```

**Benefits:**
- Self-documenting code
- Easy to modify values in one place
- Reduces typo errors
- Consistent values across test scenarios
- Improves maintainability

## Rationale

These guidelines ensure that:

1. **Examples are reliable** - They catch regressions and validate functionality
2. **Failures are visible** - Developers immediately see what's broken
3. **CI/CD integration works** - Automated systems can detect failures
4. **Examples are useful** - They demonstrate real-world usage patterns
5. **Debugging is easier** - Clear output helps identify issues quickly

## Enforcement

- Code reviews MUST verify examples follow these guidelines
- CI/CD MUST run examples and check exit codes
- Examples that don't follow guidelines MUST be fixed before merging

## See Also

- [Testing Guidelines](../../tests/network-test-fixture/README.md)
- [API Documentation](../../doc/network-test-fixture/API.md)
- [Example Programs](../../examples/network-test-fixture/README.md)
