if(NOT POCO_DNSSD_FOUND OR NOT folly_FOUND OR NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "poco_discovery_node: skipped (requires Poco DNSSD, Folly, Boost, httplib)")
    return()
endif()

add_executable(poco_discovery_node main.cpp)

target_include_directories(poco_discovery_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    ${POCO_DNSSD_INCLUDE_DIRS}
)

# network_simulator INTERFACE target propagates KYTHIRA_HAS_POCO_DNSSD, Folly, Poco DNSSD libs.
target_link_libraries(poco_discovery_node PRIVATE
    network_simulator
    httplib::httplib
)

target_compile_features(poco_discovery_node PRIVATE cxx_std_23)

set_target_properties(poco_discovery_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

message(STATUS "poco_discovery_node target: enabled")
