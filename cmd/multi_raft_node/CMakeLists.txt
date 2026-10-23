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
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)

target_compile_features(multi_raft_node PRIVATE cxx_std_23)

# The same feature macros the benchmark suite compiles under, so that
# `--transport beast` exists in exactly the configurations where the Tier B
# beast rows exist. A host that silently lacked a transport the matrix sweeps
# would produce a Tier C row that could not be compared with anything.
if(TARGET Boost::beast OR Boost_FOUND)
    target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_BENCH_HAS_BEAST)
endif()

# Proxygen on the same terms as the benchmark suite (tests/CMakeLists.txt's
# bench targets): `--transport proxygen` exists exactly when the Tier B
# proxygen rows do. Its two instantiations are their own translation units,
# added only here, so a build without Proxygen compiles nothing extra.
#
# The macro is set on main.cpp (for the dispatch) and the two Proxygen
# translation units ONLY, not target-wide. Target-wide it would make the four
# httplib/beast translation units parse the Proxygen and Folly I/O headers
# through host_stacks.hpp and the harness header -- compiler memory added to
# the very objects the split above exists to keep under budget, for templates
# none of them instantiates.
if(KYTHIRA_BUILD_PROXYGEN_TRANSPORT)
    target_sources(multi_raft_node PRIVATE
        run_proxygen_json.cpp
        run_proxygen_cbor.cpp
    )
    set_property(SOURCE main.cpp run_proxygen_json.cpp run_proxygen_cbor.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS KYTHIRA_BENCH_HAS_PROXYGEN)
    target_link_libraries(multi_raft_node PRIVATE
        proxygen::proxygen proxygen::proxygenhttpserver Folly::folly)
else()
    message(STATUS "multi_raft_node: --transport proxygen unavailable "
                   "(requires KYTHIRA_BUILD_PROXYGEN_TRANSPORT)")
endif()

# The AWS SDK, for two optional features that share one SDK lifetime in
# main.cpp (.kiro/specs/multi-raft-node-s3-persistence/ Requirement 6):
#
#   * EC2 tag-scan discovery (.kiro/specs/multi-machine-placement/ task 8),
#     whenever the SDK is found. A host built without the SDK keeps every
#     measured code path and refuses only `--discovery ec2-tag`, which no
#     measured row uses (Requirement 3.4 makes the static list the default).
#   * `--persistence s3`, when the SDK is found AND CONFIG_AWS_S3_PERSISTENCE
#     is on, the same two conditions raft_object_backup's S3 provider and the
#     aws_s3_client tests use. Without them the parser still accepts `s3` and
#     main refuses it naming the Kconfig switch (Requirement 7.2).
#
# ONE find_package with ONE component list built from what is enabled. Two
# calls would not add up: AWSSDKConfig.cmake resets AWSSDK_LINK_LIBRARIES to
# describe the latest request, so a second, narrower call would silently drop
# the first one's libraries. In practice the call is a no-op, because the
# top-level AWS_SDK block already found core, ec2, autoscaling, iam, s3 and sts
# and the config file short-circuits once AWSSDK_FOUND is true; the list
# states what this target needs, and the check below confirms the s3 library
# really is in the variable rather than assuming it.
set(_KYTHIRA_NODE_HAS_S3 FALSE)
if(AWSSDK_FOUND)
    set(_KYTHIRA_NODE_AWS_COMPONENTS core ec2)
    if(_KYTHIRA_GATE_AWS_S3_PERSISTENCE)
        list(APPEND _KYTHIRA_NODE_AWS_COMPONENTS s3)
    endif()
    find_package(AWSSDK QUIET COMPONENTS ${_KYTHIRA_NODE_AWS_COMPONENTS})
endif()
if(AWSSDK_FOUND)
    target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_HAS_AWS_SDK)
    target_link_libraries(multi_raft_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
    message(STATUS "multi_raft_node: EC2 tag-scan discovery enabled")
    if(_KYTHIRA_GATE_AWS_S3_PERSISTENCE)
        list(FIND AWSSDK_LINK_LIBRARIES aws-cpp-sdk-s3 _KYTHIRA_NODE_S3_LIB)
        if(_KYTHIRA_NODE_S3_LIB EQUAL -1)
            message(FATAL_ERROR
                "multi_raft_node: CONFIG_AWS_S3_PERSISTENCE=y but AWSSDK_LINK_LIBRARIES "
                "(${AWSSDK_LINK_LIBRARIES}) does not name aws-cpp-sdk-s3")
        endif()
        # The engines over aws_s3_client are compiled in this one translation
        # unit and reached through an interface (switchable_persistence.hpp's
        # s3_group_store), so no run_*.cpp parses an SDK header. The macro is
        # target-wide because config.cpp's help text and main.cpp's refusal
        # both read it, and it costs the heavy units nothing: they test it
        # nowhere.
        target_sources(multi_raft_node PRIVATE s3_store.cpp)
        set(_KYTHIRA_NODE_HAS_S3 TRUE)
        target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_NODE_HAS_S3_PERSISTENCE)
        message(STATUS "multi_raft_node: --persistence s3 enabled")
    else()
        message(STATUS "multi_raft_node: --persistence s3 unavailable "
                       "(CONFIG_AWS_S3_PERSISTENCE=n)")
    endif()
