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
# What the daemon and the module (service colocation task 6) share: the S3
# stores and IAM's replica, with the compile definitions saying which are
# in, so both see one dynamodb_module.hpp.
add_library(kythira_dynamodb_node_common STATIC dynamodb_gateway_node/backup_s3.cpp)
target_include_directories(kythira_dynamodb_node_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/dynamodb_gateway_node")
target_link_libraries(kythira_dynamodb_node_common PUBLIC
    kythira_dynamodb    # <kythira/dynamodb/...>, the core headers, Boost.JSON, OpenSSL
    network_simulator   # the future backend
    kythira_host_node_id   # KYTHIRA_NODE_ID_KIND, for the cluster's node ids
    Threads::Threads
)
target_compile_features(kythira_dynamodb_node_common PUBLIC cxx_std_23)

add_executable(dynamodb_gateway_node dynamodb_gateway_node/main.cpp)
target_link_libraries(dynamodb_gateway_node PRIVATE kythira_dynamodb_node_common)
# A cluster node keeps its groups in one RocksDB database when the build
# has the engine (CONFIG_ENGINE_ROCKSDB); otherwise only the memory engine
# is offered and KYTHIRA_DYNAMODB_ENGINE=rocksdb is refused at start.
if(TARGET raft_rocksdb_engine)
    target_compile_definitions(kythira_dynamodb_node_common PUBLIC KYTHIRA_DYNAMODB_ROCKSDB)
    target_link_libraries(kythira_dynamodb_node_common PUBLIC raft_rocksdb_engine)
endif()
# Backups can live in S3 through aws_s3_client under the same two
# conditions ledger_node's export store uses: the SDK found and
# CONFIG_AWS_S3_PERSISTENCE on. Only backup_s3.cpp parses an SDK header.
# Without them KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET is refused at start-up;
# KYTHIRA_DYNAMODB_BACKUP_DIR works everywhere.
if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    find_package(AWSSDK QUIET COMPONENTS core s3)
endif()
if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    list(FIND AWSSDK_LINK_LIBRARIES aws-cpp-sdk-s3 _KYTHIRA_DYNAMODB_S3_LIB)
    if(_KYTHIRA_DYNAMODB_S3_LIB EQUAL -1)
        message(FATAL_ERROR
            "dynamodb_gateway_node: CONFIG_AWS_S3_PERSISTENCE=y but AWSSDK_LINK_LIBRARIES "
            "(${AWSSDK_LINK_LIBRARIES}) does not name aws-cpp-sdk-s3")
    endif()
    target_link_libraries(kythira_dynamodb_node_common PUBLIC ${AWSSDK_LINK_LIBRARIES})
    target_compile_definitions(kythira_dynamodb_node_common PUBLIC
        KYTHIRA_HAS_AWS_SDK KYTHIRA_DYNAMODB_NODE_S3=1)
    message(STATUS "dynamodb_gateway_node: backups in S3 (aws_s3_client)")
else()
    message(STATUS "dynamodb_gateway_node: no S3 backup store (needs the AWS SDK and "
                   "CONFIG_AWS_S3_PERSISTENCE)")
endif()
# IAM authentication (task 28, KYTHIRA_DYNAMODB_AUTH=iam|both): the
# composition root wires in the IAM module's embedded replica and
# authorizer, as redis_gateway_node does. Without
# CONFIG_DYNAMODB_GATEWAY_IAM the daemon has no IAM code, documents no
# KYTHIRA_IAM_* variables, and refuses iam and both at start.
set(_DYNAMODB_GATEWAY_IAM OFF)
if(KCONFIG_DYNAMODB_GATEWAY_IAM AND KYTHIRA_BUILD_IAM AND TARGET certificate_authority)
    set(_DYNAMODB_GATEWAY_IAM ON)
    target_sources(kythira_dynamodb_node_common PRIVATE dynamodb_gateway_node/iam_auth.cpp)
    target_compile_definitions(kythira_dynamodb_node_common PUBLIC KYTHIRA_DYNAMODB_GATEWAY_IAM)
    # kythira_iam is defined by services/iam/, which the top level may enter
    # after this module; CMake resolves the name when it generates.
    target_link_libraries(kythira_dynamodb_node_common PUBLIC kythira_iam certificate_authority)
endif()
add_test(NAME dynamodb_gateway_node_iam_switch COMMAND dynamodb_gateway_node --help)
if(_DYNAMODB_GATEWAY_IAM)
    set_tests_properties(dynamodb_gateway_node_iam_switch PROPERTIES
        PASS_REGULAR_EXPRESSION "KYTHIRA_IAM_VOTERS")
else()
    set_tests_properties(dynamodb_gateway_node_iam_switch PROPERTIES
        FAIL_REGULAR_EXPRESSION "KYTHIRA_IAM_")
endif()
set_tests_properties(dynamodb_gateway_node_iam_switch PROPERTIES
    LABELS "unit;dynamodb" TIMEOUT 30)
target_compile_features(dynamodb_gateway_node PRIVATE cxx_std_23)
set_target_properties(dynamodb_gateway_node PROPERTIES
    JOB_POOL_COMPILE heavy_tu
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --help documents every setting the daemon reads.
add_test(NAME dynamodb_gateway_node_help COMMAND dynamodb_gateway_node --help)
set_tests_properties(dynamodb_gateway_node_help PROPERTIES
    PASS_REGULAR_EXPRESSION "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE"
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 30)

# The DynamoDB module for kythira_node (.kiro/specs/service-colocation/
# task 6): its registrar, linked whole-archive through KYTHIRA_NODE_MODULES
# (cmd/kythira_node/CMakeLists.txt). It instantiates the module over the
# node's Beast transport. kythira_node is not built for coverage, so
# neither is this.
if(KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT AND NOT ENABLE_COVERAGE)
    add_library(kythira_dynamodb_module STATIC
        dynamodb_gateway_node/dynamodb_module_registrar.cpp)
    target_link_libraries(kythira_dynamodb_module PUBLIC kythira_dynamodb_node_common)
    set_target_properties(kythira_dynamodb_module PROPERTIES JOB_POOL_COMPILE heavy_tu)
    set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULES kythira_dynamodb_module)
    set_property(GLOBAL APPEND PROPERTY KYTHIRA_NODE_MODULE_NAMES dynamodb)
endif()
