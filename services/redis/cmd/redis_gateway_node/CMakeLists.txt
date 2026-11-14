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

# One multi_raft stack per translation unit — cmd/multi_raft_node's lesson:
# run_cbor*.cpp and run_json.cpp each instantiate one serving stack
# (redis_serving.hpp); run_cbor_node.cpp and main.cpp none.
set(_REDIS_GATEWAY_SOURCES
    main.cpp
    config.cpp
    run_cbor.cpp
    run_cbor_file.cpp
    run_cbor_node.cpp
    run_json.cpp
)

# IAM authentication (.kiro/specs/iam-compatible-auth/ task 21): the
# composition root wires in the IAM module's embedded replica and verifier.
# Without CONFIG_REDIS_GATEWAY_IAM the daemon has no IAM code and no
# KYTHIRA_IAM_* variables (checked by redis_gateway_node_iam_switch below).
set(_REDIS_GATEWAY_IAM OFF)
if(KCONFIG_REDIS_GATEWAY_IAM AND KYTHIRA_BUILD_IAM AND TARGET certificate_authority)
    set(_REDIS_GATEWAY_IAM ON)
endif()
# persistent-state-machine task 7: KYTHIRA_CACHE_PERSISTENCE=engine keeps
# values in a RocksDB database per node. Its stack is one more translation
# unit, built only when Kconfig asks, so other builds never compile or link
# RocksDB.
if(KCONFIG_REDIS_GATEWAY_ENGINE AND NOT TARGET raft_rocksdb_engine)
    message(FATAL_ERROR
        "CONFIG_REDIS_GATEWAY_ENGINE needs CONFIG_ENGINE_ROCKSDB and the vcpkg feature rocksdb")
endif()

# What the daemon and the kythira_node module share: the include path,
# the libraries, and the Kconfig choices, which change the state machine's
# snapshot format and so must match on every node of a cluster.
function(_kythira_redis_gateway_settings target scope)
    target_include_directories(${target} ${scope}
        "${CMAKE_CURRENT_SOURCE_DIR}"
        # No tests/: the transport handles and the Types bundle come from the
        # public <raft/multi_raft_transport_handles.hpp>.
    )
    target_link_libraries(${target} ${scope}
        kythira_redis       # <kythira/redis/...> and the core headers
        network_simulator   # Folly, boost_context, Boost::system/thread
        boost_json
        OpenSSL::SSL
        OpenSSL::Crypto
        kythira_host_node_id   # KYTHIRA_NODE_ID_KIND
    )
    target_compile_definitions(${target} ${scope} KYTHIRA_BENCH_HAS_BEAST)
    target_compile_features(${target} ${scope} cxx_std_23)
    set_target_properties(${target} PROPERTIES JOB_POOL_COMPILE heavy_tu)
endfunction()

# The Kconfig-selected build: CONFIG_REDIS_GATEWAY_TRANSACTIONS wraps the KV
# state machine for cross-shard transactions (.kiro/specs/
# multi-raft-transactions/ task 9), CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION
# adds a multi-versioned KV and a timestamp oracle group (task 14), each
# off unless Kconfig turns it on explicitly; IAM and the engine stack as
# above.
function(_kythira_redis_gateway_kconfig target scope)
    if(KCONFIG_REDIS_GATEWAY_TRANSACTIONS)
        target_compile_definitions(${target} ${scope} KYTHIRA_REDIS_GATEWAY_TRANSACTIONS)
    endif()
    if(KCONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
        target_compile_definitions(${target} ${scope} KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION)
    endif()
    if(_REDIS_GATEWAY_IAM)
        target_sources(${target} PRIVATE iam_auth.cpp)
        target_compile_definitions(${target} ${scope} KYTHIRA_REDIS_GATEWAY_IAM)
        # kythira_iam is defined by services/iam/, which the top level
        # enters after this module; CMake resolves the name when it
        # generates. certificate_authority carries KYTHIRA_HAS_OPENSSL,
        # which the IAM replica's TLS RPC headers need, as for iam_node.
        target_link_libraries(${target} ${scope} kythira_iam certificate_authority)
    endif()
    if(KCONFIG_REDIS_GATEWAY_ENGINE)
        target_compile_definitions(${target} ${scope} KYTHIRA_REDIS_GATEWAY_ENGINE)
        target_link_libraries(${target} ${scope} raft_rocksdb_engine)
    endif()
endfunction()

add_executable(redis_gateway_node ${_REDIS_GATEWAY_SOURCES})
_kythira_redis_gateway_settings(redis_gateway_node PRIVATE)
_kythira_redis_gateway_kconfig(redis_gateway_node PRIVATE)
if(KCONFIG_REDIS_GATEWAY_ENGINE)
    target_sources(redis_gateway_node PRIVATE run_cbor_engine.cpp)
endif()
set_target_properties(redis_gateway_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# The snapshot-isolation variant for the transaction chaos image
# (.kiro/specs/multi-raft-transactions/ task 15), whatever Kconfig selects
# for redis_gateway_node above: services/redis/docker/txn-chaos/Dockerfile
# copies it.
# EXCLUDE_FROM_ALL, so only that image's target pays for its compile.
add_executable(redis_gateway_node_snapshot EXCLUDE_FROM_ALL ${_REDIS_GATEWAY_SOURCES})
_kythira_redis_gateway_settings(redis_gateway_node_snapshot PRIVATE)
target_compile_definitions(redis_gateway_node_snapshot PRIVATE
    KYTHIRA_REDIS_GATEWAY_TRANSACTIONS
    KYTHIRA_REDIS_GATEWAY_SNAPSHOT_ISOLATION
)
set_target_properties(redis_gateway_node_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# The Redis module for kythira_node (.kiro/specs/service-colocation/
# task 7): its registrar and one serving stack per storage over the node's
# Beast transport, with the Kconfig choices redis_gateway_node has, linked
# whole-archive through KYTHIRA_NODE_MODULES (cmd/kythira_node/). Not in a
# coverage build, which has no kythira_node to link it into.
if(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT AND KYTHIRA_FUTURE_BACKEND_AVAILABLE
   AND NOT ENABLE_COVERAGE)
    add_library(kythira_redis_module STATIC
        config.cpp
        redis_module_registrar.cpp
        redis_module_memory.cpp
        redis_module_file.cpp
    )
    _kythira_redis_gateway_settings(kythira_redis_module PUBLIC)
    _kythira_redis_gateway_kconfig(kythira_redis_module PUBLIC)
    if(KCONFIG_REDIS_GATEWAY_ENGINE)
        target_sources(kythira_redis_module PRIVATE redis_module_engine.cpp)
    endif()
    set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULES kythira_redis_module)
    set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULE_NAMES redis)
endif()

# A build with CONFIG_REDIS_GATEWAY_IAM off has no IAM variables, and one
# with it on documents them (iam-compatible-auth task 21).
add_test(NAME redis_gateway_node_iam_switch COMMAND redis_gateway_node --help)
if(_REDIS_GATEWAY_IAM)
    set_tests_properties(redis_gateway_node_iam_switch PROPERTIES
        PASS_REGULAR_EXPRESSION "KYTHIRA_IAM_VOTERS")
else()
    set_tests_properties(redis_gateway_node_iam_switch PROPERTIES
        FAIL_REGULAR_EXPRESSION "KYTHIRA_IAM_")
endif()
set_tests_properties(redis_gateway_node_iam_switch PROPERTIES LABELS "unit;redis" TIMEOUT 30)
