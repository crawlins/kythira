# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# multi_raft_node — a measurement host for `multi_raft`
# (.kiro/specs/multi-raft-host-binary/ Requirement 1).
#
# Deliberately NOT in any install target (Requirement 6.3). This is a
# measurement and testing host, not a supported server, and the surest way to
# keep it from being deployed is for `cmake --install` never to place it
# anywhere.

if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "multi_raft_node: skipped (needs folly, Boost and cpp-httplib)")
    return()
endif()

add_executable(multi_raft_node main.cpp config.cpp)

target_include_directories(multi_raft_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/cmd/multi_raft_node"
    "${CMAKE_SOURCE_DIR}/cmd/multi_raft_bench_common"
    # The workload vocabulary — kv_key, kv_shard_ranges, kv_partitioner and the
    # transport bundle — is SHARED with the in-process harness rather than
    # copied here. Requirement 4.1: a second implementation of the workload
    # turns every cross-tier delta into a comparison of two workloads. This
    # include path is what makes the sharing possible, and it is the reason
    # this binary is a measurement tool rather than a product.
    "${CMAKE_SOURCE_DIR}/tests"
)

target_link_libraries(multi_raft_node PRIVATE
    network_simulator   # Folly, boost_context, Boost::system/thread
    boost_json
    httplib::httplib
)

target_compile_features(multi_raft_node PRIVATE cxx_std_23)

# The same feature macros the benchmark suite compiles under, so that
# `--transport beast` exists in exactly the configurations where the Tier B
# beast rows exist. A host that silently lacked a transport the matrix sweeps
# would produce a Tier C row that could not be compared with anything.
if(TARGET Boost::beast OR Boost_FOUND)
    target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_BENCH_HAS_BEAST)
endif()

set_target_properties(multi_raft_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
