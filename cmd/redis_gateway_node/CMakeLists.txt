# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# redis_gateway_node — the Redis-compatible cache daemon
# (.kiro/specs/redis-compatible-kv/ task 10).
#
# Gated on CONFIG_REDIS_GATEWAY (Kconfig) by the top-level CMakeLists.txt, so
# a build with the gateway off has no target here at all (Requirement 18.1).
# Not in any install target: like multi_raft_node it is run from the build
# tree and from the container images under tests/docker_chaos/.

if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT OpenSSL_FOUND)
    message(STATUS "redis_gateway_node: skipped (needs folly, Boost and OpenSSL)")
    return()
endif()

# One multi_raft stack per translation unit — cmd/multi_raft_node's lesson.
add_executable(redis_gateway_node
    main.cpp
    config.cpp
    run_cbor.cpp
    run_json.cpp
)

target_include_directories(redis_gateway_node PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/cmd/redis_gateway_node"
    # transport_client_handle / transport_server_handle and the Beast bundle
    # are shared with the in-process harness rather than copied.
    "${CMAKE_SOURCE_DIR}/tests"
)

target_link_libraries(redis_gateway_node PRIVATE
    network_simulator   # Folly, boost_context, Boost::system/thread
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_compile_definitions(redis_gateway_node PRIVATE KYTHIRA_BENCH_HAS_BEAST)
target_compile_features(redis_gateway_node PRIVATE cxx_std_23)

set_target_properties(redis_gateway_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
