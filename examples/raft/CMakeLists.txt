# Raft examples

# Helper function for creating Raft example executables
function(add_raft_example example_name source_file)
    add_executable(${example_name} ${source_file})
    target_link_libraries(${example_name} PRIVATE
        network_simulator
        boost_json
    )
    if(TARGET Folly::folly)
        target_link_libraries(${example_name} PRIVATE Folly::folly)
    endif()
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    
    # Set output directory for the executable
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
    )
    
    # Register as a test so it can be run with ctest
    add_test(NAME ${example_name} COMMAND ${example_name})
    set_tests_properties(${example_name} PROPERTIES
        LABELS "example;raft"
        TIMEOUT 60
    )
endfunction()

# Add Raft example programs
add_raft_example(basic_cluster basic_cluster.cpp)
add_raft_example(failure_scenarios failure_scenarios.cpp)
add_raft_example(membership_changes membership_changes.cpp)
add_raft_example(snapshot_example snapshot_example.cpp)
