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

    # Add Boost.Beast transport dependencies for beast_transport_example
    # (.kiro/specs/boost-beast-http-transport/) -- only registered at all
    # when KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT is set, see below.
    if(${example_name} STREQUAL "beast_transport_example")
        target_link_libraries(${example_name} PRIVATE Boost::beast)
        if(TARGET OpenSSL::SSL)
            target_link_libraries(${example_name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_link_libraries(${example_name} PRIVATE atomic)
        endif()
    endif()

    # Add Proxygen transport dependencies for proxygen_transport_example
    # (.kiro/specs/proxygen-http-transport/) -- only registered at all when
    # KYTHIRA_BUILD_PROXYGEN_TRANSPORT is set, see below.
    if(${example_name} STREQUAL "proxygen_transport_example")
        target_link_libraries(${example_name} PRIVATE proxygen::proxygen proxygen::proxygenhttpserver)
        if(TARGET OpenSSL::SSL)
            target_link_libraries(${example_name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_link_libraries(${example_name} PRIVATE atomic)
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

if(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT)
    add_raft_example(beast_transport_example beast_transport_example.cpp)
else()
    message(STATUS "beast_transport_example: skipped (requires KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT)")
endif()

if(KYTHIRA_BUILD_PROXYGEN_TRANSPORT)
    add_raft_example(proxygen_transport_example proxygen_transport_example.cpp)
else()
    message(STATUS "proxygen_transport_example: skipped (requires KYTHIRA_BUILD_PROXYGEN_TRANSPORT)")
endif()

# Requirement 17 (.kiro/specs/proxygen-http-transport/) / the mid-implementation
# request to compare all three HTTP transports directly against each other:
# needs all three transports linked into the same binary, so it's gated on
# both flags rather than either add_raft_example()'s per-name special case.
if(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT AND KYTHIRA_BUILD_PROXYGEN_TRANSPORT)
    add_raft_example(http_transport_comparison_benchmark http_transport_comparison_benchmark.cpp)
    target_link_libraries(http_transport_comparison_benchmark PRIVATE
        Boost::beast proxygen::proxygen proxygen::proxygenhttpserver)
    if(TARGET OpenSSL::SSL)
        target_link_libraries(http_transport_comparison_benchmark PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_libraries(http_transport_comparison_benchmark PRIVATE atomic)
    endif()
    # add_raft_example()'s own name-based TIMEOUT heuristic (above) doesn't
    # match this target's name and would default it to 60s -- measured full
    # runtime is dominated by cpp-httplib's default TCP_NODELAY=false
    # behavior (~80ms/call Nagle/delayed-ACK interaction,
    # doc/http_transport_performance_comparison.md's own measured finding),
    # roughly 3 minutes for the original 2000-iteration cross-transport
    # comparison alone. A generous fixed override, not the "performance"
    # regex match, since this is a real measured comparison run (not a
    # correctness check needing only a reduced smoke test the way
    # future_backend_benchmark_report_test's own CTest entry is). Bumped
    # from the original 300s: Requirement 17.1/17.3's two added comparisons
    # (generic-bridge-vs-fast-path, and the same comparison at a 1 MiB
    # install_snapshot body size) run two more 2000-iteration scenarios plus
    # two 200-iteration large-body ones in the same process -- this margin
    # is a conservative estimate, not itself a re-measured number (this
    # session's environment could not run a full vcpkg/proxygen/folly build
    # to re-measure; see doc/http_transport_performance_comparison.md).
    set_tests_properties(http_transport_comparison_benchmark_test PROPERTIES
        TIMEOUT 600
        # Confirmed by direct observation: running this test's ~3 minutes of
        # real network I/O concurrently with raft_comprehensive_performance_
        # benchmark's own latency-coefficient-of-variation check under
        # `ctest -j4` caused that unrelated test to fail on CPU contention
        # alone (it passes cleanly standalone) -- the same category of CI
        # flakiness ca_cluster_node_test's own PROCESSORS property (below,
        # this same mechanism) already documents and works around. PROCESSORS
        # matching the runner's core count stops ctest's scheduler from
        # starting other tests alongside this one, without hard-serializing
        # the whole suite around it (RUN_SERIAL would do that unconditionally).
        PROCESSORS 4
        LABELS "example;raft;performance;slow"
    )
else()
    message(STATUS "http_transport_comparison_benchmark: skipped (requires both "
                    "KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT and KYTHIRA_BUILD_PROXYGEN_TRANSPORT)")
endif()

# CoAP transport examples - demonstrate CoAP/DTLS integration.
# Gated on the feature symbol: CONFIG_COAP_TRANSPORT=n means no CoAP target
# exists anywhere, examples included.
if(KYTHIRA_BUILD_COAP_TRANSPORT)
    add_raft_example(coap_transport_basic_example coap_transport_basic_example.cpp)
    add_raft_example(coap_dtls_security_example coap_dtls_security_example.cpp)
    add_raft_example(coap_multicast_example coap_multicast_example.cpp)
    add_raft_example(coap_block_transfer_example coap_block_transfer_example.cpp)
    add_raft_example(coap_performance_validation_example coap_performance_validation_example.cpp)
    add_raft_example(coap_final_integration_validation coap_final_integration_validation.cpp)
endif()

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