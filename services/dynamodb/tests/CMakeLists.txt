# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The DynamoDB module's tests (.kiro/specs/dynamodb-compatible-api/).

include(kythira_test_helpers)

# add_raft_completion_test plus the module's headers. Binaries and working
# directories go to build/tests/, which the coverage job collects test
# binaries from, as the other modules' do.
function(kythira_dynamodb_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_dynamodb)
    target_compile_definitions(${test_name} PRIVATE
        KYTHIRA_DYNAMODB_SOURCE_DIR="${KYTHIRA_DYNAMODB_SOURCE_DIR}")
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

# Every module test carries `dynamodb` and `dynamodb-module`;
# dynamodb-module.yml checks that no `dynamodb-module` test exists with the
# module off.

# Task 1: every operation in the pinned models is implemented or listed in
# doc/unsupported.md (Requirement 2.1, 2.2), and the generated shape tables
# load into the core codec.
kythira_dynamodb_test(dynamodb_model_coverage_test dynamodb_model_coverage_test.cpp)
set_tests_properties(dynamodb_model_coverage_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# Task 1: every request the recorded clients sent (evidence/) frames and
# validates against the pinned model, unknown members refused
# (Requirement 2.3).
kythira_dynamodb_test(dynamodb_wire_evidence_test dynamodb_wire_evidence_test.cpp)
set_tests_properties(dynamodb_wire_evidence_test PROPERTIES
    LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)

# The checked-in shape tables are what core's scripts/gen_aws_json_shapes.py
# (KYTHIRA_AWS_JSON_SHAPES_GENERATOR) makes of the pinned models, so a model
# upgrade carries its regenerated tables and their diff (Requirement 2.4).
# tools/regen_models.sh writes them; these run the same commands with
# --check. Skipped when configure found no Python, like core's
# aws_json_fixture_model_fresh.
if(Python3_EXECUTABLE)
foreach(_ddb_model IN ITEMS dynamodb dynamodbstreams)
    add_test(NAME ${_ddb_model}_model_header_fresh
        COMMAND ${Python3_EXECUTABLE} "${KYTHIRA_AWS_JSON_SHAPES_GENERATOR}"
            model/${_ddb_model}-2012-08-10.json
            include/kythira/dynamodb/model/${_ddb_model}_model.hpp
            --namespace kythira::dynamodb::${_ddb_model}_model
            --label services/dynamodb/model/${_ddb_model}-2012-08-10.json
            --check
        WORKING_DIRECTORY "${KYTHIRA_DYNAMODB_SOURCE_DIR}")
    set_tests_properties(${_ddb_model}_model_header_fresh PROPERTIES
        LABELS "unit;dynamodb;dynamodb-module" TIMEOUT 60)
endforeach()
endif()

kythira_scale_test_timeouts()
