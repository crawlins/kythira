# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The cairn-chaos-testing workload client (task 4). Needs only cpp-httplib
# and Boost.JSON; it reads the history format from tests/docker_chaos/.
if(NOT Boost_FOUND OR NOT httplib_FOUND)
    return()
endif()

find_package(Threads REQUIRED)

add_executable(chaos_workload main.cpp)

target_include_directories(chaos_workload PRIVATE
    "${CMAKE_SOURCE_DIR}/cmd/chaos_workload"
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos"
)

target_link_libraries(chaos_workload PRIVATE
    boost_json
    httplib::httplib
    Threads::Threads
)

set_target_properties(chaos_workload PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
