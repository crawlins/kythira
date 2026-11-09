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

# Task 1: ledger_node links and runs.
add_test(NAME ledger_node_self_test COMMAND ledger_node --self-test)
set_tests_properties(ledger_node_self_test PROPERTIES LABELS "unit;ledger" TIMEOUT 60)

kythira_scale_test_timeouts()
