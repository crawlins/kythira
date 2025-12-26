# Helper function for creating example executables
function(add_network_example example_name source_file)
    add_executable(${example_name} ${source_file})
    target_link_libraries(${example_name} PRIVATE
        network_simulator
    )
    if(TARGET Folly::folly)
        target_link_libraries(${example_name} PRIVATE Folly::folly)
    endif()
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    
    # Set output directory for the executable
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )
endfunction()

# Example programs
add_network_example(basic_connectionless_example basic_connectionless.cpp)
add_network_example(connection_oriented_example connection_oriented.cpp)
add_network_example(network_topology_example network_topology.cpp)

# Generic future architecture examples
add_network_example(generic_future_example generic_future_example.cpp)
add_network_example(generic_future_transport_example generic_future_transport_example.cpp)
add_network_example(generic_future_network_simulator_example generic_future_network_simulator_example.cpp)
add_network_example(migration_guide_example migration_guide_example.cpp)

# Performance benchmark report
add_network_example(performance_benchmark_report performance_benchmark_report.cpp)

# Concepts usage examples
add_network_example(concepts_usage_examples concepts_usage_examples.cpp)

# Helper function for creating folly wrapper example executables
function(add_folly_wrapper_example example_name source_file)
    add_executable(${example_name} ${source_file})
    target_link_libraries(${example_name} PRIVATE
        network_simulator
    )
    if(TARGET Folly::folly)
        target_link_libraries(${example_name} PRIVATE Folly::folly)
    endif()
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    
    # Add compiler flags for better debugging and optimization
    target_compile_options(${example_name} PRIVATE
        $<$<CONFIG:Debug>:-g -O0 -Wall -Wextra>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    )
    
    # Set output directory for the executable
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/folly-wrappers"
    )
endfunction()

# Folly concept wrapper examples
add_folly_wrapper_example(promise_example folly-wrappers/promise_example.cpp)
add_folly_wrapper_example(executor_example folly-wrappers/executor_example.cpp)
add_folly_wrapper_example(factory_example folly-wrappers/factory_example.cpp)
add_folly_wrapper_example(collector_example folly-wrappers/collector_example.cpp)
add_folly_wrapper_example(continuation_example folly-wrappers/continuation_example.cpp)

# Register folly wrapper examples as tests
add_test(NAME promise_example_test COMMAND promise_example)
set_tests_properties(promise_example_test PROPERTIES
    TIMEOUT 60
    LABELS "example;folly-wrappers;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME executor_example_test COMMAND executor_example)
set_tests_properties(executor_example_test PROPERTIES
    TIMEOUT 60
    LABELS "example;folly-wrappers;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME factory_example_test COMMAND factory_example)
set_tests_properties(factory_example_test PROPERTIES
    TIMEOUT 60
    LABELS "example;folly-wrappers;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME collector_example_test COMMAND collector_example)
set_tests_properties(collector_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;folly-wrappers;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME continuation_example_test COMMAND continuation_example)
set_tests_properties(continuation_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;folly-wrappers;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

# Add Raft examples subdirectory
add_subdirectory(raft)
