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
add_network_example(custom_types_example custom_types_example.cpp)

# Register network simulator examples as tests
add_test(NAME basic_connectionless_example_test COMMAND basic_connectionless_example)
set_tests_properties(basic_connectionless_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;network-simulator;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME connection_oriented_example_test COMMAND connection_oriented_example)
set_tests_properties(connection_oriented_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;network-simulator;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME network_topology_example_test COMMAND network_topology_example)
set_tests_properties(network_topology_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;network-simulator;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME custom_types_example_test COMMAND custom_types_example)
set_tests_properties(custom_types_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;network-simulator;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

# Generic future architecture examples
add_network_example(generic_future_example generic_future_example.cpp)
add_network_example(generic_future_transport_example generic_future_transport_example.cpp)
add_network_example(migration_guide_example migration_guide_example.cpp)

# Register generic future examples as tests
add_test(NAME generic_future_example_test COMMAND generic_future_example)
set_tests_properties(generic_future_example_test PROPERTIES
    TIMEOUT 60
    LABELS "example;future;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME generic_future_transport_example_test COMMAND generic_future_transport_example)
set_tests_properties(generic_future_transport_example_test PROPERTIES
    TIMEOUT 120
    LABELS "example;future;transport;integration"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

add_test(NAME migration_guide_example_test COMMAND migration_guide_example)
set_tests_properties(migration_guide_example_test PROPERTIES
    TIMEOUT 60
    LABELS "example;migration;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

# stdexec-future-backend spec (.kiro/specs/stdexec-future-backend/, Task 32):
# Folly-vs-stdexec backend comparison. network_simulator already links
# STDEXEC::stdexec/TBB::tbb and defines KYTHIRA_HAS_STDEXEC when
# stdexec_FOUND (root CMakeLists.txt), so add_network_example needs no
# extra wiring — only gating the registration itself so this target simply
# doesn't exist when the optional stdexec dependency isn't available.
if(stdexec_FOUND)
    add_network_example(stdexec_backend_migration_guide_example stdexec-backend/migration_guide_example.cpp)
    add_test(NAME stdexec_backend_migration_guide_example_test COMMAND stdexec_backend_migration_guide_example)
    set_tests_properties(stdexec_backend_migration_guide_example_test PROPERTIES
        TIMEOUT 60
        LABELS "example;migration;stdexec;future-backend;unit"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )
endif()

# future-backend-performance-benchmark spec: standalone Folly-vs-stdexec
# comparison report generator. Unlike stdexec_backend_migration_guide_example
# above, this target is NOT gated behind stdexec_FOUND — it must build and
# run (Folly-only) even when stdexec is unavailable (Requirement 1.4,
# Property 6); add_network_example already links network_simulator, which
# conditionally links STDEXEC::stdexec/TBB::tbb and defines
# KYTHIRA_HAS_STDEXEC only when stdexec_FOUND, so no extra wiring is
# needed here either way. Report-quality runs (full iteration counts) are
# not CTest-registered (Requirement 5) — run manually, e.g.
# ./build/examples/future_backend_benchmark_report. The reduced-iteration
# CTest entry below is a smoke test only (catches a build/runtime
# regression in the report binary itself — CLI parsing, table printing,
# CSV/JSON writing), mirroring performance_benchmark_report_test below;
# neither contributes to the coverage ratchet either way, since
# scripts/pre-commit-coverage.sh's llvm-cov invocation excludes all of
# examples/ from line-coverage measurement.
add_network_example(future_backend_benchmark_report future-backend-benchmark/benchmark_report.cpp)

add_test(NAME future_backend_benchmark_report_test
    COMMAND future_backend_benchmark_report --iterations 300 --warmup 20)
set_tests_properties(future_backend_benchmark_report_test PROPERTIES
    TIMEOUT 60
    LABELS "example;performance;benchmark;future-backend"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

# Performance benchmark report
add_network_example(performance_benchmark_report performance_benchmark_report.cpp)

# Register performance benchmark as test
add_test(NAME performance_benchmark_report_test COMMAND performance_benchmark_report)
set_tests_properties(performance_benchmark_report_test PROPERTIES
    TIMEOUT 180
    LABELS "example;performance;benchmark"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

# Concepts usage examples
add_network_example(concepts_usage_examples concepts_usage_examples.cpp)

# Register concepts usage example as test
add_test(NAME concepts_usage_examples_test COMMAND concepts_usage_examples)
set_tests_properties(concepts_usage_examples_test PROPERTIES
    TIMEOUT 60
    LABELS "example;concepts;unit"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)

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
