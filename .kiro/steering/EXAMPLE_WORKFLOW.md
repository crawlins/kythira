---
title: Example Program Workflow
inclusion: manual
---

# Example Program Workflow

Complete workflow for creating and integrating example programs.

## Step-by-Step Guide

### Step 1: Create the Example Source File

Create your example in the appropriate directory:

```bash
# For network fixture examples
examples/network-test-fixture/my_feature_example.cpp

# For raft examples
examples/raft/my_raft_example.cpp
```

### Step 2: Write the Example Code

Follow the example program guidelines:

```cpp
// Example: Demonstrating My Feature
// This example shows how to:
// 1. Set up the feature
// 2. Use the feature in common scenarios
// 3. Handle errors properly

#include <network_fixture/local/LocalNetworkFixture.hpp>
#include <iostream>

using namespace network_fixture;

// Define constants
namespace {
    constexpr const char* node_a_id = "node_a";
    constexpr const char* node_b_id = "node_b";
    constexpr std::size_t test_iterations = 10;
}

auto print_separator(const std::string& title) -> void {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n\n";
}

auto test_scenario_1() -> bool {
    print_separator("Test 1: Basic Feature Usage");
    
    try {
        LocalNetworkFixture fixture;
        fixture.start();
        
        auto node_a = fixture.create(node_a_id);
        auto node_b = fixture.create(node_b_id);
        
        // Test logic here
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

auto test_scenario_2() -> bool {
    print_separator("Test 2: Advanced Feature Usage");
    
    try {
        // Test logic here
        
        std::cout << "  ✓ Scenario passed\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  ✗ Scenario failed: " << e.what() << "\n";
        return false;
    }
}

int main() {
    std::cout << "My Feature Example\n";
    std::cout << "==================\n";
    
    int failed_scenarios = 0;
    
    // Run all scenarios
    if (!test_scenario_1()) failed_scenarios++;
    if (!test_scenario_2()) failed_scenarios++;
    
    // Print summary
    print_separator("Summary");
    
    if (failed_scenarios > 0) {
        std::cerr << "❌ " << failed_scenarios << " scenario(s) failed\n";
        return 1;
    }
    
    std::cout << "✅ All scenarios passed!\n";
    std::cout << "\nKey Features Demonstrated:\n";
    std::cout << "  • Feature capability 1\n";
    std::cout << "  • Feature capability 2\n";
    
    return 0;
}
```

### Step 3: Add to CMakeLists.txt

Add your example to the appropriate CMakeLists.txt file:

**For network-test-fixture examples** (`examples/CMakeLists.txt`):

```cmake
# Add after existing examples
add_network_example(my_feature_example 
    network-test-fixture/my_feature_example.cpp
    network-test-fixture)
```

**For raft examples** (`examples/raft/CMakeLists.txt`):

```cmake
add_executable(my_raft_example my_raft_example.cpp)
target_link_libraries(my_raft_example PRIVATE 
    raft
    network_fixture
    Folly::folly
)
target_compile_features(my_raft_example PRIVATE cxx_std_23)
set_target_properties(my_raft_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
)
```

### Step 4: Build the Example

```bash
# Reconfigure CMake (if needed)
cmake -S . -B build

# Build the specific example
cmake --build build --target my_feature_example

# Or build all examples
cmake --build build
```

### Step 5: Test the Example

```bash
# Run the example
./build/examples/network-test-fixture/my_feature_example

# Verify exit code
echo $?  # Should be 0 on success, non-zero on failure
```

### Step 6: Verify Checklist

Before committing, verify:

- [x] Example runs all scenarios
- [x] Returns correct exit codes (0 = success, 1 = failure)
- [x] Clear pass/fail indication with ✓ and ✗
- [x] Handles errors gracefully
- [x] Continues after individual failures
- [x] Provides summary at end
- [x] Self-contained (no external dependencies)
- [x] Includes explanatory comments
- [x] Cleans up resources
- [x] Uses named constants
- [x] Added to CMakeLists.txt
- [x] Builds successfully
- [x] Linked against required libraries
- [x] Placed in correct output directory

### Step 7: Document the Example

Add documentation if needed:

```bash
# Create documentation file
doc/examples/my-feature-example.md
```

Include:
- Purpose of the example
- What it demonstrates
- How to run it
- Expected output
- Key concepts illustrated

## Common Patterns

### Helper Function in CMakeLists.txt

If you have many examples in a category, create a helper function:

```cmake
# In examples/CMakeLists.txt
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

# Use the helper
add_network_example(example1 network-test-fixture/example1.cpp network-test-fixture)
add_network_example(example2 network-test-fixture/example2.cpp network-test-fixture)
add_network_example(example3 network-test-fixture/example3.cpp network-test-fixture)
```

### Shared Constants Across Examples

For constants used in multiple examples:

```cpp
// In include/network_fixture/examples/common_constants.hpp
#pragma once

namespace network_fixture::examples {
    inline constexpr const char* default_client_id = "client";
    inline constexpr const char* default_server_id = "server";
    inline constexpr std::size_t default_test_iterations = 100;
}
```

### CI/CD Integration Script

Create a script to run all examples:

```bash
#!/bin/bash
# scripts/run_examples.sh

set -e

echo "Running all example programs..."

failed=0
total=0

for example in build/examples/*/*; do
    if [ -x "$example" ]; then
        total=$((total + 1))
        echo ""
        echo "=========================================="
        echo "Running: $(basename $example)"
        echo "=========================================="
        
        if ! "$example"; then
            echo "FAILED: $example"
            failed=$((failed + 1))
        else
            echo "PASSED: $example"
        fi
    fi
done

echo ""
echo "=========================================="
echo "Summary: $((total - failed))/$total examples passed"
echo "=========================================="

if [ $failed -gt 0 ]; then
    exit 1
fi
```

## Troubleshooting

### Example Doesn't Build

**Problem**: CMake can't find the example target

**Solution**: 
1. Verify the example is added to CMakeLists.txt
2. Reconfigure CMake: `cmake -S . -B build`
3. Check for typos in target name

### Linking Errors

**Problem**: Undefined references during linking

**Solution**:
1. Verify all required libraries are linked in `target_link_libraries`
2. Check library dependencies are built
3. Ensure correct library names (e.g., `network_fixture`, not `network-fixture`)

### Example Not in Output Directory

**Problem**: Can't find executable after building

**Solution**:
1. Check `RUNTIME_OUTPUT_DIRECTORY` is set correctly
2. Look in `build/examples/category/` directory
3. Verify the example built successfully (check build output)

### Example Fails in CI/CD

**Problem**: Example passes locally but fails in CI/CD

**Solution**:
1. Ensure example is self-contained (no external dependencies)
2. Check for hardcoded paths or assumptions
3. Verify all resources are created/cleaned up properly
4. Test in a clean build environment

## Best Practices Summary

1. **One Example, One Feature** - Focus each example on a specific feature
2. **Multiple Scenarios** - Show different use cases within one example
3. **Clear Output** - Use visual indicators and descriptive messages
4. **Named Constants** - Define all literals as constants
5. **Error Handling** - Catch and report errors gracefully
6. **Build Integration** - Always add to CMakeLists.txt
7. **Documentation** - Include comments explaining what's demonstrated
8. **Self-Contained** - Don't depend on external state or files
9. **Exit Codes** - Return 0 on success, non-zero on failure
10. **Summary** - Provide clear summary of results at the end

## See Also

- [Example Program Guidelines](example-programs.md)
- [C++ Coding Standards](cpp-coding-standards.md)
- [Quick Reference](QUICK_REFERENCE.md)
- [Changelog](CHANGELOG.md)
