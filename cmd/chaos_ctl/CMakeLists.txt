# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# chaos_ctl, the cairn tool server's logic (cairn-chaos-testing task 7). Like
# chaos_workload it needs only cpp-httplib and Boost.JSON, and it builds on
# the docker_chaos harness and checkers in tests/docker_chaos/.
if(NOT Boost_FOUND OR NOT httplib_FOUND)
    return()
endif()

find_package(Threads REQUIRED)

add_executable(chaos_ctl main.cpp)

target_include_directories(chaos_ctl PRIVATE
    "${CMAKE_SOURCE_DIR}/cmd/chaos_ctl"
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos"
)

target_link_libraries(chaos_ctl PRIVATE
    boost_json
    httplib::httplib
    Threads::Threads
)

set_target_properties(chaos_ctl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# fault.failpoint's allowlist, next to the binary where chaos_ctl looks for
# it by default. scripts/gen-cairn-failpoints.py --check keeps the source
# current with include/raft/.
configure_file("${CMAKE_SOURCE_DIR}/docker/cairn/failpoints.allowlist"
               "${CMAKE_BINARY_DIR}/cairn-failpoints.allowlist" COPYONLY)

# workload.start runs chaos_workload from the same directory.
if(TARGET chaos_workload)
    add_dependencies(chaos_ctl chaos_workload)
endif()
