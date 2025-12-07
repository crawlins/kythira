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

# Add Raft examples subdirectory
add_subdirectory(raft)
