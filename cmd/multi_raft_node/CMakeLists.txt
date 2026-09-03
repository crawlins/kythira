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

# ONE TRANSLATION UNIT PER (transport x wire serializer) PAIR, and that is
# load-bearing rather than tidiness. All four `multi_raft` instantiations used
# to be in main.cpp; measured with this project's own CI compiler (g++-13,
# Release, stdexec backend) that single translation unit peaked at 10,974 MiB of
# compiler RSS -- the heaviest in the tree, ahead of the 5.6 GiB
# three_way_http_transport_equivalence_test that tests/CMakeLists.txt's
# `heavy_tu` Ninja job pool was created for. A GitHub runner has 16 GiB, so it
# plus any two ordinary compiles exhausted the machine: the
# `Full suite (stdexec future backend)` leg died once as an 82-minute stall
# into the 180-minute timeout and once as `exit 143` VM reclamation, with no
# compiler error anywhere in either log.
#
# It passed on main only because ccache answered for these objects. Any change
# under cmd/multi_raft_node/ made them cold and the leg failed -- which is why
# it read as intermittent.
#
# See cmd/multi_raft_node/host_stacks.hpp for the rest of the note.
add_executable(multi_raft_node
    main.cpp
    config.cpp
    run_httplib_json.cpp
    run_httplib_cbor.cpp
    run_beast_json.cpp
    run_beast_cbor.cpp
)

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

# The EC2 tag-scan discovery back-end, when the SDK is present
# (.kiro/specs/multi-machine-placement/ task 8). OPTIONAL on purpose, and
# guarded exactly as aws_ec2_quorum_manager is: a host built without the SDK
# keeps every measured code path it has today and refuses only `--discovery
# ec2-tag`, which no measured row uses. Requirement 3.4 makes the static list
# the default for a row, so the SDK is never in a measured window's way.
if(TARGET AWS::aws-sdk-cpp-ec2 OR AWSSDK_FOUND)
    find_package(AWSSDK QUIET COMPONENTS core ec2)
    if(AWSSDK_FOUND)
        target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_HAS_AWS_SDK)
        target_link_libraries(multi_raft_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
        message(STATUS "multi_raft_node: EC2 tag-scan discovery enabled")
    endif()
else()
    message(STATUS "multi_raft_node: no AWS SDK — --discovery ec2-tag unavailable")
endif()

# The split alone is not the whole fix. Each run_*.cpp still measures
# 5,407 MiB -- the same class as three_way_http_transport_equivalence_test's
# 5,649 MiB -- so three of them at once under `-j3` would put the runner right
# back over 16 GiB. `heavy_tu` (declared in the top-level CMakeLists.txt, where
# the full measurement table lives) admits one at a time, which is what makes
# the four of them safe to have.
#
# The property is per-target, so main.cpp and config.cpp are serialised with
# them. That costs nothing worth naming: 25 s and 3 s respectively, against
# 204 s for one run_*.cpp.
set_target_properties(multi_raft_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
