# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis module's composition root
# (.kiro/specs/kinesis-compatible-streams/ task 6): kinesis_gateway_node,
# the daemon. Like the other module daemons it runs from the build tree and
# is in no install target. Its core *_impl.hpp includes are listed in
# ../boundary-allow.txt.
#
# It hosts a multi-raft cluster over Boost.Beast, so it needs what
# redis_gateway_node needs: folly plus a future backend (through
# network_simulator), Boost and OpenSSL. Not built for coverage, like
# redis_gateway_node: the instantiation is heavy and its logic is covered by
# the module's tests through the in-process cluster.

if(ENABLE_COVERAGE OR NOT KYTHIRA_FUTURE_BACKEND_AVAILABLE OR NOT folly_FOUND
   OR NOT Boost_FOUND OR NOT OpenSSL_FOUND)
    message(STATUS "kinesis_gateway_node: skipped (needs folly, a future backend, Boost and "
                   "OpenSSL, and a non-coverage build)")
    return()
endif()

# One multi_raft stack per translation unit: run_host.cpp alone holds it.
add_executable(kinesis_gateway_node
    kinesis_gateway_node/main.cpp
    kinesis_gateway_node/config.cpp
    kinesis_gateway_node/run_host.cpp
)
target_include_directories(kinesis_gateway_node PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/kinesis_gateway_node")
target_link_libraries(kinesis_gateway_node PRIVATE
    kythira_kinesis        # <kythira/kinesis/...> and the core headers
    network_simulator      # Folly, boost_context, Boost::system/thread
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)
target_compile_definitions(kinesis_gateway_node PRIVATE KYTHIRA_BENCH_HAS_BEAST)
target_compile_features(kinesis_gateway_node PRIVATE cxx_std_23)
set_target_properties(kinesis_gateway_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Tiering to S3 (task 10): only with the AWS SDK's s3 component, compiled in
# tier_s3.cpp alone so no other unit parses an SDK header. Without it the
# daemon refuses KINESIS_TIER_S3_BUCKET at startup and keeps every segment
# on disk.
if(AWSSDK_FOUND)
    find_package(AWSSDK QUIET COMPONENTS core s3)
endif()
set(_KINESIS_S3_TIER FALSE)
if(AWSSDK_FOUND)
    list(FIND AWSSDK_LINK_LIBRARIES aws-cpp-sdk-s3 _KINESIS_S3_LIB)
    if(NOT _KINESIS_S3_LIB EQUAL -1)
        set(_KINESIS_S3_TIER TRUE)
    endif()
endif()
if(_KINESIS_S3_TIER)
    target_sources(kinesis_gateway_node PRIVATE kinesis_gateway_node/tier_s3.cpp)
    target_link_libraries(kinesis_gateway_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
    target_compile_definitions(kinesis_gateway_node PRIVATE KYTHIRA_KINESIS_HAS_S3_TIER)
    message(STATUS "kinesis_gateway_node: S3 tiering enabled")
else()
    message(STATUS "kinesis_gateway_node: S3 tiering unavailable (no AWS SDK s3 component)")
endif()
