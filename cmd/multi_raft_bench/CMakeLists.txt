# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# multi_raft_bench — the load driver, in its own process
# (.kiro/specs/multi-raft-host-binary/ Requirement 3).
#
# Its own process is the more expensive answer to Appendix B's question 2 and
# is chosen deliberately: a driver sharing a process with a host makes the
# host's CPU and the client's indistinguishable in every number, which is
# precisely the confound Tiers C, D and E exist to remove.
#
# Not in any install target, for the same reason as the host.

if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "multi_raft_bench: skipped (needs folly, Boost and cpp-httplib)")
    return()
endif()

add_executable(multi_raft_bench main.cpp)

target_include_directories(multi_raft_bench PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/cmd/multi_raft_bench"
    "${CMAKE_SOURCE_DIR}/cmd/multi_raft_bench_common"
    # The workload, the statistics and the row format are SHARED with the
    # in-process harness. If a future reader finds this binary generating its
    # own keys, the design has failed and every cross-tier delta it produced is
    # a comparison of two workloads (Requirement 4.1).
    "${CMAKE_SOURCE_DIR}/tests"
)

target_link_libraries(multi_raft_bench PRIVATE
    network_simulator
    boost_json
    httplib::httplib
)

target_compile_features(multi_raft_bench PRIVATE cxx_std_23)

set_target_properties(multi_raft_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
