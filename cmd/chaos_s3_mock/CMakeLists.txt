# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# tests/s3_stateful_mock.hpp as a container-ready server, for the cairn M3
# stack (.kiro/specs/cairn-chaos-testing/ task 14.2). The mock includes the
# object-store engine header for its MD5 helper, so it needs the future
# backend as chaos_node does; it is added from chaos_node's block.

find_package(Threads REQUIRED)

add_executable(chaos_s3_mock main.cpp)

target_include_directories(chaos_s3_mock PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests"
)

# network_simulator for the future backend's include flags and libraries:
# the mock's header reaches raft/future.hpp through the object-store engine.
target_link_libraries(chaos_s3_mock PRIVATE
    network_simulator
    boost_json
    httplib::httplib
    Threads::Threads
)

target_compile_features(chaos_s3_mock PRIVATE cxx_std_23)

set_target_properties(chaos_s3_mock PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
