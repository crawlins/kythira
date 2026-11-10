# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger module's tests (.kiro/specs/verifiable-ledger/).

include(kythira_test_helpers)

# add_raft_completion_test plus the module's headers. Binaries and working
# directories go to build/tests/, which the coverage job collects test
# binaries from, as the other modules' do.
function(kythira_ledger_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_ledger)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

# Every module test carries `ledger`.

# Task 2: the Ion hash against the amazon-ion/ion-hash-test suite
# (vectors/README.md) and the properties the ledger relies on.
kythira_ledger_test(ion_hash_test ion_hash_test.cpp)
target_compile_definitions(ion_hash_test PRIVATE
    KYTHIRA_LEDGER_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vectors")
set_tests_properties(ion_hash_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 2: dot, proof folding and the Merkle accumulator, against the AWS QLDB
# verification tutorial's functions (tools/qldb_dot_vectors.py).
kythira_ledger_test(ledger_merkle_test ledger_merkle_test.cpp)
set_tests_properties(ledger_merkle_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 3: the ledger state machine on memory_kv_engine: tables, document
# writes, revisions, the block chain, the leader's hash check, restart and
# checkpoint.
kythira_ledger_test(ledger_state_machine_test ledger_state_machine_test.cpp)
set_tests_properties(ledger_state_machine_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 5: transactions: snapshot reads, read-set validation, limits,
# session expiry and leader change, and the serializability property test.
kythira_ledger_test(ledger_transactions_test ledger_transactions_test.cpp)
set_tests_properties(ledger_transactions_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)
# Task 6: history, equality indexes and bounded scans.
kythira_ledger_test(ledger_history_index_test ledger_history_index_test.cpp)
set_tests_properties(ledger_history_index_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 8: segments, offload and ExportJournal.
kythira_ledger_test(ledger_segments_test ledger_segments_test.cpp)
set_tests_properties(ledger_segments_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 10: offline verification, the byte-by-byte tamper sweep, proofs, and
# ledger_ctl's exit codes (the test runs the built tool).
kythira_ledger_test(ledger_verifier_test ledger_verifier_test.cpp)
target_compile_definitions(ledger_verifier_test PRIVATE
    KYTHIRA_LEDGER_CTL="$<TARGET_FILE:ledger_ctl>")
add_dependencies(ledger_verifier_test ledger_ctl)
set_tests_properties(ledger_verifier_test PROPERTIES LABELS "unit;ledger" TIMEOUT 180)

# Task 7: the ledger catalog and the native API over an in-process backend.
kythira_ledger_test(ledger_api_test ledger_api_test.cpp)
set_tests_properties(ledger_api_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)

# Task 7: the API over HTTPS with mTLS and the C++ client, against
# certificates from the test CA (certificate_authority, which needs OpenSSL
# and a future backend; the Ion job has both).
if(TARGET certificate_authority)
    kythira_ledger_test(ledger_http_test ledger_http_test.cpp)
    target_link_libraries(ledger_http_test PRIVATE certificate_authority OpenSSL::SSL)
    # ...and ledger_ctl verify --api against it (task 10).
    target_compile_definitions(ledger_http_test PRIVATE
        KYTHIRA_LEDGER_CTL="$<TARGET_FILE:ledger_ctl>")
    add_dependencies(ledger_http_test ledger_ctl)
    set_tests_properties(ledger_http_test PROPERTIES LABELS "unit;ledger;https" TIMEOUT 180)

    # Task 10: CA-signed digests, their checks, GetDigest with a signer, and
    # ledger_ctl's --ca.
    kythira_ledger_test(ledger_signing_test ledger_signing_test.cpp)
    target_link_libraries(ledger_signing_test PRIVATE certificate_authority)
    target_compile_definitions(ledger_signing_test PRIVATE
        KYTHIRA_LEDGER_CTL="$<TARGET_FILE:ledger_ctl>")
    add_dependencies(ledger_signing_test ledger_ctl)
    set_tests_properties(ledger_signing_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)
else()
    message(STATUS "ledger_http_test, ledger_signing_test: skipped (require certificate_authority)")
endif()

# Task 7c: the ledger node's group state machine (catalog or ledger by
# range, the split/merge veto), and the ledger host over multi_raft on the
# in-process message fabric (placement, routing, background index builds and
# seals, leader change).
kythira_ledger_test(ledger_group_state_machine_test ledger_group_state_machine_test.cpp)
set_tests_properties(ledger_group_state_machine_test PROPERTIES LABELS "unit;ledger" TIMEOUT 120)
kythira_ledger_test(ledger_host_test ledger_host_test.cpp)
target_link_libraries(ledger_host_test PRIVATE kythira_test_support OpenSSL::Crypto)
set_tests_properties(ledger_host_test PROPERTIES LABELS "integration;ledger" TIMEOUT 300)

# Task 1: ledger_node links and runs.
add_test(NAME ledger_node_self_test COMMAND ledger_node --self-test)
set_tests_properties(ledger_node_self_test PROPERTIES LABELS "unit;ledger" TIMEOUT 60)

# Task 7c: the ledger_node daemon end to end: three processes on loopback
# with Raft over mutual TLS, the HTTPS API and RocksDB, written through the
# C++ client, checked with ledger_ctl verify --api, through member kills and
# a whole-cluster restart. Only where the daemon itself is built (RocksDB and
# certificate_authority; services/ledger/cmd/CMakeLists.txt).
if(KYTHIRA_LEDGER_NODE_DAEMON AND TARGET certificate_authority)
    kythira_ledger_test(ledger_node_cluster_test ledger_node_cluster_test.cpp)
    target_link_libraries(ledger_node_cluster_test PRIVATE
        certificate_authority kythira_test_support OpenSSL::SSL)
    target_compile_definitions(ledger_node_cluster_test PRIVATE
        KYTHIRA_LEDGER_NODE="$<TARGET_FILE:ledger_node>"
        KYTHIRA_LEDGER_CTL="$<TARGET_FILE:ledger_ctl>")
    add_dependencies(ledger_node_cluster_test ledger_node ledger_ctl)
    # With the S3 export store built, the test also exports the ledger to
    # the in-process S3 mock and re-hashes what the node wrote.
    if(KYTHIRA_LEDGER_NODE_S3)
        target_compile_definitions(ledger_node_cluster_test PRIVATE KYTHIRA_LEDGER_NODE_S3=1)
    endif()
    set_tests_properties(ledger_node_cluster_test PROPERTIES
        LABELS "integration;ledger;https" TIMEOUT 300)
else()
    message(STATUS "ledger_node_cluster_test: skipped (requires the ledger_node daemon build)")
endif()

# Task 11 (Requirement 12.4): a replica on RocksDB killed at every kill
# point the state machine has (apply before and after the engine batch,
# sealing before, during and after the segment file, an offload's copy
# removal), recovered by replaying the same entries, and checked against a
# replica that never crashed. Needs libfiu (the kill points) and RocksDB.
if(FIU_FOUND AND TARGET raft_rocksdb_engine)
    kythira_ledger_test(ledger_crash_test ledger_crash_test.cpp)
    target_compile_definitions(ledger_crash_test PRIVATE FIU_ENABLE)
    target_link_libraries(ledger_crash_test PRIVATE raft_rocksdb_engine ${FIU_LDFLAGS})
    target_include_directories(ledger_crash_test PRIVATE ${FIU_INCLUDE_DIRS})
    set_tests_properties(ledger_crash_test PROPERTIES LABELS "integration;ledger" TIMEOUT 300)
else()
    message(STATUS "ledger_crash_test: skipped (requires libfiu and raft_rocksdb_engine)")
endif()

# Task 11 (Requirement 12.5): the container scenario, where the daemon is
# built (the image carries it), under the guard tests/docker_chaos/ is
# entered with.
if(KYTHIRA_LEDGER_NODE_DAEMON AND TARGET certificate_authority AND Boost_FOUND
   AND httplib_FOUND)
    add_subdirectory(docker)
endif()

kythira_scale_test_timeouts()
