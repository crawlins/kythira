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

set_tests_properties(
    kinesis_model_coverage
    kinesis_model_coverage_checker
    kinesis_operations_up_to_date
    kinesis_wire_evidence
    PROPERTIES LABELS "unit;kinesis;kinesis-module" TIMEOUT 60)

# Task 6: kinesis_gateway_node links, checks its tables, and reads its
# configuration: a complete one is printed with the secret elided, and the
# refusals name the variable (cmd/kinesis_gateway_node/config.cpp). The
# daemon is skipped in coverage builds and without folly, and so are these.
if(TARGET kinesis_gateway_node)
    add_test(NAME kinesis_gateway_node_self_test COMMAND kinesis_gateway_node --self-test)
    set(_kinesis_env
        KYTHIRA_NODE_ID=1
        "KYTHIRA_PEERS=1=http://127.0.0.1:7001,2=http://127.0.0.1:7002"
        KYTHIRA_RAFT_BIND=127.0.0.1
        KINESIS_ALLOW_PLAINTEXT=true
        KINESIS_STATIC_KEYS_FILE=/nonexistent/keys
        KINESIS_DATA_DIR=/nonexistent/data
        KINESIS_FORWARD_SECRET=0123456789abcdef0123)
    add_test(NAME kinesis_gateway_node_config
        COMMAND "${CMAKE_COMMAND}" -E env ${_kinesis_env}
                $<TARGET_FILE:kinesis_gateway_node> --check-config)
    set_tests_properties(kinesis_gateway_node_config PROPERTIES
        PASS_REGULAR_EXPRESSION "forward_secret=set\niterator_keys=derived from the forward secret\npeer_gateway 1=127.0.0.1:4567\npeer_gateway 2=127.0.0.1:4567"
        FAIL_REGULAR_EXPRESSION "0123456789abcdef0123")
    # Two voters and no secret: refused, naming the variable.
    add_test(NAME kinesis_gateway_node_config_needs_secret
        COMMAND "${CMAKE_COMMAND}" -E env ${_kinesis_env} KINESIS_FORWARD_SECRET=
                $<TARGET_FILE:kinesis_gateway_node> --check-config)
    set_tests_properties(kinesis_gateway_node_config_needs_secret PROPERTIES
        PASS_REGULAR_EXPRESSION "KINESIS_FORWARD_SECRET")
    # No TLS and no plaintext opt-in: refused.
    add_test(NAME kinesis_gateway_node_config_needs_tls
        COMMAND "${CMAKE_COMMAND}" -E env ${_kinesis_env} KINESIS_ALLOW_PLAINTEXT=false
                $<TARGET_FILE:kinesis_gateway_node> --check-config)
    set_tests_properties(kinesis_gateway_node_config_needs_tls PROPERTIES
        PASS_REGULAR_EXPRESSION "KINESIS_TLS_CERT and KINESIS_TLS_KEY are required")
    set_tests_properties(
        kinesis_gateway_node_self_test
        kinesis_gateway_node_config
        kinesis_gateway_node_config_needs_secret
        kinesis_gateway_node_config_needs_tls
        PROPERTIES LABELS "unit;kinesis;kinesis-module" TIMEOUT 60)
endif()

# add_raft_completion_test plus the module's headers. Binaries and working
# directories go to build/tests/, which the coverage job collects test
# binaries from, as the IAM and Redis modules' do.
function(kythira_kinesis_test test_name source_file)
    add_raft_completion_test(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_kinesis)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        LABELS "unit;kinesis;kinesis-module"
        TIMEOUT 120)
endfunction()

# Task 5: hash keys, sequence numbers, routing keys and catalog records.
kythira_kinesis_test(kinesis_catalog_test kinesis_catalog_test.cpp)
# Task 5: the stream state machine (catalog commands, counters, segment
# index, split hook, split/absorb law, snapshot restore).
kythira_kinesis_test(kinesis_stream_state_machine_test kinesis_stream_state_machine_test.cpp)

# Task 4: the front end (SigV4, JSON and CBOR, authorization, dispatch), the
# generated action table and static-key roles, and the compatibility
# profile.
kythira_kinesis_test(kinesis_front_end_test kinesis_front_end_test.cpp)
kythira_kinesis_test(kinesis_actions_test kinesis_actions_test.cpp)
kythira_kinesis_test(kinesis_compat_profile_test kinesis_compat_profile_test.cpp)

# Task 6: stream lifecycle and writes, through the front end over the
# in-process cluster.
kythira_kinesis_test(kinesis_service_test kinesis_service_test.cpp)
# Task 7: shard iterators, the shard reader, GetShardIterator and
# GetRecords.
kythira_kinesis_test(kinesis_reads_test kinesis_reads_test.cpp)
# Task 6: forwarding proposals to the group leader (wire, HMAC) and the
# multi-raft cluster adapter.
kythira_kinesis_test(kinesis_forward_test kinesis_forward_test.cpp)
# Task 6: the gateway's HTTP surface on a real listener (API, internal
# propose and segment endpoints, health) and the peer client over plain
# HTTP and verified TLS. certificate_authority issues the test PKI.
if(TARGET certificate_authority)
    kythira_kinesis_test(kinesis_gateway_test kinesis_gateway_test.cpp)
    target_link_libraries(kinesis_gateway_test PRIVATE certificate_authority)
endif()

# The checked-in shape tables are what core's scripts/gen_aws_json_shapes.py
# (KYTHIRA_AWS_JSON_SHAPES_GENERATOR) makes of the pinned model, and the
# action table what tools/gen_kinesis_actions.py makes of the pinned service
# reference, so an upgrade carries its regenerated headers and their diff
# (Requirement 2.4).
add_test(NAME kinesis_model_header_fresh
    COMMAND "${Python3_EXECUTABLE}" "${KYTHIRA_AWS_JSON_SHAPES_GENERATOR}"
            model/service-2.json
            include/kythira/kinesis/model/kinesis_model.hpp
            --namespace kythira::kinesis::kinesis_model
            --label services/kinesis/model/service-2.json
            --check
    WORKING_DIRECTORY "${KYTHIRA_KINESIS_SOURCE_DIR}")
add_test(NAME kinesis_actions_header_fresh
    COMMAND "${Python3_EXECUTABLE}" tools/gen_kinesis_actions.py
            tools/kinesis-service-reference.json
            include/kythira/kinesis/kinesis_actions.hpp --check
    WORKING_DIRECTORY "${KYTHIRA_KINESIS_SOURCE_DIR}")
set_tests_properties(kinesis_model_header_fresh kinesis_actions_header_fresh PROPERTIES
    LABELS "unit;kinesis;kinesis-module" TIMEOUT 60)

kythira_scale_test_timeouts()

# Task 7: the cluster in containers (docker/), by service name only, with
# the AWS CLI v2 and boto3, under Docker or rootless Podman. Custom
# targets, not CTest entries.
add_subdirectory(docker)
