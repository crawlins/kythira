# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# redis_gateway_node — the Redis-compatible cache daemon
# (.kiro/specs/redis-compatible-kv/ task 10).
#
# Gated on CONFIG_REDIS_GATEWAY (services/redis/Kconfig) by the top-level
# CMakeLists.txt, which enters services/redis/ only with it on, so a build
# with the gateway off has no target here at all (Requirement 18.1). Not in
# any install target: like multi_raft_node it is run from the build tree and
# from the container images under services/redis/docker/.

if(NOT folly_FOUND OR NOT Boost_FOUND OR NOT OpenSSL_FOUND)
    message(STATUS "redis_gateway_node: skipped (needs folly, Boost and OpenSSL)")
    return()
endif()

# One multi_raft stack per translation unit — cmd/multi_raft_node's lesson.
add_executable(redis_gateway_node
    main.cpp
    config.cpp
    run_cbor.cpp
    run_cbor_file.cpp
    run_json.cpp
)

target_include_directories(redis_gateway_node PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # No tests/: the transport handles and the Types bundle come from the
    # public <raft/multi_raft_transport_handles.hpp>.
)

target_link_libraries(redis_gateway_node PRIVATE
    kythira_redis       # <kythira/redis/...> and the core headers
    network_simulator   # Folly, boost_context, Boost::system/thread
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
)

target_compile_definitions(redis_gateway_node PRIVATE KYTHIRA_BENCH_HAS_BEAST)
# Cross-shard transactions (.kiro/specs/multi-raft-transactions/ task 9):
# the KV state machine is wrapped, which changes its snapshot format, so it
# is a build choice. Off unless Kconfig turns it on explicitly.
if(KCONFIG_REDIS_GATEWAY_TRANSACTIONS)
    target_compile_definitions(redis_gateway_node PRIVATE KYTHIRA_REDIS_GATEWAY_TRANSACTIONS)
endif()
# Tier 2 (task 14): a multi-versioned KV and a timestamp oracle group.
if(KCONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
    target_compile_definitions(redis_gateway_node PRIVATE KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
endif()
target_compile_features(redis_gateway_node PRIVATE cxx_std_23)

set_target_properties(redis_gateway_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# The snapshot-isolation variant for the transaction chaos image
# (.kiro/specs/multi-raft-transactions/ task 15), whatever Kconfig selects
# for redis_gateway_node above: services/redis/docker/txn-chaos/Dockerfile
# copies it.
# EXCLUDE_FROM_ALL, so only that image's target pays for its compile.
add_executable(redis_gateway_node_snapshot EXCLUDE_FROM_ALL
    main.cpp
    config.cpp
    run_cbor.cpp
    run_cbor_file.cpp
    run_json.cpp
)
target_include_directories(redis_gateway_node_snapshot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(redis_gateway_node_snapshot PRIVATE
    kythira_redis
    network_simulator
    boost_json
    OpenSSL::SSL
    OpenSSL::Crypto
    kythira_host_node_id
)
target_compile_definitions(redis_gateway_node_snapshot PRIVATE
    KYTHIRA_BENCH_HAS_BEAST
    KYTHIRA_REDIS_GATEWAY_TRANSACTIONS
    KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION
)
target_compile_features(redis_gateway_node_snapshot PRIVATE cxx_std_23)
set_target_properties(redis_gateway_node_snapshot PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
