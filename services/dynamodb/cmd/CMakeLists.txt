# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# dynamodb_gateway_node, the DynamoDB-compatible daemon
# (.kiro/specs/dynamodb-compatible-api/ task 7). Like the other module
# daemons it runs from the build tree and is in no install target.
#
# The state machines include the core persistence headers, which need a
# future backend: network_simulator carries the selected one (Folly, stdexec
# or Boost.Thread). The HTTPS listener is core's Beast host
# (raft/beast_https_server.hpp), header-only over Boost.Asio and OpenSSL.

if(NOT KYTHIRA_FUTURE_BACKEND_AVAILABLE OR NOT Boost_FOUND)
    message(STATUS "dynamodb_gateway_node: skipped (needs a future backend and Boost)")
    return()
endif()

find_package(Threads REQUIRED)
add_executable(dynamodb_gateway_node dynamodb_gateway_node/main.cpp)
target_link_libraries(dynamodb_gateway_node PRIVATE
    kythira_dynamodb    # <kythira/dynamodb/...>, the core headers, Boost.JSON, OpenSSL
    network_simulator   # the future backend
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND, for the cluster's node ids
    Threads::Threads
)
# A cluster node keeps its groups in one RocksDB database when the build
# has the engine (CONFIG_ENGINE_ROCKSDB); otherwise only the memory engine
# is offered and KYTHIRA_DYNAMODB_ENGINE=rocksdb is refused at start.
if(TARGET raft_rocksdb_engine)
    target_compile_definitions(dynamodb_gateway_node PRIVATE KYTHIRA_DYNAMODB_ROCKSDB)
    target_link_libraries(dynamodb_gateway_node PRIVATE raft_rocksdb_engine)
endif()
target_compile_features(dynamodb_gateway_node PRIVATE cxx_std_23)
set_target_properties(dynamodb_gateway_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --help documents every setting the daemon reads.
add_test(NAME dynamodb_gateway_node_help COMMAND dynamodb_gateway_node --help)
set_tests_properties(dynamodb_gateway_node_help PROPERTIES
    PASS_REGULAR_EXPRESSION "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE"
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 30)
