# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module's tests (.kiro/specs/iam-compatible-auth/).

include(kythira_test_helpers)

# add_raft_completion_test plus the module's headers. Binaries and working
# directories go to build/tests/, which the coverage job collects test
# binaries from, as the Redis module's do.
function(kythira_iam_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_iam)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

# Every module test carries `iam` (shared with core's IAM tests: the JWT,
# OIDC and SigV4 suites) and `iam-module`, which only module tests carry;
# iam-module.yml checks that no `iam-module` test exists with the module off.

# Task 10: the policy parser and matchers, run against the policy corpus
# (corpus/README.md has the format; tasks 11 and 12 grow it).
kythira_iam_test(iam_policy_test iam_policy_test.cpp)
target_compile_definitions(iam_policy_test PRIVATE
    KYTHIRA_IAM_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")
set_tests_properties(iam_policy_test PROPERTIES LABELS "unit;iam;iam-module" TIMEOUT 120)

# Task 8: the IAM state machine and its secret sealing, with property tests
# that replicas and snapshot restores agree and no plaintext secret reaches
# the log or a snapshot.
kythira_iam_test(iam_state_machine_test iam_state_machine_test.cpp)
set_tests_properties(iam_state_machine_test PROPERTIES LABELS "unit;iam;iam-module" TIMEOUT 120)

# The request-parameter rules of AWS's IAM and STS API references
# (iam_validation.hpp), one case per invariant group in
# .kiro/specs/iam-compatible-auth/invariants.md.
kythira_iam_test(iam_validation_test iam_validation_test.cpp)
set_tests_properties(iam_validation_test PROPERTIES LABELS "unit;iam;iam-module" TIMEOUT 60)

# Task 9: the admin operations behind iam_node's /v1/admin/* routes, and
# the request routing, against a bare state machine.
kythira_iam_test(iam_admin_test iam_admin_test.cpp)
set_tests_properties(iam_admin_test PROPERTIES LABELS "unit;iam;iam-module" TIMEOUT 120)

# Task 9: the throwaway PKI the iam_node tests use. iam_test_certs writes it
# for the compose scenario (docker/); iam_node_test builds it in process.
if(TARGET certificate_authority)
    add_executable(iam_test_certs iam_test_certs.cpp)
    target_link_libraries(iam_test_certs PRIVATE
        kythira_iam certificate_authority kythira_host_node_id)
    target_compile_features(iam_test_certs PRIVATE cxx_std_23)
    set_target_properties(iam_test_certs PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

# Task 9: iam_node and kythira-iam as processes: a three-node cluster over
# mutual-TLS Raft RPC, a read replica, leader loss, and a member with the
# wrong unseal passphrase. Ports come from test_ports.hpp (the test fabric).
if(TARGET iam_node AND TARGET kythira-iam AND TARGET certificate_authority)
    kythira_iam_test(iam_node_test iam_node_test.cpp)
    target_link_libraries(iam_node_test PRIVATE
        kythira_test_support certificate_authority kythira_host_node_id)
    target_compile_definitions(iam_node_test PRIVATE
        IAM_NODE_PATH="$<TARGET_FILE:iam_node>"
        KYTHIRA_IAM_PATH="$<TARGET_FILE:kythira-iam>")
    add_dependencies(iam_node_test iam_node kythira-iam)
    set_tests_properties(iam_node_test PROPERTIES
        LABELS "integration;iam;iam-module" TIMEOUT 600)
else()
    message(STATUS "iam_node_test: skipped (needs iam_node, kythira-iam and certificate_authority)")
endif()

# Task 9: the same cluster in containers (docker/), by service name only,
# under Docker or rootless Podman. Custom targets, not CTest entries.
add_subdirectory(docker)
