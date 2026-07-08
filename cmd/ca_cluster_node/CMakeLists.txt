if(NOT TARGET certificate_authority)
    message(STATUS "ca_cluster_node: skipped (requires OpenSSL)")
    return()
endif()
if(NOT TARGET httplib::httplib OR NOT TARGET boost_json)
    message(STATUS "ca_cluster_node: skipped (requires httplib and boost_json)")
    return()
endif()
if(NOT folly_FOUND OR NOT Boost_FOUND)
    message(STATUS "ca_cluster_node: skipped (requires Folly and Boost)")
    return()
endif()

add_executable(ca_cluster_node main.cpp)

target_include_directories(ca_cluster_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/cmd/ca_cluster_node"
)

target_link_libraries(ca_cluster_node PRIVATE
    certificate_authority   # brings in network_simulator -> Folly::folly + boost_context
    network_simulator
    boost_json
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_features(ca_cluster_node PRIVATE cxx_std_23)
set_target_properties(ca_cluster_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
message(STATUS "ca_cluster_node target: enabled")
