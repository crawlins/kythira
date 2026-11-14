# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# kythira_node, the composite daemon (.kiro/specs/service-colocation/
# task 3): the services named in KYTHIRA_SERVICES, from those compiled in,
# on one Beast Raft listener. Entered from the top level after every
# services/ module, so each module's library exists when it is linked.
#
# A module joins the node by appending its library to the global property
# KYTHIRA_NODE_MODULES:
#
#   set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULES kythira_<m>_module)
#   set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULE_NAMES <m>)
#
# The second names the service as KYTHIRA_SERVICES does, so a test below
# checks that the linked node really offers it.
#
# The library holds the module's adapter and its one
# module_registrar<kythira_node_host> (include/raft/kythira_node_host.hpp).
# It is linked whole-archive, because nothing in the node names the
# registrar and the linker would otherwise drop it; so adding a module
# changes nothing here (Requirement 1.3).

if(ENABLE_COVERAGE OR NOT KYTHIRA_FUTURE_BACKEND_AVAILABLE OR NOT Boost_FOUND
   OR NOT OpenSSL_FOUND OR NOT KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT)
    message(STATUS "kythira_node: skipped (needs a future backend, Boost, OpenSSL, the Beast "
                   "transport and a non-coverage build)")
    return()
endif()

add_executable(kythira_node main.cpp)
target_link_libraries(kythira_node PRIVATE
    kythira_core_headers
    network_simulator      # Folly, boost_context, Boost::system/thread
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)
get_property(_kythira_node_modules GLOBAL PROPERTY KYTHIRA_NODE_MODULES)
foreach(_m IN LISTS _kythira_node_modules)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        target_link_libraries(kythira_node PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_m}>")
    else()
        target_link_libraries(kythira_node PRIVATE -Wl,--whole-archive ${_m} -Wl,--no-whole-archive)
    endif()
endforeach()
target_compile_features(kythira_node PRIVATE cxx_std_23)
set_target_properties(kythira_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(TARGET host_binaries)
    add_dependencies(host_binaries kythira_node)
endif()

# Requirement 2.1: --help lists the compiled-in services, and naming one
# that is not compiled in is a startup error that lists them.
add_test(NAME kythira_node_help COMMAND kythira_node --help)
set_tests_properties(kythira_node_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Compiled-in services"
    TIMEOUT 30 LABELS "unit;node")
add_test(NAME kythira_node_unknown_service COMMAND kythira_node)
set_tests_properties(kythira_node_unknown_service PROPERTIES
    ENVIRONMENT "KYTHIRA_SERVICES=no-such-service;KYTHIRA_NODE_ID=1;KYTHIRA_NODE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/unknown-service;KYTHIRA_NODE_INSECURE_PLAINTEXT=1"
    PASS_REGULAR_EXPRESSION "service 'no-such-service' is not compiled in; compiled in: "
    TIMEOUT 30 LABELS "unit;node")
get_property(_kythira_node_module_names GLOBAL PROPERTY KYTHIRA_NODE_MODULE_NAMES)
foreach(_name IN LISTS _kythira_node_module_names)
    add_test(NAME kythira_node_offers_${_name} COMMAND kythira_node --help)
    set_tests_properties(kythira_node_offers_${_name} PROPERTIES
        PASS_REGULAR_EXPRESSION "\n  ${_name}  \\("
        TIMEOUT 30 LABELS "unit;node")
endforeach()
