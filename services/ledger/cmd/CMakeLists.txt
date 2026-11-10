# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger module's composition roots (.kiro/specs/verifiable-ledger/).
# ledger_node is the daemon (task 7c); ledger_ctl verifies and proves
# (task 10). Like the other module daemons they run from the build
# tree and are in no install target.

# ledger_node: the daemon (task 7c) where its stack can be built: RocksDB
# (raft_rocksdb_engine, CONFIG_ENGINE_ROCKSDB) for the groups' state, and
# certificate_authority for OpenSSL and the future backend (as for
# iam_node; it carries KYTHIRA_HAS_OPENSSL and network_simulator). Elsewhere
# it is the self-test only, so the module still links everywhere it builds.
set(KYTHIRA_LEDGER_NODE_DAEMON OFF PARENT_SCOPE)
if(TARGET raft_rocksdb_engine AND TARGET certificate_authority)
    find_package(Threads REQUIRED)
    add_executable(ledger_node ledger_node/main.cpp ledger_node/config.cpp ledger_node/run.cpp
                               ledger_node/export_store.cpp)
    target_link_libraries(ledger_node PRIVATE
        kythira_ledger
        raft_rocksdb_engine     # the engine, and network_simulator (future backend)
        certificate_authority   # KYTHIRA_HAS_OPENSSL
        kythira_host_node_id    # KYTHIRA_NODE_ID_KIND
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    target_compile_definitions(ledger_node PRIVATE KYTHIRA_LEDGER_NODE_DAEMON=1)
    set_target_properties(ledger_node PROPERTIES JOB_POOL_COMPILE heavy_tu)
    set(KYTHIRA_LEDGER_NODE_DAEMON ON PARENT_SCOPE)
    message(STATUS "ledger_node: the daemon (RocksDB, mutual TLS Raft, HTTPS API)")

    # The export runner writes ExportJournal objects to S3 through
    # aws_s3_client, under the same two conditions multi_raft_node's
    # `--persistence s3` uses: the SDK found and CONFIG_AWS_S3_PERSISTENCE
    # on. Only export_store.cpp parses an SDK header. Without them
    # KYTHIRA_LEDGER_EXPORT_S3_REGION is refused at start-up and export
    # jobs finish as failed.
    set(KYTHIRA_LEDGER_NODE_S3 OFF PARENT_SCOPE)
    if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
        find_package(AWSSDK QUIET COMPONENTS core s3)
    endif()
    if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
        list(FIND AWSSDK_LINK_LIBRARIES aws-cpp-sdk-s3 _KYTHIRA_LEDGER_S3_LIB)
        if(_KYTHIRA_LEDGER_S3_LIB EQUAL -1)
            message(FATAL_ERROR
                "ledger_node: CONFIG_AWS_S3_PERSISTENCE=y but AWSSDK_LINK_LIBRARIES "
                "(${AWSSDK_LINK_LIBRARIES}) does not name aws-cpp-sdk-s3")
        endif()
        target_link_libraries(ledger_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
        target_compile_definitions(ledger_node PRIVATE KYTHIRA_HAS_AWS_SDK KYTHIRA_LEDGER_NODE_S3=1)
        set(KYTHIRA_LEDGER_NODE_S3 ON PARENT_SCOPE)
        message(STATUS "ledger_node: journal exports to S3 (aws_s3_client)")
    else()
        message(STATUS "ledger_node: no S3 export store (needs the AWS SDK and "
                       "CONFIG_AWS_S3_PERSISTENCE)")
    endif()
else()
    add_executable(ledger_node ledger_node/main.cpp)
    target_link_libraries(ledger_node PRIVATE kythira_ledger)
    message(STATUS "ledger_node: self-test only (the daemon needs raft_rocksdb_engine and "
                   "certificate_authority)")
endif()
target_compile_features(ledger_node PRIVATE cxx_std_23)
set_target_properties(ledger_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ledger_ctl: verify and prove (task 10), offline or through a node's API.
# `verify --api` uses the ledger client (Boost.Beast and OpenSSL), whose
# API types pull in the core's future backend; as for iam_node,
# certificate_authority carries that (through network_simulator), so the
# mode is built only where that target exists.
add_executable(ledger_ctl ledger_ctl/main.cpp)
target_link_libraries(ledger_ctl PRIVATE kythira_ledger)
if(TARGET certificate_authority)
    find_package(Threads REQUIRED)
    target_link_libraries(ledger_ctl PRIVATE certificate_authority OpenSSL::SSL Threads::Threads)
    target_compile_definitions(ledger_ctl PRIVATE KYTHIRA_LEDGER_CTL_API=1)
endif()
# `verify --data-dir` opens a ledger_node's RocksDB database.
if(TARGET raft_rocksdb_engine)
    target_link_libraries(ledger_ctl PRIVATE raft_rocksdb_engine)
    target_compile_definitions(ledger_ctl PRIVATE KYTHIRA_LEDGER_CTL_DATA_DIR=1)
endif()
target_compile_features(ledger_ctl PRIVATE cxx_std_23)
set_target_properties(ledger_ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
