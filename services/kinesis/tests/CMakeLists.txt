# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis module's tests (.kiro/specs/kinesis-compatible-streams/).
#
# Every module test carries `kinesis` and `kinesis-module`. `kinesis` alone
# also marks core suites the module builds on (record_segment_store_test),
# which rightly exist with the module off; kinesis-module.yml asserts that
# no `kinesis-module` test exists in a module-off build.

include(kythira_test_helpers)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The module root, set by ../CMakeLists.txt.
set(_kinesis_dir "${KYTHIRA_KINESIS_SOURCE_DIR}/")

# Task 1, Requirement 2.2: every operation of the pinned model is
# implemented or listed in doc/unsupported.md.
add_test(NAME kinesis_model_coverage
    COMMAND "${Python3_EXECUTABLE}" "${_kinesis_dir}tools/check_coverage.py"
            "${_kinesis_dir}model/service-2.json"
            "${_kinesis_dir}implemented-operations.txt"
            "${_kinesis_dir}doc/unsupported.md")
add_test(NAME kinesis_model_coverage_checker
    COMMAND "${Python3_EXECUTABLE}" "${_kinesis_dir}tools/test_check_coverage.py")

# Task 1, Requirement 2.4: the checked-in operation table matches the model,
# so a model upgrade carries its regenerated header in the same change.
add_test(NAME kinesis_operations_up_to_date
    COMMAND "${Python3_EXECUTABLE}" "${_kinesis_dir}tools/gen_operations.py"
            "${_kinesis_dir}model/service-2.json"
            "${_kinesis_dir}include/kythira/kinesis/kinesis_operations.hpp" --check)

# Task 1: the checked-in wire evidence (evidence/README.md) is well formed,
# names only model operations, and shows each client's scenario finishing.
add_test(NAME kinesis_wire_evidence
    COMMAND "${Python3_EXECUTABLE}" "${_kinesis_dir}tests/evidence/check_evidence.py"
            "${_kinesis_dir}model/service-2.json"
            "${_kinesis_dir}tests/evidence/data")

# Task 1: kinesis_gateway_node links and runs.
add_test(NAME kinesis_gateway_node_self_test COMMAND kinesis_gateway_node --self-test)

set_tests_properties(
    kinesis_model_coverage
    kinesis_model_coverage_checker
    kinesis_operations_up_to_date
    kinesis_wire_evidence
    kinesis_gateway_node_self_test
    PROPERTIES LABELS "unit;kinesis;kinesis-module" TIMEOUT 60)

kythira_scale_test_timeouts()
