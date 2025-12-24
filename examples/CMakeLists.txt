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

# Add Raft examples subdirectory
add_subdirectory(raft)