else()
    message(STATUS "multi_raft_node: no AWS SDK — --discovery ec2-tag and --persistence s3 "
                   "unavailable")
endif()

# The split alone is not the whole fix. Each run_*.cpp still measures
# 5,407 MiB -- the same class as three_way_http_transport_equivalence_test's
# 5,649 MiB -- so three of them at once under `-j3` would put the runner right
# back over 16 GiB. `heavy_tu` (declared in the top-level CMakeLists.txt, where
# the full measurement table lives) admits one at a time, which is what makes
# the four of them -- six with Proxygen -- safe to have.
#
# The property is per-target, so main.cpp and config.cpp are serialised with
# them. That costs nothing worth naming: 25 s and 3 s respectively, against
# 204 s for one run_*.cpp.
set_target_properties(multi_raft_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ── --persistence s3 tests (.kiro/specs/multi-raft-node-s3-persistence/) ─────
# Here rather than in tests/ because they need this target's sources, its
# compile definitions and the built binary. The options unit test, which needs
# none of those, is in tests/CMakeLists.txt and runs in every build.
#
# Requirement 10.6: the store, process and write-behind tests need no cloud
# account and are registered only with the arm; the unavailable-arm test only
# without it.
if(TARGET Boost::unit_test_framework)
    if(_KYTHIRA_NODE_HAS_S3)
        # The S3 arm through the real aws_s3_client against the stateful mock:
        # fencing, the durability class, staging and the barrier (tasks 6.2/6.3).
        add_executable(multi_raft_node_s3_store_test
            "${CMAKE_SOURCE_DIR}/tests/multi_raft_node_s3_store_test.cpp"
            config.cpp
            s3_store.cpp)
        target_include_directories(multi_raft_node_s3_store_test PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_SOURCE_DIR}/cmd/multi_raft_node"
            "${CMAKE_SOURCE_DIR}/tests")
        target_compile_definitions(multi_raft_node_s3_store_test PRIVATE
            KYTHIRA_HAS_AWS_SDK KYTHIRA_NODE_HAS_S3_PERSISTENCE)
        target_link_libraries(multi_raft_node_s3_store_test PRIVATE
            Boost::unit_test_framework network_simulator boost_json httplib::httplib
            kythira_host_node_id ${AWSSDK_LINK_LIBRARIES})
        target_compile_features(multi_raft_node_s3_store_test PRIVATE cxx_std_23)
        add_test(NAME multi_raft_node_s3_store_test COMMAND multi_raft_node_s3_store_test)
        set_tests_properties(multi_raft_node_s3_store_test PROPERTIES
            TIMEOUT 120
            LABELS "unit;multi-raft;object-store;aws")

        # Three hosts on the mock: restart, duplicate host, and group commit
        # batching, crash and failed-flush cases (tasks 7.1 and 8).
        add_executable(multi_raft_node_s3_process_test
            "${CMAKE_SOURCE_DIR}/tests/multi_raft_node_s3_process_test.cpp")
        target_include_directories(multi_raft_node_s3_process_test PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${CMAKE_SOURCE_DIR}/cmd/multi_raft_bench_common"
            "${CMAKE_SOURCE_DIR}/tests")
        target_compile_definitions(multi_raft_node_s3_process_test PRIVATE
            MULTI_RAFT_NODE_PATH="$<TARGET_FILE:multi_raft_node>")
        # network_simulator for Folly's include flags and definitions:
        # multi_raft_kv_workload.hpp, for the command encoders, reaches
        # future.hpp, and glog refuses to be included without them.
        target_link_libraries(multi_raft_node_s3_process_test PRIVATE
            Boost::unit_test_framework network_simulator boost_json httplib::httplib
            kythira_host_node_id)
        target_compile_features(multi_raft_node_s3_process_test PRIVATE cxx_std_23)
        add_dependencies(multi_raft_node_s3_process_test multi_raft_node)
        add_test(NAME multi_raft_node_s3_process_test COMMAND multi_raft_node_s3_process_test)
        set_tests_properties(multi_raft_node_s3_process_test PROPERTIES
            TIMEOUT 900
            LABELS "integration;multi-raft;object-store;aws")
    else()
        add_executable(multi_raft_node_s3_unavailable_test
            "${CMAKE_SOURCE_DIR}/tests/multi_raft_node_s3_unavailable_test.cpp")
        target_compile_definitions(multi_raft_node_s3_unavailable_test PRIVATE
            MULTI_RAFT_NODE_PATH="$<TARGET_FILE:multi_raft_node>")
        target_link_libraries(multi_raft_node_s3_unavailable_test PRIVATE
            Boost::unit_test_framework)
        target_compile_features(multi_raft_node_s3_unavailable_test PRIVATE cxx_std_23)
        add_dependencies(multi_raft_node_s3_unavailable_test multi_raft_node)
        add_test(NAME multi_raft_node_s3_unavailable_test
            COMMAND multi_raft_node_s3_unavailable_test)
        set_tests_properties(multi_raft_node_s3_unavailable_test PROPERTIES
            TIMEOUT 60
            LABELS "unit;multi-raft;cli")
    endif()
endif()
