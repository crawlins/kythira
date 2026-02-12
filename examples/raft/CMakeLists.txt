# Raft examples

# This CMakeLists.txt builds Raft example programs that demonstrate various
# aspects of the Raft consensus algorithm implementation, including:
# - Basic Raft operations (leader election, log replication, membership changes)
# - Transport layer implementations (HTTP, CoAP with DTLS)
# - Completion components (commit waiting, async operations, configuration sync)
# - Error handling and recovery mechanisms
#
# All examples are registered as CTest tests and can be run with:
#   ctest -L "example;raft"
#
# Completion examples demonstrate the async coordination layer:
#   ctest -L "completion"

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
    # Add HTTP transport dependencies for http_transport_example
    if(${example_name} STREQUAL "http_transport_example")
        if(TARGET httplib::httplib)
            target_link_libraries(${example_name} PRIVATE httplib::httplib)
        endif()
        if(TARGET OpenSSL::SSL)
            target_link_libraries(${example_name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()
    
    # Add CoAP transport dependencies for CoAP examples
    if(${example_name} MATCHES "coap_.*_example")
        # Link libcoap if available
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBCOAP QUIET libcoap-3)
            if(LIBCOAP_FOUND)
                target_link_libraries(${example_name} PRIVATE ${LIBCOAP_LIBRARIES})
                target_include_directories(${example_name} PRIVATE ${LIBCOAP_INCLUDE_DIRS})
                target_compile_definitions(${example_name} PRIVATE ${LIBCOAP_CFLAGS_OTHER})
            endif()
        endif()
        
        # Link OpenSSL for DTLS support
        if(TARGET OpenSSL::SSL)
            target_link_libraries(${example_name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()
    target_compile_features(${example_name} PRIVATE cxx_std_23)
    
    # Set output directory for the executable
    set_target_properties(${example_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
    )
    
    # Register as a test so it can be run with ctest
    add_test(NAME ${example_name}_test COMMAND ${example_name})
    
    # Set appropriate timeout and labels based on example type
    if(${example_name} MATCHES ".*_completion_.*|async_operations_.*|commit_waiting_.*|configuration_sync_.*|error_handling_.*")
        # Completion examples may take longer due to async operations and timeouts
        set_tests_properties(${example_name}_test PROPERTIES
            LABELS "example;raft;completion;integration"
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
        )
    elseif(${example_name} MATCHES "coap_.*_example")
        # CoAP examples may involve network operations
        set_tests_properties(${example_name}_test PROPERTIES
            LABELS "example;raft;coap;integration"
            TIMEOUT 90
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
        )
    elseif(${example_name} MATCHES ".*performance.*")
        # Performance examples may take longer
        set_tests_properties(${example_name}_test PROPERTIES
            LABELS "example;raft;performance;slow"
            TIMEOUT 180
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
        )
    else()
        # Standard Raft examples
        set_tests_properties(${example_name}_test PROPERTIES
            LABELS "example;raft;unit"
            TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/examples/raft"
        )
    endif()
endfunction()

# Basic Raft examples - core consensus operations
add_raft_example(basic_cluster basic_cluster.cpp)
add_raft_example(failure_scenarios failure_scenarios.cpp)
add_raft_example(membership_changes membership_changes.cpp)
add_raft_example(snapshot_example snapshot_example.cpp)

# Transport layer examples
add_raft_example(http_transport_example http_transport_example.cpp)

# CoAP transport examples - demonstrate CoAP/DTLS integration
add_raft_example(coap_transport_basic_example coap_transport_basic_example.cpp)
add_raft_example(coap_dtls_security_example coap_dtls_security_example.cpp)
add_raft_example(coap_multicast_example coap_multicast_example.cpp)
add_raft_example(coap_block_transfer_example coap_block_transfer_example.cpp)
add_raft_example(coap_performance_validation_example coap_performance_validation_example.cpp)
add_raft_example(coap_final_integration_validation coap_final_integration_validation.cpp)

# Raft completion examples - demonstrate async coordination and error handling
add_raft_example(async_operations_example async_operations_example.cpp)
add_raft_example(commit_waiting_example commit_waiting_example.cpp)
add_raft_example(configuration_sync_example configuration_sync_example.cpp)
add_raft_example(error_handling_example error_handling_example.cpp)

# State machine examples - demonstrate state machine patterns
add_raft_example(state_machine_example state_machine_example.cpp)
add_raft_example(state_machine_migration_example state_machine_migration_example.cpp)

# Additional CTest targets for completion examples
# These can be run separately to validate completion functionality:
#
# Run all completion examples:
#   ctest -L "completion"
#
# Run specific completion examples:
#   ctest -R "async_operations_example_test"
#   ctest -R "commit_waiting_example_test" 
#   ctest -R "configuration_sync_example_test"
#
# Run with verbose output to see example execution details:
#   ctest -L "completion" --verbose
#
# Run completion examples in parallel:
#   ctest -L "completion" -j$(nproc)