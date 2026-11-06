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
