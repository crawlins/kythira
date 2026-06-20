if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT httplib_FOUND)
    return()
endif()

add_executable(chaos_node main.cpp)

target_include_directories(chaos_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/raft"
    "${CMAKE_SOURCE_DIR}/cmd/chaos_node"
)

target_link_libraries(chaos_node PRIVATE
    network_simulator   # brings in Folly::folly + boost_context + Boost::system/thread
    boost_json
    httplib::httplib
)

if(CHAOS_TESTS_ENABLED)
    target_compile_definitions(chaos_node PRIVATE
        FIU_ENABLE
        KYTHIRA_FAULT_INJECTION
    )
    target_include_directories(chaos_node PRIVATE ${FIU_INCLUDE_DIRS})
    target_link_libraries(chaos_node PRIVATE ${FIU_LIBRARIES})
    target_compile_options(chaos_node PRIVATE ${FIU_CFLAGS_OTHER})
    message(STATUS "chaos_node: fault injection enabled (custom fiu_tcp_rc)")
else()
    message(STATUS "chaos_node: fault injection disabled (libfiu not found)")
endif()

set_target_properties(chaos_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
