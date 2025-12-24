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
add_raft_example(http_transport_example http_transport_example.cpp)

# Add CoAP transport example programs
add_raft_example(coap_transport_basic_example coap_transport_basic_example.cpp)
add_raft_example(coap_dtls_security_example coap_dtls_security_example.cpp)
add_raft_example(coap_multicast_example coap_multicast_example.cpp)
add_raft_example(coap_block_transfer_example coap_block_transfer_example.cpp)

# Add CoAP integration and validation examples
add_raft_example(coap_raft_integration_example coap_raft_integration_example.cpp)
add_raft_example(coap_performance_validation_example coap_performance_validation_example.cpp)
